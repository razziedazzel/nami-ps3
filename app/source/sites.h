#ifndef SITES_H
#define SITES_H

#include <stddef.h>

/* The sites the app reads from, best first: Anikoto and its mirrors, then AnimeSuge as a last resort.
 *
 * AnimeSuge runs on the same backend as Anikoto with its own page templates (LAYOUT_SUGE). Older
 * shows have the same ids and slugs (the name in the show's address) on both. Newer shows and their
 * episodes are numbered differently, the same number can be another show on the other site, and
 * their slugs often differ in the random ending; titles match. So the app's show ids are Anikoto's
 * plain numbers, or "s" + AnimeSuge's number for shows first seen there, and a show the user has
 * saved keeps the id it was saved with. On the other kind of site a show is found by its slug, else
 * by its exact title. Episode keys only go to the kind of site that gave them. */

#define SITE_URL_MAX 64
#define SITES_MAX 24
#define SITE_SLUG_MAX 160

enum site_layout { LAYOUT_ANIKOTO, LAYOUT_SUGE };
#define LAYOUT_ANY (-1)

struct http_client;

/* dir ends in a slash. Loads the official list saved by the last check, if any. */
void sites_init(const char *dir);
/* Reads Anikoto's official domain page if a day has passed since the last check, or now with force
 * (still at most once every 10 minutes). Returns 1 if the list changed. */
int sites_refresh(struct http_client *http, int force);

int sites_layout_of(const char *site);
/* The sites to try, the last one that worked first; only those of one layout unless LAYOUT_ANY. */
int sites_list(int layout, char out[][SITE_URL_MAX], int max);
void sites_worked(const char *site);
/* While on AnimeSuge, Anikoto is tried first again after this long (seconds; tests set 0). */
extern int sites_main_retry_seconds;

/* Writes the path (starting with "/") to ask this site for. Returns 0 to skip the site. */
typedef int (*site_path_fn)(void *ctx, int layout, const char *site, char *path, size_t size);

/* GET from the first site that gives a real answer: a page of the site, or JSON for xhr. If none
 * does, checks the official list once for new addresses and tries those. On success returns 0 and
 * a malloc'd body, and the site that answered. */
int sites_get(struct http_client *http, int layout, site_path_fn path, void *ctx, int xhr, char **body,
              size_t *len, char *site_out);

/* The kind of site a show belongs to: by the app's id ("s" + number is AnimeSuge's, a plain number
 * Anikoto's), or with no id yet, by its address. */
int sites_home_layout(const char *app_id, const char *watch_url);
/* Find a show on a site of the other kind: by its slug, else by its exact title, taking it only if
 * just one show has that title. Writes its slug there. Returns 0 if found. */
int sites_find_show(struct http_client *http, const char *site, int layout, const char *slug, const char *title,
                    char *slug_out, size_t size);

/* The slug in ".../watch/<slug>" or ".../anime/<slug>". Returns 0 if found. */
int sites_slug(const char *url, char *out, size_t size);
/* The show page's path on a layout: /watch/<slug> on Anikoto, /anime/<slug> on AnimeSuge. */
void sites_show_path(const char *slug, int layout, char *out, size_t size);
/* The official list on Anikoto's domain page, primary first, keeping only https Anikoto sites. */
int sites_parse_domains(const char *html, char out[][SITE_URL_MAX], int max);

#endif
