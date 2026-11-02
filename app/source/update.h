#ifndef UPDATE_H
#define UPDATE_H

#include <stddef.h>

/* Finding new versions of the app on GitHub, from the latest.json in the newest release. */

#define UPDATE_NAME_MAX 24
#define UPDATE_URL_MAX 256
#define UPDATE_SHA_MAX 65
#define UPDATE_NOTES_MAX 160

struct update_info {
	int version_code;
	char version_name[UPDATE_NAME_MAX];
	char pkg_url[UPDATE_URL_MAX];
	char sha256[UPDATE_SHA_MAX];
	char notes[UPDATE_NOTES_MAX];
};

/* Returns 0 if the manifest has what's needed. */
int update_parse_manifest(const char *json, struct update_info *out);
/* The SHA-256 of some data, as lowercase hex, into out[UPDATE_SHA_MAX]. */
void update_sha256_hex(const void *data, size_t size, char *out);

#endif
