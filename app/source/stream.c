/* Finds an episode's stream, downloads it over HTTPS, and unpacks its muxed MPEG-TS pieces into
 * video and audio. */
#include <sys/time.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <unistd.h>

#include "config.h"
#include "resolve.h"
#include "sps_patch.h"
#include "stream.h"
#include "subs.h"

#define CA_FILE APP_DIR "cacert.pem"
#define DOH_URL "https://cloudflare-dns.com/dns-query"
#define SYNC_SEARCH 4096
#define SYNC_CHECKS 8
#define TS_PACKET 188
#define MAX_AUS 2048
#define MAX_SEGMENTS 4096
#define QUEUE_MAX 8
#define VIDEO_QUEUE_DEPTH 4
#define AUDIO_QUEUE_DEPTH 4
#define RETRIES 3
#define STREAM_TYPE_H264 0x1B
#define STREAM_TYPE_AAC 0x0F

struct track {
	const char *name;
	u32 es_type;
	u32 depth;
	struct segment *queue[QUEUE_MAX];
	u32 head, count;
	volatile int finished;
	sys_ppu_thread_t thread;
	int started;
	u32 segments, retries;
	u64 bytes, busy_ms;
	u32 slowest_ms, slowest_kbps_bytes;
	u32 empty_waits;
};

static struct track video = {.name = "video", .es_type = STREAM_TYPE_H264, .depth = VIDEO_QUEUE_DEPTH};
static struct track audio = {.name = "audio", .es_type = STREAM_TYPE_AAC, .depth = AUDIO_QUEUE_DEPTH};
static sys_mutex_t lock;
static volatile int stopping;
static const char *failure;
static int lock_ready;
static char start_ids[RESOLVE_URL_MAX];
static struct resolve_options start_options;
static float start_seconds;
static int reuse_stream;
static volatile float total_seconds;
static volatile int seek_pending;
static volatile float seek_target;
static volatile u32 stream_generation;
static volatile int resolved;
static struct subtitles subtitles;
static volatile int subtitles_ready;
static u32 sps_patched, sps_failed;
static int last_curl;
static long last_http;
static struct resolved_stream stream_info;
static u32 resolve_ms;

struct buffer {
	u8 *data;
	size_t len, cap;
};

static u64 now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (u64)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static size_t on_data(char *ptr, size_t size, size_t count, void *user)
{
	struct buffer *b = user;
	size_t n = size * count;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 1024 * 1024;
		while (b->len + n + 1 > cap)
			cap *= 2;
		u8 *grown = realloc(b->data, cap);
		if (!grown)
			return 0;
		b->data = grown;
		b->cap = cap;
	}
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = 0;
	return n;
}

/* On success the caller owns out->data. */
static int download(CURL *c, const char *url, const char *referer, int xhr, struct buffer *out)
{
	char referer_line[192];
	struct curl_slist *headers = NULL;
	long code = 0;

	snprintf(referer_line, sizeof(referer_line), "Referer: %s", referer);
	headers = curl_slist_append(headers, referer_line);
	if (xhr)
		headers = curl_slist_append(headers, "X-Requested-With: XMLHttpRequest");

	memset(out, 0, sizeof(*out));
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
	CURLcode r = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(headers);
	if (r == CURLE_OK && code == 200 && out->data)
		return 0;
	last_curl = (int)r;
	last_http = code;
	free(out->data);
	out->data = NULL;
	return -1;
}

static int resolve_fetch(void *ctx, const char *url, const char *referer, int xhr, char **body,
                         size_t *len)
{
	struct buffer b;
	if (download(ctx, url, referer, xhr, &b) != 0)
		return -1;
	*body = (char *)b.data;
	*len = b.len;
	return 0;
}

static u32 read_pts(const u8 *p)
{
	/* Low 32 of the 33 bits: enough for 13 hours of video. */
	return ((u32)(p[0] & 0x0E) << 29) | ((u32)p[1] << 22) | ((u32)(p[2] & 0xFE) << 14) |
	       ((u32)p[3] << 7) | (p[4] >> 1);
}

/* Reduce a segment in place to its elementary stream. Each PES packet starts one access unit. */
static int demux(struct segment *s, u32 es_type)
{
	int pmt_pid = -1, es_pid = -1;
	size_t out = 0;
	u32 n = 0;
	struct access_unit *aus = malloc(sizeof(*aus) * MAX_AUS);
	if (!aus)
		return -1;

	for (size_t off = 0; off + TS_PACKET <= s->size; off += TS_PACKET) {
		const u8 *p = s->data + off;
		if (p[0] != 0x47) {
			free(aus);
			return -1;
		}
		int pid = ((p[1] & 0x1F) << 8) | p[2];
		int start_unit = p[1] & 0x40;
		int adaptation = (p[3] >> 4) & 3;
		size_t start = 4;
		if (adaptation & 2)
			start += 1 + p[4];
		if (!(adaptation & 1) || start >= TS_PACKET)
			continue;

		if (pid == 0 && start_unit) {
			const u8 *t = p + start + 1 + p[start];
			pmt_pid = ((t[10] & 0x1F) << 8) | t[11];
		} else if (pid == pmt_pid && start_unit) {
			const u8 *t = p + start + 1 + p[start];
			int section = ((t[1] & 0x0F) << 8) | t[2];
			int i = 12 + (((t[10] & 0x0F) << 8) | t[11]);
			int end = 3 + section - 4;
			while (i + 5 <= end) {
				if (t[i] == es_type)
					es_pid = ((t[i + 1] & 0x1F) << 8) | t[i + 2];
				i += 5 + (((t[i + 3] & 0x0F) << 8) | t[i + 4]);
			}
		} else if (pid == es_pid) {
			const u8 *payload = p + start;
			size_t len = TS_PACKET - start;
			if (start_unit) {
				if (len < 14 || payload[0] || payload[1] || payload[2] != 1 ||
				    n == MAX_AUS) {
					free(aus);
					return -1;
				}
				u8 flags = payload[7];
				u32 pts = (flags & 0x80) ? read_pts(payload + 9) : 0;
				u32 dts = (flags & 0xC0) == 0xC0 ? read_pts(payload + 14) : pts;
				size_t header = 9 + payload[8];
				if (n > 0)
					aus[n - 1].size = out - aus[n - 1].offset;
				aus[n].offset = out;
				aus[n].size = 0;
				aus[n].pts = pts;
				aus[n].dts = dts;
				n++;
				payload += header;
				len -= header;
			}
			if (n > 0) {
				/* Headers are dropped, so the write position is always behind the read. */
				memmove(s->data + out, payload, len);
				out += len;
			}
		}
	}
	if (n == 0) {
		free(aus);
		return -1;
	}
	aus[n - 1].size = out - aus[n - 1].offset;
	s->size = out;
	s->aus = aus;
	s->au_count = n;
	return 0;
}

static int queue_has_room(struct track *t)
{
	sysMutexLock(lock, 0);
	int room = t->count < t->depth;
	sysMutexUnlock(lock);
	return room;
}

static struct segment *dequeue(struct track *t, int *finished)
{
	struct segment *s = NULL;
	sysMutexLock(lock, 0);
	if (t->count > 0) {
		s = t->queue[t->head];
		t->head = (t->head + 1) % QUEUE_MAX;
		t->count--;
	}
	sysMutexUnlock(lock);
	*finished = !s && t->finished;
	if (!s && !t->finished)
		t->empty_waits++;
	return s;
}

void stream_free(struct segment *s);

static void fail(const char *why)
{
	if (!failure)
		failure = why;
}

/* Strip whatever precedes the transport stream, such as a fake PNG header. */
static int strip_to_ts(struct buffer *b)
{
	for (size_t off = 0; off < SYNC_SEARCH; off++) {
		if (off + TS_PACKET * SYNC_CHECKS > b->len)
			return -1;
		int ok = 1;
		for (int i = 0; i < SYNC_CHECKS && ok; i++)
			ok = b->data[off + TS_PACKET * (size_t)i] == 0x47;
		if (ok) {
			memmove(b->data, b->data + off, b->len - off);
			b->len -= off;
			b->len -= b->len % TS_PACKET;
			return 0;
		}
	}
	return -1;
}

static struct segment *make_segment(const u8 *ts, size_t size, u32 es_type)
{
	struct segment *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->data = malloc(size + 1);
	if (!s->data) {
		free(s);
		return NULL;
	}
	memcpy(s->data, ts, size);
	s->size = size;
	if (demux(s, es_type) != 0) {
		free(s->data);
		free(s);
		return NULL;
	}
	return s;
}

/* Lets a stop interrupt a download that's getting nothing. */
static int on_progress(void *user, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
	(void)user;
	(void)dt;
	(void)dn;
	(void)ut;
	(void)un;
	/* Only a stop interrupts a download. A seek lets it finish, keeping the connection for the next
	 * piece: a new encrypted connection costs more than the rest of a piece does. */
	return stopping ? 1 : 0;
}

/* The piece holding a time. */
static int first_piece(const float *lengths, int count, float seconds)
{
	int first = 0;
	float elapsed = 0;
	while (first + 1 < count && elapsed + lengths[first] <= seconds) {
		elapsed += lengths[first];
		first++;
	}
	return first;
}

/* Queue a piece's video and audio, unless a seek arrived after it was fetched. Returns whether it did. */
static int enqueue_pair(struct segment *v, struct segment *a)
{
	int queued = 0;
	sysMutexLock(lock, 0);
	if (!seek_pending) {
		video.queue[(video.head + video.count) % QUEUE_MAX] = v;
		video.count++;
		audio.queue[(audio.head + audio.count) % QUEUE_MAX] = a;
		audio.count++;
		queued = 1;
	}
	sysMutexUnlock(lock);
	return queued;
}

static void download_thread(void *arg)
{
	(void)arg;
	struct buffer list = {0};
	/* One handle, so connections and DNS results get reused between pieces. */
	CURL *c = curl_easy_init();

	if (!c) {
		fail("curl_init");
		goto done;
	}
	curl_easy_setopt(c, CURLOPT_CAINFO, CA_FILE);
	/* As in the TV app: the home firewall's DNS filters the video host. */
	curl_easy_setopt(c, CURLOPT_DOH_URL, DOH_URL);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 3600L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);

	if (!reuse_stream) {
		u64 resolve_start = now_ms();
		const char *reason = resolve_stream(resolve_fetch, c, start_ids, &start_options, &stream_info);
		resolve_ms = (u32)(now_ms() - resolve_start);
		if (reason) {
			fail(reason);
			goto done;
		}
		if (stream_info.subtitle_url[0]) {
			struct buffer vtt;
			/* Subtitles are a bonus: playback goes ahead without them. */
			if (download(c, stream_info.subtitle_url, stream_info.referer, 0, &vtt) == 0) {
				int cues = subs_parse(&subtitles, (char *)vtt.data);
				app_log("subtitles cues=%d\n", cues);
				subtitles_ready = cues > 0;
				free(vtt.data);
			}
		}
	}
	resolved = 1;
	if (download(c, stream_info.media_url, stream_info.referer, 0, &list) != 0) {
		fail("media_playlist");
		goto done;
	}

	/* Walk the playlist once for its length and the piece to start from. */
	char **uris = malloc(sizeof(char *) * MAX_SEGMENTS);
	float *lengths = malloc(sizeof(float) * MAX_SEGMENTS);
	int count = 0;
	float pending_length = 0, total = 0;
	if (!uris || !lengths) {
		fail("out_of_memory");
		free(uris);
		free(lengths);
		goto done;
	}
	char *save = NULL;
	for (char *line = strtok_r((char *)list.data, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		if (strncmp(line, "#EXTINF:", 8) == 0) {
			pending_length = (float)atof(line + 8);
		} else if (line[0] && line[0] != '#' && count < MAX_SEGMENTS) {
			uris[count] = line;
			lengths[count] = pending_length;
			total += pending_length;
			count++;
		}
	}
	total_seconds = total;

	int index = first_piece(lengths, count, start_seconds);
	while (!stopping) {
		if (seek_pending) {
			index = first_piece(lengths, count, seek_target);
			seek_pending = 0;
		}
		if (index >= count) {
			/* The end, but a seek can still bring it back. */
			video.finished = 1;
			audio.finished = 1;
			usleep(20000);
			continue;
		}
		const char *uri = uris[index];
		while ((!queue_has_room(&video) || !queue_has_room(&audio)) && !stopping && !seek_pending)
			usleep(10000);
		if (stopping)
			break;
		if (seek_pending)
			continue;

		char url[RESOLVE_URL_MAX];
		resolve_join_url(stream_info.media_url, uri, url, sizeof(url));

		struct buffer body;
		u64 start = now_ms();
		int ok = -1;
		for (int attempt = 0; attempt < RETRIES && ok != 0 && !stopping && !seek_pending; attempt++) {
			ok = download(c, url, stream_info.referer, 0, &body);
			if (ok != 0 && !seek_pending)
				video.retries++;
		}
		if (seek_pending) {
			if (ok == 0)
				free(body.data);
			continue;
		}
		if (ok != 0) {
			if (!stopping)
				fail("segment_download");
			break;
		}
		u32 ms = (u32)(now_ms() - start);
		video.segments++;
		video.bytes += body.len;
		video.busy_ms += ms;
		if (ms > video.slowest_ms)
			video.slowest_ms = ms;

		struct segment *v = NULL, *a = NULL;
		if (strip_to_ts(&body) == 0) {
			v = make_segment(body.data, body.len, STREAM_TYPE_H264);
			for (u32 i = 0; v && stream_info.too_tall && i < v->au_count; i++) {
				int patched = sps_patch_au(v->data + v->aus[i].offset, v->aus[i].size);
				if (patched < 0)
					sps_failed++;
				else
					sps_patched += (u32)patched;
			}
			a = make_segment(body.data, body.len, STREAM_TYPE_AAC);
		}
		free(body.data);
		if (!v || !a) {
			fail("demux");
			stream_free(v);
			stream_free(a);
			break;
		}
		audio.segments++;
		audio.bytes += a->size;
		if (!enqueue_pair(v, a)) {
			stream_free(v);
			stream_free(a);
			continue;
		}
		index++;
	}
	free(uris);
	free(lengths);
done:
	free(list.data);
	if (c)
		curl_easy_cleanup(c);
	video.finished = 1;
	audio.finished = 1;
	sysThreadExit(0);
}

int stream_start(const char *server_ids, const struct resolve_options *options, float seconds, int reuse)
{
	if (!lock_ready) {
		sys_mutex_attr_t attr;
		sysMutexAttrInitialize(attr);
		sysMutexCreate(&lock, &attr);
		lock_ready = 1;
	}
	snprintf(start_ids, sizeof(start_ids), "%s", server_ids);
	start_options = *options;
	start_seconds = seconds;
	reuse_stream = reuse && stream_info.media_url[0];
	resolved = 0;
	stopping = 0;
	seek_pending = 0;
	failure = NULL;
	last_curl = 0;
	last_http = 0;
	if (!reuse_stream) {
		memset(&stream_info, 0, sizeof(stream_info));
		subtitles_ready = 0;
		subs_free(&subtitles);
		total_seconds = 0;
		sps_patched = sps_failed = 0;
	}
	resolve_ms = 0;
	for (int t = 0; t < 2; t++) {
		struct track *tr = t ? &audio : &video;
		tr->head = tr->count = 0;
		tr->finished = 0;
		tr->segments = tr->retries = tr->empty_waits = 0;
		tr->bytes = tr->busy_ms = 0;
		tr->slowest_ms = 0;
	}
	if (sysThreadCreate(&video.thread, download_thread, NULL, 1100, 512 * 1024,
	                    THREAD_JOINABLE, "download") != 0)
		return -1;
	video.started = 1;
	return 0;
}

struct segment *stream_next_video(int *finished)
{
	return dequeue(&video, finished);
}

struct segment *stream_next_audio(int *finished)
{
	return dequeue(&audio, finished);
}

void stream_free(struct segment *s)
{
	if (!s)
		return;
	free(s->aus);
	free(s->data);
	free(s);
}

void stream_seek(float seconds)
{
	sysMutexLock(lock, 0);
	seek_target = seconds;
	seek_pending = 1;
	stream_generation++;
	for (int t = 0; t < 2; t++) {
		struct track *tr = t ? &audio : &video;
		while (tr->count > 0) {
			stream_free(tr->queue[tr->head]);
			tr->head = (tr->head + 1) % QUEUE_MAX;
			tr->count--;
		}
		tr->finished = 0;
	}
	sysMutexUnlock(lock);
}

u32 stream_generation_now(void)
{
	return stream_generation;
}

void stream_request_stop(void)
{
	stopping = 1;
}

void stream_stop(void)
{
	u64 code;
	stopping = 1;
	if (video.started)
		sysThreadJoin(video.thread, &code);
	video.started = 0;
	for (int t = 0; t < 2; t++) {
		struct track *tr = t ? &audio : &video;
		while (tr->count > 0) {
			stream_free(tr->queue[tr->head]);
			tr->head = (tr->head + 1) % QUEUE_MAX;
			tr->count--;
		}
	}
}

float stream_duration(void)
{
	return total_seconds;
}

const char *stream_subtitle_at(float seconds)
{
	return subtitles_ready ? subs_at(&subtitles, seconds) : NULL;
}

int stream_intro(float *start, float *end)
{
	if (!resolved || stream_info.intro_end <= stream_info.intro_start)
		return 0;
	*start = stream_info.intro_start;
	*end = stream_info.intro_end;
	return 1;
}

const char *stream_failure(void)
{
	return failure;
}

void stream_log_stats(void)
{
	app_log("stream: %s ms=%u server=%s rendition=%dx%d too_tall=%d patched=%u/%u last_curl=%d last_http=%ld failure=%s\n",
	        stream_info.log, resolve_ms, stream_info.server, stream_info.width, stream_info.height,
	        stream_info.too_tall, sps_patched, sps_failed, last_curl, last_http, failure ? failure : "none");
	app_log("stream: pieces=%u bytes=%llu retries=%u slowest_ms=%u mbps_while_busy=%.2f\n", video.segments,
	        (unsigned long long)video.bytes, video.retries, video.slowest_ms,
	        video.busy_ms ? video.bytes * 8.0 / video.busy_ms / 1000.0 : 0.0);
}
