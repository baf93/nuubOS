/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "upnp.h"
#include "common.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_BODY (256 * 1024)

bool url_parse(const char *text, struct url *out)
{
	const char *p, *slash, *colon;
	size_t host_len;

	memset(out, 0, sizeof(*out));
	if (strncasecmp(text, "http://", 7))
		return false;
	p = text + 7;
	slash = strchr(p, '/');
	colon = strchr(p, ':');
	if (colon && slash && colon > slash)
		colon = NULL;
	host_len = (size_t)((colon ? colon : slash ? slash : p + strlen(p)) - p);
	if (host_len == 0 || host_len >= sizeof(out->host))
		return false;
	memcpy(out->host, p, host_len);
	out->port = colon ? atoi(colon + 1) : 80;
	copy_text(out->path, sizeof(out->path), slash ? slash : "/");
	return out->port > 0;
}

void url_resolve(const struct url *base, const char *ref, struct url *out)
{
	if (url_parse(ref, out))
		return;
	*out = *base;
	if (ref[0] == '/') {
		copy_text(out->path, sizeof(out->path), ref);
	} else {
		char *last = strrchr(out->path, '/');
		char dir[384];

		copy_text(dir, sizeof(dir), out->path);
		if (last)
			dir[last - out->path + 1] = '\0';
		snprintf(out->path, sizeof(out->path), "%s%s", dir, ref);
	}
}

int http_request(const struct url *u, const char *soap_action, const char *body_in, char **body,
		 int timeout_ms)
{
	char head[1024];
	struct bytes in = { 0 };
	int64_t deadline = now_ms() + timeout_ms;
	int fd, n, status = -1;
	char *sep;

	*body = NULL;
	fd = tcp_connect(u->host, u->port, timeout_ms);
	if (fd < 0)
		return -1;
	if (soap_action)
		n = snprintf(head, sizeof(head),
			     "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
			     "SOAPAction: \"%s\"\r\nContent-Length: %zu\r\nConnection: close\r\n"
			     "User-Agent: nuubOS/1.0 UPnP/1.0 DLNADOC/1.50\r\n\r\n",
			     u->path, u->host, u->port, soap_action, strlen(body_in));
	else
		n = snprintf(head, sizeof(head),
			     "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
			     "User-Agent: nuubOS/1.0 UPnP/1.0 DLNADOC/1.50\r\n\r\n",
			     u->path, u->host, u->port);
	if (write_all(fd, head, (size_t)n) != 0 || (soap_action && write_all(fd, body_in, strlen(body_in)) != 0))
		goto out;
	for (;;) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		char buf[4096];
		ssize_t r;
		int64_t left = deadline - now_ms();

		if (left <= 0 || poll(&pfd, 1, (int)left) <= 0)
			break;
		r = read(fd, buf, sizeof(buf));
		if (r <= 0)
			break;
		if (bytes_append(&in, buf, (size_t)r) != 0 || in.len > MAX_BODY)
			goto out;
	}
	if (bytes_append(&in, "", 1) != 0)
		goto out;
	if (sscanf((char *)in.data, "HTTP/%*s %d", &status) != 1) {
		status = -1;
		goto out;
	}
	sep = strstr((char *)in.data, "\r\n\r\n");
	if (sep) {
		bool chunked = false;
		char *te = strcasestr((char *)in.data, "\ntransfer-encoding:");

		if (te && te < sep && strcasestr(te, "chunked") && strcasestr(te, "chunked") < sep)
			chunked = true;
		sep += 4;
		if (chunked) {
			/* De-chunk in place. */
			char *src = sep, *dst = sep;

			for (;;) {
				char *end;
				long len = strtol(src, &end, 16);

				if (end == src || len <= 0)
					break;
				src = strstr(end, "\r\n");
				if (!src)
					break;
				src += 2;
				if ((size_t)len > strlen(src))
					len = (long)strlen(src);
				memmove(dst, src, (size_t)len);
				dst += len;
				src += len;
				if (!strncmp(src, "\r\n", 2))
					src += 2;
			}
			*dst = '\0';
		}
		*body = strdup(sep);
	}
out:
	close(fd);
	bytes_free(&in);
	return status;
}

const char *xml_text(const char *from, const char *tag, char *out, size_t size)
{
	const char *p = from;
	size_t tag_len = strlen(tag);

	out[0] = '\0';
	while ((p = strchr(p, '<')) != NULL) {
		const char *name = p + 1, *colon, *end, *close;

		p++;
		if (*name == '/' || *name == '?' || *name == '!')
			continue;
		end = name + strcspn(name, " \t\r\n/>");
		colon = memchr(name, ':', (size_t)(end - name));
		if (colon)
			name = colon + 1;
		if ((size_t)(end - name) != tag_len || strncmp(name, tag, tag_len))
			continue;
		end = strchr(end, '>');
		if (!end)
			return NULL;
		if (end[-1] == '/')
			return end + 1; /* empty element */
		end++;
		close = strstr(end, "</");
		while (close) {
			const char *cn = close + 2, *ce = cn + strcspn(cn, ">"), *cc = memchr(cn, ':', (size_t)(ce - cn));

			if (cc)
				cn = cc + 1;
			if ((size_t)(ce - cn) == tag_len && !strncmp(cn, tag, tag_len))
				break;
			close = strstr(close + 2, "</");
		}
		if (!close)
			return NULL;
		snprintf(out, size, "%.*s", (int)(close - end), end);
		/* Unescape the common entities. */
		for (char *q = out; (q = strchr(q, '&')) != NULL; q++) {
			static const struct { const char *e; char c; } ents[] = {
				{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
			};

			for (size_t k = 0; k < ARRAY_SIZE(ents); k++) {
				size_t el = strlen(ents[k].e);

				if (!strncmp(q, ents[k].e, el)) {
					*q = ents[k].c;
					memmove(q + 1, q + el, strlen(q + el) + 1);
					break;
				}
			}
		}
		return close;
	}
	return NULL;
}

bool upnp_control_url(const char *description, const char *type, char *out, size_t size,
		      char *full_type, size_t full_size)
{
	const char *p = description;
	char service[2048], service_type[256];

	while ((p = xml_text(p, "service", service, sizeof(service))) != NULL) {
		xml_text(service, "serviceType", service_type, sizeof(service_type));
		if (strstr(service_type, type)) {
			copy_text(full_type, full_size, service_type);
			xml_text(service, "controlURL", out, size);
			return out[0] != '\0';
		}
	}
	return false;
}

void xml_escape(const char *in, char *out, size_t size)
{
	size_t n = 0;

	for (; *in && n + 7 < size; in++) {
		const char *rep = NULL;

		switch (*in) {
		case '&': rep = "&amp;"; break;
		case '<': rep = "&lt;"; break;
		case '>': rep = "&gt;"; break;
		case '"': rep = "&quot;"; break;
		case '\'': rep = "&apos;"; break;
		default: break;
		}
		if (rep) {
			size_t l = strlen(rep);

			memcpy(out + n, rep, l);
			n += l;
		} else {
			out[n++] = *in;
		}
	}
	out[n] = '\0';
}
