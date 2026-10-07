/* SPDX-License-Identifier: MIT */
/*
 * nuubOS Notification contract (EPIC-006), producer side.
 *
 * A notification is a typed event owned by the service that knows about it.
 * Producers never send display text: they send an event kind plus raw values
 * and the renderer localizes them. One line per message:
 *
 *   POST    id=<id> event=<kind> [key=value ...]   show or replace in place
 *   UPDATE  id=<id> event=<kind> [key=value ...]   replace only while shown
 *   DISMISS id=<id>                                 remove
 *
 * <id> is stable per subject (e.g. one id per controller) so a reconnect or a
 * late battery reading updates the same notification instead of stacking.
 * A message carrying progress=<-1..100> is a Live Notification (-1 means
 * indeterminate); the next POST for the same id without progress completes it.
 *
 * Event kinds and their fields:
 *   music.track             title= artist=
 *   controller.connected    name= player= battery=
 *   controller.disconnected name=
 *   headphones.connected    name= battery=
 *   headphones.disconnected name=
 *   accessory.battery.low   name= percent= kind=controller|headphones
 *   battery.saver           percent=
 *   battery.low             percent=
 *   battery.critical        percent=
 *   battery.charging        percent=
 *   battery.full
 *   wifi.connected          ssid=
 *   wifi.lost               ssid=
 *   storage.job             action=backup|move-back|adopt
 *                           state=running|succeeded|failed [progress=]
 *   game.state.saved        [slot=]   (no slot: automatic slot)
 *   game.state.loaded       [slot=]
 *   game.state.empty        slot=
 *   game.state.failed       op=save|load [slot=]
 *   game.slot               slot=      (changed with a controller hotkey)
 *   game.fastforward        state=on|off
 *   game.failed             reason=start|crash
 *   game.screenshot.saved              (screenshot written)
 *   game.screenshot.failed
 *   media.failed                       (playback could not start / ended in error)
 *   web.failed              reason=start|crash  (Web Mode browser could not start / closed)
 *   media.connected         name=      (USB storage mounted)
 *   media.removed           name=
 *   job                     type= state=queued|running|succeeded|failed|cancelled
 *                           [progress=] [step=] [reason=]   (nuubos-jobd)
 *   stream.paired           name=      (PC streaming host paired)
 *   stream.pair.failed      name=
 *   stream.failed           reason=unreachable|unpaired|app|start|connection|steamlink
 *                           [name=]    (stream could not start / was lost)
 *   stream.steamlink.installed version=   (Valve Steam Link downloaded)
 *   stream.steamlink.failed reason=network|space|archive|write
 *
 * Values are percent-encoded. Sending is fire-and-forget and never blocks:
 * a missing or busy router simply drops a transient notification.
 */
#ifndef NUUBOS_NOTIFY_H
#define NUUBOS_NOTIFY_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef NUUBOS_NOTIFY_SOCKET
#define NUUBOS_NOTIFY_SOCKET "/run/nuubos/notifyd.sock"
#endif
#define NUUBOS_NOTIFY_MAX_LINE 1024

struct nuubos_notify {
	char line[NUUBOS_NOTIFY_MAX_LINE];
	size_t len;
	bool overflow;
};

static inline void nuubos_notify_raw(struct nuubos_notify *n,
				     const char *text, size_t len)
{
	if (n->overflow || n->len + len + 2 > sizeof(n->line)) {
		n->overflow = true;
		return;
	}
	memcpy(n->line + n->len, text, len);
	n->len += len;
	n->line[n->len] = '\0';
}

static inline void nuubos_notify_escaped(struct nuubos_notify *n,
					 const char *value)
{
	static const char hex[] = "0123456789ABCDEF";
	const unsigned char *p = (const unsigned char *)(value ? value : "");

	for (; *p; p++) {
		if (*p <= 0x20 || *p == 0x7f || *p == '%' || *p == '=') {
			char enc[3] = { '%', hex[*p >> 4], hex[*p & 0x0f] };
			nuubos_notify_raw(n, enc, sizeof(enc));
		} else {
			nuubos_notify_raw(n, (const char *)p, 1);
		}
	}
}

static inline void nuubos_notify_str(struct nuubos_notify *n,
				     const char *key, const char *value)
{
	nuubos_notify_raw(n, " ", 1);
	nuubos_notify_raw(n, key, strlen(key));
	nuubos_notify_raw(n, "=", 1);
	nuubos_notify_escaped(n, value);
}

static inline void nuubos_notify_int(struct nuubos_notify *n,
				     const char *key, int value)
{
	char text[16];

	snprintf(text, sizeof(text), "%d", value);
	nuubos_notify_str(n, key, text);
}

/* verb is "POST", "UPDATE" or "DISMISS"; event may be NULL for DISMISS. */
static inline void nuubos_notify_begin(struct nuubos_notify *n,
				       const char *verb, const char *id,
				       const char *event)
{
	n->len = 0;
	n->overflow = false;
	n->line[0] = '\0';
	nuubos_notify_raw(n, verb, strlen(verb));
	nuubos_notify_str(n, "id", id);
	if (event)
		nuubos_notify_str(n, "event", event);
}

static inline int nuubos_notify_send(struct nuubos_notify *n)
{
	struct sockaddr_un addr;
	int fd;
	int rc = -1;

	if (n->overflow)
		return -1;
	nuubos_notify_raw(n, "\n", 1);
	if (n->overflow)
		return -1;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", NUUBOS_NOTIFY_SOCKET);

	/* A local stream connect completes immediately or fails with EAGAIN
	 * when the backlog is full; neither case may stall the producer. */
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
	    send(fd, n->line, n->len, MSG_NOSIGNAL) == (ssize_t)n->len)
		rc = 0;
	close(fd);
	return rc;
}

static inline int nuubos_notify_dismiss(const char *id)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "DISMISS", id, NULL);
	return nuubos_notify_send(&n);
}

#endif
