#include "sites.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "http.h"

#ifdef __PPU__
#include <sys/mutex.h>
#include <sys/systime.h>
#include "config.h"
static sys_mutex_t lock;
static int lock_made;
static void lock_take(void)
{
	if (!lock_made) {
		sys_mutex_attr_t attr;
		memset(&attr, 0, sizeof(attr));
		sysMutexAttrInitialize(attr);
		sysMutexCreate(&lock, &attr);
		lock_made = 1;
	}
	sysMutexLock(lock, 0);
}
static void lock_give(void) { sysMutexUnlock(lock); }
static long long now_seconds(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return (long long)sec;
}
#else
#include <time.h>
void app_log(const char *fmt, ...);
static void lock_take(void) {}
static void lock_give(void) {}
static long long now_seconds(void) { return (long long)time(NULL); }
#endif

#define DOMAINS_PAGE "https://anikoto.site/"
#define SAVE_FILE "sites.txt"
#define REFRESH_EVERY (24 * 60 * 60)
#define RETRY_AFTER (10 * 60)
#define PATH_MAX_LEN 2560

/* Known when the app was built: Anikoto's official list (primary first), then older addresses. */
static const char *const built_in[] = {
	"https://anikototv.bz", "https://anikototv.se", "https://anikoto.me",
	"https://anikoto.net",  "https://anikoto.cz",   "https://anikototv.to",
};
/* AnimeSuge: the same shows as Anikoto, used only when no Anikoto site works. */
static const char *const suge[] = {
	"https://animesuge.re", "https://anisuge.tv",   "https://animesugez.tv",
	"https://suge.to",      "https://animesuge.cz", "https://anisuge.se",
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static char sites[SITES_MAX][SITE_URL_MAX];
static int site_count;
static char current[SITE_URL_MAX];
static char save_path[256];
static long long checked_at, last_attempt;
int sites_main_retry_seconds = RETRY_AFTER;
/* When Anikoto was last tried first while AnimeSuge was in use. */
static long long main_tried_at;

/* Site addresses are always short; the precision tells the compiler so. */
static void copy_site(char *dst, const char *src)
{
	snprintf(dst, SITE_URL_MAX, "%.*s", SITE_URL_MAX - 1, src);
}

static int contains(char list[][SITE_URL_MAX], int n, const char *site)
{
	for (int i = 0; i < n; i++)
		if (strcmp(list[i], site) == 0)
			return 1;
	return 0;
}

/* Official sites first, then built-in ones they don't include, then AnimeSuge. Call locked. */
static int merge(char official[][SITE_URL_MAX], int n, char out[][SITE_URL_MAX])
{
	int count = 0;
	for (int i = 0; i < n && count < SITES_MAX; i++)
		if (!contains(out, count, official[i]))
			copy_site(out[count++], official[i]);
	for (int i = 0; i < COUNT(built_in) && count < SITES_MAX; i++)
		if (!contains(out, count, built_in[i]))
			copy_site(out[count++], built_in[i]);
	for (int i = 0; i < COUNT(suge) && count < SITES_MAX; i++)
		if (!contains(out, count, suge[i]))
			copy_site(out[count++], suge[i]);
	return count;
}

int sites_layout_of(const char *site)
{
	return strstr(site, "suge") ? LAYOUT_SUGE : LAYOUT_ANIKOTO;
}

/* "https://host" for an https link whose host names Anikoto, else -1. Keeps a tampered domain page
 * from pointing the app at an unrelated site. */
static int normalize(const char *url, char *out, size_t size)
{
	if (strncasecmp(url, "https://", 8) != 0)
		return -1;
	const char *host = url + 8;
	size_t n = strcspn(host, "/?#:");
	char lower[SITE_URL_MAX];
	if (n == 0 || n + 9 > SITE_URL_MAX || n + 9 > size)
		return -1;
	for (size_t i = 0; i < n; i++) {
		char c = (char)tolower((unsigned char)host[i]);
		if (!(isalnum((unsigned char)c) || c == '-' || c == '.'))
			return -1;
		lower[i] = c;
	}
	lower[n] = 0;
	const char *dot = strrchr(lower, '.');
	if (!strstr(lower, "anikoto") || !dot || strlen(dot) < 3 || strcmp(lower, "anikoto.site") == 0)
		return -1;
	snprintf(out, size, "https://%.*s", SITE_URL_MAX - 9, lower);
	return 0;
}

int sites_parse_domains(const char *html, char out[][SITE_URL_MAX], int max)
{
	static const char marker[] = "class=\"domain";
	int count = 0;
	for (const char *p = strstr(html, marker); p && count < max; p = strstr(p + 1, marker)) {
		/* The card is <a class="domain ..." href="...">. */
		const char *tag_start = p;
		while (tag_start > html && *tag_start != '<')
			tag_start--;
		const char *tag_end = strchr(p, '>');
		if (strncmp(tag_start, "<a ", 3) != 0 || !tag_end)
			continue;
		const char *href = strstr(tag_start, "href=\"");
		if (!href || href > tag_end)
			continue;
		href += 6;
		char url[256];
		size_t n = strcspn(href, "\"");
		if (n >= sizeof(url))
			continue;
		memcpy(url, href, n);
		url[n] = 0;
		char site[SITE_URL_MAX];
		if (normalize(url, site, sizeof(site)) == 0 && !contains(out, count, site))
			copy_site(out[count++], site);
	}
	return count;
}

void sites_init(const char *dir)
{
	char official[SITES_MAX][SITE_URL_MAX];
	int n = 0;
	snprintf(save_path, sizeof(save_path), "%s" SAVE_FILE, dir);
	FILE *f = fopen(save_path, "r");
	if (f) {
		char line[256];
		if (fgets(line, sizeof(line), f))
			checked_at = atoll(line);
		while (n < SITES_MAX && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (normalize(line, official[n], SITE_URL_MAX) == 0)
				n++;
		}
		fclose(f);
	}
	lock_take();
	site_count = merge(official, n, sites);
	lock_give();
	app_log("sites: %d known, %d from the last check\n", site_count, n);
}

int sites_refresh(struct http_client *http, int force)
{
	long long now = now_seconds();
	if (now - last_attempt < RETRY_AFTER)
		return 0;
	if (!force && checked_at && now - checked_at < REFRESH_EVERY)
		return 0;
	last_attempt = now;

	char *body = NULL;
	size_t len;
	if (http_get(http, DOMAINS_PAGE, NULL, 0, &body, &len) != 0)
		return 0;
	char official[SITES_MAX][SITE_URL_MAX];
	int n = sites_parse_domains(body, official, SITES_MAX);
	free(body);
	app_log("sites: domain page lists %d\n", n);
	if (n == 0)
		return 0;

	checked_at = now;
	FILE *f = save_path[0] ? fopen(save_path, "w") : NULL;
	if (f) {
		fprintf(f, "%lld\n", now);
		for (int i = 0; i < n; i++)
			fprintf(f, "%s\n", official[i]);
		fclose(f);
	}
	char merged[SITES_MAX][SITE_URL_MAX];
	int count = merge(official, n, merged);
	lock_take();
	int changed = count != site_count;
	for (int i = 0; i < count && !changed; i++)
		changed = strcmp(merged[i], sites[i]) != 0;
	memcpy(sites, merged, sizeof(merged[0]) * (size_t)count);
	site_count = count;
	lock_give();
	return changed;
}

int sites_list(int layout, char out[][SITE_URL_MAX], int max)
{
	int count = 0;
	lock_take();
	/* On AnimeSuge, go back to Anikoto's order now and then, so it's used again once it's back. */
	int lead = current[0] != 0;
	if (lead && layout == LAYOUT_ANY && sites_layout_of(current) == LAYOUT_SUGE &&
	    now_seconds() - main_tried_at >= sites_main_retry_seconds) {
		main_tried_at = now_seconds();
		lead = 0;
	}
	if (lead && contains(sites, site_count, current) && (layout == LAYOUT_ANY || sites_layout_of(current) == layout))
		copy_site(out[count++], current);
	for (int i = 0; i < site_count && count < max; i++)
		if ((layout == LAYOUT_ANY || sites_layout_of(sites[i]) == layout) && !contains(out, count, sites[i]))
			copy_site(out[count++], sites[i]);
	lock_give();
	return count;
}

void sites_worked(const char *site)
{
	lock_take();
	/* Just moved to AnimeSuge: Anikoto has just failed, so wait before trying it first again. */
	if (sites_layout_of(site) == LAYOUT_SUGE && (!current[0] || sites_layout_of(current) != LAYOUT_SUGE))
		main_tried_at = now_seconds();
	copy_site(current, site);
	lock_give();
}

/* Parked, seized or replaced domains still answer 200, so check it's really the site. */
static int looks_real(const char *body, int xhr)
{
	if (xhr) {
		while (*body == ' ' || *body == '\n' || *body == '\r' || *body == '\t')
			body++;
		return *body == '{';
	}
	return strstr(body, "nikoto") || strstr(body, "NIKOTO") || strstr(body, "nimesuge") || strstr(body, "NIMESUGE");
}

int sites_get(struct http_client *http, int layout, site_path_fn path, void *ctx, int xhr, char **body,
              size_t *len, char *site_out)
{
	char tried[SITES_MAX][SITE_URL_MAX];
	int tried_count = 0;
	for (int round = 0; round < 2; round++) {
		char list[SITES_MAX][SITE_URL_MAX];
		int n = sites_list(layout, list, SITES_MAX);
		for (int i = 0; i < n; i++) {
			if (contains(tried, tried_count, list[i]))
				continue;
			copy_site(tried[tried_count++], list[i]);
			char p[PATH_MAX_LEN], url[PATH_MAX_LEN + SITE_URL_MAX], referer[SITE_URL_MAX + 2];
			if (!path(ctx, sites_layout_of(list[i]), list[i], p, sizeof(p)))
				continue;
			snprintf(url, sizeof(url), "%.*s%s", SITE_URL_MAX - 1, list[i], p);
			snprintf(referer, sizeof(referer), "%.*s/", SITE_URL_MAX - 1, list[i]);
			if (http_get(http, url, referer, xhr, body, len) != 0)
				continue;
			if (!looks_real(*body, xhr)) {
				app_log("sites: %s gave something else\n", list[i]);
				free(*body);
				*body = NULL;
				continue;
			}
			sites_worked(list[i]);
			if (site_out)
				copy_site(site_out, list[i]);
			return 0;
		}
		/* No known site worked: look for new addresses once, then try only those. */
		if (round == 0 && !sites_refresh(http, 1))
			break;
	}
	return -1;
}

int sites_home_layout(const char *app_id, const char *watch_url)
{
	if (app_id[0])
		return app_id[0] == 's' ? LAYOUT_SUGE : LAYOUT_ANIKOTO;
	char host[SITE_URL_MAX];
	const char *p = strstr(watch_url, "://");
	p = p ? p + 3 : watch_url;
	snprintf(host, sizeof(host), "%.*s", (int)strcspn(p, "/"), p);
	return sites_layout_of(host);
}

int sites_find_show(struct http_client *http, const char *site, int layout, const char *slug, const char *title,
                    char *slug_out, size_t size)
{
	char url[SITE_URL_MAX + SITE_SLUG_MAX * 3 + 64], referer[SITE_URL_MAX + 2], path[SITE_SLUG_MAX + 16];
	char *body = NULL;
	size_t len;
	snprintf(referer, sizeof(referer), "%.*s/", SITE_URL_MAX - 1, site);

	/* Older shows keep their slug on both kinds of site. */
	sites_show_path(slug, layout, path, sizeof(path));
	snprintf(url, sizeof(url), "%.*s%s", SITE_URL_MAX - 1, site, path);
	if (http_get(http, url, referer, 0, &body, &len) == 0) {
		struct anime_details *d = malloc(sizeof(*d));
		int found = d && looks_real(body, 0) && catalog_parse_details(body, layout, d) == 0 && d->id[0];
		free(d);
		free(body);
		if (found) {
			snprintf(slug_out, size, "%s", slug);
			return 0;
		}
	}

	/* Else search for the title. */
	size_t n = (size_t)snprintf(url, sizeof(url), "%.*s/filter?keyword=", SITE_URL_MAX - 1, site);
	/* Titles are kept in Latin-1; the site takes UTF-8. */
	for (const char *p = title; *p && n + 7 < sizeof(url); p++) {
		unsigned char c = (unsigned char)*p;
		if (c < 0x80 && isalnum(c))
			url[n++] = (char)c;
		else if (c < 0x80)
			n += (size_t)snprintf(url + n, 4, "%%%02X", c);
		else
			n += (size_t)snprintf(url + n, 7, "%%%02X%%%02X", 0xC0 | (c >> 6), 0x80 | (c & 0x3F));
	}
	url[n] = 0;
	if (http_get(http, url, referer, 0, &body, &len) != 0)
		return -1;
	struct anime_item *items = malloc(sizeof(*items) * ROW_ITEMS_MAX);
	int count = items ? catalog_parse_list(body, layout, items, ROW_ITEMS_MAX) : 0;
	free(body);
	int match = -1, matches = 0;
	for (int i = 0; i < count; i++) {
		if (!catalog_same_title(items[i].title, title) || (match >= 0 && strcmp(items[match].id, items[i].id) == 0))
			continue;
		match = i;
		matches++;
	}
	int ok = matches == 1 && sites_slug(items[match].watch_url, slug_out, size) == 0;
	app_log("sites: \"%s\" on %s by title: %d matches\n", title, site, matches);
	free(items);
	return ok ? 0 : -1;
}

int sites_slug(const char *url, char *out, size_t size)
{
	const char *p = strstr(url, "/watch/");
	if (!p)
		p = strstr(url, "/anime/");
	if (!p)
		return -1;
	p += 7;
	size_t n = strcspn(p, "/?#");
	if (n == 0 || n >= size)
		return -1;
	memcpy(out, p, n);
	out[n] = 0;
	return 0;
}

void sites_show_path(const char *slug, int layout, char *out, size_t size)
{
	snprintf(out, size, "%s%s", layout == LAYOUT_SUGE ? "/anime/" : "/watch/", slug);
}
