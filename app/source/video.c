/* Plays a stream with the hardware decoder, and audio as the clock: a decoded picture is shown once
 * the audio has reached its timestamp. The decoder writes into RSX memory and the shaders draw it. */
#include "video.h"

#include <malloc.h>
#include <ppu-asm.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event_queue.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <sysmodule/sysmodule.h>
#include <unistd.h>

#include <audio/audio.h>
#include <codec/vdec.h>
#include <neaacdec.h>

#include "config.h"
#include "gfx.h"
#include "stream.h"

/* The decoder returns pictures in decode order, so some wait here to be put in display order. */
#define POOL_SIZE 16
#define FEED_MIN_FREE 4
/* Big enough for any 720p stream: wider ones like 1330x720 exist, and decoded heights round up. */
#define MAX_PICTURE_WIDTH 1344
#define MAX_PICTURE_HEIGHT 736
/* The experimental 1080p mode needs larger pictures. */
#define MAX_TALL_WIDTH 1920
#define MAX_TALL_HEIGHT 1088
#define PICTURE_ALIGN 64
/* A picture may still be in use by the RSX for this many frames after a newer one is shown. */
#define KEEP_SHOWN 3

#define VDEC_LEVEL 42
#define VDEC_SPUS 4
#define VDEC_PPU_PRIO 1400
#define VDEC_SPU_PRIO 100
#define VDEC_STACK (64 * 1024)
#define MEM_PAGE_1M 0x400
#define ONE_MB (1024 * 1024)
#define LV2_MEMORY_ALLOCATE 348
#define LV2_MEMORY_FREE 349

#define AUDIO_RATE 48000
#define START_FRAMES 8
#define SEEK_END_MARGIN 5.0f
#define START_TIMEOUT_US 60000000
#define SEQUENCE_WAIT_STEPS 200
#define THREAD_PRIO_FEEDER 1000
#define THREAD_PRIO_AUDIO 900
#define FEEDER_STACK (64 * 1024)
#define AUDIO_STACK (256 * 1024)

enum slot_state { SLOT_FREE, SLOT_QUEUED, SLOT_SHOWN };

struct slot {
	u32 *pixels;
	u32 offset;
	u32 pts;
	u32 width, height;
	u32 status;
	enum slot_state state;
};

struct stats {
	u32 au_fed, au_errors, pics_out, pics_bad, pics_no_slot, pics_status_errors, last_status;
	u32 shown, late_dropped, too_old_dropped;
	u32 aac_frames, aac_errors, audio_underruns;
	u32 width, height;
};

static struct slot pool[POOL_SIZE];
static int pool_size;
static u32 picture_max_width, picture_max_height;
static char current_ids[RESOLVE_IDS_MAX];
static struct resolve_options current_options;
static sys_mutex_t pool_lock;
static int pool_lock_ready;
static struct stats st;

static u32 vdec_handle;
static u32 vdec_memory;
static int vdec_open_now;
static int modules_loaded;

static sys_ppu_thread_t feeder, audio;
static int threads_running;
static volatile int playing;
static volatile int paused;
static volatile int au_pending, seq_done, feeder_done, audio_start, audio_done, has_started;
static volatile u64 audio_samples;
static volatile u32 audio_base_pts;
static volatile int audio_base_set;
static const char *volatile failure;
static u64 wait_start_us;
static u32 last_shown_pts;
static u32 shown_since_seek;
static volatile int seeking;
/* From a seek until the decoder has been flushed, any picture is from before the seek. */
static volatile int flush_pending;
static int shown_slots[KEEP_SHOWN];

static u64 now_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (u64)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* ---- decoder ---- */

static void picture_out(void)
{
	u32 addr;
	vdecPictureFormat format;

	if (vdecGetPicItem(vdec_handle, &addr) != 0)
		return;
	vdecPicture *pic = (vdecPicture *)(u64)addr;
	/* The exact time travels in userdata: the decoder only gets rounded timestamps. */
	u32 pts = (u32)pic->userdata[0];

	memset(&format, 0, sizeof(format));
	/* Planar: the conversion to RGB happens in a shader, and takes far less memory. */
	format.format_type = VDEC_PICFMT_YUV420P;
	format.color_matrix = VDEC_COLOR_MATRIX_BT709;

	st.pics_out++;
	if (pic->status != 0) {
		st.pics_status_errors++;
		st.last_status = pic->status;
	}
	vdecH264Info *info = (vdecH264Info *)(u64)pic->codec_specific_addr;
	if (pic->attr != VDEC_PICTURE_NORMAL || !info->width || !info->height ||
	    info->width > picture_max_width || info->height > picture_max_height) {
		st.pics_bad++;
		vdecGetPicture(vdec_handle, &format, NULL);
		return;
	}
	st.width = info->width;
	st.height = info->height;
	if (flush_pending) {
		vdecGetPicture(vdec_handle, &format, NULL);
		return;
	}

	sysMutexLock(pool_lock, 0);
	int chosen = -1;
	for (int i = 0; i < pool_size; i++) {
		if (pool[i].state == SLOT_FREE) {
			chosen = i;
			break;
		}
	}
	sysMutexUnlock(pool_lock);
	if (chosen < 0) {
		st.pics_no_slot++;
		vdecGetPicture(vdec_handle, &format, NULL);
		return;
	}

	/* Only this callback takes free slots, so it stays ours without the lock. */
	vdecGetPicture(vdec_handle, &format, pool[chosen].pixels);

	sysMutexLock(pool_lock, 0);
	pool[chosen].pts = pts;
	pool[chosen].status = pic->status;
	pool[chosen].width = info->width;
	pool[chosen].height = info->height;
	pool[chosen].state = SLOT_QUEUED;
	sysMutexUnlock(pool_lock);
}

static u32 vdec_callback(u32 handle, u32 msgtype, u32 msgdata, u32 arg)
{
	(void)handle;
	(void)msgdata;
	(void)arg;
	switch (msgtype) {
	case VDEC_CALLBACK_AUDONE:
		au_pending = 0;
		break;
	case VDEC_CALLBACK_PICOUT:
		picture_out();
		break;
	case VDEC_CALLBACK_SEQDONE:
		seq_done = 1;
		break;
	}
	return 0;
}

static s32 allocate_system_memory(u64 size, u32 *addr)
{
	lv2syscall3(LV2_MEMORY_ALLOCATE, size, MEM_PAGE_1M, (u64)addr);
	return_to_user_prog(s32);
}

static s32 free_system_memory(u32 addr)
{
	lv2syscall1(LV2_MEMORY_FREE, (u64)addr);
	return_to_user_prog(s32);
}

static int open_decoder(void)
{
	vdecType type;
	vdecAttr attr;
	vdecConfig config;
	vdecClosure closure;

	if (!modules_loaded) {
		app_log("video modules vdec=%08x h264=%08x\n", sysModuleLoad(SYSMODULE_VDEC),
		        sysModuleLoad(SYSMODULE_VDEC_H264));
		modules_loaded = 1;
	}
	type.codec_type = VDEC_CODEC_TYPE_H264;
	type.profile_level = VDEC_LEVEL;
	s32 r = vdecQueryAttr(&type, &attr);
	if (r != 0) {
		app_log("vdecQueryAttr=%08x\n", r);
		return -1;
	}
	u64 alloc = ((u64)attr.mem_size + ONE_MB - 1) & ~(u64)(ONE_MB - 1);
	r = allocate_system_memory(alloc, &vdec_memory);
	if (r != 0) {
		app_log("vdec memory=%08x size=%llu\n", r, (unsigned long long)alloc);
		return -1;
	}
	memset(&config, 0, sizeof(config));
	config.mem_addr = vdec_memory;
	config.mem_size = attr.mem_size;
	config.ppu_thread_prio = VDEC_PPU_PRIO;
	config.ppu_thread_stack_size = VDEC_STACK;
	config.spu_thread_prio = VDEC_SPU_PRIO;
	config.num_spus = VDEC_SPUS;
	closure.fn = (u32)__get_opd32(vdec_callback);
	closure.arg = 0;
	r = vdecOpen(&type, &config, &closure, &vdec_handle);
	if (r == 0)
		r = vdecStartSequence(vdec_handle);
	if (r != 0) {
		app_log("vdec open=%08x\n", r);
		free_system_memory(vdec_memory);
		return -1;
	}
	vdec_open_now = 1;
	return 0;
}

static u32 free_slots(void)
{
	u32 count = 0;
	sysMutexLock(pool_lock, 0);
	for (int i = 0; i < pool_size; i++)
		count += pool[i].state == SLOT_FREE;
	sysMutexUnlock(pool_lock);
	return count;
}

static void feed_au(const struct segment *seg, const struct access_unit *a)
{
	/* A seek stops pictures being shown, so this wait must notice one or it never ends. */
	while (playing && !flush_pending && free_slots() < FEED_MIN_FREE)
		usleep(1000);
	if (flush_pending)
		return;

	vdecAU au;
	memset(&au, 0, sizeof(au));
	au.packet_addr = (u32)(u64)(seg->data + a->offset);
	au.packet_size = a->size;
	/* Finer timestamps make the decoder fail, as Movian also found: it passes microseconds
	 * rounded down to whole milliseconds. */
	u64 pts_us = ((u64)a->pts * 100 / 9) / 1000 * 1000;
	u64 dts_us = ((u64)a->dts * 100 / 9) / 1000 * 1000;
	au.pts.low = (u32)pts_us;
	au.pts.hi = (u32)(pts_us >> 32);
	au.dts.low = (u32)dts_us;
	au.dts.hi = (u32)(dts_us >> 32);
	au.userdata = a->pts;

	au_pending = 1;
	s32 r;
	while ((r = vdecDecodeAu(vdec_handle, VDEC_DECODER_MODE_NORMAL, &au)) == (s32)VDEC_ERROR_BUSY && playing &&
	       !flush_pending)
		usleep(1000);
	if (r != 0) {
		au_pending = 0;
		st.au_errors++;
		return;
	}
	while (au_pending && playing)
		usleep(200);
	st.au_fed++;
}

/* After a seek: finish with the old pictures, and restart the decoder's sequence. */
static void flush_decoder(void)
{
	u64 start = now_us();
	seq_done = 0;
	vdecEndSequence(vdec_handle);
	for (int i = 0; i < SEQUENCE_WAIT_STEPS && !seq_done && playing; i++)
		usleep(10000);
	sysMutexLock(pool_lock, 0);
	for (int i = 0; i < pool_size; i++) {
		if (pool[i].state == SLOT_QUEUED)
			pool[i].state = SLOT_FREE;
	}
	sysMutexUnlock(pool_lock);
	s32 r = vdecStartSequence(vdec_handle);
	app_log("seek: decoder flushed in %u ms, restart=%08x seq_done=%d\n", (u32)((now_us() - start) / 1000), r, seq_done);
}

static void feeder_thread(void *arg)
{
	(void)arg;
	int finished = 0;
	int sequence_open = 1;
	u32 generation = stream_generation_now();
	while (playing) {
		if (generation != stream_generation_now()) {
			generation = stream_generation_now();
			if (sequence_open) {
				flush_decoder();
			} else {
				sysMutexLock(pool_lock, 0);
				for (int i = 0; i < pool_size; i++) {
					if (pool[i].state == SLOT_QUEUED)
						pool[i].state = SLOT_FREE;
				}
				sysMutexUnlock(pool_lock);
				vdecStartSequence(vdec_handle);
				sequence_open = 1;
			}
			feeder_done = 0;
			finished = 0;
			/* A newer seek may have arrived during the flush; then this goes round again. */
			if (generation == stream_generation_now())
				flush_pending = 0;
			continue;
		}
		struct segment *seg = stream_next_video(&finished);
		if (!seg) {
			if (finished && sequence_open) {
				seq_done = 0;
				vdecEndSequence(vdec_handle);
				sequence_open = 0;
				feeder_done = 1;
			}
			usleep(5000);
			continue;
		}
		for (u32 i = 0; i < seg->au_count && playing && generation == stream_generation_now(); i++)
			feed_au(seg, &seg->aus[i]);
		stream_free(seg);
	}
	if (sequence_open) {
		seq_done = 0;
		vdecEndSequence(vdec_handle);
	}
	feeder_done = 1;
	sysThreadExit(0);
}

/* ---- audio ---- */

static struct segment *audio_seg;
static size_t audio_pos;

/* Make sure there's a current audio segment. Returns 0 if none is ready. */
static int next_audio_segment(int *finished)
{
	while (!audio_seg || audio_pos >= audio_seg->size) {
		stream_free(audio_seg);
		audio_seg = stream_next_audio(finished);
		audio_pos = 0;
		if (!audio_seg)
			return 0;
		if (!audio_base_set) {
			audio_base_pts = audio_seg->aus[0].pts;
			audio_base_set = 1;
		}
	}
	return 1;
}

/* Takes decoded audio a frame at a time, as stereo floats, whatever it was decoded as. */
struct pcm_reader {
	NeAACDecHandle dec;
	const s16 *pcm;
	u32 frames_left;
	u32 channels;
	float previous[2], current[2];
	float phase, step;
	int finished;
};

/* The next decoded frame. Returns 0 if there's nothing ready. */
static int next_pcm_frame(struct pcm_reader *r, float out[2])
{
	while (r->frames_left == 0) {
		if (!next_audio_segment(&r->finished))
			return 0;
		NeAACDecFrameInfo info;
		void *decoded = NeAACDecDecode(r->dec, &info, audio_seg->data + audio_pos, audio_seg->size - audio_pos);
		if (info.bytesconsumed == 0) {
			audio_pos = audio_seg->size; /* can't advance; skip the rest */
			continue;
		}
		audio_pos += info.bytesconsumed;
		if (info.error != 0 || !decoded || info.channels == 0) {
			st.aac_errors++;
			continue;
		}
		st.aac_frames++;
		r->pcm = decoded;
		r->channels = info.channels;
		r->frames_left = (u32)(info.samples / info.channels);
	}
	out[0] = r->pcm[0] / 32768.0f;
	out[1] = (r->channels > 1 ? r->pcm[1] : r->pcm[0]) / 32768.0f;
	r->pcm += r->channels;
	r->frames_left--;
	return 1;
}

/* One frame for the audio port, stepping through the source at its own rate. */
static int next_output_frame(struct pcm_reader *r, float out[2])
{
	while (r->phase >= 1.0f) {
		float next[2];
		if (!next_pcm_frame(r, next))
			return 0;
		r->previous[0] = r->current[0];
		r->previous[1] = r->current[1];
		r->current[0] = next[0];
		r->current[1] = next[1];
		r->phase -= 1.0f;
	}
	out[0] = r->previous[0] + (r->current[0] - r->previous[0]) * r->phase;
	out[1] = r->previous[1] + (r->current[1] - r->previous[1]) * r->phase;
	r->phase += r->step;
	return 1;
}

static void audio_thread(void *arg)
{
	(void)arg;
	audioPortParam params;
	audioPortConfig config;
	u32 port = 0;
	sys_event_queue_t queue;
	sys_ipc_key_t key;
	sys_event_t event;
	int finished = 0;

	while (playing && !next_audio_segment(&finished)) {
		if (finished) {
			if (!failure)
				failure = stream_failure() ? stream_failure() : "no_audio";
			audio_done = 1;
			sysThreadExit(0);
		}
		usleep(5000);
	}
	if (!playing) {
		audio_done = 1;
		sysThreadExit(0);
	}

	NeAACDecHandle dec = NeAACDecOpen();
	unsigned long rate = 0;
	unsigned char channels = 0;
	if (dec) {
		/* Surround comes out as stereo. */
		NeAACDecConfigurationPtr config_ptr = NeAACDecGetCurrentConfiguration(dec);
		config_ptr->downMatrix = 1;
		NeAACDecSetConfiguration(dec, config_ptr);
	}
	if (!dec || NeAACDecInit(dec, audio_seg->data, audio_seg->size, &rate, &channels) < 0 || rate == 0) {
		app_log("aac init failed rate=%lu channels=%u\n", rate, channels);
		failure = "aac_init";
		audio_done = 1;
		sysThreadExit(0);
	}

	audioInit();
	memset(&params, 0, sizeof(params));
	params.numChannels = AUDIO_PORT_2CH;
	params.numBlocks = AUDIO_BLOCK_8;
	params.attrib = AUDIO_PORT_INITLEVEL;
	params.level = 1.0f;
	if (audioPortOpen(&params, &port) != 0 || audioGetPortConfig(port, &config) != 0 ||
	    audioCreateNotifyEventQueue(&queue, &key) != 0 || audioSetNotifyEventQueue(key) != 0) {
		failure = "audio_open";
		audio_done = 1;
		sysThreadExit(0);
	}
	sysEventQueueDrain(queue);

	while (!audio_start && playing)
		usleep(1000);
	audioPortStart(port);

	app_log("audio rate=%lu channels=%u\n", rate, channels);
	struct pcm_reader reader;
	memset(&reader, 0, sizeof(reader));
	reader.dec = dec;
	reader.step = (float)rate / AUDIO_RATE;
	reader.phase = 1.0f;
	f32 *data_start = (f32 *)(u64)config.audioDataStart;
	u32 generation = stream_generation_now();

	while (playing) {
		if (sysEventQueueReceive(queue, &event, 20000) != 0)
			continue;

		/* After a seek: drop what's left of the old piece and pick up at the new time. */
		if (generation != stream_generation_now()) {
			generation = stream_generation_now();
			stream_free(audio_seg);
			audio_seg = NULL;
			audio_pos = 0;
			reader.frames_left = 0;
			reader.finished = 0;
			reader.phase = 1.0f;
			audio_base_set = 0;
			audio_samples = 0;
			audio_done = 0;
			NeAACDecPostSeekReset(dec, 0);
		}

		u64 current = *(u64 *)(u64)config.readIndex;
		u32 next = (current + 1) % config.numBlocks;
		f32 *block = data_start + config.channelCount * AUDIO_BLOCK_SAMPLES * next;
		u32 filled = 0;

		for (u32 i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			if (paused || !audio_start) {
				block[i * 2] = 0.0f;
				block[i * 2 + 1] = 0.0f;
				continue;
			}
			float frame[2];
			if (next_output_frame(&reader, frame)) {
				block[i * 2] = frame[0];
				block[i * 2 + 1] = frame[1];
				filled++;
			} else {
				block[i * 2] = 0.0f;
				block[i * 2 + 1] = 0.0f;
			}
		}
		/* The clock only counts real audio, so it waits while paused or if the download falls behind. */
		audio_samples += filled;
		if (!paused && audio_start && filled < AUDIO_BLOCK_SAMPLES) {
			if (reader.finished)
				audio_done = 1;
			else
				st.audio_underruns++;
		}
	}

	audioPortStop(port);
	audioRemoveNotifyEventQueue(key);
	audioPortClose(port);
	sysEventQueueDestroy(queue, 0);
	audioQuit();
	NeAACDecClose(dec);
	audio_done = 1;
	sysThreadExit(0);
}

/* ---- control ---- */

/* The pool is kept between episodes, and rebuilt only when the picture size limit changes. */
static int make_pool(int tall)
{
	u32 width = tall ? MAX_TALL_WIDTH : MAX_PICTURE_WIDTH;
	u32 height = tall ? MAX_TALL_HEIGHT : MAX_PICTURE_HEIGHT;
	int count = POOL_SIZE;
	if (pool_size == count && picture_max_width == width)
		return 0;
	for (int i = 0; i < pool_size; i++)
		rsxFree(pool[i].pixels);
	pool_size = 0;
	picture_max_width = width;
	picture_max_height = height;
	for (int i = 0; i < count; i++) {
		/* A full plane of Y, and quarter planes of U and V. */
		pool[i].pixels = rsxMemalign(PICTURE_ALIGN, width * height * 3 / 2);
		if (!pool[i].pixels) {
			app_log("video pool: out of RSX memory at slot %d\n", i);
			return -1;
		}
		pool[i].offset = gfx_offset(pool[i].pixels);
		pool_size = i + 1;
	}
	return 0;
}

static void begin_playback(float seconds, int reuse)
{
	memset(&st, 0, sizeof(st));
	failure = NULL;
	playing = 1;
	paused = 0;
	au_pending = seq_done = feeder_done = audio_start = audio_done = has_started = 0;
	audio_samples = 0;
	audio_base_pts = 0;
	audio_base_set = 0;
	audio_seg = NULL;
	audio_pos = 0;
	last_shown_pts = 0;
	shown_since_seek = 0;
	seeking = 0;
	flush_pending = 0;
	for (int i = 0; i < KEEP_SHOWN; i++)
		shown_slots[i] = -1;
	wait_start_us = now_us();

	if (!pool_lock_ready) {
		sys_mutex_attr_t attr;
		sysMutexAttrInitialize(attr);
		sysMutexCreate(&pool_lock, &attr);
		pool_lock_ready = 1;
	}
	if (make_pool(current_options.allow_too_tall) != 0) {
		failure = "out_of_memory";
		return;
	}
	for (int i = 0; i < pool_size; i++)
		pool[i].state = SLOT_FREE;

	if (open_decoder() != 0) {
		failure = "decoder";
		return;
	}
	if (stream_start(current_ids, &current_options, seconds, reuse) != 0) {
		failure = "stream_start";
		return;
	}
	if (sysThreadCreate(&feeder, feeder_thread, NULL, THREAD_PRIO_FEEDER, FEEDER_STACK, THREAD_JOINABLE,
	                    "feeder") != 0 ||
	    sysThreadCreate(&audio, audio_thread, NULL, THREAD_PRIO_AUDIO, AUDIO_STACK, THREAD_JOINABLE,
	                    "audio") != 0) {
		failure = "threads";
		return;
	}
	threads_running = 1;
}

void video_start(const char *server_ids, const struct resolve_options *options, float seconds)
{
	video_stop();
	snprintf(current_ids, sizeof(current_ids), "%s", server_ids);
	current_options = *options;
	begin_playback(seconds, 0);
}

void video_seek(float seconds)
{
	float length = stream_duration();
	if (seconds < 0)
		seconds = 0;
	if (length > 0 && seconds > length - SEEK_END_MARGIN)
		seconds = length - SEEK_END_MARGIN;
	if (seconds < 0)
		seconds = 0;
	/* Everything stays open: the last picture stays up until the new ones are ready. */
	flush_pending = 1;
	audio_start = 0;
	has_started = 0;
	seeking = 1;
	paused = 0;
	feeder_done = 0;
	audio_done = 0;
	last_shown_pts = 0;
	shown_since_seek = 0;
	wait_start_us = now_us();
	stream_seek(seconds);
}

int video_seeking(void)
{
	return seeking;
}

float video_position(void)
{
	if (!audio_base_set)
		return 0;
	return (float)audio_base_pts / 90000.0f + (float)audio_samples / AUDIO_RATE;
}

float video_duration(void)
{
	return stream_duration();
}

const char *video_subtitle(void)
{
	return has_started ? stream_subtitle_at(video_position()) : NULL;
}

int video_intro(float *start, float *end)
{
	return stream_intro(start, end);
}

static u32 queued_count(void)
{
	u32 count = 0;
	sysMutexLock(pool_lock, 0);
	for (int i = 0; i < pool_size; i++)
		count += pool[i].state == SLOT_QUEUED;
	sysMutexUnlock(pool_lock);
	return count;
}

/* The newest picture that's due moves to the shown list, and the oldest shown is released. */
static void pick_due_picture(void)
{
	u32 clock_90k = audio_base_pts + (u32)(audio_samples * 90000 / AUDIO_RATE);
	int best = -1;

	sysMutexLock(pool_lock, 0);
	/* A picture older than the one on screen arrived too late; showing it would jump back. */
	for (int i = 0; i < pool_size && shown_since_seek > 0; i++) {
		if (pool[i].state == SLOT_QUEUED && pool[i].pts <= last_shown_pts) {
			pool[i].state = SLOT_FREE;
			st.too_old_dropped++;
		}
	}
	for (int i = 0; i < pool_size; i++) {
		if (pool[i].state == SLOT_QUEUED && pool[i].pts <= clock_90k &&
		    (best < 0 || pool[i].pts > pool[best].pts))
			best = i;
	}
	if (best >= 0) {
		for (int i = 0; i < pool_size; i++) {
			if (i != best && pool[i].state == SLOT_QUEUED && pool[i].pts < pool[best].pts) {
				pool[i].state = SLOT_FREE;
				st.late_dropped++;
			}
		}
		int oldest = shown_slots[KEEP_SHOWN - 1];
		if (oldest >= 0)
			pool[oldest].state = SLOT_FREE;
		for (int i = KEEP_SHOWN - 1; i > 0; i--)
			shown_slots[i] = shown_slots[i - 1];
		shown_slots[0] = best;
		pool[best].state = SLOT_SHOWN;
		last_shown_pts = pool[best].pts;
		shown_since_seek++;
		st.shown++;
	}
	sysMutexUnlock(pool_lock);
}

/* How many pictures to wait for: never more than can be queued, or playback would never start. The
 * feeder stops with FEED_MIN_FREE free, and a seek leaves KEEP_SHOWN held by what was on screen. */
static u32 start_frames(void)
{
	int room = pool_size - FEED_MIN_FREE - KEEP_SHOWN;
	if (room < 1)
		room = 1;
	return room < START_FRAMES ? (u32)room : START_FRAMES;
}

int video_frame(void)
{
	if (!playing)
		return 1;
	if (!failure && stream_failure() && !has_started)
		failure = stream_failure();
	if (failure)
		return 1;

	if (!has_started) {
		if (flush_pending) {
			/* Not yet: anything queued is from before the seek. */
		} else if (queued_count() >= start_frames() || (feeder_done && queued_count() > 0)) {
			if (seeking)
				app_log("seek: ready after %u ms\n", (u32)((now_us() - wait_start_us) / 1000));
			has_started = 1;
			seeking = 0;
			audio_start = 1;
		} else if (now_us() - wait_start_us > START_TIMEOUT_US) {
			failure = "timed_out";
			return 1;
		}
	}

	if (has_started && !paused)
		pick_due_picture();

	int current = shown_slots[0];
	if (current >= 0) {
		const struct slot *s = &pool[current];
		/* Fit to the screen, keeping the picture's shape. */
		float w = SCREEN_W, h = SCREEN_W * s->height / s->width;
		if (h > SCREEN_H) {
			h = SCREEN_H;
			w = SCREEN_H * s->width / s->height;
		}
		gfx_video(s->offset, s->width, s->height, (SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h);
	}
	return has_started && feeder_done && audio_done && queued_count() == 0;
}

void video_toggle_pause(void)
{
	if (has_started)
		paused = !paused;
}

int video_paused(void)
{
	return paused;
}

int video_started(void)
{
	return has_started;
}

const char *video_failure(void)
{
	return failure;
}

void video_stop(void)
{
	u64 code;
	if (!playing && !vdec_open_now)
		return;
	playing = 0;
	stream_request_stop();
	if (threads_running) {
		sysThreadJoin(feeder, &code);
		sysThreadJoin(audio, &code);
		threads_running = 0;
	}
	stream_stop();
	stream_free(audio_seg);
	audio_seg = NULL;
	if (vdec_open_now) {
		for (int i = 0; i < SEQUENCE_WAIT_STEPS && !seq_done; i++)
			usleep(10000);
		vdecClose(vdec_handle);
		free_system_memory(vdec_memory);
		vdec_open_now = 0;
	}
	/* The RSX may still be drawing the last picture for a frame or two. */
	app_log("video: fed=%u out=%u bad=%u no_slot=%u status_errors=%u/%08x shown=%u late=%u old=%u aac=%u/%u underruns=%u size=%ux%u seconds=%.1f failure=%s\n",
	        st.au_fed, st.pics_out, st.pics_bad, st.pics_no_slot, st.pics_status_errors, st.last_status,
	        st.shown, st.late_dropped, st.too_old_dropped, st.aac_frames, st.aac_errors, st.audio_underruns,
	        st.width, st.height, (double)audio_samples / AUDIO_RATE, failure ? failure : "none");
	stream_log_stats();
}
