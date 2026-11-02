#include "resolve.h"

#include <mbedtls/aes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SITE "https://anikototv.to"
#define SITE_REFERER SITE "/"
/* From megaplay.buzz/lib/app.main.js. If links stop decrypting, check there first. */
#define ENC_KEY "i?LMTAx0Q6,:}50U"
#define ENC_IV "W0;27ToaUpl_P%'c"
#define AES_KEY_BYTES 32
#define AES_BLOCK 16

#define MAX_HEIGHT 720
#define MAX_SERVERS 8
#define NOTHING_PLAYABLE 1 /* the server answered, with only versions too tall to play */
#define ID_MAX 2048

static void logf_append(struct resolved_stream *out, const char *fmt, ...)
{
	size_t used = strlen(out->log);
	va_list args;
	va_start(args, fmt);
	vsnprintf(out->log + used, sizeof(out->log) - used, fmt, args);
	va_end(args);
}

/* Undo JSON string escapes in place. */
static void json_unescape(char *s)
{
	char *w = s;
	for (char *r = s; *r; r++) {
		if (*r != '\\' || !r[1]) {
			*w++ = *r;
			continue;
		}
		r++;
		switch (*r) {
		case 'n': *w++ = '\n'; break;
		case 't': *w++ = '\t'; break;
		case 'r': *w++ = '\r'; break;
		default: *w++ = *r; break; /* \" \/ \\ */
		}
	}
	*w = 0;
}

/* Copy the text between after and the next until. Returns the position following it, or NULL. */
static const char *copy_between(const char *text, const char *after, char until, char *out,
                                size_t size)
{
	const char *start = strstr(text, after);
	if (!start)
		return NULL;
	start += strlen(after);
	const char *end = strchr(start, until);
	if (!end || (size_t)(end - start) >= size)
		return NULL;
	memcpy(out, start, end - start);
	out[end - start] = 0;
	return end + 1;
}

/* Read a JSON string field, allowing any spacing around the colon. Stops at the first unescaped
 * quote, and leaves escapes in place. Returns 0 if found. */
static int json_string(const char *json, const char *key, char *out, size_t size)
{
	char pattern[32];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(json, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);
	while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
		p++;
	if (*p++ != ':')
		return -1;
	while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
		p++;
	if (*p++ != '"')
		return -1;
	size_t n = 0;
	for (; *p && *p != '"'; p++) {
		if (*p == '\\' && p[1]) {
			if (n + 1 < size)
				out[n++] = *p;
			p++;
		}
		if (n + 1 >= size)
			return -1;
		out[n++] = *p;
	}
	if (*p != '"')
		return -1;
	out[n] = 0;
	json_unescape(out);
	return 0;
}

static int base64url_decode(const char *in, unsigned char *out, size_t size, size_t *out_len)
{
	uint32_t acc = 0;
	int bits = 0;
	size_t n = 0;
	for (; *in && *in != '='; in++) {
		int v;
		char c = *in;
		if (c >= 'A' && c <= 'Z') v = c - 'A';
		else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
		else if (c >= '0' && c <= '9') v = c - '0' + 52;
		else if (c == '-' || c == '+') v = 62;
		else if (c == '_' || c == '/') v = 63;
		else return -1;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n >= size)
				return -1;
			out[n++] = (unsigned char)((acc >> bits) & 0xFF);
		}
	}
	*out_len = n;
	return 0;
}

/* Decrypt the enc field to its JSON text, NUL-terminated in out. */
static int decrypt_enc(const char *enc, char *out, size_t size)
{
	unsigned char key[AES_KEY_BYTES] = {0};
	unsigned char iv[AES_BLOCK];
	size_t len = strlen(enc) * 3 / 4 + 4;
	unsigned char *cipher = malloc(len);
	size_t cipher_len = 0;
	int result = -1;

	if (!cipher)
		return -1;
	memcpy(key, ENC_KEY, strlen(ENC_KEY));
	memcpy(iv, ENC_IV, AES_BLOCK);

	if (base64url_decode(enc, cipher, len, &cipher_len) == 0 && cipher_len > 0 &&
	    cipher_len % AES_BLOCK == 0 && cipher_len < size) {
		mbedtls_aes_context aes;
		mbedtls_aes_init(&aes);
		if (mbedtls_aes_setkey_dec(&aes, key, AES_KEY_BYTES * 8) == 0 &&
		    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipher_len, iv, cipher,
		                          (unsigned char *)out) == 0) {
			unsigned pad = (unsigned char)out[cipher_len - 1];
			if (pad >= 1 && pad <= AES_BLOCK) {
				out[cipher_len - pad] = 0;
				result = 0;
			}
		}
		mbedtls_aes_free(&aes);
	}
	free(cipher);
	return result;
}

void resolve_join_url(const char *base, const char *uri, char *out, size_t size)
{
	if (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0) {
		snprintf(out, size, "%s", uri);
		return;
	}
	const char *host = strstr(base, "://");
	host = host ? host + 3 : base;
	size_t keep;
	if (uri[0] == '/') {
		const char *path = strchr(host, '/');
		keep = path ? (size_t)(path - base) : strlen(base);
	} else {
		const char *query = strchr(host, '?');
		size_t end = query ? (size_t)(query - base) : strlen(base);
		keep = end;
		while (keep > (size_t)(host - base) && base[keep - 1] != '/')
			keep--;
		if (keep == (size_t)(host - base))
			keep = end; /* no path at all */
	}
	snprintf(out, size, "%.*s%s%s", (int)keep, base,
	         uri[0] != '/' && (keep == 0 || base[keep - 1] != '/') ? "/" : "", uri);
}

/* The rendition to play from a master playlist: the tallest at or under the standard height, or
 * else the shortest at or under the maximum. Returns 0 if one was chosen. */
static int choose_rendition(const char *master, const char *master_url, const struct resolve_options *options,
                            struct resolved_stream *out)
{
	const int target = options->target_height;
	int limit = MAX_HEIGHT;
	int best_h = 0, best_w = 0;
	char any_uri[RESOLVE_URL_MAX] = "";
	int any_h = 0, any_w = 0;
	char best_uri[RESOLVE_URL_MAX] = "";
	const char *p = master;

	while ((p = strstr(p, "#EXT-X-STREAM-INF")) != NULL) {
		const char *line_end = strchr(p, '\n');
		const char *res = strstr(p, "RESOLUTION=");
		int w = 0, h = 0;
		if (!line_end)
			break;
		if (res && res < line_end && sscanf(res, "RESOLUTION=%dx%d", &w, &h) == 2 && h > 0) {
			const char *uri = line_end + 1;
			const char *uri_end = uri + strcspn(uri, "\r\n");
			if ((!any_h || h < any_h) && (size_t)(uri_end - uri) < sizeof(any_uri)) {
				memcpy(any_uri, uri, uri_end - uri);
				any_uri[uri_end - uri] = 0;
				any_h = h;
				any_w = w;
			}
			int better = h <= limit && (!best_h || (h <= target ? best_h > target || h > best_h
			                                                    : best_h > target && h < best_h));
			if (better && (size_t)(uri_end - uri) < sizeof(best_uri)) {
				memcpy(best_uri, uri, uri_end - uri);
				best_uri[uri_end - uri] = 0;
				best_h = h;
				best_w = w;
			}
		}
		p = line_end;
	}
	if (!best_h && options->allow_too_tall && any_h) {
		strcpy(best_uri, any_uri);
		best_h = any_h;
		best_w = any_w;
		out->too_tall = 1;
	}
	if (!best_h)
		return -1;
	resolve_join_url(master_url, best_uri, out->media_url, sizeof(out->media_url));
	out->width = best_w;
	out->height = best_h;
	return 0;
}

/* From one server's link ID to a playable rendition. Returns 0 on success. */
/* What getSources says besides the link: English subtitles, and the intro's times. */
static void read_extras(const char *json, struct resolved_stream *out)
{
	const char *p = strstr(json, "\"tracks\"");
	while (p && (p = strstr(p, "\"file\"")) != NULL) {
		char file[RESOLVE_URL_MAX], label[32] = "";
		const char *object_end = strchr(p, '}');
		if (!object_end)
			break;
		size_t span = (size_t)(object_end - p);
		char *object = malloc(span + 1);
		if (!object)
			break;
		memcpy(object, p, span);
		object[span] = 0;
		int ok = json_string(object, "file", file, sizeof(file)) == 0 &&
		         json_string(object, "label", label, sizeof(label)) == 0;
		free(object);
		/* "English" is the plain track; "English 2" and so on are alternates. */
		if (ok && strcmp(label, "English") == 0) {
			snprintf(out->subtitle_url, sizeof(out->subtitle_url), "%s", file);
			break;
		}
		if (ok && !out->subtitle_url[0] && strncmp(label, "English", 7) == 0)
			snprintf(out->subtitle_url, sizeof(out->subtitle_url), "%s", file);
		p = object_end;
	}
	const char *intro = strstr(json, "\"intro\"");
	if (intro) {
		const char *start = strstr(intro, "\"start\"");
		const char *end = start ? strstr(start, "\"end\"") : NULL;
		if (start && end) {
			start = strchr(start, ':');
			end = strchr(end, ':');
			if (start && end) {
				out->intro_start = (float)atof(start + 1);
				out->intro_end = (float)atof(end + 1);
			}
		}
	}
}

static int try_server(resolve_fetch_fn fetch, void *ctx, const char *link_id,
                      const struct resolve_options *options, struct resolved_stream *out)
{
	char url[RESOLVE_URL_MAX + ID_MAX], embed[RESOLVE_URL_MAX], data_id[32], origin[128];
	char master_url[RESOLVE_URL_MAX], s_param[16] = "";
	char *body = NULL;
	size_t len;
	int result = -1;

	snprintf(url, sizeof(url), SITE "/ajax/server?get=%s", link_id);
	if (fetch(ctx, url, SITE_REFERER, 1, &body, &len) != 0) {
		logf_append(out, "fail=server_fetch ");
		return -1;
	}
	int found = json_string(body, "url", embed, sizeof(embed));
	free(body);
	body = NULL;
	if (found != 0) {
		logf_append(out, "fail=no_embed_url ");
		return -1;
	}

	const char *host = strstr(embed, "://");
	const char *path = host ? strchr(host + 3, '/') : NULL;
	if (!path || (size_t)(path - embed) >= sizeof(origin)) {
		logf_append(out, "fail=embed_url_format ");
		return -1;
	}
	memcpy(origin, embed, path - embed);
	origin[path - embed] = 0;
	const char *s = strstr(embed, "s=");
	if (s && (s[-1] == '?' || s[-1] == '&'))
		snprintf(s_param, sizeof(s_param), "%.*s", (int)strcspn(s + 2, "&"), s + 2);

	if (fetch(ctx, embed, SITE_REFERER, 0, &body, &len) != 0) {
		logf_append(out, "fail=embed_fetch ");
		return -1;
	}
	const char *ok = copy_between(body, "data-id=\"", '"', data_id, sizeof(data_id));
	free(body);
	body = NULL;
	if (!ok) {
		logf_append(out, "fail=no_data_id ");
		return -1;
	}

	snprintf(url, sizeof(url), "%s/stream/getSources?id=%s%s%s", origin, data_id,
	         s_param[0] ? "&s=" : "", s_param);
	if (fetch(ctx, url, embed, 1, &body, &len) != 0) {
		logf_append(out, "fail=sources_fetch ");
		return -1;
	}
	size_t enc_size = len + 1;
	char *enc = malloc(enc_size);
	char *plain = malloc(enc_size);
	read_extras(body, out);
	if (enc && plain && json_string(body, "enc", enc, enc_size) == 0) {
		if (decrypt_enc(enc, plain, enc_size) == 0) {
			if (json_string(plain, "file", master_url, sizeof(master_url)) == 0)
				result = 0;
		} else {
			logf_append(out, "fail=decrypt ");
		}
	}
	free(enc);
	free(plain);
	free(body);
	body = NULL;
	if (result != 0) {
		logf_append(out, "fail=no_stream_url ");
		return -1;
	}

	snprintf(out->referer, sizeof(out->referer), "%s/", origin);
	if (fetch(ctx, master_url, out->referer, 0, &body, &len) != 0) {
		logf_append(out, "fail=master_fetch ");
		return -1;
	}
	result = choose_rendition(body, master_url, options, out);
	if (result != 0)
		result = NOTHING_PLAYABLE;
	if (result != 0)
		logf_append(out, "none_at_or_under_%d ", MAX_HEIGHT);
	free(body);
	return result;
}

const char *resolve_stream(resolve_fetch_fn fetch, void *ctx, const char *ids, const struct resolve_options *options,
                           struct resolved_stream *out)
{
	char url[RESOLVE_URL_MAX + ID_MAX];
	char *body = NULL;
	size_t len;

	memset(out, 0, sizeof(*out));

	snprintf(url, sizeof(url), SITE "/ajax/server/list?servers=%s", ids);
	if (fetch(ctx, url, SITE_REFERER, 1, &body, &len) != 0)
		return "server_list";
	json_unescape(body);

	/* One kind of server: the section runs from data-type="..." to the next data-type. */
	const char *kind = options->want_dub ? "data-type=\"dub\"" : "data-type=\"sub\"";
	char *section = strstr(body, kind);
	if (!section) {
		free(body);
		return options->want_dub ? "no_dub_servers" : "no_sub_servers";
	}
	char *section_end = strstr(section + 1, "data-type=\"");
	if (section_end)
		*section_end = 0;

	const char *p = section;
	/* Whether a server answered and had nothing playable, as opposed to none answering at all. */
	const char *failure = "servers_unavailable";
	for (int i = 0; i < MAX_SERVERS; i++) {
		char link_id[ID_MAX];
		p = copy_between(p, "data-link-id=\"", '"', link_id, sizeof(link_id));
		if (!p)
			break;
		const char *close = strchr(p, '>');
		if (close) {
			const char *name = close + 1;
			while (*name == ' ' || *name == '\n' || *name == '\t' || *name == '\r')
				name++;
			snprintf(out->server, sizeof(out->server), "%.*s", (int)strcspn(name, "<\r\n"), name);
		}
		logf_append(out, "try=%s ", out->server);
		int outcome = try_server(fetch, ctx, link_id, options, out);
		if (outcome == 0) {
			logf_append(out, "chose=%dx%d ", out->width, out->height);
			free(body);
			return NULL;
		}
		if (outcome == NOTHING_PLAYABLE)
			failure = "not_available_at_720p_or_lower";
	}
	free(body);
	return failure;
}
