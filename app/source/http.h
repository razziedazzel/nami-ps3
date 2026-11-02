#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

struct http_client;

/* Call once before any client is made. */
void http_global_init(void);
/* One client per thread; it reuses connections and DNS results between requests. */
struct http_client *http_client_new(void);
/* On success returns 0 and a malloc'd, NUL-terminated *body. */
int http_get(struct http_client *c, const char *url, const char *referer, int xhr, char **body,
             size_t *len);

#endif
