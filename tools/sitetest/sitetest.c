/* Runs the PS3 app's site code (sites.c, catalog.c, resolve.c, unchanged) on this PC against the
 * live sites, with Anikoto working, down, or parked. Build and run with tools/sitetest/run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "http.h"
#include "resolve.h"
#include "sites.h"

static int failures;
#define CHECK(cond, ...)                                                    \
	do {                                                                    \
		if (!(cond)) {                                                      \
			failures++;                                                     \
			printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);        \
			printf(__VA_ARGS__);                                            \
			printf("\n");                                                   \
		}                                                                   \
	} while (0)

static int same_path(void *ctx, int layout, const char *site, char *path, size_t size)
{
	(void)layout;
	(void)site;
	snprintf(path, size, "%s", (const char *)ctx);
	return 1;
}

/* As main.c does it: the show's own slug on its kind of site, else looked up on the other kind. */
struct show_lookup {
	struct http_client *http;
	const char *slug, *title;
	int home;
};

static int show_page_path(void *ctx, int layout, const char *site, char *path, size_t size)
{
	struct show_lookup *l = ctx;
	char slug[SITE_SLUG_MAX];
	if (layout == l->home)
		snprintf(slug, sizeof(slug), "%s", l->slug);
	else if (sites_find_show(l->http, site, layout, l->slug, l->title, slug, sizeof(slug)) != 0)
		return 0;
	sites_show_path(slug, layout, path, size);
	return 1;
}

static int fetch(void *ctx, const char *url, const char *referer, int xhr, char **body, size_t *len)
{
	return http_get(ctx, url, referer, xhr, body, len);
}

static struct anime_item items[ROW_ITEMS_MAX];
static struct episode episodes[EPISODES_MAX];

/* Home row, a show page, its seasons and episodes, and a playable stream: what the app does. */
/* home: the kind of site the saved show (slug, title) comes from. */
static void walk(struct http_client *http, const char *want_site, int home, const char *slug, const char *title,
                 int episodes_expected, int resolve)
{
	char *body = NULL, site[SITE_URL_MAX] = "", path[256];
	size_t len;

	CHECK(sites_get(http, LAYOUT_ANY, same_path, "/filter?keyword=&sort=most-viewed&page=1", 0, &body, &len, site) == 0, "row");
	printf("  row from %s\n", site);
	CHECK(strcmp(site, want_site) == 0, "row came from %s, wanted %s", site, want_site);
	int layout = sites_layout_of(site);
	int n = body ? catalog_parse_list(body, layout, items, ROW_ITEMS_MAX) : 0;
	free(body);
	printf("  Trending: %d shows, first \"%s\" id %s subs %d dubs %d\n    poster %s\n    url %s\n", n,
	       items[0].title, items[0].id, items[0].sub_count, items[0].dub_count, items[0].poster_url, items[0].watch_url);
	CHECK(n >= 20, "%d shows", n);
	for (int i = 0; i < n; i++) {
		CHECK(items[i].title[0] && items[i].id[0] && strncmp(items[i].poster_url, "https://", 8) == 0 &&
		      (strstr(items[i].watch_url, "/watch/") || strstr(items[i].watch_url, "/anime/")) &&
		      !strstr(items[i].watch_url, "/ep-") && items[i].sub_count > 0,
		      "item %d: %s|%s|%s|%s|%d", i, items[i].id, items[i].title, items[i].poster_url, items[i].watch_url,
		      items[i].sub_count);
	}

	body = NULL;
	struct show_lookup lookup = {http, slug, title, home};
	CHECK(sites_get(http, LAYOUT_ANY, show_page_path, &lookup, 0, &body, &len, site) == 0, "show page");
	layout = sites_layout_of(site);
	struct anime_details d;
	CHECK(body && catalog_parse_details(body, layout, &d) == 0, "details parse");
	free(body);
	printf("  %s: id %s | %s | %s | %s | %s | %s\n    synopsis %.70s...\n    poster %s\n", site, d.id, d.title,
	       d.type, d.status, d.rating, d.genres, d.synopsis, d.poster_url);
	CHECK(catalog_same_title(d.title, title), "title %s", d.title);
	CHECK(d.id[0] && d.type[0] && d.status[0] && d.aired[0] && d.rating[0] && d.genres[0] && strlen(d.synopsis) > 100 &&
	      strncmp(d.poster_url, "https://", 8) == 0, "fields");

	snprintf(path, sizeof(path), "/api/seasons/%s", d.id);
	body = NULL;
	CHECK(sites_get(http, layout, same_path, path, 1, &body, &len, NULL) == 0, "seasons");
	struct season seasons[SEASONS_MAX];
	int ns = 0;
	if (body) {
		catalog_json_unescape(body);
		ns = catalog_parse_seasons(body, layout, seasons, SEASONS_MAX);
		free(body);
	}
	printf("  seasons:");
	for (int i = 0; i < ns; i++)
		printf(" [%s%s %s]", seasons[i].name, seasons[i].current ? "*" : "", strrchr(seasons[i].watch_url, '/'));
	printf("\n");
	CHECK(ns >= 2, "%d seasons", ns);

	snprintf(path, sizeof(path), "/ajax/episode/list/%s", d.id);
	body = NULL;
	CHECK(sites_get(http, layout, same_path, path, 1, &body, &len, NULL) == 0, "episodes");
	int ne = 0;
	if (body) {
		catalog_json_unescape(body);
		ne = catalog_parse_episodes(body, layout, episodes, EPISODES_MAX);
		free(body);
	}
	printf("  episodes: %d, first %s \"%s\" sub %d dub %d, last %s\n", ne, episodes[0].number, episodes[0].title,
	       episodes[0].has_sub, episodes[0].has_dub, ne ? episodes[ne - 1].number : "-");
	CHECK(ne == episodes_expected, "%d episodes", ne);
	CHECK(strcmp(episodes[0].number, "1") == 0 && episodes[0].title[0] && episodes[0].has_sub, "episode 1");

	if (!resolve || !ne)
		return;
	struct resolve_options options = {.target_height = 480, .want_dub = 0, .allow_too_tall = 1, .layout = layout};
	static struct resolved_stream out;
	const char *reason = resolve_stream(fetch, http, episodes[0].server_ids, &options, &out);
	printf("  stream: %s server \"%s\" %dx%d\n    %.100s\n    subs %.80s\n    log %s\n", reason ? reason : "OK",
	       out.server, out.width, out.height, out.media_url, out.subtitle_url, out.log);
	CHECK(!reason && strstr(out.media_url, ".m3u8") && out.server[0], "resolve");
	CHECK(strstr(out.log, "site=") && strstr(out.log, layout == LAYOUT_SUGE ? "suge" : "anikoto"),
	      "ids went to the wrong kind of site");
}

int main(void)
{
	struct http_client *http = http_client_new();
	static const char all_anikoto[] = "anikototv.bz anikototv.se anikoto.me anikoto.net anikoto.cz anikototv.to";
	const char *dir = getenv("SITETEST_DIR");

	printf("== Anikoto's domain page\n");
	sites_init(dir);
	CHECK(sites_refresh(http, 1) == 0, "the built-in list already matches the page");
	char list[SITES_MAX][SITE_URL_MAX];
	int n = sites_list(LAYOUT_ANY, list, SITES_MAX);
	printf("  order:");
	for (int i = 0; i < n; i++)
		printf(" %s", list[i] + 8);
	printf("\n");
	CHECK(n == 12 && strcmp(list[0], "https://anikototv.bz") == 0 && strcmp(list[6], "https://animesuge.re") == 0, "order");
	char parsed[SITES_MAX][SITE_URL_MAX];
	CHECK(sites_parse_domains("<a class=\"domain primary\" href=\"https://AniKoto.new/\">x</a>"
	                          "<a class=\"domain\" href=\"https://evil.example/anikoto\">y</a>"
	                          "<a class=\"domain\" href=\"http://anikoto.me\">z</a>",
	                          parsed, SITES_MAX) == 1 &&
	          strcmp(parsed[0], "https://anikoto.new") == 0, "only https Anikoto hosts are taken");

	printf("== Anikoto working\n");
	walk(http, "https://anikototv.bz", LAYOUT_ANIKOTO, "frieren-beyond-journey-s-end-c6fbj", "Frieren: Beyond Journey's End", 28, 1);

	printf("== anikototv.bz parked: next Anikoto mirror\n");
	setenv("SITETEST_PARKED", "anikototv.bz", 1);
	sites_worked("https://anikototv.bz");
	walk(http, "https://anikototv.se", LAYOUT_ANIKOTO, "frieren-beyond-journey-s-end-c6fbj", "Frieren: Beyond Journey's End", 28, 0);
	unsetenv("SITETEST_PARKED");

	printf("== every Anikoto site down: AnimeSuge\n");
	setenv("SITETEST_DOWN", all_anikoto, 1);
	walk(http, "https://animesuge.re", LAYOUT_ANIKOTO, "frieren-beyond-journey-s-end-c6fbj", "Frieren: Beyond Journey's End", 28, 1);
	printf("== a newer show, numbered differently on AnimeSuge\n");
	walk(http, "https://animesuge.re", LAYOUT_ANIKOTO, "dorohedoro-season-2-bqfe6", "Dorohedoro Season 2", 11, 1);
	printf("== a saved Anikoto show whose slug is different on AnimeSuge: found by title\n");
	walk(http, "https://animesuge.re", LAYOUT_ANIKOTO, "frieren-beyond-journey-s-end-season-2-cp7ym",
	     "Frieren: Beyond Journey's End Season 2", 10, 1);
	printf("== Anikoto is back: used again once the retry time has passed\n");
	unsetenv("SITETEST_DOWN");
	{
		char first[SITES_MAX][SITE_URL_MAX];
		sites_list(LAYOUT_ANY, first, SITES_MAX);
		CHECK(strcmp(first[0], "https://animesuge.re") == 0, "stays on AnimeSuge within 10 minutes: %s", first[0]);
	}
	sites_main_retry_seconds = 0;
	walk(http, "https://anikototv.bz", LAYOUT_SUGE, "frieren-beyond-journey-s-end-season-2-3yvr5",
	     "Frieren: Beyond Journey's End Season 2", 10, 0);
	sites_main_retry_seconds = 600;
	printf("== a show the other site doesn't have isn't guessed\n");
	setenv("SITETEST_DOWN", all_anikoto, 1);
	{
		char found[SITE_SLUG_MAX];
		CHECK(sites_find_show(http, "https://animesuge.re", LAYOUT_SUGE, "no-such-show-zzzzz", "No Such Show 12345",
		                      found, sizeof(found)) != 0, "found %s", found);
		CHECK(sites_home_layout("s8561", "") == LAYOUT_SUGE && sites_home_layout("8384", "") == LAYOUT_ANIKOTO &&
		      sites_home_layout("", "https://anisuge.tv/anime/x") == LAYOUT_SUGE &&
		      sites_home_layout("", "https://anikototv.to/watch/animesuge-x") == LAYOUT_ANIKOTO, "home layout");
		CHECK(catalog_same_title("Frieren:  Beyond journey's end ", "frieren: beyond Journey's End") &&
		      !catalog_same_title("Frieren", "Frieren Season 2") && !catalog_same_title("", ""), "titles");
	}

	printf("== animesuge.re down too: next AnimeSuge site\n");
	setenv("SITETEST_DOWN", "anikototv.bz anikototv.se anikoto.me anikoto.net anikoto.cz anikototv.to animesuge.re", 1);
	walk(http, "https://anisuge.tv", LAYOUT_ANIKOTO, "frieren-beyond-journey-s-end-c6fbj", "Frieren: Beyond Journey's End", 28, 0);
	unsetenv("SITETEST_DOWN");

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures != 0;
}
