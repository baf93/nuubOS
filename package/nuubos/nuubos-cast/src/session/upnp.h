/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Small UPnP helpers: HTTP GET/POST with a timeout (device descriptions,
 * SOAP actions) and the bits of XML DLNA needs.
 */
#ifndef NUUBOS_CAST_UPNP_H
#define NUUBOS_CAST_UPNP_H

#include <stdbool.h>
#include <stddef.h>

struct url {
	char host[64];
	int port;
	char path[384];
};

bool url_parse(const char *text, struct url *out);
/* Resolves `ref` (absolute URL or path) against `base`. */
void url_resolve(const struct url *base, const char *ref, struct url *out);

/* Returns the HTTP status (body malloc'ed into *body, NUL terminated), or
 * -1. `soap_action` NULL = GET. */
int http_request(const struct url *u, const char *soap_action, const char *body_in, char **body,
		 int timeout_ms);

/* Text of the first <tag>...</tag> (namespace prefixes ignored) after
 * `from`; returns a pointer past it or NULL. */
const char *xml_text(const char *from, const char *tag, char *out, size_t size);
/* The <service> block whose serviceType contains `type`: its controlURL
 * and full service type (e.g. "...:AVTransport:1"). */
bool upnp_control_url(const char *description, const char *type, char *out, size_t size,
		      char *full_type, size_t full_size);
void xml_escape(const char *in, char *out, size_t size);

#endif
