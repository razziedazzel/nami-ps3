#ifndef RESOLVE_H
#define RESOLVE_H

#include <stddef.h>

#define RESOLVE_URL_MAX 1024
#define RESOLVE_LOG_MAX 1024

/* Fetches url into a malloc'd, NUL-terminated buffer. Returns 0 on success. */
typedef int (*resolve_fetch_fn)(void *ctx, const char *url, const char *referer, int xhr,
                                char **body, size_t *len);

struct resolve_options {
	int target_height;  /* the tallest version at or under this; else the shortest up to 720p */
	int want_dub;       /* use the dub servers */
	int allow_too_tall; /* with nothing playable, take the shortest anyway */
};

struct resolved_stream {
	char media_url[RESOLVE_URL_MAX]; /* the chosen rendition's playlist */
	char subtitle_url[RESOLVE_URL_MAX]; /* English, or empty */
	int too_tall;                       /* needs the experimental decoder patch */
	float intro_start, intro_end;       /* seconds; equal if unknown */
	char referer[192];
	char server[32];
	int width, height;
	char log[RESOLVE_LOG_MAX];
};

/* Follow an episode's server IDs to a stream the decoder can play, trying each server. Returns NULL on success, or a short failure reason. */
const char *resolve_stream(resolve_fetch_fn fetch, void *ctx, const char *ids, const struct resolve_options *options,
                           struct resolved_stream *out);

/* Join a playlist entry to the URL of the playlist it came from. */
void resolve_join_url(const char *base, const char *uri, char *out, size_t size);

#endif
