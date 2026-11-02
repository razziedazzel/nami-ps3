#ifndef STORE_H
#define STORE_H

/* What the app remembers on the hard drive. Everything here has safe defaults: a missing or damaged
 * file just means starting from those. */

#define STORE_TEXT_MAX 128
#define STORE_URL_MAX 192
#define STORE_ID_MAX 16
#define STORE_EP_MAX 8
#define HISTORY_MAX 40
#define MYLIST_MAX 60
#define WATCHED_EPISODES_MAX 2000
#define WATCHED_BYTES (WATCHED_EPISODES_MAX / 8)

#define QUALITY_DEFAULT 480
#define QUALITY_HIGH 720

struct settings {
	int quality;           /* 480 or 720 */
	int subtitles;         /* shown when available */
	int experimental_1080; /* try 1080p-only shows, with corruption */
	int installed;         /* the first-run poster install is done */
	int play_first_unwatched; /* Play offers the earliest unfinished episode, not the latest watched */
};

struct saved_show {
	char id[STORE_ID_MAX];
	char title[STORE_TEXT_MAX];
	char poster_url[STORE_URL_MAX];
	char watch_url[STORE_URL_MAX];
};

struct history_entry {
	struct saved_show show;
	int episode_index;
	char episode[STORE_EP_MAX];
	float position, duration; /* seconds */
	unsigned sequence;        /* larger is more recent */
	unsigned char watched[WATCHED_BYTES]; /* one bit per episode index */
};

extern struct settings settings;
extern struct history_entry history[HISTORY_MAX]; /* most recent first */
extern int history_count;
extern struct saved_show mylist[MYLIST_MAX]; /* most recent first */
extern int mylist_count;

/* dir ends in a slash and must exist. Loads everything. */
void store_init(const char *dir);
int settings_save(void);

struct history_entry *history_find(const char *id);
/* Record progress, moving the show to the front. */
void history_update(const struct saved_show *show, int episode_index, const char *episode, float position,
                    float duration);
void history_clear(void);
int history_save(void);
/* Whether the entry's current episode has been played far enough to count as watched. */
int history_entry_finished(const struct history_entry *e);
/* Which episodes of a show have been finished, by index. Out-of-range indexes are never watched. */
int history_is_watched(const struct history_entry *e, int episode_index);
void history_set_watched(struct history_entry *e, int episode_index);
void history_clear_watched(struct history_entry *e, int episode_index);
/* The show's entry, adding one with nothing played if it has none yet. */
struct history_entry *history_ensure(const struct saved_show *show);
/* Forget a show, including which of its episodes were watched. */
void history_remove(const char *id);

int mylist_contains(const char *id);
/* Add it if absent, remove it if present. Returns whether it's now on the list. */
int mylist_toggle(const struct saved_show *show);

#endif
