#ifndef STREAM_H
#define STREAM_H

#include <ppu-types.h>

#include "resolve.h"

struct access_unit {
	u32 offset, size, pts, dts;
};

/* One downloaded HLS segment with its transport stream unpacked. */
struct segment {
	u8 *data;
	size_t size;
	struct access_unit *aus;
	u32 au_count;
};

/* Find an episode's stream and start downloading from a time. With reuse, skip finding it again and
 * keep the same stream, as for a seek. Returns 0 on success. */
int stream_start(const char *server_ids, const struct resolve_options *options, float seconds, int reuse);
/* Seconds in the whole stream, or 0 until known. */
float stream_duration(void);
/* The subtitle to show at a time, or NULL. */
const char *stream_subtitle_at(float seconds);
/* The intro's times, if known. Returns 1 if so. */
int stream_intro(float *start, float *end);
/* Next segment, or NULL if none is ready; *finished is set once none will ever come. */
struct segment *stream_next_video(int *finished);
struct segment *stream_next_audio(int *finished);
void stream_free(struct segment *s);
/* Jump to a time, keeping the connection. What's queued is dropped. */
void stream_seek(float seconds);
/* Changes with every seek, so the players can tell what they hold is out of date. */
u32 stream_generation_now(void);
/* Ask the download to stop, without waiting. */
void stream_request_stop(void);
/* Stop, wait for the download, and free what's queued. */
void stream_stop(void);
const char *stream_failure(void);
void stream_log_stats(void);

#endif
