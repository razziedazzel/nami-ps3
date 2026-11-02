#include "catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy text, decoding the entities titles use and UTF-8 into Latin-1 ('?' if it doesn't fit). */
static void copy_title(const char *src, size_t len, char *out, size_t size)
{
	size_t n = 0;
	for (size_t i = 0; i < len && n + 1 < size;) {
		unsigned char c = (unsigned char)src[i];
		if (c == '&') {
			static const struct { const char *name; char value; } entities[] = {
				{"&amp;", '&'}, {"&quot;", '"'}, {"&#039;", '\''}, {"&#39;", '\''},
				{"&lt;", '<'}, {"&gt;", '>'},
			};
			int matched = 0;
			for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++) {
				size_t l = strlen(entities[e].name);
				if (i + l <= len && strncmp(src + i, entities[e].name, l) == 0) {
					out[n++] = entities[e].value;
					i += l;
					matched = 1;
					break;
				}
			}
			if (matched)
				continue;
		}
		if (c < 0x80) {
			out[n++] = (char)c;
			i++;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
			unsigned cp = ((c & 0x1Fu) << 6) | ((unsigned char)src[i + 1] & 0x3Fu);
			out[n++] = cp < 0x100 ? (char)cp : '?';
			i += 2;
		} else {
			out[n++] = '?';
			i += (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
		}
	}
	out[n] = 0;
}

static int copy_attr(const char *from, const char *limit, const char *name, char *out, size_t size)
{
	const char *p = strstr(from, name);
	if (!p || (limit && p >= limit))
		return -1;
	p += strlen(name);
	const char *end = strchr(p, '"');
	if (!end || (size_t)(end - p) >= size)
		return -1;
	memcpy(out, p, end - p);
	out[end - p] = 0;
	return 0;
}

static int count_after(const char *from, const char *limit, const char *marker)
{
	const char *p = strstr(from, marker);
	if (!p || (limit && p >= limit))
		return 0;
	p += strlen(marker);
	while (*p && *p != '<' && (*p < '0' || *p > '9'))
		p++;
	return atoi(p);
}

int catalog_parse_list(const char *html, struct anime_item *items, int max)
{
	static const char item_marker[] = "<div class=\"item";
	const char *p = strstr(html, "id=\"list-items\"");
	int count = 0;
	if (!p)
		return 0;

	p = strstr(p, item_marker);
	while (p && count < max) {
		const char *next = strstr(p + 1, item_marker);
		struct anime_item *it = &items[count];
		memset(it, 0, sizeof(*it));

		const char *name = strstr(p, "class=\"name");
		const char *text = name && (!next || name < next) ? strchr(name, '>') : NULL;
		const char *text_end = text ? strchr(text, '<') : NULL;

		if (text_end && (!next || text_end < next) &&
		    copy_attr(p, next, "href=\"", it->watch_url, sizeof(it->watch_url)) == 0 &&
		    copy_attr(p, next, "data-tip=\"", it->id, sizeof(it->id)) == 0 &&
		    copy_attr(p, next, "<img src=\"", it->poster_url, sizeof(it->poster_url)) == 0) {
			copy_title(text + 1, (size_t)(text_end - text - 1), it->title, sizeof(it->title));
			/* Drop the trailing /ep-N so the link points at the show. */
			char *ep = strstr(it->watch_url, "/ep-");
			if (ep)
				*ep = 0;
			it->sub_count = count_after(p, next, "ep-status sub\"><span>");
			it->dub_count = count_after(p, next, "ep-status dub\"><span>");
			count++;
		}
		p = next;
	}
	return count;
}

void catalog_json_unescape(char *s)
{
	char *w = s;
	for (char *r = s; *r; r++) {
		if (*r != '\\' || !r[1]) {
			*w++ = *r;
			continue;
		}
		r++;
		switch (*r) {
		case 'n': *w++ = '\n'; break;
		case 't': *w++ = '\t'; break;
		case 'r': *w++ = '\r'; break;
		default: *w++ = *r; break;
		}
	}
	*w = 0;
}

/* Text with tags removed and spaces collapsed, entities and UTF-8 handled as in copy_title. */
static void copy_plain(const char *src, size_t len, char *out, size_t size)
{
	char *tmp = malloc(len + 1);
	size_t n = 0;
	int in_tag = 0, last_space = 1;
	if (!tmp) {
		out[0] = 0;
		return;
	}
	for (size_t i = 0; i < len; i++) {
		char c = src[i];
		if (c == '<') {
			in_tag = 1;
		} else if (c == '>' && in_tag) {
			in_tag = 0;
		} else if (!in_tag) {
			int space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
			if (!space || !last_space)
				tmp[n++] = space ? ' ' : c;
			last_space = space;
		}
	}
	while (n > 0 && tmp[n - 1] == ' ')
		n--;
	/* Lists in the page's markup leave a space before each comma. */
	size_t kept = 0;
	for (size_t i = 0; i < n; i++) {
		if (tmp[i] == ' ' && i + 1 < n && tmp[i + 1] == ',')
			continue;
		tmp[kept++] = tmp[i];
	}
	copy_title(tmp, kept, out, size);
	free(tmp);
}

/* The inner text of the element that holds the first match of marker. */
static int copy_element_text(const char *html, const char *marker, const char *end_tag, char *out, size_t size)
{
	const char *p = strstr(html, marker);
	if (!p)
		return -1;
	p = strchr(p, '>');
	if (!p)
		return -1;
	p++;
	const char *end = strstr(p, end_tag);
	if (!end)
		return -1;
	copy_plain(p, (size_t)(end - p), out, size);
	return 0;
}

/* A "Label: <span>value</span>" row of the info panel. */
static void copy_meta(const char *panel, const char *label, char *out, size_t size)
{
	const char *p = strstr(panel, label);
	const char *span = p ? strstr(p, "<span") : NULL;
	const char *start = span ? strchr(span, '>') : NULL;
	/* The value can hold nested spans, and its row ends at the closing div. */
	const char *end = start ? strstr(start, "</div>") : NULL;
	out[0] = 0;
	if (end)
		copy_plain(start + 1, (size_t)(end - start - 1), out, size);
}

int catalog_parse_details(const char *html, struct anime_details *out)
{
	memset(out, 0, sizeof(*out));
	/* A season's link gives only its address, so the page supplies its ID and poster. */
	const char *main_tag = strstr(html, "id=\"watch-main\"");
	if (main_tag) {
		const char *tag_end = strchr(main_tag, '>');
		copy_attr(main_tag, tag_end, "data-id=\"", out->id, sizeof(out->id));
	}
	const char *panel = strstr(html, "id=\"w-info\"");
	if (!panel)
		return -1;
	const char *poster = strstr(panel, "class=\"poster\"");
	if (poster)
		copy_attr(poster, NULL, "src=\"", out->poster_url, sizeof(out->poster_url));
	if (copy_element_text(panel, "<h1", "</h1>", out->title, sizeof(out->title)) != 0)
		return -1;
	const char *synopsis = strstr(panel, "class=\"synopsis");
	if (synopsis)
		copy_element_text(synopsis, "class=\"content\"", "</div>", out->synopsis, sizeof(out->synopsis));
	copy_element_text(panel, "class=\"rating\"", "</i>", out->rating, sizeof(out->rating));
	const char *meta = strstr(panel, "class=\"bmeta\"");
	if (meta) {
		copy_meta(meta, "Type:", out->type, sizeof(out->type));
		copy_meta(meta, "Status:", out->status, sizeof(out->status));
		copy_meta(meta, "Aired:", out->aired, sizeof(out->aired));
		copy_meta(meta, "Genres:", out->genres, sizeof(out->genres));
	}
	return 0;
}

int catalog_parse_episodes(const char *html, struct episode *episodes, int max)
{
	int count = 0;
	const char *p = html;
	while (count < max && (p = strstr(p, "<a ")) != NULL) {
		const char *tag_end = strchr(p, '>');
		if (!tag_end)
			break;
		struct episode *e = &episodes[count];
		memset(e, 0, sizeof(*e));
		if (copy_attr(p, tag_end, "data-num=\"", e->number, sizeof(e->number)) == 0 &&
		    copy_attr(p, tag_end, "data-ids=\"", e->server_ids, sizeof(e->server_ids)) == 0) {
			char flag[4] = "";
			if (copy_attr(p, tag_end, "data-sub=\"", flag, sizeof(flag)) == 0)
				e->has_sub = flag[0] == '1';
			flag[0] = 0;
			if (copy_attr(p, tag_end, "data-dub=\"", flag, sizeof(flag)) == 0)
				e->has_dub = flag[0] == '1';
			const char *close = strstr(tag_end, "</a>");
			const char *title = strstr(tag_end, "d-title");
			if (close && title && title < close) {
				const char *start = strchr(title, '>');
				const char *end = start ? strchr(start, '<') : NULL;
				if (end && end < close)
					copy_plain(start + 1, (size_t)(end - start - 1), e->title, sizeof(e->title));
			}
			count++;
		}
		p = tag_end;
	}
	return count;
}

int catalog_parse_seasons(const char *html, struct season *seasons, int max)
{
	static const char marker[] = "swiper-slide season";
	int count = 0;
	const char *p = html;
	while (count < max && (p = strstr(p, marker)) != NULL) {
		const char *next = strstr(p + 1, marker);
		const char *quote = strchr(p, '"');
		struct season *out = &seasons[count];
		memset(out, 0, sizeof(*out));
		out->current = quote && strstr(p, "active") && strstr(p, "active") < quote;
		const char *name = strstr(p, "class=\"name\"");
		if (copy_attr(p, next, "href=\"", out->watch_url, sizeof(out->watch_url)) == 0 && name &&
		    (!next || name < next) &&
		    copy_element_text(name, "class=\"name\"", "</div>", out->name, sizeof(out->name)) == 0) {
			char *ep = strstr(out->watch_url, "/ep-");
			if (ep)
				*ep = 0;
			count++;
		}
		p = p + 1;
	}
	return count;
}
