#ifndef VIDEO_H
#define VIDEO_H

#include "resolve.h"

#define RESOLVE_IDS_MAX 400

/* Start playing an episode from a time: server_ids is its data-ids, from the episode list. */
void video_start(const char *server_ids, const struct resolve_options *options, float seconds);
/* Jump within the same stream, keeping the decoder, audio and connection open. It lands on the start
 * of the piece holding that time. */
void video_seek(float seconds);
/* Whether a seek is still getting its first pictures. */
int video_seeking(void);
/* Where it's up to, and how long it is, in seconds. */
float video_position(void);
float video_duration(void);
/* The subtitle to show now, or NULL. */
const char *video_subtitle(void);
/* The intro's times, if known. Returns 1 if so. */
int video_intro(float *start, float *end);
/* Call once per frame while playing, between gfx_begin_frame and gfx_end_frame. Draws the picture.
 * Returns 0 while there's something to show, 1 once it has finished or failed. */
int video_frame(void);
void video_toggle_pause(void);
int video_paused(void);
/* Whether the first picture has arrived. */
int video_started(void);
/* Why it failed, or NULL. */
const char *video_failure(void);
/* Stop and release everything. Safe to call when already stopped. */
void video_stop(void);

#endif
