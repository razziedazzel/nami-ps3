/* Nami, an anime app for PS3. */
#include <jpgdec/jpgdec.h>
#include <malloc.h>
#include <pngdec/pngdec.h>
#include <ppu-asm.h>
#include <ppu-types.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#include <unistd.h>

#include "catalog.h"
#include "config.h"
#include "gfx.h"
#include "http.h"
#include "input.h"
#include "store.h"
#include "update.h"
#include "video.h"

#define SITE "https://anikototv.to"
#define SITE_REFERER SITE "/"
#define DATA_DIR APP_DIR "data/"
#define PACKAGE_DIR "/dev_hdd0/packages/"
#define UPDATE_MANIFEST_URL "https://github.com/razziedazzel/nami-ps3/releases/latest/download/latest.json"
#define CACHE_DIR DATA_DIR "cache/"

/* Power-of-two sizes: every texture seen to draw so far has been one. */
#define POSTER_TEX_W 256
#define POSTER_TEX_H 256
#define POSTER_BYTES (POSTER_TEX_W * POSTER_TEX_H * 4)
#define CARD_W 108.0f
#define CARD_H 154.0f
#define CARD_GAP 14.0f
#define MARGIN_X 44.0f
#define HEADER_H 62.0f
#define ROW_H 232.0f
#define ROW_TITLE_H 24.0f
#define SCROLL_EASE 0.2f
#define NEAR_ROWS 1
#define NEAR_CARDS 7
#define BACKDROP_DIM 0x0E0E12D0
#define BACKDROP_SETTLE_FRAMES 20 /* a third of a second, so scrolling past doesn't churn it */
#define BACKDROP_FADE_FRAMES 30
#define SCREEN_FADE_FRAMES 14
#define CARD_GROW 4.0f
#define CARD_GROW_FRAMES 8
/* The part of a poster that fills a wide screen without squashing it. */
#define BACKDROP_TOP 0.13f
#define BACKDROP_BOTTOM 0.56f
#define BADGE_SUB 0x303040E0
#define BADGE_DUB 0x8B7CF6E6
#define TITLE_LINES 2
#define TITLE_HOLD_FRAMES 60
#define TITLE_SCROLL_PER_FRAME 0.7f
#define HINT_PAD_X 10.0f
#define HINT_PAD_Y 4.0f
#define HINT_BOTTOM 14.0f
#define HINT_SOFT 3.0f
#define HINT_SHADOW_OUTER 0x00000060
#define HINT_SHADOW_INNER 0x000000C0
#define PROGRESS_H 4.0f
#define DETAIL_LEFT_W 340.0f
#define DETAIL_SYNOPSIS_LINES 4
#define SEASON_PANEL_W 420.0f
#define SEASON_ROW_H 34.0f
#define SEASON_ROWS_SHOWN 10
#define DETAIL_BUTTON_W 280.0f
#define DETAIL_BUTTON_H 30.0f
#define DETAIL_BUTTON_GAP 6.0f
#define DETAIL_BUTTONS_TOP 300.0f
#define DETAIL_GRID_X 424.0f
#define DETAIL_GRID_Y 76.0f
#define DETAIL_GRID_W 380.0f
#define DETAIL_GRID_BOTTOM 56.0f
#define DETAIL_COLUMNS 5
#define DETAIL_CELL_W 68.0f
#define DETAIL_CELL_H 34.0f
#define DETAIL_CELL_GAP 8.0f
#define WATCHED_BAR_H 3.0f
#define MENU_W 460.0f
#define MENU_ROW_H 34.0f
#define DIALOG_W 360.0f
#define DIALOG_H 130.0f
#define WATCHDOG_MS 30000
#define DECODE_NEEDS_PPU 1
#define SEEK_SHORT 10.0f
#define SEEK_LONG 60.0f
#define SEEK_SETTLE_FRAMES 40
#define OVERLAY_FRAMES 240
#define NEXT_COUNTDOWN_FRAMES 600 /* 10 seconds at 60 frames a second */
#define HISTORY_SAVE_FRAMES 1800
#define HISTORY_UPDATE_FRAMES 120
#define MIN_POSITION_TO_RESUME 5.0f
#define RESUME_BACKUP_SECONDS 3.0f
#define BAR_X MARGIN_X
#define BAR_W (SCREEN_W - 2 * MARGIN_X)
#define BAR_H 5.0f
#define SUBTITLE_BOTTOM 34.0f
#define SUBTITLE_PAD_X 10.0f
#define SUBTITLE_PANEL 0x000000A8
#define SUBTITLE_LINE_MAX 200
#define QUERY_MAX 72
#define SEARCH_MIN_CHARS 2
#define SEARCH_PAUSE_FRAMES 30 /* wait for a pause in typing, half a second at 60 fps */
#define KEY_COLUMNS 10
#define KEY_W 28.0f
#define KEY_H 36.0f
#define KEY_GAP 3.0f
#define KEYBOARD_Y 104.0f
#define RESULTS_X 372.0f
#define SETTINGS_ROWS 9
#define SETTINGS_ROW_H 34.0f
#define SETTINGS_W 560.0f
#define PATH_MAX_LEN 128

struct row_def {
	const char *title;
	const char *query;
};

static const struct row_def row_defs[] = {
	{"Recently updated", "sort=latest-updated"},
	{"Trending", "sort=most-viewed"},
	{"Top rated", "sort=score"},
	{"Popular dubbed", "sort=most-viewed&language%5B%5D=dub"},
	{"Movies", "sort=most-viewed&term_type%5B%5D=Movie"},
	{"New on Anikoto", "sort=latest-added"},
};
#define SITE_ROWS ((int)(sizeof(row_defs) / sizeof(row_defs[0])))

/* Home's rows, then the search results, each with its own fixed poster textures. */
enum { ROW_CONTINUE, ROW_MYLIST, ROW_SITE_FIRST };
#define ROW_SEARCH (ROW_SITE_FIRST + SITE_ROWS)
#define ROW_COUNT (ROW_SEARCH + 1)

enum row_state { ROW_LOADING, ROW_READY, ROW_FAILED };

struct row {
	struct anime_item items[ROW_ITEMS_MAX];
	u32 *textures[ROW_ITEMS_MAX];
	volatile int count;
	volatile int state;
	int focus_col;
	float scroll_x;
};

enum screen { SCREEN_INSTALL, SCREEN_HOME, SCREEN_SEARCH, SCREEN_DETAILS, SCREEN_PLAYER, SCREEN_SETTINGS };

/* What the loader thread is doing for another screen. */
enum job_state { JOB_IDLE, JOB_REQUESTED, JOB_READY, JOB_FAILED };

static struct row rows[ROW_COUNT];
static volatile enum screen screen = SCREEN_HOME;
static enum screen details_from = SCREEN_HOME;
static volatile int running = 1;
static volatile int focus_row = ROW_SITE_FIRST, focus_col;
static FILE *log_file;
static s32 jpg_handle = -1;
static int jpg_uses_spu;
static u32 poster_buffer[POSTER_TEX_W * POSTER_TEX_H];

/* Stage and frame of the main loop, for the watchdog and the log. */
static volatile const char *main_stage = "start";
static volatile u32 main_frames;

/* The Details screen. The main thread requests; the loader fills these, then sets the state. */
static struct anime_item details_show;
static volatile enum job_state details_state;
static struct anime_details details;
static struct episode *episodes;
static volatile int episode_count;
/* An episode, or one of the buttons on the left. */
enum { DETAIL_PLAY = -1, DETAIL_AUDIO = -2, DETAIL_MYLIST = -3, DETAIL_SEASONS = -4 };
static int detail_focus = DETAIL_PLAY;
static float detail_scroll, detail_scroll_target;
static int detail_want_dub;
static struct season seasons[SEASONS_MAX];
static volatile int season_count;
static int seasons_open;
static int season_focus;
static u32 *details_texture;
static int search_return_row = ROW_SITE_FIRST;

/* Search. The main thread edits search_query; the loader searches for a copy of it. */
static char search_query[QUERY_MAX];
static char search_requested[QUERY_MAX];
static char search_done[QUERY_MAX];
static volatile enum job_state search_state;
static int search_wait_frames;
static int keyboard_focus;
static int search_on_results;
static struct anime_item search_results[ROW_ITEMS_MAX];

/* Clearing the poster cache runs on the loader, as it can take a while. */
static volatile int clear_cache_requested;
static int cache_files_removed;

/* The player. */
static int player_episode;
static int player_ended;
static int player_countdown; /* Frames until the next episode starts, or 0 for none */
static int player_seek_frames;
static float player_seek_target;
static int player_overlay_frames;
static int player_subtitles_on;
static int player_history_frames;
static int player_save_frames;
static float seek_shown_target;

static u32 backdrop_wanted, backdrop_current, backdrop_previous;
static int backdrop_settle;
static int backdrop_fade = BACKDROP_FADE_FRAMES;
static enum screen faded_screen = SCREEN_HOME;
static int screen_fade;
/* A small menu of actions, over Home or Details. */
#define MENU_ITEMS_MAX 5
#define MENU_TEXT_MAX 64
enum menu_action {
	MENU_CANCEL, MENU_TOGGLE_WATCHED, MENU_WATCHED_UP_TO, MENU_ALL_UNWATCHED, MENU_REMOVE_CONTINUE, MENU_REMOVE_MYLIST
};
struct menu_item {
	char text[MENU_TEXT_MAX];
	enum menu_action action;
};
static struct menu_item menu_items[MENU_ITEMS_MAX];
static int menu_count;
static int menu_focus;
static char menu_title[ITEM_TITLE_MAX];
static char menu_note[96];
static int menu_episode;
/* Updates. Only the loader changes the state, except to ask for work. */
enum update_state {
	UPDATE_UNKNOWN, UPDATE_CHECKING, UPDATE_CURRENT, UPDATE_AVAILABLE, UPDATE_DOWNLOADING, UPDATE_READY, UPDATE_CHECK_FAILED,
	UPDATE_DOWNLOAD_FAILED
};
static volatile enum update_state update_state;
static volatile int update_check_requested = 1;
static volatile int update_download_requested;
static int update_announced;
static struct update_info update_info;
static int settings_focus;
static float scroll_y;
static int title_frames;
static int confirm_exit;
static char status_text[96];
static int status_frames;

void app_log(const char *fmt, ...)
{
	if (!log_file)
		return;
	va_list args;
	va_start(args, fmt);
	vfprintf(log_file, fmt, args);
	va_end(args);
	fflush(log_file);
}

static void show_status(const char *text)
{
	snprintf(status_text, sizeof(status_text), "%s", text);
	status_frames = 150;
}

/* ---- posters ---- */

static void *jpg_malloc(u32 size, void *arg)
{
	(void)arg;
	return malloc(size);
}

static void jpg_free(void *ptr, void *arg)
{
	(void)arg;
	free(ptr);
}

/* Prefer decoding on an SPU; fall back to the PPU if the system refuses. */
static int jpg_open_decoder(int spu)
{
	jpgDecThreadInParam in;
	jpgDecThreadOutParam out;
	memset(&in, 0, sizeof(in));
	in.spu_enable = spu ? JPGDEC_SPU_THREAD_ENABLE : JPGDEC_SPU_THREAD_DISABLE;
	in.ppu_prio = 512;
	in.spu_prio = 200;
	in.malloc_func = (jpgCbCtrlMalloc)__get_opd32(jpg_malloc);
	in.free_func = (jpgCbCtrlFree)__get_opd32(jpg_free);
	s32 r = jpgDecCreate(&jpg_handle, &in, &out);
	app_log("jpgDecCreate spu=%d result=%08x\n", spu, r);
	return r;
}

static int jpg_init(void)
{
	s32 module = sysModuleLoad(SYSMODULE_JPGDEC);
	s32 png_module = sysModuleLoad(SYSMODULE_PNGDEC);
	app_log("jpg module=%08x png module=%08x\n", module, png_module);
	if (jpg_open_decoder(1) == 0) {
		jpg_uses_spu = 1;
		return 0;
	}
	return jpg_open_decoder(0);
}

/* Stretch decoded pixels to a poster's fixed RSX texture, dropping any transparency. */
static void fit_poster(const u32 *pixels, u32 width, u32 height, u32 pitch, u32 *texture)
{
	for (u32 y = 0; y < POSTER_TEX_H; y++) {
		const u32 *line = (const u32 *)((const u8 *)pixels + (u64)(y * height / POSTER_TEX_H) * pitch);
		u32 *dst = texture + (u64)y * POSTER_TEX_W;
		for (u32 x = 0; x < POSTER_TEX_W; x++)
			dst[x] = line[x * width / POSTER_TEX_W] | 0xFF000000;
	}
}

static int decode_png(const void *data, u32 size, u32 *texture)
{
	pngData png;
	memset(&png, 0, sizeof(png));
	s32 r = pngLoadFromBuffer(data, size, &png);
	if (r != 0 || !png.bmp_out) {
		app_log("png decode=%08x\n", r);
		return -1;
	}
	fit_poster(png.bmp_out, png.width, png.height, png.pitch, texture);
	free(png.bmp_out);
	return 0;
}

/* Decode to main memory, then fit the poster to its fixed RSX texture. */
static int decode_poster_with(const void *jpeg, u32 size, u32 *texture, int use_spu)
{
	jpgDecSource src;
	jpgDecOpnInfo open_info;
	jpgDecInfo info;
	jpgDecInParam in;
	jpgDecOutParam out;
	jpgDecDataCtrlParam ctrl;
	jpgDecDataInfo data_info;
	s32 sub;
	int result = -1;

	memset(&src, 0, sizeof(src));
	src.stream_sel = JPGDEC_BUFFER;
	src.stream_ptr = (void *)jpeg;
	src.stream_size = size;
	src.spu_enable = use_spu ? JPGDEC_SPU_THREAD_ENABLE : JPGDEC_SPU_THREAD_DISABLE;

	s32 r = jpgDecOpen(jpg_handle, &sub, &src, &open_info);
	if (r != 0) {
		app_log("jpg open=%08x\n", r);
		return -1;
	}
	r = jpgDecReadHeader(jpg_handle, sub, &info);
	if (r == (s32)JPGDEC_ERROR_SPU_UNSUPPORT) {
		jpgDecClose(jpg_handle, sub);
		return DECODE_NEEDS_PPU;
	}
	if (r != 0 || info.color_space == 0)
		app_log("jpg header=%08x space=%u\n", r, info.color_space);
	if (r == 0 && info.color_space != 0) {
		memset(&in, 0, sizeof(in));
		in.down_scale = 1;
		in.quality_mode = JPGDEC_FAST;
		in.output_mode = JPGDEC_TOP_TO_BOTTOM;
		in.color_space = JPGDEC_ARGB;
		in.alpha = 0xFF;
		r = jpgDecSetParameter(jpg_handle, sub, &in, &out);
		if (r != 0)
			app_log("jpg param=%08x\n", r);
		if (r == 0) {
			u32 pitch = out.width * 4;
			u32 *pixels = malloc(pitch * out.height);
			if (pixels) {
				memset(&ctrl, 0, sizeof(ctrl));
				ctrl.output_bytes_per_line = pitch;
				r = jpgDecDecodeData(jpg_handle, sub, (u8 *)pixels, &ctrl, &data_info);
				if (r != 0 || data_info.decode_status != 0)
					app_log("jpg decode=%08x status=%u size=%ux%u space=%u\n", r,
					        data_info.decode_status, out.width, out.height, info.color_space);
				if (r == 0 && data_info.decode_status == 0) {
					fit_poster(pixels, out.width, out.height, pitch, texture);
					result = 0;
				}
				free(pixels);
			}
		}
	}
	jpgDecClose(jpg_handle, sub);
	return result;
}

/* The SPU decoder refuses some JPEG types, so those go to the PPU. */
static int decode_poster(const void *jpeg, u32 size, u32 *texture)
{
	static u32 on_spu, on_ppu;
	static const u8 png_magic[4] = {0x89, 'P', 'N', 'G'};
	/* File names can't be trusted: some .jpg names hold other formats. */
	if (size > sizeof(png_magic) && memcmp(jpeg, png_magic, sizeof(png_magic)) == 0)
		return decode_png(jpeg, size, texture);
	int r = jpg_uses_spu ? decode_poster_with(jpeg, size, texture, 1) : DECODE_NEEDS_PPU;
	if (r == DECODE_NEEDS_PPU) {
		r = decode_poster_with(jpeg, size, texture, 0);
		on_ppu += r == 0;
	} else {
		on_spu += r == 0;
	}
	if ((on_spu + on_ppu) % 20 == 0 && r == 0)
		app_log("posters decoded spu=%u ppu=%u\n", on_spu, on_ppu);
	return r;
}

/* ---- poster cache ---- */

static void cache_path(const char *id, char *out, size_t size)
{
	snprintf(out, size, CACHE_DIR "%s.tex", id);
}

static int cache_read(const char *id, void *pixels)
{
	char path[PATH_MAX_LEN];
	cache_path(id, path, sizeof(path));
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	size_t got = fread(pixels, 1, POSTER_BYTES, f);
	fclose(f);
	return got == POSTER_BYTES ? 0 : -1;
}

static void cache_write(const char *id, const void *pixels)
{
	char path[PATH_MAX_LEN];
	cache_path(id, path, sizeof(path));
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	size_t wrote = fwrite(pixels, 1, POSTER_BYTES, f);
	fclose(f);
	if (wrote != POSTER_BYTES)
		remove(path);
}

static void cache_clear(void)
{
	s32 dir;
	sysFSDirent entry;
	u64 read_size = 0;
	int removed = 0;
	if (sysLv2FsOpenDir(CACHE_DIR, &dir) != 0)
		return;
	while (sysLv2FsReadDir(dir, &entry, &read_size) == 0 && read_size > 0) {
		if (strstr(entry.d_name, ".tex")) {
			char path[PATH_MAX_LEN];
			snprintf(path, sizeof(path), CACHE_DIR "%.32s", entry.d_name);
			removed += remove(path) == 0;
		}
	}
	sysLv2FsCloseDir(dir);
	cache_files_removed = removed;
	app_log("poster cache cleared: %d files\n", removed);
}

/* The next poster to load: on screen first, then the rest. */
static struct anime_item *next_poster(int *row_out, int *col_out)
{
	int fr = focus_row, fc = focus_col;
	for (int pass = 0; pass < 2; pass++) {
		for (int dr = 0; dr < ROW_COUNT; dr++) {
			for (int sign = 0; sign < 2; sign++) {
				int r = fr + (sign ? -dr : dr);
				if (r < 0 || r >= ROW_COUNT || (dr == 0 && sign))
					continue;
				if (pass == 0 && dr > NEAR_ROWS)
					continue;
				int base = r == fr ? fc : rows[r].focus_col;
				for (int k = 0; k < rows[r].count * 2; k++) {
					/* Spreading out from the focused card, both ways. */
					int c = k % 2 ? base - (k + 1) / 2 : base + k / 2;
					if (c < 0 || c >= rows[r].count)
						continue;
					if (pass == 0 && abs(c - base) > NEAR_CARDS)
						continue;
					if (rows[r].items[c].poster_state == POSTER_NONE) {
						*row_out = r;
						*col_out = c;
						return &rows[r].items[c];
					}
				}
			}
		}
	}
	return NULL;
}

static void load_poster(struct http_client *http, struct anime_item *it, u32 *texture)
{
	it->poster_state = POSTER_LOADING;
	u32 offset = gfx_offset(texture);

	if (cache_read(it->id, poster_buffer) == 0) {
		memcpy(texture, poster_buffer, POSTER_BYTES);
		it->poster_offset = offset;
		it->poster_state = POSTER_READY;
		return;
	}
	char *body = NULL;
	size_t len = 0;
	if (http_get(http, it->poster_url, SITE_REFERER, 0, &body, &len) != 0) {
		it->poster_state = POSTER_FAILED;
		return;
	}
	if (decode_poster(body, (u32)len, poster_buffer) != 0) {
		const u8 *b = (const u8 *)body;
		app_log("poster decode failed %.80s starts %02x%02x%02x%02x\n", it->poster_url, b[0], b[1], b[2], b[3]);
		it->poster_state = POSTER_FAILED;
	} else {
		memcpy(texture, poster_buffer, POSTER_BYTES);
		cache_write(it->id, poster_buffer);
		it->poster_offset = offset;
		it->poster_state = POSTER_READY;
	}
	free(body);
}

/* ---- updates ---- */

static void update_package_path(char *out, size_t size)
{
	snprintf(out, size, PACKAGE_DIR "Nami-PS3-%s.pkg", update_info.version_name);
}

/* Whether the new version's package is already on the hard drive, intact. */
static int update_package_present(void)
{
	char path[PATH_MAX_LEN], sha[UPDATE_SHA_MAX];
	update_package_path(path, sizeof(path));
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *data = size > 0 ? malloc((size_t)size) : NULL;
	size_t got = data ? fread(data, 1, (size_t)size, f) : 0;
	fclose(f);
	int present = 0;
	if (data && got == (size_t)size) {
		update_sha256_hex(data, got, sha);
		present = strcmp(sha, update_info.sha256) == 0;
	}
	free(data);
	return present;
}

static void check_for_update(struct http_client *http)
{
	char *body = NULL;
	size_t len;

	update_state = UPDATE_CHECKING;
	if (http_get(http, UPDATE_MANIFEST_URL, NULL, 0, &body, &len) != 0) {
		update_state = UPDATE_CHECK_FAILED;
		return;
	}
	int parsed = update_parse_manifest(body, &update_info);
	free(body);
	if (parsed != 0) {
		app_log("update manifest unreadable\n");
		update_state = UPDATE_CHECK_FAILED;
	} else if (update_info.version_code <= APP_VERSION_CODE) {
		update_state = UPDATE_CURRENT;
	} else {
		update_state = update_package_present() ? UPDATE_READY : UPDATE_AVAILABLE;
	}
	app_log("update: this=%d latest=%d state=%d\n", APP_VERSION_CODE, update_info.version_code, update_state);
}

static void download_update(struct http_client *http)
{
	char path[PATH_MAX_LEN], sha[UPDATE_SHA_MAX];
	char *body = NULL;
	size_t len;

	update_state = UPDATE_DOWNLOADING;
	if (http_get(http, update_info.pkg_url, NULL, 0, &body, &len) != 0) {
		update_state = UPDATE_DOWNLOAD_FAILED;
		return;
	}
	/* Only an exact match is ever saved where Package Manager will offer it. */
	update_sha256_hex(body, len, sha);
	if (strcmp(sha, update_info.sha256) != 0) {
		app_log("update download has the wrong checksum\n");
		free(body);
		update_state = UPDATE_DOWNLOAD_FAILED;
		return;
	}
	update_package_path(path, sizeof(path));
	FILE *f = fopen(path, "wb");
	size_t wrote = f ? fwrite(body, 1, len, f) : 0;
	if (f)
		fclose(f);
	free(body);
	if (wrote != len) {
		remove(path);
		update_state = UPDATE_DOWNLOAD_FAILED;
		return;
	}
	update_state = UPDATE_READY;
}

/* ---- loading ---- */

/* What a failure means, in words for the screen. */
static const char *failure_text(const char *reason)
{
	if (!reason)
		return "This episode couldn't be played.";
	if (strstr(reason, "not_available"))
		return "This show has no 720p or lower version. See Experimental 1080p in Settings.";
	if (strstr(reason, "dub"))
		return "There's no dub of this episode.";
	if (strstr(reason, "servers") || strstr(reason, "server_list") || strstr(reason, "download") ||
	    strstr(reason, "playlist"))
		return "The video servers aren't responding. Check the connection, or try again shortly.";
	return "This episode couldn't be played.";
}

static void load_details(struct http_client *http)
{
	char url[256];
	char *body = NULL;
	size_t len;
	int ok = 0;

	season_count = 0;
	if (http_get(http, details_show.watch_url, SITE_REFERER, 0, &body, &len) == 0) {
		ok = catalog_parse_details(body, &details) == 0;
		free(body);
	}
	episode_count = 0;
	/* Opened from a season link, the show is known only by its address until now. */
	if (ok && !details_show.id[0]) {
		snprintf(details_show.id, sizeof(details_show.id), "%s", details.id);
		snprintf(details_show.title, sizeof(details_show.title), "%s", details.title);
		ok = details_show.id[0] != 0;
	}
	if (ok && !details_show.poster_url[0])
		snprintf(details_show.poster_url, sizeof(details_show.poster_url), "%s", details.poster_url);
	if (ok) {
		snprintf(url, sizeof(url), SITE "/api/seasons/%s", details_show.id);
		if (http_get(http, url, SITE_REFERER, 1, &body, &len) == 0) {
			catalog_json_unescape(body);
			season_count = catalog_parse_seasons(body, seasons, SEASONS_MAX);
			free(body);
		}
		snprintf(url, sizeof(url), SITE "/ajax/episode/list/%s", details_show.id);
		if (http_get(http, url, SITE_REFERER, 1, &body, &len) == 0) {
			catalog_json_unescape(body);
			episode_count = catalog_parse_episodes(body, episodes, EPISODES_MAX);
			free(body);
		}
	}
	app_log("details %s ok=%d episodes=%d seasons=%d\n", details_show.id, ok, episode_count, season_count);
	details_state = ok ? JOB_READY : JOB_FAILED;
	if (ok && details_show.poster_state != POSTER_READY && details_show.poster_url[0])
		load_poster(http, &details_show, details_texture);
}

static void set_row_textures_for(struct row *row, int count)
{
	for (int c = 0; c < count; c++)
		row->items[c].poster_state = POSTER_NONE;
}

static void load_search(struct http_client *http)
{
	char url[QUERY_MAX * 3 + 64], encoded[QUERY_MAX * 3];
	char query[QUERY_MAX];
	char *body = NULL;
	size_t len, n = 0;
	struct row *row = &rows[ROW_SEARCH];

	snprintf(query, sizeof(query), "%s", search_requested);
	for (const char *p = query; *p && n + 4 < sizeof(encoded); p++) {
		unsigned char c = (unsigned char)*p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
			encoded[n++] = (char)c;
		} else if (c == ' ') {
			encoded[n++] = '+';
		} else {
			n += (size_t)snprintf(encoded + n, 4, "%%%02X", c);
		}
	}
	encoded[n] = 0;

	snprintf(url, sizeof(url), SITE "/filter?keyword=%s&page=1", encoded);
	if (http_get(http, url, SITE_REFERER, 0, &body, &len) != 0) {
		search_state = JOB_FAILED;
		return;
	}
	int count = catalog_parse_list(body, search_results, ROW_ITEMS_MAX);
	free(body);

	/* Swapped in all at once, so the old results stay up until the new ones are ready. */
	row->count = 0;
	memcpy(row->items, search_results, sizeof(search_results[0]) * (size_t)count);
	set_row_textures_for(row, count);
	row->focus_col = 0;
	row->count = count;
	snprintf(search_done, sizeof(search_done), "%s", query);
	app_log("search [%s] results=%d\n", query, count);
	search_state = JOB_READY;
}

static void loader_thread(void *arg)
{
	(void)arg;
	http_global_init();
	struct http_client *http = http_client_new();
	jpg_init();
	if (!http) {
		for (int r = ROW_SITE_FIRST; r < ROW_SEARCH; r++)
			rows[r].state = ROW_FAILED;
		sysThreadExit(0);
	}

	for (int d = 0; d < SITE_ROWS && running; d++) {
		struct row *row = &rows[ROW_SITE_FIRST + d];
		char url[256];
		char *body = NULL;
		size_t len;
		snprintf(url, sizeof(url), SITE "/filter?keyword=&%s&page=1", row_defs[d].query);
		if (http_get(http, url, SITE_REFERER, 0, &body, &len) != 0) {
			row->state = ROW_FAILED;
			continue;
		}
		row->count = catalog_parse_list(body, row->items, ROW_ITEMS_MAX);
		row->state = row->count > 0 ? ROW_READY : ROW_FAILED;
		app_log("row %s items=%d\n", row_defs[d].title, row->count);
		free(body);
	}

	while (running) {
		if (details_state == JOB_REQUESTED) {
			load_details(http);
			continue;
		}
		if (search_state == JOB_REQUESTED) {
			load_search(http);
			continue;
		}
		if (clear_cache_requested) {
			cache_clear();
			clear_cache_requested = 0;
			continue;
		}
		if (update_download_requested) {
			update_download_requested = 0;
			download_update(http);
			continue;
		}
		/* Leave the CPU and network to the video while it plays. */
		if (screen == SCREEN_PLAYER) {
			usleep(100000);
			continue;
		}
		int r, c;
		struct anime_item *it = next_poster(&r, &c);
		if (it) {
			load_poster(http, it, rows[r].textures[c]);
		} else if (update_check_requested) {
			/* After the posters, so it never slows the Home screen. */
			update_check_requested = 0;
			check_for_update(http);
		} else {
			usleep(50000);
		}
	}
	sysThreadExit(0);
}

/* ---- common drawing ---- */

static void stage(const char *name)
{
	main_stage = name;
}

static void draw_badge(const char *text, float x, float y, u32 color)
{
	float w = gfx_text_width(text, TEXT_SMALL) + 6;
	gfx_rect(x, y, w, gfx_line_height(TEXT_SMALL), color);
	gfx_text(text, x + 3, y, TEXT_SMALL, COLOR_TEXT);
}

/* A title too long for its card waits, slides across once, waits again, then goes back to normal. */
static void draw_focused_title(const char *title, float x, float y)
{
	float overflow = gfx_text_width(title, TEXT_SMALL) - CARD_W;
	float scroll_frames = overflow / TITLE_SCROLL_PER_FRAME;
	int t = title_frames - TITLE_HOLD_FRAMES;

	if (gfx_text_fits(title, CARD_W, TITLE_LINES, TEXT_SMALL) || t < 0 ||
	    t > scroll_frames + TITLE_HOLD_FRAMES) {
		gfx_text_wrapped(title, x, y, CARD_W, TITLE_LINES, TEXT_SMALL, COLOR_TEXT);
		return;
	}
	float shift = t < scroll_frames ? t * TITLE_SCROLL_PER_FRAME : overflow;
	gfx_clip(x, y, CARD_W, gfx_line_height(TEXT_SMALL) * TITLE_LINES);
	gfx_text(title, x - shift, y, TEXT_SMALL, COLOR_TEXT);
	gfx_clip_off();
}

/* progress runs from 0 to 1, or is below 0 for none. */
static void draw_card(const struct anime_item *it, float x, float y, int focused, float progress)
{
	/* The selected card eases up to size. */
	float grow = 0.0f;
	if (focused) {
		float t = title_frames >= CARD_GROW_FRAMES ? 1.0f : (float)title_frames / CARD_GROW_FRAMES;
		grow = CARD_GROW * t * (2.0f - t);
	}
	float cx = x - grow, cy = y - grow, cw = CARD_W + 2 * grow, ch = CARD_H + 2 * grow;

	if (it->poster_state == POSTER_READY)
		gfx_image(it->poster_offset, POSTER_TEX_W, POSTER_TEX_H, cx, cy, cw, ch);
	else
		gfx_rect(cx, cy, cw, ch, 0x22222AFF);
	if (focused)
		gfx_outline(cx, cy, cw, ch, 2.5f, COLOR_ACCENT);

	char label[24];
	float by = y + CARD_H - gfx_line_height(TEXT_SMALL) - 3;
	float bx = x + 3;
	if (it->sub_count > 0) {
		snprintf(label, sizeof(label), "SUB %d", it->sub_count);
		draw_badge(label, bx, by, BADGE_SUB);
		bx += gfx_text_width(label, TEXT_SMALL) + 9;
	}
	if (it->dub_count > 0 && bx < x + CARD_W - 30) {
		snprintf(label, sizeof(label), "DUB %d", it->dub_count);
		draw_badge(label, bx, by, BADGE_DUB);
	}
	if (progress >= 0) {
		gfx_rect(x, y + CARD_H - PROGRESS_H, CARD_W, PROGRESS_H, 0x00000080);
		gfx_rect(x, y + CARD_H - PROGRESS_H, CARD_W * (progress > 1 ? 1 : progress), PROGRESS_H, COLOR_ACCENT);
	}
	if (focused)
		draw_focused_title(it->title, x, y + CARD_H + 7);
	else
		gfx_text_wrapped(it->title, x, y + CARD_H + 7, CARD_W, TITLE_LINES, TEXT_SMALL, COLOR_MUTED);
}

/* The buttons' help, on a dark panel so it reads over any picture. */
static void draw_hint(const char *hint)
{
	float w = gfx_text_width(hint, TEXT_SMALL) + 2 * HINT_PAD_X;
	float h = gfx_line_height(TEXT_SMALL) + 2 * HINT_PAD_Y;
	float x = SCREEN_W - MARGIN_X - w, y = SCREEN_H - HINT_BOTTOM - h;
	gfx_rect(x - HINT_SOFT, y - HINT_SOFT, w + 2 * HINT_SOFT, h + 2 * HINT_SOFT, HINT_SHADOW_OUTER);
	gfx_rect(x, y, w, h, HINT_SHADOW_INNER);
	gfx_text(hint, x + HINT_PAD_X, y + HINT_PAD_Y, TEXT_SMALL, COLOR_TEXT);
}

static void draw_status(void)
{
	if (status_frames <= 0)
		return;
	status_frames--;
	float w = gfx_text_width(status_text, TEXT_BODY) + 32;
	float x = (SCREEN_W - w) / 2, y = 24;
	gfx_rect(x, y, w, 34, COLOR_SURFACE);
	gfx_outline(x, y, w, 34, 1.5f, COLOR_ACCENT);
	gfx_text(status_text, x + 16, y + 6, TEXT_BODY, COLOR_TEXT);
}

/* The background: the show's poster, crossfading when the selection has settled on another. */
static void draw_backdrop(const struct anime_item *show)
{
	u32 wanted = show && show->poster_state == POSTER_READY ? show->poster_offset : 0;
	if (wanted != backdrop_wanted) {
		backdrop_wanted = wanted;
		backdrop_settle = backdrop_current ? BACKDROP_SETTLE_FRAMES : 0;
	}
	if (backdrop_settle > 0) {
		backdrop_settle--;
	} else if (backdrop_wanted && backdrop_wanted != backdrop_current) {
		backdrop_previous = backdrop_current;
		backdrop_current = backdrop_wanted;
		backdrop_fade = 0;
	}
	if (backdrop_fade < BACKDROP_FADE_FRAMES)
		backdrop_fade++;

	u32 alpha = (u32)backdrop_fade * 255 / BACKDROP_FADE_FRAMES;
	if (backdrop_previous && alpha < 255)
		gfx_image_rows(backdrop_previous, POSTER_TEX_W, POSTER_TEX_H, 0, 0, SCREEN_W, SCREEN_H, BACKDROP_TOP,
		                  BACKDROP_BOTTOM, 255);
	if (backdrop_current)
		gfx_image_rows(backdrop_current, POSTER_TEX_W, POSTER_TEX_H, 0, 0, SCREEN_W, SCREEN_H, BACKDROP_TOP,
		                  BACKDROP_BOTTOM, alpha);
}

/* Every screen ends here: a new screen fades in over a few frames. */
static void end_frame(void)
{
	if (screen != faded_screen) {
		faded_screen = screen;
		screen_fade = SCREEN_FADE_FRAMES;
	}
	if (screen_fade > 0) {
		u32 alpha = (u32)screen_fade * 255 / SCREEN_FADE_FRAMES;
		gfx_rect(0, 0, SCREEN_W, SCREEN_H, (COLOR_BACKGROUND & 0xFFFFFF00) | alpha);
		screen_fade--;
	}
	gfx_end_frame();
}

static void format_time(float seconds, char *out, size_t size)
{
	int s = seconds < 0 ? 0 : (int)seconds;
	if (s >= 3600)
		snprintf(out, size, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
	else
		snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

/* ---- menus ---- */

static void menu_begin(const char *title, const char *note)
{
	snprintf(menu_title, sizeof(menu_title), "%s", title);
	snprintf(menu_note, sizeof(menu_note), "%s", note);
	menu_count = 0;
	menu_focus = 0;
}

static void menu_add(const char *text, enum menu_action action)
{
	if (menu_count == MENU_ITEMS_MAX)
		return;
	snprintf(menu_items[menu_count].text, MENU_TEXT_MAX, "%s", text);
	menu_items[menu_count].action = action;
	menu_count++;
}

static void draw_menu(void)
{
	if (menu_count == 0)
		return;
	float note_h = menu_note[0] ? gfx_line_height(TEXT_SMALL) * 2 + 8 : 0;
	float h = (float)menu_count * MENU_ROW_H + 62 + note_h;
	float x = (SCREEN_W - MENU_W) / 2, y = (SCREEN_H - h) / 2;
	gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0x00000090);
	gfx_rect(x, y, MENU_W, h, COLOR_SURFACE);
	gfx_outline(x, y, MENU_W, h, 1.5f, COLOR_ACCENT);
	gfx_text_wrapped(menu_title, x + 18, y + 14, MENU_W - 36, 1, TEXT_TITLE, COLOR_TEXT);
	float ry = y + 48;
	if (menu_note[0]) {
		gfx_text_wrapped(menu_note, x + 18, ry, MENU_W - 36, 2, TEXT_SMALL, COLOR_MUTED);
		ry += note_h;
	}
	for (int i = 0; i < menu_count; i++, ry += MENU_ROW_H) {
		if (i == menu_focus)
			gfx_rect(x + 8, ry, MENU_W - 16, MENU_ROW_H - 4, COLOR_ACCENT);
		gfx_text(menu_items[i].text, x + 18, ry + 5, TEXT_BODY, COLOR_TEXT);
	}
}

/* Returns the action chosen, or -1 while it's still open. Closes it on a choice. */
static int update_menu(void)
{
	if (input_pressed(ACT_BACK)) {
		menu_count = 0;
		return MENU_CANCEL;
	}
	if (input_pressed(ACT_UP) && menu_focus > 0)
		menu_focus--;
	if (input_pressed(ACT_DOWN) && menu_focus + 1 < menu_count)
		menu_focus++;
	if (input_pressed(ACT_OK)) {
		int action = menu_items[menu_focus].action;
		menu_count = 0;
		return action;
	}
	return -1;
}

/* ---- rows that come from what's saved ---- */

static void fill_saved_item(struct anime_item *it, const struct saved_show *show)
{
	memset(it, 0, sizeof(*it));
	snprintf(it->id, sizeof(it->id), "%s", show->id);
	snprintf(it->title, sizeof(it->title), "%s", show->title);
	snprintf(it->poster_url, sizeof(it->poster_url), "%s", show->poster_url);
	snprintf(it->watch_url, sizeof(it->watch_url), "%s", show->watch_url);
}

static void rebuild_saved_rows(void)
{
	struct row *cont = &rows[ROW_CONTINUE], *list = &rows[ROW_MYLIST];
	cont->count = 0;
	list->count = 0;
	int n = history_count < ROW_ITEMS_MAX ? history_count : ROW_ITEMS_MAX;
	for (int i = 0; i < n; i++)
		fill_saved_item(&cont->items[i], &history[i].show);
	cont->count = n;
	n = mylist_count < ROW_ITEMS_MAX ? mylist_count : ROW_ITEMS_MAX;
	for (int i = 0; i < n; i++)
		fill_saved_item(&list->items[i], &mylist[i]);
	list->count = n;
	cont->state = list->state = ROW_READY;
	if (cont->focus_col >= cont->count)
		cont->focus_col = cont->count ? cont->count - 1 : 0;
	if (list->focus_col >= list->count)
		list->focus_col = list->count ? list->count - 1 : 0;
}

static const char *row_title(int r)
{
	if (r == ROW_CONTINUE)
		return "Continue watching";
	if (r == ROW_MYLIST)
		return "My List";
	if (r == ROW_SEARCH)
		return "Results";
	return row_defs[r - ROW_SITE_FIRST].title;
}

/* Home hides its two personal rows when they're empty. */
static int row_visible(int r)
{
	if (r == ROW_SEARCH)
		return 0;
	if (r == ROW_CONTINUE || r == ROW_MYLIST)
		return rows[r].count > 0;
	return 1;
}

static int visible_index(int row)
{
	int index = 0;
	for (int r = 0; r < row; r++)
		index += row_visible(r);
	return index;
}

static void draw_row_cards(int r, float left, float y, int has_focus)
{
	struct row *row = &rows[r];
	float cards_y = y + ROW_TITLE_H + 8;

	gfx_text(row_title(r), left, y, TEXT_TITLE, COLOR_TEXT);
	if (row->count == 0) {
		gfx_text(row->state == ROW_FAILED ? "Couldn't load" : "Loading...", left, cards_y, TEXT_BODY, COLOR_MUTED);
		return;
	}
	int col = has_focus ? focus_col : row->focus_col;
	row->scroll_x += (col * (CARD_W + CARD_GAP) - row->scroll_x) * SCROLL_EASE;
	for (int c = 0; c < row->count; c++) {
		float x = left + c * (CARD_W + CARD_GAP) - row->scroll_x;
		if (x > SCREEN_W || x + CARD_W < left - CARD_W)
			continue;
		float progress = -1;
		if (r == ROW_CONTINUE && c < history_count && history[c].duration > 0)
			progress = history[c].position / history[c].duration;
		draw_card(&row->items[c], x, cards_y, has_focus && c == col, progress);
	}
}

/* ---- home ---- */

static const struct anime_item *focused_item(void)
{
	if (focus_col < rows[focus_row].count)
		return &rows[focus_row].items[focus_col];
	return NULL;
}

static void draw_home(void)
{
	stage("home");
	gfx_begin_frame(COLOR_BACKGROUND);

	/* Behind everything: the focused show, dimmed, and a violet glow. */
	draw_backdrop(focused_item());
	gfx_rect(0, 0, SCREEN_W, SCREEN_H, BACKDROP_DIM);
	gfx_hgradient(SCREEN_W * 0.55f, 0, SCREEN_W * 0.45f, 200, 0x8B7CF600, 0x8B7CF638);
	gfx_vgradient(0, 0, SCREEN_W, SCREEN_H, 0x0E0E1240, 0x0E0E12E0);

	float target = visible_index(focus_row) * ROW_H;
	scroll_y += (target - scroll_y) * SCROLL_EASE;

	int index = 0;
	for (int r = 0; r < ROW_COUNT; r++) {
		if (!row_visible(r))
			continue;
		float y = HEADER_H + index * ROW_H - scroll_y;
		index++;
		if (y > SCREEN_H || y + ROW_H < 0)
			continue;
		draw_row_cards(r, MARGIN_X, y, r == focus_row);
	}

	/* The header stays put, over a fade so rows pass under it cleanly. */
	gfx_vgradient(0, 0, SCREEN_W, HEADER_H, 0x0E0E12FF, 0x0E0E1200);
	gfx_text("Nami", MARGIN_X, 14, TEXT_HEAD, COLOR_TEXT);
	gfx_rect(MARGIN_X, 50, 34, 2.5f, COLOR_ACCENT);

	if (focus_row == ROW_CONTINUE || focus_row == ROW_MYLIST)
		draw_hint(input_last_device() == 2
		              ? "Enter Open    " ICON_TRIANGLE " Search    " ICON_SQUARE " Settings    Select Remove    Return Exit"
		              : ICON_CROSS " Open    " ICON_TRIANGLE " Search    " ICON_SQUARE " Settings    Select Remove    " ICON_CIRCLE " Exit");
	else
		draw_hint(input_last_device() == 2 ? "Enter Open    " ICON_TRIANGLE " Search    " ICON_SQUARE " Settings    Return Exit"
		                                   : ICON_CROSS " Open    " ICON_TRIANGLE " Search    " ICON_SQUARE " Settings    " ICON_CIRCLE " Exit");
	draw_menu();
	draw_status();

	if (confirm_exit) {
		float x = (SCREEN_W - DIALOG_W) / 2, y = (SCREEN_H - DIALOG_H) / 2;
		gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0x00000090);
		gfx_rect(x, y, DIALOG_W, DIALOG_H, COLOR_SURFACE);
		gfx_outline(x, y, DIALOG_W, DIALOG_H, 1.5f, COLOR_ACCENT);
		gfx_text("Exit Nami?", x + 24, y + 22, TEXT_TITLE, COLOR_TEXT);
		gfx_text(input_last_device() == 2 ? "Enter Exit      Return Stay" : ICON_CROSS " Exit      " ICON_CIRCLE " Stay", x + 24, y + 78,
		         TEXT_BODY, COLOR_MUTED);
	}
	stage("flip");
	end_frame();
}

static void open_details(const struct anime_item *item, enum screen from)
{
	/* A copy: the row it came from can change while Details is open. */
	details_show = *item;
	details_from = from;
	episode_count = 0;
	detail_focus = DETAIL_PLAY;
	detail_scroll = detail_scroll_target = 0;
	detail_want_dub = 0;
	seasons_open = 0;
	season_count = 0;
	details_state = JOB_REQUESTED;
	screen = SCREEN_DETAILS;
}

static void open_season(const struct season *chosen)
{
	struct anime_item item;
	memset(&item, 0, sizeof(item));
	snprintf(item.title, sizeof(item.title), "%s", chosen->name);
	snprintf(item.watch_url, sizeof(item.watch_url), "%s", chosen->watch_url);
	/* It replaces this show, so Back still returns to where the show was opened from. */
	seasons_open = 0;
	open_details(&item, details_from);
}

static void open_search(void)
{
	rows[focus_row].focus_col = focus_col;
	search_query[0] = 0;
	search_done[0] = 0;
	search_requested[0] = 0;
	rows[ROW_SEARCH].count = 0;
	rows[ROW_SEARCH].scroll_x = 0;
	search_state = JOB_IDLE;
	search_wait_frames = 0;
	keyboard_focus = 0;
	search_on_results = 0;
	search_return_row = focus_row;
	focus_row = ROW_SEARCH;
	focus_col = 0;
	title_frames = 0;
	screen = SCREEN_SEARCH;
}

/* After removing a card: stay near it, or move on if the row has emptied. */
static void settle_home_focus(void)
{
	rebuild_saved_rows();
	int count = rows[focus_row].count;
	if (count == 0) {
		focus_row = rows[ROW_MYLIST].count && focus_row == ROW_CONTINUE ? ROW_MYLIST : ROW_SITE_FIRST;
		focus_col = rows[focus_row].focus_col;
	} else if (focus_col >= count) {
		focus_col = count - 1;
	}
	title_frames = 0;
}

/* Say so once, when something is waiting for this person. */
static void announce_update(void)
{
	if (update_announced)
		return;
	if (update_state == UPDATE_AVAILABLE) {
		update_announced = 1;
		show_status("A new version of Nami is available. See Settings.");
	} else if (update_state == UPDATE_READY) {
		update_announced = 1;
		show_status("A new version is downloaded. Install it from Package Manager.");
	}
}

static void update_home(void)
{
	announce_update();
	if (menu_count > 0) {
		const struct anime_item *it = focused_item();
		int action = update_menu();
		if (action == MENU_REMOVE_CONTINUE && it) {
			char id[ITEM_ID_MAX];
			snprintf(id, sizeof(id), "%s", it->id);
			history_remove(id);
			settle_home_focus();
		} else if (action == MENU_REMOVE_MYLIST && it) {
			struct saved_show saved;
			memset(&saved, 0, sizeof(saved));
			snprintf(saved.id, sizeof(saved.id), "%s", it->id);
			mylist_toggle(&saved);
			settle_home_focus();
		}
		return;
	}
	if (confirm_exit) {
		if (input_pressed(ACT_OK))
			running = 0;
		if (input_pressed(ACT_BACK))
			confirm_exit = 0;
		return;
	}
	if (input_pressed(ACT_BACK)) {
		confirm_exit = 1;
		return;
	}
	if (input_pressed(ACT_OPTION)) {
		open_search();
		return;
	}
	if (input_pressed(ACT_MENU)) {
		settings_focus = 0;
		screen = SCREEN_SETTINGS;
		return;
	}
	const struct anime_item *focused = focused_item();
	if (input_pressed(ACT_OK) && focused) {
		open_details(focused, SCREEN_HOME);
		return;
	}
	if (input_pressed(ACT_SELECT) && focused && focus_row == ROW_CONTINUE) {
		menu_begin(focused->title, "Its watched episodes and place are forgotten too.");
		menu_add("Remove from Continue watching", MENU_REMOVE_CONTINUE);
		menu_add("Cancel", MENU_CANCEL);
		return;
	}
	if (input_pressed(ACT_SELECT) && focused && focus_row == ROW_MYLIST) {
		menu_begin(focused->title, "");
		menu_add("Remove from My List", MENU_REMOVE_MYLIST);
		menu_add("Cancel", MENU_CANCEL);
		return;
	}
	int fr = focus_row;
	int before_row = focus_row, before_col = focus_col;
	if (input_pressed(ACT_UP)) {
		for (int r = fr - 1; r >= 0; r--) {
			if (row_visible(r)) {
				fr = r;
				break;
			}
		}
	}
	if (input_pressed(ACT_DOWN)) {
		for (int r = fr + 1; r < ROW_COUNT; r++) {
			if (row_visible(r)) {
				fr = r;
				break;
			}
		}
	}
	if (fr != focus_row) {
		rows[focus_row].focus_col = focus_col;
		focus_row = fr;
		focus_col = rows[fr].focus_col;
	}
	int count = rows[focus_row].count;
	if (input_pressed(ACT_LEFT) && focus_col > 0)
		focus_col--;
	if (input_pressed(ACT_RIGHT) && focus_col + 1 < count)
		focus_col++;
	if (focus_row != before_row || focus_col != before_col)
		title_frames = 0;
	else
		title_frames++;
}

/* ---- first-run install ---- */

static void install_progress(int *done, int *total, int *rows_ready)
{
	*done = *total = 0;
	*rows_ready = 1;
	for (int r = ROW_SITE_FIRST; r < ROW_SEARCH; r++) {
		if (rows[r].state == ROW_LOADING)
			*rows_ready = 0;
		for (int c = 0; c < rows[r].count; c++) {
			int state = rows[r].items[c].poster_state;
			(*total)++;
			*done += state == POSTER_READY || state == POSTER_FAILED;
		}
	}
}

static void finish_install(void)
{
	settings.installed = 1;
	settings_save();
	screen = SCREEN_HOME;
}

static void update_install(void)
{
	int done, total, rows_ready;
	install_progress(&done, &total, &rows_ready);
	if (input_pressed(ACT_OK) || (rows_ready && total > 0 && done == total))
		finish_install();
}

static void draw_install(void)
{
	int done, total, rows_ready;
	char line[64];
	install_progress(&done, &total, &rows_ready);

	stage("install");
	gfx_begin_frame(COLOR_BACKGROUND);
	gfx_hgradient(SCREEN_W * 0.55f, 0, SCREEN_W * 0.45f, 200, 0x8B7CF600, 0x8B7CF638);
	gfx_text("Nami", MARGIN_X, 150, TEXT_HEAD, COLOR_TEXT);
	gfx_rect(MARGIN_X, 186, 34, 2.5f, COLOR_ACCENT);
	gfx_text("Setting up the Home screen", MARGIN_X, 214, TEXT_TITLE, COLOR_TEXT);
	gfx_text("This only happens once. Posters are saved to the hard drive.", MARGIN_X, 244, TEXT_BODY, COLOR_MUTED);
	if (total > 0)
		snprintf(line, sizeof(line), "%d of %d posters", done, total);
	else
		snprintf(line, sizeof(line), "Getting the list of shows...");
	gfx_text(line, MARGIN_X, 284, TEXT_BODY, COLOR_TEXT);
	gfx_rect(MARGIN_X, 314, BAR_W, BAR_H, COLOR_SURFACE);
	if (total > 0)
		gfx_rect(MARGIN_X, 314, BAR_W * (float)done / (float)total, BAR_H, COLOR_ACCENT);
	draw_hint(input_last_device() == 2 ? "Enter Skip" : ICON_CROSS " Skip");
	stage("flip");
	end_frame();
}

/* ---- search ---- */

/* The keyboard's keys. Letters and digits type themselves; these are the others. */
static const char *const keyboard_keys[] = {
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P",
	"A", "S", "D", "F", "G", "H", "J", "K", "L", "'",
	"Z", "X", "C", "V", "B", "N", "M", "-", ":", ".",
	"Space", "Space", "Space", "Space", "Del", "Del", "Del", "Clear", "Clear", "Clear",
};
#define KEY_COUNT ((int)(sizeof(keyboard_keys) / sizeof(keyboard_keys[0])))

static void query_changed(void)
{
	search_wait_frames = SEARCH_PAUSE_FRAMES;
}

static void query_add(char c)
{
	size_t n = strlen(search_query);
	if (n + 1 < sizeof(search_query)) {
		search_query[n] = c;
		search_query[n + 1] = 0;
		query_changed();
	}
}

static void query_delete(void)
{
	size_t n = strlen(search_query);
	if (n > 0) {
		search_query[n - 1] = 0;
		query_changed();
	}
}

static void press_key(const char *key)
{
	if (strcmp(key, "Space") == 0) {
		/* Not at the start, and not two in a row. */
		size_t n = strlen(search_query);
		if (n > 0 && search_query[n - 1] != ' ')
			query_add(' ');
	} else if (strcmp(key, "Del") == 0) {
		query_delete();
	} else if (strcmp(key, "Clear") == 0) {
		search_query[0] = 0;
		rows[ROW_SEARCH].count = 0;
		search_done[0] = 0;
		search_requested[0] = 0;
		search_wait_frames = 0;
	} else {
		char c = key[0];
		query_add(c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c);
	}
}

/* Move across a key that spans two cells in one go. */
static int key_step(int focus, int delta)
{
	int next = focus + delta;
	while (next >= 0 && next < KEY_COUNT && (next / KEY_COLUMNS) == (focus / KEY_COLUMNS) &&
	       strcmp(keyboard_keys[next], keyboard_keys[focus]) == 0)
		next += delta;
	return next;
}

static void update_search_typing(void)
{
	if (search_wait_frames > 0) {
		search_wait_frames--;
		return;
	}
	/* One search at a time; a newer query follows as soon as this one is done. */
	if (search_state == JOB_REQUESTED)
		return;
	if (strlen(search_query) >= SEARCH_MIN_CHARS && strcmp(search_query, search_done) != 0 &&
	    strcmp(search_query, search_requested) != 0) {
		snprintf(search_requested, sizeof(search_requested), "%s", search_query);
		search_state = JOB_REQUESTED;
	}
}

static void update_search(void)
{
	update_search_typing();

	if (input_pressed(ACT_BACK)) {
		search_requested[0] = 0;
		focus_row = search_return_row;
		focus_col = rows[focus_row].focus_col;
		title_frames = 0;
		screen = SCREEN_HOME;
		return;
	}
	if (input_pressed(ACT_MENU)) {
		query_delete();
		return;
	}
	if (input_pressed(ACT_OPTION)) {
		press_key("Space");
		return;
	}

	int results = rows[ROW_SEARCH].count;
	if (search_on_results) {
		int before = focus_col;
		if (input_pressed(ACT_LEFT)) {
			if (focus_col > 0)
				focus_col--;
			else
				search_on_results = 0;
		}
		if (input_pressed(ACT_RIGHT) && focus_col + 1 < results)
			focus_col++;
		title_frames = focus_col == before ? title_frames + 1 : 0;
		if (results == 0)
			search_on_results = 0;
		if (input_pressed(ACT_OK) && focus_col < results)
			open_details(&rows[ROW_SEARCH].items[focus_col], SCREEN_SEARCH);
		return;
	}

	if (input_pressed(ACT_LEFT) && keyboard_focus % KEY_COLUMNS > 0) {
		int next = key_step(keyboard_focus, -1);
		if (next >= 0 && next / KEY_COLUMNS == keyboard_focus / KEY_COLUMNS)
			keyboard_focus = next;
	}
	if (input_pressed(ACT_RIGHT)) {
		int next = key_step(keyboard_focus, 1);
		if (next < KEY_COUNT && next / KEY_COLUMNS == keyboard_focus / KEY_COLUMNS) {
			keyboard_focus = next;
		} else if (results > 0) {
			search_on_results = 1;
			focus_col = 0;
			title_frames = 0;
		}
	}
	if (input_pressed(ACT_UP) && keyboard_focus >= KEY_COLUMNS)
		keyboard_focus -= KEY_COLUMNS;
	if (input_pressed(ACT_DOWN) && keyboard_focus + KEY_COLUMNS < KEY_COUNT)
		keyboard_focus += KEY_COLUMNS;
	if (input_pressed(ACT_OK))
		press_key(keyboard_keys[keyboard_focus]);
}

static void draw_keyboard(void)
{
	for (int i = 0; i < KEY_COUNT; i++) {
		/* A key spanning several cells is drawn once, from its first. */
		if (i % KEY_COLUMNS > 0 && strcmp(keyboard_keys[i], keyboard_keys[i - 1]) == 0)
			continue;
		int span = 1;
		while ((i + span) % KEY_COLUMNS != 0 && i + span < KEY_COUNT &&
		       strcmp(keyboard_keys[i + span], keyboard_keys[i]) == 0)
			span++;
		float x = MARGIN_X + (float)(i % KEY_COLUMNS) * (KEY_W + KEY_GAP);
		float y = KEYBOARD_Y + (float)(i / KEY_COLUMNS) * (KEY_H + KEY_GAP);
		float w = KEY_W * (float)span + KEY_GAP * (float)(span - 1);
		int on = !search_on_results && strcmp(keyboard_keys[keyboard_focus], keyboard_keys[i]) == 0 &&
		         keyboard_focus / KEY_COLUMNS == i / KEY_COLUMNS;
		gfx_rect(x, y, w, KEY_H, on ? COLOR_ACCENT : COLOR_SURFACE);
		if (on)
			gfx_outline(x, y, w, KEY_H, 2.0f, COLOR_TEXT);
		const char *label = keyboard_keys[i];
		gfx_text(label, x + (w - gfx_text_width(label, TEXT_BODY)) / 2, y + 6, TEXT_BODY, COLOR_TEXT);
	}
}

static void draw_search(void)
{
	char line[QUERY_MAX + 8];
	stage("search");
	gfx_begin_frame(COLOR_BACKGROUND);
	if (search_on_results)
		draw_backdrop(focused_item());
	gfx_rect(0, 0, SCREEN_W, SCREEN_H, BACKDROP_DIM);
	gfx_vgradient(0, 0, SCREEN_W, SCREEN_H, 0x0E0E1240, 0x0E0E12E0);

	gfx_text("Search", MARGIN_X, 18, TEXT_HEAD, COLOR_TEXT);
	gfx_rect(MARGIN_X, 56, 34, 2.5f, COLOR_ACCENT);

	/* What's been typed, with a cursor. */
	snprintf(line, sizeof(line), "%s_", search_query);
	gfx_rect(MARGIN_X, 66, KEY_W * KEY_COLUMNS + KEY_GAP * (KEY_COLUMNS - 1), 30, COLOR_SURFACE);
	gfx_text(line, MARGIN_X + 10, 71, TEXT_BODY, COLOR_TEXT);

	draw_keyboard();

	gfx_clip(RESULTS_X - 8, 0, SCREEN_W - RESULTS_X + 8, SCREEN_H);
	if (rows[ROW_SEARCH].count > 0) {
		draw_row_cards(ROW_SEARCH, RESULTS_X, 70, search_on_results);
	} else if (search_state == JOB_REQUESTED || search_wait_frames > 0) {
		gfx_text("Searching...", RESULTS_X, 76, TEXT_BODY, COLOR_MUTED);
	} else if (search_state == JOB_FAILED) {
		gfx_text("The search didn't work.", RESULTS_X, 76, TEXT_BODY, COLOR_MUTED);
	} else if (strlen(search_query) >= SEARCH_MIN_CHARS && search_done[0]) {
		gfx_text("No shows found.", RESULTS_X, 76, TEXT_BODY, COLOR_MUTED);
	} else {
		gfx_text("Results appear as you type.", RESULTS_X, 76, TEXT_BODY, COLOR_MUTED);
	}
	gfx_clip_off();

	draw_hint(input_last_device() == 2
	              ? "Enter Type    " ICON_SQUARE " Delete    " ICON_TRIANGLE " Space    Return Back"
	              : ICON_CROSS " Select    " ICON_SQUARE " Delete    " ICON_TRIANGLE " Space    " ICON_CIRCLE " Back");
	stage("flip");
	end_frame();
}

/* ---- details ---- */

static int details_has_dub(void)
{
	for (int i = 0; i < episode_count; i++) {
		if (episodes[i].has_dub)
			return 1;
	}
	return 0;
}

static struct saved_show details_saved(void)
{
	struct saved_show s;
	memset(&s, 0, sizeof(s));
	snprintf(s.id, sizeof(s.id), "%s", details_show.id);
	snprintf(s.title, sizeof(s.title), "%s", details.title[0] ? details.title : details_show.title);
	snprintf(s.poster_url, sizeof(s.poster_url), "%s", details_show.poster_url);
	snprintf(s.watch_url, sizeof(s.watch_url), "%s", details_show.watch_url);
	return s;
}

/* Which episode Play means, and where in it. */
static int play_target(float *seconds)
{
	const struct history_entry *h = history_find(details_show.id);
	*seconds = 0;
	if (!h || h->episode_index >= episode_count)
		return 0;
	if (settings.play_first_unwatched) {
		int first = 0;
		while (first + 1 < episode_count && history_is_watched(h, first))
			first++;
		/* Only the episode in progress has a place to resume from. */
		if (first == h->episode_index && !history_entry_finished(h) && !history_is_watched(h, first) &&
		    h->position > MIN_POSITION_TO_RESUME)
			*seconds = h->position - RESUME_BACKUP_SECONDS;
		return first;
	}
	if (!history_entry_finished(h) && !history_is_watched(h, h->episode_index)) {
		if (h->position > MIN_POSITION_TO_RESUME)
			*seconds = h->position - RESUME_BACKUP_SECONDS;
		return h->episode_index;
	}
	return h->episode_index + 1 < episode_count ? h->episode_index + 1 : h->episode_index;
}

static void start_episode(int index, float seconds)
{
	struct resolve_options options;
	options.target_height = settings.quality;
	options.want_dub = detail_want_dub;
	options.allow_too_tall = settings.experimental_1080;
	player_episode = index;
	player_ended = 0;
	player_countdown = 0;
	player_seek_frames = 0;
	player_overlay_frames = OVERLAY_FRAMES;
	player_history_frames = 0;
	player_save_frames = 0;
	/* Subtitles belong with the Japanese audio. */
	player_subtitles_on = settings.subtitles && !detail_want_dub;
	video_start(episodes[index].server_ids, &options, seconds);
	screen = SCREEN_PLAYER;
}

static void draw_season_list(void)
{
	int shown = season_count < SEASON_ROWS_SHOWN ? season_count : SEASON_ROWS_SHOWN;
	float h = (float)shown * SEASON_ROW_H + 60;
	float x = (SCREEN_W - SEASON_PANEL_W) / 2, y = (SCREEN_H - h) / 2;
	int first = season_focus - shown / 2;
	if (first > season_count - shown)
		first = season_count - shown;
	if (first < 0)
		first = 0;

	gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0x00000090);
	gfx_rect(x, y, SEASON_PANEL_W, h, COLOR_SURFACE);
	gfx_outline(x, y, SEASON_PANEL_W, h, 1.5f, COLOR_ACCENT);
	gfx_text("Seasons", x + 18, y + 14, TEXT_TITLE, COLOR_TEXT);
	for (int i = 0; i < shown; i++) {
		int s = first + i;
		float ry = y + 50 + (float)i * SEASON_ROW_H;
		if (s == season_focus)
			gfx_rect(x + 8, ry, SEASON_PANEL_W - 16, SEASON_ROW_H - 4, COLOR_ACCENT);
		gfx_text(seasons[s].name, x + 18, ry + 5, TEXT_BODY, COLOR_TEXT);
		if (seasons[s].current) {
			const char *now = "Now showing";
			gfx_text(now, x + SEASON_PANEL_W - 18 - gfx_text_width(now, TEXT_SMALL), ry + 7, TEXT_SMALL,
			         s == season_focus ? COLOR_TEXT : COLOR_ACCENT);
		}
	}
}

static void draw_details(void)
{
	float x = MARGIN_X, y = 40;

	stage("details");
	gfx_begin_frame(COLOR_BACKGROUND);
	draw_backdrop(&details_show);
	gfx_hgradient(0, 0, SCREEN_W, SCREEN_H, 0x0E0E12F4, 0x0E0E12A0);

	if (details_state != JOB_READY) {
		gfx_text_wrapped(details_show.title, x, y, DETAIL_LEFT_W, 2, TEXT_HEAD, COLOR_TEXT);
		gfx_text(details_state == JOB_FAILED ? "Couldn't load this show." : "Loading...", x, y + 90, TEXT_BODY, COLOR_MUTED);
		draw_hint(input_last_device() == 2 ? "Return Back" : ICON_CIRCLE " Back");
		stage("flip");
		end_frame();
		return;
	}

	/* Left: about the show, and Play. */
	gfx_text_wrapped(details.title, x, y, DETAIL_LEFT_W, 2, TEXT_HEAD, COLOR_TEXT);
	y += gfx_line_height(TEXT_HEAD) * (gfx_text_width(details.title, TEXT_HEAD) > DETAIL_LEFT_W ? 2 : 1) + 8;

	char meta[DETAIL_TEXT_MAX * 4 + 16] = "";
	const char *parts[] = {details.type, details.rating, details.status, details.aired};
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		if (parts[i][0]) {
			if (meta[0])
				strcat(meta, "  -  ");
			strcat(meta, parts[i]);
		}
	}
	gfx_text_wrapped(meta, x, y, DETAIL_LEFT_W, 2, TEXT_SMALL, COLOR_MUTED);
	y += gfx_line_height(TEXT_SMALL) * (gfx_text_width(meta, TEXT_SMALL) > DETAIL_LEFT_W ? 2 : 1) + 6;
	if (details.genres[0]) {
		gfx_text_wrapped(details.genres, x, y, DETAIL_LEFT_W, 2, TEXT_SMALL, COLOR_ACCENT);
		y += gfx_line_height(TEXT_SMALL) * (gfx_text_width(details.genres, TEXT_SMALL) > DETAIL_LEFT_W ? 2 : 1) + 8;
	}
	gfx_text_wrapped(details.synopsis, x, y, DETAIL_LEFT_W, DETAIL_SYNOPSIS_LINES, TEXT_SMALL, COLOR_TEXT);

	float seconds;
	int target = play_target(&seconds);
	char label[64];
	if (episode_count == 0) {
		snprintf(label, sizeof(label), "No episodes yet");
	} else if (seconds > 0) {
		char when[16];
		format_time(seconds, when, sizeof(when));
		snprintf(label, sizeof(label), "Resume episode %s at %s", episodes[target].number, when);
	} else {
		snprintf(label, sizeof(label), "Play episode %s", episodes[target].number);
	}
	float button_y = DETAIL_BUTTONS_TOP;
	char audio_label[48];
	snprintf(audio_label, sizeof(audio_label), "Audio: %s", detail_want_dub ? "English dub" : "Japanese + subtitles");
	char seasons_label[48] = "";
	const char *current_season = NULL;
	for (int i = 0; i < season_count; i++) {
		if (seasons[i].current)
			current_season = seasons[i].name;
	}
	if (season_count > 1)
		snprintf(seasons_label, sizeof(seasons_label), "Seasons (%d)%s%.24s", season_count,
		         current_season ? ": " : "", current_season ? current_season : "");
	const struct { const char *text; int id; } buttons[] = {
		{label, DETAIL_PLAY},
		{audio_label, DETAIL_AUDIO},
		{mylist_contains(details_show.id) ? "In My List (remove)" : "Add to My List", DETAIL_MYLIST},
		{seasons_label, DETAIL_SEASONS},
	};
	size_t button_count = season_count > 1 ? 4 : 3;
	for (size_t b = 0; b < button_count; b++) {
		int on = detail_focus == buttons[b].id;
		float w = gfx_text_width(buttons[b].text, TEXT_BODY) + 36;
		if (w < DETAIL_BUTTON_W)
			w = DETAIL_BUTTON_W;
		gfx_rect(x, button_y, w, DETAIL_BUTTON_H, on ? COLOR_ACCENT : COLOR_SURFACE);
		if (on)
			gfx_outline(x, button_y, w, DETAIL_BUTTON_H, 2.0f, COLOR_TEXT);
		gfx_text(buttons[b].text, x + 18, button_y + 5, TEXT_BODY, COLOR_TEXT);
		button_y += DETAIL_BUTTON_H + DETAIL_BUTTON_GAP;
	}

	char line[96];
	/* Right: the episodes. */
	float grid_x = DETAIL_GRID_X, grid_y = DETAIL_GRID_Y;
	snprintf(line, sizeof(line), "Episodes (%d)", episode_count);
	gfx_text(line, grid_x, 40, TEXT_TITLE, COLOR_TEXT);

	const struct history_entry *h = history_find(details_show.id);

	detail_scroll += (detail_scroll_target - detail_scroll) * SCROLL_EASE;
	gfx_clip(grid_x - 6, grid_y - 6, DETAIL_GRID_W + 12, SCREEN_H - grid_y - DETAIL_GRID_BOTTOM);
	for (int i = 0; i < episode_count; i++) {
		int row = i / DETAIL_COLUMNS, col = i % DETAIL_COLUMNS;
		float cx = grid_x + col * (DETAIL_CELL_W + DETAIL_CELL_GAP);
		float cy = grid_y + row * (DETAIL_CELL_H + DETAIL_CELL_GAP) - detail_scroll;
		if (cy > SCREEN_H || cy + DETAIL_CELL_H < 0)
			continue;
		int focused = i == detail_focus;
		gfx_rect(cx, cy, DETAIL_CELL_W, DETAIL_CELL_H, focused ? COLOR_ACCENT : COLOR_SURFACE);
		/* A full bar for a finished episode, and a part-way one for the episode in progress. */
		float bar = 0;
		if (h && history_is_watched(h, i))
			bar = 1;
		else if (h && i == h->episode_index && h->duration > 0 && !history_entry_finished(h))
			bar = h->position / h->duration;
		if (bar > 0)
			gfx_rect(cx, cy + DETAIL_CELL_H - WATCHED_BAR_H, DETAIL_CELL_W * bar, WATCHED_BAR_H, focused ? COLOR_TEXT : COLOR_ACCENT);
		if (focused)
			gfx_outline(cx, cy, DETAIL_CELL_W, DETAIL_CELL_H, 2.0f, COLOR_TEXT);
		const char *num = episodes[i].number;
		gfx_text(num, cx + (DETAIL_CELL_W - gfx_text_width(num, TEXT_BODY)) / 2, cy + 6, TEXT_BODY, COLOR_TEXT);
	}
	gfx_clip_off();

	draw_status();
	if (seasons_open)
		draw_season_list();
	if (detail_focus >= 0)
		draw_hint(input_last_device() == 2 ? "Enter Play    " ICON_TRIANGLE " Options    Return Back"
		                                   : ICON_CROSS " Play    " ICON_TRIANGLE " Options    " ICON_CIRCLE " Back");
	else
		draw_hint(input_last_device() == 2 ? "Enter Select    Return Back" : ICON_CROSS " Select    " ICON_CIRCLE " Back");
	draw_menu();
	stage("flip");
	end_frame();
}

static void open_episode_menu(int index)
{
	char text[MENU_TEXT_MAX], title[48];
	const struct history_entry *h = history_find(details_show.id);
	int watched = h && history_is_watched(h, index);
	snprintf(title, sizeof(title), "Episode %s", episodes[index].number);
	menu_begin(title, "");
	menu_episode = index;
	menu_add(watched ? "Mark as not watched" : "Mark as watched", MENU_TOGGLE_WATCHED);
	if (index > 0) {
		snprintf(text, sizeof(text), "Mark episodes %s to %s as watched", episodes[0].number, episodes[index].number);
		menu_add(text, MENU_WATCHED_UP_TO);
	}
	if (h)
		menu_add("Mark all as not watched", MENU_ALL_UNWATCHED);
	menu_add("Cancel", MENU_CANCEL);
}

static void apply_episode_menu(int action)
{
	if (action != MENU_TOGGLE_WATCHED && action != MENU_WATCHED_UP_TO && action != MENU_ALL_UNWATCHED)
		return;
	struct saved_show saved = details_saved();
	struct history_entry *e = history_ensure(&saved);
	if (!e->episode[0]) {
		/* New to the history: its place is the episode being marked. */
		e->episode_index = menu_episode;
		snprintf(e->episode, sizeof(e->episode), "%s", episodes[menu_episode].number);
	}
	if (action == MENU_TOGGLE_WATCHED) {
		if (history_is_watched(e, menu_episode)) {
			history_clear_watched(e, menu_episode);
			/* Otherwise the old position would still count it as finished. */
			if (e->episode_index == menu_episode)
				e->position = 0;
		} else {
			history_set_watched(e, menu_episode);
		}
	} else if (action == MENU_WATCHED_UP_TO) {
		for (int i = 0; i <= menu_episode; i++)
			history_set_watched(e, i);
		/* Carry on from here, unless already further along. */
		if (menu_episode >= e->episode_index) {
			e->episode_index = menu_episode;
			snprintf(e->episode, sizeof(e->episode), "%s", episodes[menu_episode].number);
		}
	} else {
		memset(e->watched, 0, sizeof(e->watched));
		e->position = 0;
	}
	history_save();
}

static void update_season_list(void)
{
	if (input_pressed(ACT_BACK)) {
		seasons_open = 0;
		return;
	}
	if (input_pressed(ACT_UP) && season_focus > 0)
		season_focus--;
	if (input_pressed(ACT_DOWN) && season_focus + 1 < season_count)
		season_focus++;
	if (input_pressed(ACT_OK)) {
		if (seasons[season_focus].current)
			seasons_open = 0;
		else
			open_season(&seasons[season_focus]);
	}
}

static void update_details(void)
{
	if (seasons_open) {
		update_season_list();
		return;
	}
	if (menu_count > 0) {
		int action = update_menu();
		if (action >= 0)
			apply_episode_menu(action);
		return;
	}
	if (input_pressed(ACT_BACK)) {
		if (details_state == JOB_REQUESTED)
			return;
		details_state = JOB_IDLE;
		if (details_from == SCREEN_HOME)
			rebuild_saved_rows();
		screen = details_from;
		return;
	}
	if (details_state != JOB_READY)
		return;

	int focus = detail_focus;
	if (focus >= 0 && input_pressed(ACT_OPTION)) {
		open_episode_menu(focus);
		return;
	}
	if (focus < 0) {
		if (input_pressed(ACT_UP) && focus < DETAIL_PLAY)
			focus++;
		int lowest = season_count > 1 ? DETAIL_SEASONS : DETAIL_MYLIST;
		if (input_pressed(ACT_DOWN) && focus > lowest)
			focus--;
		if (input_pressed(ACT_OK) && focus == DETAIL_SEASONS) {
			seasons_open = 1;
			season_focus = 0;
			for (int i = 0; i < season_count; i++) {
				if (seasons[i].current)
					season_focus = i;
			}
			return;
		}
		if (input_pressed(ACT_RIGHT) && episode_count > 0)
			focus = 0;
		if (input_pressed(ACT_OK) && focus == DETAIL_AUDIO) {
			if (detail_want_dub || details_has_dub())
				detail_want_dub = !detail_want_dub;
			else
				show_status("This show has no dub.");
			return;
		}
		if (input_pressed(ACT_OK) && focus == DETAIL_MYLIST) {
			struct saved_show saved = details_saved();
			show_status(mylist_toggle(&saved) ? "Added to My List" : "Removed from My List");
			return;
		}
	} else {
		if (input_pressed(ACT_LEFT)) {
			focus = focus % DETAIL_COLUMNS == 0 ? DETAIL_PLAY : focus - 1;
		} else if (input_pressed(ACT_RIGHT)) {
			if (focus + 1 < episode_count)
				focus++;
		} else if (input_pressed(ACT_UP)) {
			if (focus >= DETAIL_COLUMNS)
				focus -= DETAIL_COLUMNS;
		} else if (input_pressed(ACT_DOWN)) {
			if (focus + DETAIL_COLUMNS < episode_count)
				focus += DETAIL_COLUMNS;
			else if (focus / DETAIL_COLUMNS < (episode_count - 1) / DETAIL_COLUMNS)
				focus = episode_count - 1;
		}
	}
	detail_focus = focus;
	if (focus >= 0) {
		float cell_y = (float)(focus / DETAIL_COLUMNS) * (DETAIL_CELL_H + DETAIL_CELL_GAP);
		float visible = SCREEN_H - DETAIL_GRID_Y - DETAIL_GRID_BOTTOM - DETAIL_CELL_H - 12;
		if (cell_y < detail_scroll_target)
			detail_scroll_target = cell_y;
		else if (cell_y > detail_scroll_target + visible)
			detail_scroll_target = cell_y - visible;
	}
	int on_episode_or_play = focus >= 0 || focus == DETAIL_PLAY;
	if (((input_pressed(ACT_OK) && on_episode_or_play) || input_pressed(ACT_PLAY)) && episode_count > 0) {
		float seconds = 0;
		int index = focus < 0 ? play_target(&seconds) : focus;
		if (focus >= 0) {
			/* Choosing the episode that's in progress resumes it too. */
			const struct history_entry *h = history_find(details_show.id);
			if (h && h->episode_index == focus && !history_entry_finished(h) && !history_is_watched(h, focus) &&
			    h->position > MIN_POSITION_TO_RESUME)
				seconds = h->position - RESUME_BACKUP_SECONDS;
		}
		start_episode(index, seconds);
	}
}

/* ---- player ---- */

static void player_record(int to_disk)
{
	float position = video_position(), length = video_duration();
	if (length <= 0 || position < MIN_POSITION_TO_RESUME)
		return;
	struct saved_show s = details_saved();
	history_update(&s, player_episode, episodes[player_episode].number, position, length);
	if (history_entry_finished(&history[0]))
		history_set_watched(&history[0], player_episode);
	if (to_disk)
		history_save();
}

static void player_leave(void)
{
	if (video_started())
		player_record(1);
	video_stop();
	screen = SCREEN_DETAILS;
}

static void player_change_episode(int index)
{
	if (index < 0 || index >= episode_count)
		return;
	player_record(1);
	start_episode(index, 0);
}

static void player_nudge(float seconds)
{
	if (player_seek_frames == 0)
		player_seek_target = video_position();
	player_seek_target += seconds;
	if (player_seek_target < 0)
		player_seek_target = 0;
	float length = video_duration();
	if (length > 0 && player_seek_target > length)
		player_seek_target = length;
	player_seek_frames = SEEK_SETTLE_FRAMES;
	player_overlay_frames = OVERLAY_FRAMES;
}

/* Whether the intro is playing and can be skipped. */
static int player_in_intro(float *end)
{
	float start;
	if (!video_started() || !video_intro(&start, end))
		return 0;
	float now = video_position();
	return now >= start && now < *end;
}

static void update_player(void)
{
	if (player_ended) {
		int has_next = !video_failure() && player_episode + 1 < episode_count;
		if (input_pressed(ACT_BACK) || input_pressed(ACT_STOP) || (input_pressed(ACT_OK) && !has_next)) {
			video_stop();
			screen = SCREEN_DETAILS;
		} else if (has_next && (input_pressed(ACT_OK) || (player_countdown > 0 && --player_countdown == 0))) {
			player_change_episode(player_episode + 1);
		}
		return;
	}
	if (input_pressed(ACT_BACK) || input_pressed(ACT_STOP)) {
		player_leave();
		return;
	}
	if (!video_started() && !video_seeking())
		return;

	if (input_pressed(ACT_OK) || input_pressed(ACT_PLAY) || input_pressed(ACT_PAUSE)) {
		video_toggle_pause();
		player_overlay_frames = OVERLAY_FRAMES;
	}
	if (input_pressed(ACT_LEFT) || input_pressed(ACT_REWIND))
		player_nudge(-SEEK_SHORT);
	if (input_pressed(ACT_RIGHT) || input_pressed(ACT_FORWARD))
		player_nudge(SEEK_SHORT);
	if (input_pressed(ACT_DOWN))
		player_nudge(-SEEK_LONG);
	if (input_pressed(ACT_UP))
		player_nudge(SEEK_LONG);
	if (input_pressed(ACT_NEXT)) {
		player_change_episode(player_episode + 1);
		return;
	}
	if (input_pressed(ACT_PREV)) {
		player_change_episode(player_episode - 1);
		return;
	}
	if (input_pressed(ACT_OPTION) || input_pressed(ACT_SUBTITLE)) {
		player_subtitles_on = !player_subtitles_on;
		show_status(player_subtitles_on ? "Subtitles on" : "Subtitles off");
	}
	float intro_end;
	if (input_pressed(ACT_MENU) && player_in_intro(&intro_end)) {
		player_seek_target = intro_end;
		player_seek_frames = 1;
	}

	if (player_seek_frames > 0 && --player_seek_frames == 0) {
		seek_shown_target = player_seek_target;
		video_seek(player_seek_target);
	}
	if (video_seeking())
		return;

	if (++player_history_frames >= HISTORY_UPDATE_FRAMES) {
		player_history_frames = 0;
		player_save_frames += HISTORY_UPDATE_FRAMES;
		int save = player_save_frames >= HISTORY_SAVE_FRAMES;
		if (save)
			player_save_frames = 0;
		player_record(save);
	}
}

static void draw_subtitle(void)
{
	const char *text = player_subtitles_on ? video_subtitle() : NULL;
	if (!text)
		return;

	/* Count lines, to stack them up from the bottom. */
	int lines = 1;
	for (const char *p = text; *p; p++)
		lines += *p == '\n';
	float line_h = gfx_line_height(TEXT_TITLE);
	float y = SCREEN_H - SUBTITLE_BOTTOM - lines * line_h;
	const char *p = text;
	while (*p) {
		char line[SUBTITLE_LINE_MAX];
		size_t n = strcspn(p, "\n");
		size_t copy = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
		memcpy(line, p, copy);
		line[copy] = 0;
		float w = gfx_text_width(line, TEXT_TITLE);
		enum text_size size = TEXT_TITLE;
		if (w > SCREEN_W - 2 * MARGIN_X) {
			size = TEXT_BODY;
			w = gfx_text_width(line, size);
		}
		float x = (SCREEN_W - w) / 2;
		gfx_rect(x - SUBTITLE_PAD_X, y, w + 2 * SUBTITLE_PAD_X, line_h, SUBTITLE_PANEL);
		gfx_text(line, x, y + (size == TEXT_BODY ? 3 : 0), size, COLOR_TEXT);
		y += line_h;
		p += n;
		if (*p == '\n')
			p++;
	}
}

static void draw_player_overlay(void)
{
	char line[96], now_text[16], end_text[16];
	float length = video_duration();
	float now = player_seek_frames > 0 ? player_seek_target : video_seeking() ? seek_shown_target : video_position();

	gfx_vgradient(0, SCREEN_H - 130, SCREEN_W, 130, 0x00000000, 0x000000E0);
	gfx_text(details.title, MARGIN_X, SCREEN_H - 112, TEXT_TITLE, COLOR_TEXT);
	snprintf(line, sizeof(line), "Episode %s%s", episodes[player_episode].number, video_paused() ? "   -   Paused" : "");
	gfx_text(line, MARGIN_X, SCREEN_H - 86, TEXT_BODY, COLOR_MUTED);

	gfx_rect(BAR_X, SCREEN_H - 58, BAR_W, BAR_H, 0xFFFFFF40);
	if (length > 0) {
		float fraction = now / length;
		if (fraction > 1)
			fraction = 1;
		gfx_rect(BAR_X, SCREEN_H - 58, BAR_W * fraction, BAR_H, COLOR_ACCENT);
	}
	format_time(now, now_text, sizeof(now_text));
	format_time(length, end_text, sizeof(end_text));
	snprintf(line, sizeof(line), "%s / %s", now_text, end_text);
	gfx_text(line, MARGIN_X, SCREEN_H - 46, TEXT_SMALL, COLOR_TEXT);
	draw_hint(input_last_device() == 2 ? "Play Pause    Arrows Seek    Next/Prev Episode    Subtitle    Return Back"
	                                   : ICON_CROSS " Pause    D-pad Seek    L1/R1 Episode    " ICON_TRIANGLE " Subtitles    " ICON_CIRCLE " Back");
}

static void draw_player(void)
{
	stage("player");
	gfx_begin_frame(0x000000FF);
	int done = player_ended ? 1 : video_frame();
	if (done && !player_ended) {
		/* Marked now, so it counts even if the next one is never started. */
		if (!video_failure())
			player_record(1);
		player_ended = 1;
		if (!video_failure() && player_episode + 1 < episode_count && settings.autoplay)
			player_countdown = NEXT_COUNTDOWN_FRAMES;
	}

	char line[64];
	snprintf(line, sizeof(line), "Episode %s", episodes[player_episode].number);

	if (player_ended) {
		const char *reason = video_failure();
		gfx_text(details.title, MARGIN_X, 200, TEXT_TITLE, COLOR_TEXT);
		int has_next = !reason && player_episode + 1 < episode_count;
		char next[96];
		if (has_next && player_countdown > 0)
			snprintf(next, sizeof(next), "Episode %s starts in %d", episodes[player_episode + 1].number,
			         (player_countdown + 59) / 60);
		else if (has_next)
			snprintf(next, sizeof(next), "Next is episode %s", episodes[player_episode + 1].number);
		else
			snprintf(next, sizeof(next), "That's the latest episode.");
		gfx_text_wrapped(reason ? failure_text(reason) : next, MARGIN_X, 230, SCREEN_W - 2 * MARGIN_X, 2,
		                 TEXT_BODY, COLOR_MUTED);
		if (reason) {
			char code[64];
			snprintf(code, sizeof(code), "Error: %s", reason);
			gfx_text(code, MARGIN_X, 280, TEXT_SMALL, COLOR_MUTED);
		}
		if (has_next)
			draw_hint(input_last_device() == 2 ? "Enter Play now    Return Back"
			                                   : ICON_CROSS " Play now    " ICON_CIRCLE " Back");
		else
			draw_hint(input_last_device() == 2 ? "Return Back" : ICON_CIRCLE " Back");
	} else if (video_seeking()) {
		/* The last picture stays up, with the time being jumped to. */
		draw_player_overlay();
	} else if (!video_started()) {
		gfx_text(details.title, MARGIN_X, 200, TEXT_TITLE, COLOR_TEXT);
		gfx_text(line, MARGIN_X, 228, TEXT_BODY, COLOR_MUTED);
		gfx_text("Loading...", MARGIN_X, 254, TEXT_BODY, COLOR_MUTED);
		draw_hint(input_last_device() == 2 ? "Return Back" : ICON_CIRCLE " Back");
	} else {
		draw_subtitle();
		float intro_end;
		if (player_in_intro(&intro_end)) {
			const char *skip = ICON_SQUARE " Skip intro";
			float w = gfx_text_width(skip, TEXT_BODY) + 24;
			gfx_rect(SCREEN_W - MARGIN_X - w, 30, w, 32, 0x000000C0);
			gfx_outline(SCREEN_W - MARGIN_X - w, 30, w, 32, 1.5f, COLOR_ACCENT);
			gfx_text(skip, SCREEN_W - MARGIN_X - w + 12, 36, TEXT_BODY, COLOR_TEXT);
		}
		if (player_overlay_frames > 0 || video_paused()) {
			if (!video_paused())
				player_overlay_frames--;
			draw_player_overlay();
		}
		draw_status();
	}
	stage("flip");
	end_frame();
}

/* ---- settings ---- */

static void settings_row_text(int i, const char **label, char *value, size_t size)
{
	switch (i) {
	case 0:
		*label = "Playback quality";
		snprintf(value, size, "%dp", settings.quality);
		break;
	case 1:
		*label = "Subtitles";
		snprintf(value, size, "%s", settings.subtitles ? "On" : "Off");
		break;
	case 2:
		*label = "Experimental 1080p (expect corruption)";
		snprintf(value, size, "%s", settings.experimental_1080 ? "On" : "Off");
		break;
	case 3:
		*label = "Play button offers";
		snprintf(value, size, "%s", settings.play_first_unwatched ? "First unwatched" : "Latest watched");
		break;
	case 4:
		*label = "Autoplay next episode";
		snprintf(value, size, "%s", settings.autoplay ? "On" : "Off");
		break;
	case 5:
		*label = "Clear poster cache";
		snprintf(value, size, "%s", clear_cache_requested ? "Clearing..." : "");
		break;
	case 6:
		*label = "Clear watch history";
		snprintf(value, size, "%d shows", history_count);
		break;
	case 7:
		*label = "Set up Home posters again";
		snprintf(value, size, "%s", "");
		break;
	default:
		*label = "Version " APP_VERSION_NAME " - Check for updates";
		switch (update_state) {
		case UPDATE_CHECKING: snprintf(value, size, "Checking..."); break;
		case UPDATE_CURRENT: snprintf(value, size, "Up to date"); break;
		case UPDATE_AVAILABLE: snprintf(value, size, "Get %s", update_info.version_name); break;
		case UPDATE_DOWNLOADING: snprintf(value, size, "Downloading..."); break;
		case UPDATE_READY: snprintf(value, size, "Ready to install"); break;
		case UPDATE_CHECK_FAILED: snprintf(value, size, "Couldn't check"); break;
		case UPDATE_DOWNLOAD_FAILED: snprintf(value, size, "Download failed"); break;
		default: snprintf(value, size, "%s", ""); break;
		}
		break;
	}
}

static void update_settings(void)
{
	if (input_pressed(ACT_BACK)) {
		settings_save();
		rebuild_saved_rows();
		screen = SCREEN_HOME;
		return;
	}
	if (input_pressed(ACT_UP) && settings_focus > 0)
		settings_focus--;
	if (input_pressed(ACT_DOWN) && settings_focus + 1 < SETTINGS_ROWS)
		settings_focus++;
	int change = input_pressed(ACT_OK) || input_pressed(ACT_LEFT) || input_pressed(ACT_RIGHT);
	if (!change)
		return;
	switch (settings_focus) {
	case 0:
		settings.quality = settings.quality == QUALITY_DEFAULT ? QUALITY_HIGH : QUALITY_DEFAULT;
		break;
	case 1:
		settings.subtitles = !settings.subtitles;
		break;
	case 2:
		settings.experimental_1080 = !settings.experimental_1080;
		break;
	case 3:
		settings.play_first_unwatched = !settings.play_first_unwatched;
		break;
	case 4:
		*label = "Play button offers";
		snprintf(value, size, "%s", settings.play_first_unwatched ? "First unwatched" : "Latest watched");
		break;
	case 4:
		*label = "Autoplay next episode";
		snprintf(value, size, "%s", settings.autoplay ? "On" : "Off");
		break;
	case 5:
		*label = "Clear poster cache";
		snprintf(value, size, "%s", clear_cache_requested ? "Clearing..." : "");
		break;
	case 6:
		*label = "Clear watch history";
		snprintf(value, size, "%d shows", history_count);
		break;
	case 7:
		*label = "Set up Home posters again";
		snprintf(value, size, "%s", "");
		break;
	default:
		*label = "Version " APP_VERSION_NAME " - Check for updates";
		switch (update_state) {
		case UPDATE_CHECKING: snprintf(value, size, "Checking..."); break;
		case UPDATE_CURRENT: snprintf(value, size, "Up to date"); break;
		case UPDATE_AVAILABLE: snprintf(value, size, "Get %s", update_info.version_name); break;
		case UPDATE_DOWNLOADING: snprintf(value, size, "Downloading..."); break;
		case UPDATE_READY: snprintf(value, size, "Ready to install"); break;
		case UPDATE_CHECK_FAILED: snprintf(value, size, "Couldn't check"); break;
		case UPDATE_DOWNLOAD_FAILED: snprintf(value, size, "Download failed"); break;
		default: snprintf(value, size, "%s", ""); break;
		}
		break;
	}
}

static void update_settings(void)
{
	if (input_pressed(ACT_BACK)) {
		settings_save();
		rebuild_saved_rows();
		screen = SCREEN_HOME;
		return;
	}
	if (input_pressed(ACT_UP) && settings_focus > 0)
		settings_focus--;
	if (input_pressed(ACT_DOWN) && settings_focus + 1 < SETTINGS_ROWS)
		settings_focus++;
	int change = input_pressed(ACT_OK) || input_pressed(ACT_LEFT) || input_pressed(ACT_RIGHT);
	if (!change)
		return;
	switch (settings_focus) {
	case 0:
		settings.quality = settings.quality == QUALITY_DEFAULT ? QUALITY_HIGH : QUALITY_DEFAULT;
		break;
	case 1:
		settings.subtitles = !settings.subtitles;
		break;
	case 2:
		settings.experimental_1080 = !settings.experimental_1080;
		break;
	case 4:
		settings.autoplay = !settings.autoplay;
		break;
	case 5:
		if (input_pressed(ACT_OK)) {
			clear_cache_requested = 1;
			show_status("Posters will be downloaded again as needed.");
		}
		break;
	case 6:
		if (input_pressed(ACT_OK)) {
			history_clear();
			show_status("Watch history cleared.");
		}
		break;
	case 7:
		if (input_pressed(ACT_OK)) {
			settings.installed = 0;
			show_status("This runs the next time Nami starts.");
		}
		break;
	default:
		if (input_pressed(ACT_OK)) {
			if (update_state == UPDATE_AVAILABLE || update_state == UPDATE_DOWNLOAD_FAILED)
				update_download_requested = 1;
			else if (update_state != UPDATE_CHECKING && update_state != UPDATE_DOWNLOADING &&
			         update_state != UPDATE_READY)
				update_check_requested = 1;
		}
		break;
	}
	settings_save();
}

static void draw_settings(void)
{
	stage("settings");
	gfx_begin_frame(COLOR_BACKGROUND);
	gfx_hgradient(SCREEN_W * 0.55f, 0, SCREEN_W * 0.45f, 200, 0x8B7CF600, 0x8B7CF638);
	gfx_text("Settings", MARGIN_X, 22, TEXT_HEAD, COLOR_TEXT);
	gfx_rect(MARGIN_X, 60, 34, 2.5f, COLOR_ACCENT);

	float y = 88;
	for (int i = 0; i < SETTINGS_ROWS; i++, y += SETTINGS_ROW_H) {
		const char *label = "";
		char value[32];
		settings_row_text(i, &label, value, sizeof(value));
		int focused = i == settings_focus;
		gfx_rect(MARGIN_X, y, SETTINGS_W, SETTINGS_ROW_H - 6, focused ? COLOR_ACCENT : COLOR_SURFACE);
		gfx_text(label, MARGIN_X + 14, y + 6, TEXT_BODY, COLOR_TEXT);
		gfx_text(value, MARGIN_X + SETTINGS_W - 14 - gfx_text_width(value, TEXT_BODY), y + 6, TEXT_BODY, COLOR_TEXT);
	}
	if (settings_focus == 0)
		gfx_text_wrapped("480p is the default. If a show doesn't have the chosen quality, the nearest one it has at 720p or below plays.",
		                 MARGIN_X, y + 8, SETTINGS_W, 3, TEXT_SMALL, COLOR_MUTED);
	else if (settings_focus == 8 && update_state == UPDATE_READY)
		gfx_text_wrapped("The new version is downloaded but not installed. Exit Nami, then go to Package Manager > Install Package Files > Standard and install it.",
		                 MARGIN_X, y + 8, SETTINGS_W, 3, TEXT_SMALL, COLOR_MUTED);
	else if (settings_focus == 8 && update_state == UPDATE_AVAILABLE)
		gfx_text_wrapped(update_info.notes[0] ? update_info.notes : "A new version is available. Press X to download it.",
		                 MARGIN_X, y + 8, SETTINGS_W, 3, TEXT_SMALL, COLOR_MUTED);
	else if (settings_focus == 4)
		gfx_text_wrapped("When an episode ends, the next one starts after a 10 second countdown. Off waits for you to press X.",
		                 MARGIN_X, y + 8, SETTINGS_W, 3, TEXT_SMALL, COLOR_MUTED);
	else if (settings_focus == 3)
		gfx_text_wrapped("Latest watched continues from the most recent episode you played. First unwatched goes to the earliest episode you haven't finished.",
		                 MARGIN_X, y + 8, SETTINGS_W, 3, TEXT_SMALL, COLOR_MUTED);
	else if (settings_focus == 2)
		gfx_text_wrapped("Lets shows with only 1080p play, using a workaround for a limit of the PS3's video decoder. Expect garbage on screen. It can't harm the console, but it could freeze the app.",
		                 MARGIN_X, y + 8, SETTINGS_W, 3, TEXT_SMALL, COLOR_MUTED);
	draw_status();
	draw_hint(input_last_device() == 2 ? "Enter Change    Return Back" : ICON_CROSS " Change    " ICON_CIRCLE " Back");
	stage("flip");
	end_frame();
}

/* ---- main ---- */

static u64 now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (u64)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* If the main loop stalls, say where and leave for the XMB rather than need a hard reset. */
static void watchdog_thread(void *arg)
{
	(void)arg;
	u32 last_frames = main_frames;
	u64 last_change = now_ms();
	while (running) {
		usleep(200000);
		u64 now = now_ms();
		if (main_frames != last_frames) {
			last_frames = main_frames;
			last_change = now;
		}
		if (now - last_change > WATCHDOG_MS) {
			app_log("WATCHDOG main loop stuck at frame=%u stage=%s; exiting\n", last_frames, (const char *)main_stage);
			gfx_log_state();
			sysProcessExit(1);
		}
	}
	sysThreadExit(0);
}

static void on_sysutil_event(u64 status, u64 param, void *user)
{
	(void)param;
	(void)user;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

static int make_poster_textures(void)
{
	details_texture = gfx_alloc(POSTER_BYTES);
	if (!details_texture)
		return -1;
	for (int r = 0; r < ROW_COUNT; r++) {
		for (int c = 0; c < ROW_ITEMS_MAX; c++) {
			rows[r].textures[c] = gfx_alloc(POSTER_BYTES);
			if (!rows[r].textures[c]) {
				app_log("out of poster textures at row %d card %d\n", r, c);
				return -1;
			}
		}
	}
	return 0;
}

int main(void)
{
	sys_ppu_thread_t loader, watchdog;
	u64 code;

	sysLv2FsMkdir(DATA_DIR, 0777);
	sysLv2FsMkdir(CACHE_DIR, 0777);
	log_file = fopen(DATA_DIR "app_log.txt", "w");
	episodes = malloc(sizeof(struct episode) * EPISODES_MAX);
	if (!episodes)
		return 1;

	store_init(DATA_DIR);

	if (gfx_init() != 0 || make_poster_textures() != 0) {
		app_log("graphics setup failed\n");
		return 1;
	}
	input_init();
	sysUtilRegisterCallback(0, on_sysutil_event, NULL);
	rebuild_saved_rows();
	/* Start on the first row with something in it. */
	focus_row = rows[ROW_CONTINUE].count ? ROW_CONTINUE : ROW_SITE_FIRST;
	if (!settings.installed)
		screen = SCREEN_INSTALL;

	if (sysThreadCreate(&watchdog, watchdog_thread, NULL, 500, 64 * 1024, THREAD_JOINABLE, "watchdog") != 0)
		app_log("watchdog thread failed\n");
	if (sysThreadCreate(&loader, loader_thread, NULL, 1100, 512 * 1024, THREAD_JOINABLE, "loader") != 0) {
		app_log("loader thread failed\n");
		return 1;
	}

	while (running) {
		sysUtilCheckCallback();
		input_update();
		{
			switch (screen) {
			case SCREEN_INSTALL: update_install(); break;
			case SCREEN_HOME: update_home(); break;
			case SCREEN_SEARCH: update_search(); break;
			case SCREEN_DETAILS: update_details(); break;
			case SCREEN_PLAYER: update_player(); break;
			case SCREEN_SETTINGS: update_settings(); break;
			}
		}
		switch (screen) {
		case SCREEN_INSTALL: draw_install(); break;
		case SCREEN_HOME: draw_home(); break;
		case SCREEN_SEARCH: draw_search(); break;
		case SCREEN_DETAILS: draw_details(); break;
		case SCREEN_PLAYER: draw_player(); break;
		case SCREEN_SETTINGS: draw_settings(); break;
		}
		main_frames++;
	}

	if (screen == SCREEN_PLAYER && video_started())
		player_record(1);
	video_stop();
	sysThreadJoin(loader, &code);
	sysThreadJoin(watchdog, &code);
	sysUtilUnregisterCallback(0);
	return 0;
}
