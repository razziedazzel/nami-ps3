#include "http.h"

#include <sys/time.h>
#include <curl/curl.h>
#include <net/net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysmodule/sysmodule.h>

#include "config.h"

#define CA_FILE APP_DIR "cacert.pem"
/* As in the TV app: the home firewall's DNS filters video hosts. */
#define DOH_URL "https://cloudflare-dns.com/dns-query"
#define FIRST_BUFFER (256 * 1024)

struct http_client {
	CURL *curl;
};

struct buffer {
	char *data;
	size_t len, cap;
};

void http_global_init(void)
{
	s32 module = sysModuleLoad(SYSMODULE_NET);
	s32 init = netInitialize();
	app_log("net module=%08x init=%08x\n", module, init);
	curl_global_init(CURL_GLOBAL_ALL);
}

struct http_client *http_client_new(void)
{
	struct http_client *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->curl = curl_easy_init();
	if (!c->curl) {
		free(c);
		return NULL;
	}
	curl_easy_setopt(c->curl, CURLOPT_CAINFO, CA_FILE);
	curl_easy_setopt(c->curl, CURLOPT_DOH_URL, DOH_URL);
	curl_easy_setopt(c->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c->curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c->curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c->curl, CURLOPT_TIMEOUT, 40L);
	curl_easy_setopt(c->curl, CURLOPT_DNS_CACHE_TIMEOUT, 3600L);
	return c;
}

static size_t on_data(char *ptr, size_t size, size_t count, void *user)
{
	struct buffer *b = user;
	size_t n = size * count;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : FIRST_BUFFER;
		while (b->len + n + 1 > cap)
			cap *= 2;
		char *grown = realloc(b->data, cap);
		if (!grown)
			return 0;
		b->data = grown;
		b->cap = cap;
	}
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = 0;
	return n;
}

int http_get(struct http_client *c, const char *url, const char *referer, int xhr, char **body,
             size_t *len)
{
	struct buffer b = {0};
	struct curl_slist *headers = NULL;
	char referer_line[256];
	long code = 0;

	if (referer) {
		snprintf(referer_line, sizeof(referer_line), "Referer: %s", referer);
		headers = curl_slist_append(headers, referer_line);
	}
	if (xhr)
		headers = curl_slist_append(headers, "X-Requested-With: XMLHttpRequest");

	curl_easy_setopt(c->curl, CURLOPT_URL, url);
	curl_easy_setopt(c->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, &b);
	CURLcode r = curl_easy_perform(c->curl);
	curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_setopt(c->curl, CURLOPT_HTTPHEADER, NULL);
	curl_slist_free_all(headers);

	if (r != CURLE_OK || code != 200 || !b.data) {
		app_log("http fail curl=%d http=%ld %.100s\n", (int)r, code, url);
		free(b.data);
		return -1;
	}
	*body = b.data;
	*len = b.len;
	return 0;
}
