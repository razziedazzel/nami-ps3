#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_MAX_LEN 192
#define BASE_DIR_MAX 128
#define LINE_MAX_LEN (1024 + WATCHED_BYTES * 2)
#define FINISHED_FRACTION 0.92f
#define SEPARATOR '\t'

struct settings settings = {QUALITY_DEFAULT, 1, 0, 0, 0};
struct history_entry history[HISTORY_MAX];
int history_count;
struct saved_show mylist[MYLIST_MAX];
int mylist_count;

static char base_dir[BASE_DIR_MAX];
static unsigned next_sequence = 1;

static void path_for(const char *name, char *out, size_t size)
{
	snprintf(out, size, "%s%s", base_dir, name);
}

/* Write through a temporary file, so a power cut can't leave half a file behind. */
static FILE *begin_write(const char *name, char *temp, size_t size)
{
	snprintf(temp, size, "%s%s.tmp", base_dir, name);
	return fopen(temp, "w");
}

static int end_write(FILE *f, const char *temp, const char *name)
{
	char final[PATH_MAX_LEN];
	int bad = ferror(f);
	if (fclose(f) != 0 || bad)
		return -1;
	path_for(name, final, sizeof(final));
	remove(final);
	return rename(temp, final);
}

/* Split a line in place at tabs. Returns the number of fields. */
static int split(char *line, char **fields, int max)
{
	int n = 0;
	line[strcspn(line, "\r\n")] = 0;
	while (n < max) {
		fields[n++] = line;
		char *tab = strchr(line, SEPARATOR);
		if (!tab)
			break;
		*tab = 0;
		line = tab + 1;
	}
	return n;
}

static void copy_field(char *out, size_t size, const char *in)
{
	snprintf(out, size, "%s", in);
}

static void clean_copy(char *out, size_t size, const char *in)
{
	size_t n = 0;
	for (; *in && n + 1 < size; in++)
		out[n++] = (*in == SEPARATOR || *in == '\n' || *in == '\r') ? ' ' : *in;
	out[n] = 0;
}

static void load_settings(void)
{
	char path[PATH_MAX_LEN], line[LINE_MAX_LEN];
	path_for("settings.txt", path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *fields[2];
		if (split(line, fields, 2) != 2)
			continue;
		int v = atoi(fields[1]);
		if (strcmp(fields[0], "quality") == 0)
			settings.quality = v == QUALITY_HIGH ? QUALITY_HIGH : QUALITY_DEFAULT;
		else if (strcmp(fields[0], "subtitles") == 0)
			settings.subtitles = v != 0;
		else if (strcmp(fields[0], "experimental_1080") == 0)
			settings.experimental_1080 = v != 0;
		else if (strcmp(fields[0], "installed") == 0)
			settings.installed = v != 0;
		else if (strcmp(fields[0], "play_first_unwatched") == 0)
			settings.play_first_unwatched = v != 0;
	}
	fclose(f);
}

int settings_save(void)
{
	char temp[PATH_MAX_LEN];
	FILE *f = begin_write("settings.txt", temp, sizeof(temp));
	if (!f)
		return -1;
	fprintf(f, "quality\t%d\nsubtitles\t%d\nexperimental_1080\t%d\ninstalled\t%d\nplay_first_unwatched\t%d\n",
	        settings.quality, settings.subtitles, settings.experimental_1080, settings.installed,
	        settings.play_first_unwatched);
	return end_write(f, temp, "settings.txt");
}

/* As hex, leaving off trailing zero bytes to keep the file small. */
static void watched_to_hex(const struct history_entry *e, char *out)
{
	static const char digits[] = "0123456789abcdef";
	int last = WATCHED_BYTES - 1;
	while (last >= 0 && e->watched[last] == 0)
		last--;
	for (int i = 0; i <= last; i++) {
		*out++ = digits[e->watched[i] >> 4];
		*out++ = digits[e->watched[i] & 0x0F];
	}
	*out = 0;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static void watched_from_hex(struct history_entry *e, const char *in)
{
	for (int i = 0; i < WATCHED_BYTES && in[0] && in[1]; i++, in += 2) {
		int high = hex_value(in[0]), low = hex_value(in[1]);
		if (high < 0 || low < 0)
			return;
		e->watched[i] = (unsigned char)((high << 4) | low);
	}
}

static void load_history(void)
{
	char path[PATH_MAX_LEN], line[LINE_MAX_LEN];
	path_for("history.txt", path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	while (history_count < HISTORY_MAX && fgets(line, sizeof(line), f)) {
		/* Older files have no tenth field, the watched episodes. */
		char *x[10];
		int fields = split(line, x, 10);
		if (fields < 9 || !x[0][0])
			continue;
		struct history_entry *e = &history[history_count++];
		memset(e, 0, sizeof(*e));
		copy_field(e->show.id, sizeof(e->show.id), x[0]);
		copy_field(e->show.title, sizeof(e->show.title), x[1]);
		copy_field(e->show.poster_url, sizeof(e->show.poster_url), x[2]);
		copy_field(e->show.watch_url, sizeof(e->show.watch_url), x[3]);
		e->episode_index = atoi(x[4]);
		copy_field(e->episode, sizeof(e->episode), x[5]);
		e->position = (float)atof(x[6]);
		e->duration = (float)atof(x[7]);
		e->sequence = (unsigned)strtoul(x[8], NULL, 10);
		if (e->sequence >= next_sequence)
			next_sequence = e->sequence + 1;
		if (fields == 10)
			watched_from_hex(e, x[9]);
	}
	fclose(f);
}

int history_save(void)
{
	char temp[PATH_MAX_LEN];
	FILE *f = begin_write("history.txt", temp, sizeof(temp));
	if (!f)
		return -1;
	for (int i = 0; i < history_count; i++) {
		const struct history_entry *e = &history[i];
		char hex[WATCHED_BYTES * 2 + 1];
		watched_to_hex(e, hex);
		fprintf(f, "%s\t%s\t%s\t%s\t%d\t%s\t%.1f\t%.1f\t%u\t%s\n", e->show.id, e->show.title, e->show.poster_url,
		        e->show.watch_url, e->episode_index, e->episode, e->position, e->duration, e->sequence, hex);
	}
	return end_write(f, temp, "history.txt");
}

struct history_entry *history_find(const char *id)
{
	for (int i = 0; i < history_count; i++) {
		if (strcmp(history[i].show.id, id) == 0)
			return &history[i];
	}
	return NULL;
}

void history_update(const struct saved_show *show, int episode_index, const char *episode, float position,
                    float duration)
{
	struct history_entry entry;
	struct history_entry *old = history_find(show->id);
	int from = old ? (int)(old - history) : (history_count < HISTORY_MAX ? history_count++ : HISTORY_MAX - 1);

	memset(&entry, 0, sizeof(entry));
	clean_copy(entry.show.id, sizeof(entry.show.id), show->id);
	clean_copy(entry.show.title, sizeof(entry.show.title), show->title);
	clean_copy(entry.show.poster_url, sizeof(entry.show.poster_url), show->poster_url);
	clean_copy(entry.show.watch_url, sizeof(entry.show.watch_url), show->watch_url);
	entry.episode_index = episode_index;
	clean_copy(entry.episode, sizeof(entry.episode), episode);
	entry.position = position;
	entry.duration = duration;
	entry.sequence = next_sequence++;
	if (old)
		memcpy(entry.watched, old->watched, sizeof(entry.watched));

	memmove(&history[1], &history[0], sizeof(history[0]) * (size_t)from);
	history[0] = entry;
}

void history_clear(void)
{
	history_count = 0;
	history_save();
}

int history_entry_finished(const struct history_entry *e)
{
	return e->duration > 0 && e->position >= e->duration * FINISHED_FRACTION;
}

int history_is_watched(const struct history_entry *e, int episode_index)
{
	if (episode_index < 0 || episode_index >= WATCHED_EPISODES_MAX)
		return 0;
	return (e->watched[episode_index / 8] >> (episode_index % 8)) & 1;
}

void history_set_watched(struct history_entry *e, int episode_index)
{
	if (episode_index < 0 || episode_index >= WATCHED_EPISODES_MAX)
		return;
	e->watched[episode_index / 8] |= (unsigned char)(1u << (episode_index % 8));
}

void history_clear_watched(struct history_entry *e, int episode_index)
{
	if (episode_index < 0 || episode_index >= WATCHED_EPISODES_MAX)
		return;
	e->watched[episode_index / 8] &= (unsigned char)~(1u << (episode_index % 8));
}

struct history_entry *history_ensure(const struct saved_show *show)
{
	struct history_entry *e = history_find(show->id);
	if (e)
		return e;
	history_update(show, 0, "", 0, 0);
	return &history[0];
}

void history_remove(const char *id)
{
	struct history_entry *e = history_find(id);
	if (!e)
		return;
	int index = (int)(e - history);
	memmove(&history[index], &history[index + 1], sizeof(history[0]) * (size_t)(history_count - index - 1));
	history_count--;
	history_save();
}

static void load_mylist(void)
{
	char path[PATH_MAX_LEN], line[LINE_MAX_LEN];
	path_for("mylist.txt", path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	while (mylist_count < MYLIST_MAX && fgets(line, sizeof(line), f)) {
		char *x[4];
		if (split(line, x, 4) != 4 || !x[0][0])
			continue;
		struct saved_show *s = &mylist[mylist_count++];
		copy_field(s->id, sizeof(s->id), x[0]);
		copy_field(s->title, sizeof(s->title), x[1]);
		copy_field(s->poster_url, sizeof(s->poster_url), x[2]);
		copy_field(s->watch_url, sizeof(s->watch_url), x[3]);
	}
	fclose(f);
}

static int mylist_save(void)
{
	char temp[PATH_MAX_LEN];
	FILE *f = begin_write("mylist.txt", temp, sizeof(temp));
	if (!f)
		return -1;
	for (int i = 0; i < mylist_count; i++)
		fprintf(f, "%s\t%s\t%s\t%s\n", mylist[i].id, mylist[i].title, mylist[i].poster_url, mylist[i].watch_url);
	return end_write(f, temp, "mylist.txt");
}

int mylist_contains(const char *id)
{
	for (int i = 0; i < mylist_count; i++) {
		if (strcmp(mylist[i].id, id) == 0)
			return 1;
	}
	return 0;
}

int mylist_toggle(const struct saved_show *show)
{
	for (int i = 0; i < mylist_count; i++) {
		if (strcmp(mylist[i].id, show->id) == 0) {
			memmove(&mylist[i], &mylist[i + 1], sizeof(mylist[0]) * (size_t)(mylist_count - i - 1));
			mylist_count--;
			mylist_save();
			return 0;
		}
	}
	if (mylist_count == MYLIST_MAX)
		mylist_count--;
	memmove(&mylist[1], &mylist[0], sizeof(mylist[0]) * (size_t)mylist_count);
	struct saved_show clean;
	clean_copy(clean.id, sizeof(clean.id), show->id);
	clean_copy(clean.title, sizeof(clean.title), show->title);
	clean_copy(clean.poster_url, sizeof(clean.poster_url), show->poster_url);
	clean_copy(clean.watch_url, sizeof(clean.watch_url), show->watch_url);
	mylist[0] = clean;
	mylist_count++;
	mylist_save();
	return 1;
}

void store_init(const char *dir)
{
	snprintf(base_dir, sizeof(base_dir), "%s", dir);
	load_settings();
	load_history();
	load_mylist();
}
