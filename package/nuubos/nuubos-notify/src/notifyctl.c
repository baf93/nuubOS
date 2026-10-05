/* SPDX-License-Identifier: MIT */
/*
 * nuubos-notifyctl — post or observe nuubOS notifications.
 *
 *   nuubos-notifyctl post   <id> <event> [key=value ...]
 *   nuubos-notifyctl update <id> <event> [key=value ...]
 *   nuubos-notifyctl dismiss <id>
 *   nuubos-notifyctl watch
 *
 * Values are passed raw and percent-encoded here. Intended for shell-based
 * owners (storage/time helpers) and for hardware qualification.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "notify.h"

static int connect_router(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", NUUBOS_NOTIFY_SOCKET);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int usage(void)
{
	fprintf(stderr,
		"usage: nuubos-notifyctl post|update <id> <event> [key=value ...]\n"
		"       nuubos-notifyctl dismiss <id>\n"
		"       nuubos-notifyctl watch\n");
	return 2;
}

static int exchange(struct nuubos_notify *n)
{
	char reply[128];
	ssize_t got;
	int fd;

	nuubos_notify_raw(n, "\n", 1);
	if (n->overflow) {
		fprintf(stderr, "nuubos-notifyctl: notification too long\n");
		return 1;
	}
	fd = connect_router();
	if (fd < 0) {
		fprintf(stderr, "nuubos-notifyctl: notifyd unavailable: %s\n", strerror(errno));
		return 1;
	}
	if (send(fd, n->line, n->len, MSG_NOSIGNAL) != (ssize_t)n->len) {
		close(fd);
		return 1;
	}
	shutdown(fd, SHUT_WR);
	got = read(fd, reply, sizeof(reply) - 1);
	close(fd);
	if (got <= 0)
		return 1;
	reply[got] = '\0';
	fputs(reply, stdout);
	return strncmp(reply, "OK", 2) == 0 ? 0 : 1;
}

static int watch(void)
{
	char buf[NUUBOS_NOTIFY_MAX_LINE];
	ssize_t got;
	int fd = connect_router();

	if (fd < 0) {
		fprintf(stderr, "nuubos-notifyctl: notifyd unavailable: %s\n", strerror(errno));
		return 1;
	}
	if (send(fd, "SUBSCRIBE\n", 10, MSG_NOSIGNAL) != 10) {
		close(fd);
		return 1;
	}
	while ((got = read(fd, buf, sizeof(buf))) > 0) {
		fwrite(buf, 1, (size_t)got, stdout);
		fflush(stdout);
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	struct nuubos_notify n;

	if (argc < 2)
		return usage();

	if (!strcmp(argv[1], "watch"))
		return argc == 2 ? watch() : usage();

	if (!strcmp(argv[1], "dismiss")) {
		if (argc != 3)
			return usage();
		nuubos_notify_begin(&n, "DISMISS", argv[2], NULL);
		return exchange(&n);
	}

	if (strcmp(argv[1], "post") != 0 && strcmp(argv[1], "update") != 0)
		return usage();
	if (argc < 4)
		return usage();

	nuubos_notify_begin(&n, !strcmp(argv[1], "post") ? "POST" : "UPDATE",
			    argv[2], argv[3]);
	for (int i = 4; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		char key[32];
		size_t key_len;

		if (!eq || eq == argv[i] || (key_len = (size_t)(eq - argv[i])) >= sizeof(key))
			return usage();
		memcpy(key, argv[i], key_len);
		key[key_len] = '\0';
		nuubos_notify_str(&n, key, eq + 1);
	}
	return exchange(&n);
}
