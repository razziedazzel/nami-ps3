#include "subs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CUES 256

/* mm:ss.mmm or hh:mm:ss.mmm. Returns the position after it, or NULL. */
static const char *parse_time(const char *p, float *out)
{
	int a, b, c, ms, used = 0;
	if (sscanf(p, "%d:%d:%d.%d%n", &a, &b, &c, &ms, &used) == 4 && used > 0) {
		*out = (float)a * 3600 + (float)b * 60 + (float)c + (float)ms / 1000.0f;
		return p + used;
	}
	if (sscanf(p, "%d:%d.%d%n", &a, &b, &ms, &used) == 3 && used > 0) {
		*out = (float)a * 60 + (float)b + (float)ms / 1000.0f;
		return p + used;
	}
	return NULL;
}

/* Copy a cue's text: no tags, entities decoded, UTF-8 to Latin-1, curly quotes and dashes made plain. */
static char *clean_text(const char *src, size_t len)
{
	char *out = malloc(len + 1);
	size_t n = 0;
	if (!out)
		return NULL;
	for (size_t i = 0; i < len;) {
		unsigned char c = (unsigned char)src[i];
		if (c == '<') {
			const char *close = memchr(src + i, '>', len - i);
			if (close) {
				i = (size_t)(close - src) + 1;
				continue;
			}
		}
		if (c == '&') {
			static const struct { const char *name; char value; } entities[] = {
				{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''},
				{"&apos;", '\''}, {"&nbsp;", ' '}, {"&lrm;", 0}, {"&rlm;", 0},
			};
			int matched = 0;
			for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++) {
				size_t l = strlen(entities[e].name);
				if (i + l <= len && strncmp(src + i, entities[e].name, l) == 0) {
					if (entities[e].value)
						out[n++] = entities[e].value;
					i += l;
					matched = 1;
					break;
				}
			}
			if (matched)
				continue;
		}
		if (c < 0x80) {
			if (c != '\r')
				out[n++] = (char)c;
			i++;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
			unsigned cp = ((c & 0x1Fu) << 6) | ((unsigned char)src[i + 1] & 0x3Fu);
			out[n++] = cp < 0x100 ? (char)cp : '?';
			i += 2;
		} else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
			unsigned cp = ((c & 0x0Fu) << 12) | (((unsigned char)src[i + 1] & 0x3Fu) << 6) |
			              ((unsigned char)src[i + 2] & 0x3Fu);
			i += 3;
			if (cp == 0x2018 || cp == 0x2019)
				out[n++] = '\'';
			else if (cp == 0x201C || cp == 0x201D)
				out[n++] = '"';
			else if (cp == 0x2013 || cp == 0x2014)
				out[n++] = '-';
			else if (cp == 0x2026) {
				out[n++] = '.';
				/* Three dots in one code point's three bytes of room. */
				out[n++] = '.';
				out[n++] = '.';
			} else if (cp == 0x266A || cp == 0x266B)
				out[n++] = '#';
			else if (cp == 0x200B || cp == 0x200E || cp == 0x200F || cp == 0xFEFF)
				continue;
			else
				out[n++] = '?';
		} else {
			out[n++] = '?';
			i += (c & 0xF8) == 0xF0 ? 4 : 1;
		}
	}
	out[n] = 0;
	return out;
}

int subs_parse(struct subtitles *s, const char *vtt)
{
	int capacity = INITIAL_CUES;
	subs_free(s);
	s->cues = malloc(sizeof(*s->cues) * (size_t)capacity);
	if (!s->cues)
		return 0;

	const char *p = vtt;
	while ((p = strstr(p, "-->")) != NULL) {
		/* The start time is at the beginning of this line. */
		const char *line = p;
		while (line > vtt && line[-1] != '\n')
			line--;
		float start, end;
		const char *after = p + 3;
		while (*after == ' ')
			after++;
		if (!parse_time(line, &start) || !parse_time(after, &end)) {
			p += 3;
			continue;
		}
		const char *text = strchr(p, '\n');
		if (!text)
			break;
		text++;
		/* The text runs to the first blank line. */
		const char *stop = text;
		while (*stop) {
			const char *nl = strchr(stop, '\n');
			if (!nl) {
				stop += strlen(stop);
				break;
			}
			const char *next = nl + 1;
			if (*next == '\n' || (*next == '\r' && next[1] == '\n')) {
				stop = nl;
				break;
			}
			stop = next;
		}
		if (s->count == capacity) {
			struct subtitle_cue *grown = realloc(s->cues, sizeof(*s->cues) * (size_t)capacity * 2);
			if (!grown)
				break;
			s->cues = grown;
			capacity *= 2;
		}
		char *clean = clean_text(text, (size_t)(stop - text));
		if (clean) {
			s->cues[s->count].start = start;
			s->cues[s->count].end = end;
			s->cues[s->count].text = clean;
			s->count++;
		}
		p = stop;
	}
	return s->count;
}

void subs_free(struct subtitles *s)
{
	for (int i = 0; i < s->count; i++)
		free(s->cues[i].text);
	free(s->cues);
	s->cues = NULL;
	s->count = 0;
}

const char *subs_at(const struct subtitles *s, float seconds)
{
	/* Cues are in time order; find the last that has started. */
	int low = 0, high = s->count - 1, found = -1;
	while (low <= high) {
		int mid = (low + high) / 2;
		if (s->cues[mid].start <= seconds) {
			found = mid;
			low = mid + 1;
		} else {
			high = mid - 1;
		}
	}
	/* Cues can overlap, so look back a little for one still showing. */
	for (int i = found; i >= 0 && i > found - 3; i--) {
		if (seconds < s->cues[i].end)
			return s->cues[i].text;
	}
	return NULL;
}
