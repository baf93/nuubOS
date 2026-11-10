/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Receiver discovery, one shot (no daemon):
 *   mDNS  _googlecast._tcp  Google Cast (TXT fn = friendly name, md = model)
 *   mDNS  _display._tcp     Miracast over Infrastructure (MS-MICE) sinks
 *   SSDP  MediaRenderer:1   DLNA renderers with an AVTransport service
 * Prints one line per receiver:
 *   @rx PROTOCOL<TAB>ADDRESS<TAB>PORT<TAB>NAME<TAB>MODEL<TAB>LOCATION
 */
#include "session.h"
#include "common.h"
#include "upnp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_FOUND 32

struct found {
	char proto[8];
	char address[64];
	int port;
	char name[128];
	char model[96];
	char location[384];
	bool printed;
};

static struct found found[MAX_FOUND];
static int found_count;

/* One entry per protocol and address; DLNA: per description (a device
 * may host several renderers). */
static struct found *add(const char *proto, const char *address, const char *location)
{
	for (int i = 0; i < found_count; i++)
		if (!strcmp(found[i].proto, proto) && !strcmp(found[i].address, address) &&
		    (!location || !strcmp(found[i].location, location)))
			return &found[i];
	if (found_count >= MAX_FOUND)
		return NULL;
	memset(&found[found_count], 0, sizeof(found[0]));
	copy_text(found[found_count].proto, sizeof(found[0].proto), proto);
	copy_text(found[found_count].address, sizeof(found[0].address), address);
	if (location)
		copy_text(found[found_count].location, sizeof(found[0].location), location);
	return &found[found_count++];
}

/* ---- mDNS ---- */

static size_t put_name(uint8_t *p, const char *name)
{
	size_t n = 0;

	while (*name) {
		size_t len = strcspn(name, ".");

		p[n++] = (uint8_t)len;
		memcpy(p + n, name, len);
		n += len;
		name += len;
		if (*name == '.')
			name++;
	}
	p[n++] = 0;
	return n;
}

static void mdns_query(int fd)
{
	static const char *const services[] = { "_googlecast._tcp.local", "_display._tcp.local" };
	struct sockaddr_in to = { 0 };

	to.sin_family = AF_INET;
	to.sin_port = htons(5353);
	to.sin_addr.s_addr = inet_addr("224.0.0.251");
	for (size_t i = 0; i < ARRAY_SIZE(services); i++) {
		uint8_t q[128] = { 0 };
		size_t n = 12;

		q[5] = 1; /* one question */
		n += put_name(q + n, services[i]);
		q[n++] = 0;
		q[n++] = 12; /* PTR */
		q[n++] = 0;
		q[n++] = 1;  /* IN; sent from a random port: legacy unicast replies */
		if (sendto(fd, q, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
			log_msg("mDNS query: %s", strerror(errno));
	}
}

static const uint8_t *skip_name(const uint8_t *p, const uint8_t *end)
{
	while (p < end) {
		if (*p == 0)
			return p + 1;
		if ((*p & 0xc0) == 0xc0)
			return p + 2 <= end ? p + 2 : NULL;
		p += *p + 1;
	}
	return NULL;
}

/* Full dotted name (compression followed). */
static void read_name(const uint8_t *msg, size_t len, const uint8_t *p, char *out, size_t size)
{
	size_t n = 0;
	int hops = 0;

	out[0] = '\0';
	while (p >= msg && p < msg + len && hops < 16) {
		if ((*p & 0xc0) == 0xc0) {
			if (p + 1 >= msg + len)
				break;
			p = msg + (((p[0] & 0x3f) << 8) | p[1]);
			hops++;
			continue;
		}
		if (*p == 0 || p + 1 + *p > msg + len)
			break;
		if (n && n + 1 < size)
			out[n++] = '.';
		for (int i = 0; i < *p && n + 1 < size; i++)
			out[n++] = (char)p[1 + i];
		p += *p + 1;
	}
	out[n] = '\0';
}

static void txt_value(const uint8_t *p, int len, const char *key, char *out, size_t size)
{
	size_t klen = strlen(key);
	const uint8_t *end = p + len;

	while (p < end) {
		int l = *p++;

		if (p + l > end)
			break;
		if ((size_t)l > klen && !strncasecmp((const char *)p, key, klen) && p[klen] == '=') {
			snprintf(out, size, "%.*s", (int)(l - (int)klen - 1), (const char *)p + klen + 1);
			return;
		}
		p += l;
	}
}

static void mdns_read(int fd)
{
	uint8_t msg[4096];
	struct sockaddr_in from;
	socklen_t from_len = sizeof(from);
	ssize_t len;

	while ((len = recvfrom(fd, msg, sizeof(msg), MSG_DONTWAIT, (struct sockaddr *)&from, &from_len)) > 12) {
		const uint8_t *end = msg + len, *p = msg + 12;
		int qd = (msg[4] << 8) | msg[5];
		int rr = ((msg[6] << 8) | msg[7]) + ((msg[8] << 8) | msg[9]) + ((msg[10] << 8) | msg[11]);
		char address[64], owner[256], instance[128] = "", fn[128] = "", md[96] = "";
		const char *proto = NULL;
		int port = 0;
		struct found *f;

		inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));
		for (int i = 0; i < qd && p; i++) {
			p = skip_name(p, end);
			if (p)
				p += 4;
		}
		for (int i = 0; i < rr && p && p + 10 <= end; i++) {
			const uint8_t *name_at = p;
			int type, rdlen;

			p = skip_name(p, end);
			if (!p || p + 10 > end)
				break;
			type = (p[0] << 8) | p[1];
			rdlen = (p[8] << 8) | p[9];
			p += 10;
			if (p + rdlen > end)
				break;
			read_name(msg, (size_t)len, name_at, owner, sizeof(owner));
			if (strstr(owner, "_googlecast._tcp"))
				proto = "cast";
			else if (strstr(owner, "_display._tcp"))
				proto = "mice";
			if (type == 12) { /* PTR → instance */
				char target[256];

				read_name(msg, (size_t)len, p, target, sizeof(target));
				if (!instance[0])
					snprintf(instance, sizeof(instance), "%.*s", (int)strcspn(target, "."), target);
			} else if (type == 33 && rdlen >= 6) {
				port = (p[4] << 8) | p[5];
			} else if (type == 16) {
				txt_value(p, rdlen, "fn", fn, sizeof(fn));
				txt_value(p, rdlen, "md", md, sizeof(md));
			}
			p += rdlen;
		}
		from_len = sizeof(from);
		if (!proto)
			continue;
		f = add(proto, address, NULL);
		if (!f)
			continue;
		if (port)
			f->port = port;
		if (fn[0])
			copy_text(f->name, sizeof(f->name), fn);
		else if (!f->name[0] && instance[0])
			copy_text(f->name, sizeof(f->name), instance);
		if (md[0])
			copy_text(f->model, sizeof(f->model), md);
		if (!f->port)
			f->port = !strcmp(proto, "cast") ? 8009 : 7250;
	}
}

/* ---- SSDP ---- */

static void ssdp_query(int fd)
{
	static const char req[] =
		"M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
		"ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";
	struct sockaddr_in to = { 0 };

	to.sin_family = AF_INET;
	to.sin_port = htons(1900);
	to.sin_addr.s_addr = inet_addr("239.255.255.250");
	if (sendto(fd, req, sizeof(req) - 1, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
		log_msg("SSDP query: %s", strerror(errno));
}

static void ssdp_read(int fd)
{
	char msg[2048];
	struct sockaddr_in from;
	socklen_t from_len = sizeof(from);
	ssize_t len;

	while ((len = recvfrom(fd, msg, sizeof(msg) - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &from_len)) > 0) {
		char address[64], *loc;

		msg[len] = '\0';
		from_len = sizeof(from);
		loc = strcasestr(msg, "\nLOCATION:");
		if (!loc)
			continue;
		loc += 10;
		while (*loc == ' ')
			loc++;
		loc[strcspn(loc, "\r\n")] = '\0';
		inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));
		log_msg("SSDP renderer %s at %s", address, loc);
		add("dlna", address, loc);
	}
}

/* Name/model from the description; renderers without AVTransport are
 * dropped. */
static void dlna_describe(struct found *f)
{
	struct url u;
	char *body = NULL, control[256], type[160];

	if (!url_parse(f->location, &u) || http_request(&u, NULL, NULL, &body, 2500) != 200 || !body ||
	    !upnp_control_url(body, "service:AVTransport:", control, sizeof(control), type, sizeof(type))) {
		log_msg("%s: no usable AVTransport description", f->location);
		f->proto[0] = '\0';
		free(body);
		return;
	}
	xml_text(body, "friendlyName", f->name, sizeof(f->name));
	xml_text(body, "modelName", f->model, sizeof(f->model));
	f->port = u.port;
	free(body);
}

static void print(struct found *f)
{
	if (f->printed || !f->proto[0])
		return;
	if (!f->name[0])
		copy_text(f->name, sizeof(f->name), f->address);
	sanitize(f->name);
	sanitize(f->model);
	emit("@rx %s\t%s\t%d\t%s\t%s\t%s", f->proto, f->address, f->port, f->name, f->model, f->location);
	f->printed = true;
}

int discover_run(int seconds)
{
	int mdns = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int ssdp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int64_t end, resend;

	if (seconds < 1 || seconds > 15)
		seconds = 3;
	if (mdns < 0 || ssdp < 0)
		return 1;
	end = now_ms() + seconds * 1000;
	resend = now_ms() + 1000;
	mdns_query(mdns);
	ssdp_query(ssdp);
	while (now_ms() < end) {
		struct pollfd pfd[2] = { { mdns, POLLIN, 0 }, { ssdp, POLLIN, 0 } };
		int64_t left = end - now_ms();

		if (poll(pfd, 2, left > 250 ? 250 : (int)left) > 0) {
			if (pfd[0].revents)
				mdns_read(mdns);
			if (pfd[1].revents)
				ssdp_read(ssdp);
		}
		/* UDP is lossy on Wi-Fi: ask once more. */
		if (resend && now_ms() >= resend) {
			mdns_query(mdns);
			ssdp_query(ssdp);
			resend = 0;
		}
		for (int i = 0; i < found_count; i++)
			if (strcmp(found[i].proto, "dlna"))
				print(&found[i]);
	}
	for (int i = 0; i < found_count; i++)
		if (!strcmp(found[i].proto, "dlna") && !found[i].printed) {
			dlna_describe(&found[i]);
			print(&found[i]);
		}
	emit("@done");
	close(mdns);
	close(ssdp);
	return 0;
}
