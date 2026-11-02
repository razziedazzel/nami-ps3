#ifndef SUBS_H
#define SUBS_H

#include <stddef.h>

struct subtitle_cue {
	float start, end; /* seconds */
	char *text;       /* Latin-1, with \n between lines */
};

struct subtitles {
	struct subtitle_cue *cues;
	int count;
};

/* Parse WebVTT into s, replacing what it held. Returns the number of cues. */
int subs_parse(struct subtitles *s, const char *vtt);
void subs_free(struct subtitles *s);
/* The cue to show at a time, or NULL. */
const char *subs_at(const struct subtitles *s, float seconds);

#endif
