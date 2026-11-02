#ifndef CATALOG_H
#define CATALOG_H

#define ITEM_ID_MAX 16
#define ITEM_TITLE_MAX 128
#define ITEM_URL_MAX 192
#define ROW_ITEMS_MAX 30

enum poster_state { POSTER_NONE, POSTER_LOADING, POSTER_READY, POSTER_FAILED };

struct anime_item {
	char id[ITEM_ID_MAX];
	char title[ITEM_TITLE_MAX]; /* Latin-1, as the font holds */
	char poster_url[ITEM_URL_MAX];
	char watch_url[ITEM_URL_MAX];
	int sub_count, dub_count;
	volatile int poster_state;
	unsigned poster_offset; /* RSX texture offset once ready */
};

#define DETAIL_TEXT_MAX 64
#define SYNOPSIS_MAX 1024
#define GENRES_MAX 160
#define EPISODE_IDS_MAX 400
#define EPISODE_TITLE_MAX 64
#define EPISODE_NUM_MAX 8
#define EPISODES_MAX 2000

#define SEASONS_MAX 24
#define SEASON_NAME_MAX 64

struct season {
	char name[SEASON_NAME_MAX];
	char watch_url[ITEM_URL_MAX];
	int current;
};

struct anime_details {
	char id[ITEM_ID_MAX];
	char poster_url[ITEM_URL_MAX];
	char title[ITEM_TITLE_MAX];
	char type[DETAIL_TEXT_MAX], status[DETAIL_TEXT_MAX], aired[DETAIL_TEXT_MAX], rating[DETAIL_TEXT_MAX];
	char genres[GENRES_MAX];
	char synopsis[SYNOPSIS_MAX];
};

struct episode {
	char number[EPISODE_NUM_MAX];
	char title[EPISODE_TITLE_MAX];
	char server_ids[EPISODE_IDS_MAX];
	int has_sub, has_dub;
};

/* Fill items from a /filter page. Returns how many were found. */
int catalog_parse_list(const char *html, struct anime_item *items, int max);
/* Fill details from a show's /watch page. Returns 0 if it looks like one. */
int catalog_parse_details(const char *html, struct anime_details *out);
/* Fill episodes from the unescaped result of /ajax/episode/list. Returns how many were found. */
int catalog_parse_episodes(const char *html, struct episode *episodes, int max);
/* Fill seasons from the unescaped result of /api/seasons. Returns how many were found. */
int catalog_parse_seasons(const char *html, struct season *seasons, int max);
/* Undo JSON string escapes in place. */
void catalog_json_unescape(char *s);

#endif
