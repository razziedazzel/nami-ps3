/* http.h for the host test: runs curl, the way the PS3's libcurl is set up (DoH, redirects).
 * Hosts listed in SITETEST_DOWN (spaces between) fail, as if those sites were down; hosts in
 * SITETEST_PARKED answer 200 with a parking page. */
#include "http.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct http_client { int unused; };

void app_log(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	fprintf(stderr, "  log: ");
	vfprintf(stderr, fmt, a);
	va_end(a);
}

void http_global_init(void) {}
struct http_client *http_client_new(void) { return calloc(1, sizeof(struct http_client)); }

static int listed(const char *env, const char *url)
{
	const char *list = getenv(env);
	const char *host = strstr(url, "://");
	if (!list || !host)
		return 0;
	host += 3;
	size_t n = strcspn(host, "/?#");
	for (const char *p = list; *p;) {
		size_t m = strcspn(p, " ");
		if (m == n && strncmp(p, host, n) == 0)
			return 1;
		p += m;
		while (*p == ' ')
			p++;
	}
	return 0;
}

int http_requests;

int http_get(struct http_client *c, const char *url, const char *referer, int xhr, char **body, size_t *len)
{
	(void)c;
	http_requests++;
	if (listed("SITETEST_DOWN", url))
		return -1;
	if (listed("SITETEST_PARKED", url)) {
		*body = strdup("<html><title>This domain is for sale</title></html>");
		*len = strlen(*body);
		return 0;
	}
	char tmp[] = "/tmp/sitetest-XXXXXX";
	int fd = mkstemp(tmp);
	if (fd < 0)
		return -1;
	close(fd);
	int pipefd[2];
	if (pipe(pipefd) != 0)
		return -1;
	pid_t pid = fork();
	if (pid == 0) {
		const char *argv[24];
		int a = 0;
		argv[a++] = "curl"; argv[a++] = "-s"; argv[a++] = "-g"; argv[a++] = "-L"; argv[a++] = "-m"; argv[a++] = "40";
		argv[a++] = "--doh-url"; argv[a++] = "https://cloudflare-dns.com/dns-query";
		argv[a++] = "-A"; argv[a++] = "Mozilla/5.0";
		argv[a++] = "-o"; argv[a++] = tmp; argv[a++] = "-w"; argv[a++] = "%{http_code}";
		if (referer) { argv[a++] = "-e"; argv[a++] = referer; }
		if (xhr) { argv[a++] = "-H"; argv[a++] = "X-Requested-With: XMLHttpRequest"; }
		argv[a++] = url;
		argv[a] = NULL;
		dup2(pipefd[1], 1);
		close(pipefd[0]);
		execvp("curl", (char *const *)argv);
		_exit(127);
	}
	close(pipefd[1]);
	char code[8] = "";
	ssize_t got = read(pipefd[0], code, sizeof(code) - 1);
	close(pipefd[0]);
	waitpid(pid, NULL, 0);
	if (got <= 0 || strncmp(code, "200", 3) != 0) {
		unlink(tmp);
		return -1;
	}
	FILE *f = fopen(tmp, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	*body = malloc((size_t)size + 1);
	*len = fread(*body, 1, (size_t)size, f);
	(*body)[*len] = 0;
	fclose(f);
	unlink(tmp);
	return 0;
}
