#include "update.h"

#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHA256_BYTES 32

/* The value of "key" in simple flat JSON, up to its closing quote or the end of its number. */
static const char *find_value(const char *json, const char *key)
{
	char pattern[32];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(json, pattern);
	if (!p)
		return NULL;
	p += strlen(pattern);
	while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
		p++;
	if (*p++ != ':')
		return NULL;
	while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
		p++;
	return p;
}

static int read_string(const char *json, const char *key, char *out, size_t size)
{
	const char *p = find_value(json, key);
	size_t n = 0;
	if (!p || *p++ != '"')
		return -1;
	for (; *p && *p != '"'; p++) {
		if (*p == '\\' && p[1])
			p++;
		if (n + 1 >= size)
			return -1;
		out[n++] = *p;
	}
	if (*p != '"')
		return -1;
	out[n] = 0;
	return 0;
}

int update_parse_manifest(const char *json, struct update_info *out)
{
	memset(out, 0, sizeof(*out));
	const char *code = find_value(json, "versionCode");
	if (!code)
		return -1;
	out->version_code = atoi(code);
	if (out->version_code <= 0 || read_string(json, "versionName", out->version_name, sizeof(out->version_name)) != 0 ||
	    read_string(json, "pkgUrl", out->pkg_url, sizeof(out->pkg_url)) != 0 ||
	    read_string(json, "sha256", out->sha256, sizeof(out->sha256)) != 0 ||
	    strlen(out->sha256) != SHA256_BYTES * 2)
		return -1;
	/* Notes are optional, and only ever shown. */
	if (read_string(json, "notes", out->notes, sizeof(out->notes)) != 0)
		out->notes[0] = 0;
	/* A file name comes from the version, so keep it to plain characters. */
	for (const char *c = out->version_name; *c; c++) {
		if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-'))
			return -1;
	}
	return 0;
}

void update_sha256_hex(const void *data, size_t size, char *out)
{
	unsigned char digest[SHA256_BYTES];
	mbedtls_sha256_context sha;
	mbedtls_sha256_init(&sha);
	mbedtls_sha256_starts_ret(&sha, 0);
	mbedtls_sha256_update_ret(&sha, data, size);
	mbedtls_sha256_finish_ret(&sha, digest);
	mbedtls_sha256_free(&sha);
	for (int i = 0; i < SHA256_BYTES; i++)
		sprintf(out + i * 2, "%02x", digest[i]);
	out[SHA256_BYTES * 2] = 0;
}
