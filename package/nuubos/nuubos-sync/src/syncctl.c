/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-syncctl — command line of the Syncthing capability (EPIC-034),
 * a client of nuubos-syncd (/run/nuubos/syncd.sock) for the active user.
 *
 *   status                       the snapshot (see syncd.c), end=1
 *   enable | disable
 *   folder saves|states|screenshots|library|history on|off
 *   internet on|off              also sync away from the local network
 *   relays on|off                allow the public relays (with internet on)
 *   add DEVICE [NAME]            accept a request / add a nearby device
 *   accept DEVICE [NAME]         same as add
 *   ignore DEVICE                refuse a request
 *   remove DEVICE
 *   folder-id saves|states|screenshots ID|-   use an existing folder id
 *                                (- = nuubOS's own folder)
 *   use FOLDER saves|states|screenshots       take an offered folder
 *   decline FOLDER               refuse an offered folder
 *
 * Prints OK or ERR <reason>; exit status 0 only for OK.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#define SOCKET_PATH RUN_ROOT "/syncd.sock"

/* Keeps the last bytes of the reply (enough to find "end=1\n"). */
static void keep_tail(char *tail, size_t cap, const char *b, size_t n)
{
	size_t t = strlen(tail);

	if (n >= cap - 1) {
		memcpy(tail, b + n - (cap - 1), cap - 1);
		tail[cap - 1] = '\0';
		return;
	}
	if (t + n > cap - 1) {
		memmove(tail, tail + t + n - (cap - 1), cap - 1 - n);
		t = cap - 1 - n;
	}
	memcpy(tail + t, b, n);
	tail[t + n] = '\0';
}

/* One request; prints the reply (until end=1 for a snapshot). */
static int request(const char *line, bool multi)
{
	struct sockaddr_un a;
	/* ADD and IGNORE reconfigure Syncthing (up to a few REST calls). */
	struct timeval tv = { .tv_sec = 45 };
	char buf[4096], first[4] = "", tail[40] = "";
	size_t got = 0;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return puts("ERR service"), 1;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", SOCKET_PATH);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) ||
	    write(fd, line, strlen(line)) != (ssize_t)strlen(line) || write(fd, "\n", 1) != 1) {
		close(fd);
		return puts("ERR service"), 1;
	}
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		fwrite(buf, 1, (size_t)n, stdout);
		/* The first bytes (OK / ERR) and the last ones (end=1). */
		for (ssize_t i = 0; i < n && got < sizeof(first) - 1; i++)
			first[got++] = buf[i];
		keep_tail(tail, sizeof(tail), buf, (size_t)n);
		if (!strncmp(first, "ERR", 3) ? strchr(tail, '\n') != NULL
		    : multi ? strstr(tail, "end=1\n") != NULL : strchr(tail, '\n') != NULL)
			break;
	}
	close(fd);
	fflush(stdout);
	if (!strncmp(first, "ERR", 3))
		return 1;
	return multi ? !strstr(tail, "end=1\n") : strncmp(first, "OK", 2) != 0;
}

static bool on_off(const char *v)
{
	return !strcmp(v, "on") || !strcmp(v, "off");
}

int main(int argc, char **argv)
{
	char line[512];

	if (argc == 2 && !strcmp(argv[1], "status"))
		return request("STATUS", true);
	if (argc == 2 && (!strcmp(argv[1], "enable") || !strcmp(argv[1], "disable")))
		return request(argv[1][0] == 'e' ? "ENABLE" : "DISABLE", false);
	if (argc == 4 && !strcmp(argv[1], "folder") && on_off(argv[3]) && !strchr(argv[2], '\t')) {
		snprintf(line, sizeof(line), "FOLDER\t%.32s\t%d", argv[2], !strcmp(argv[3], "on"));
		return request(line, false);
	}
	if (argc == 3 && (!strcmp(argv[1], "internet") || !strcmp(argv[1], "relays")) && on_off(argv[2])) {
		snprintf(line, sizeof(line), "%s\t%d", argv[1][0] == 'i' ? "INTERNET" : "RELAYS", !strcmp(argv[2], "on"));
		return request(line, false);
	}
	if ((argc == 3 || argc == 4) && (!strcmp(argv[1], "add") || !strcmp(argv[1], "accept"))) {
		char name[96] = "";

		if (argc == 4) {
			snprintf(name, sizeof(name), "%s", argv[3]);
			for (char *p = name; *p; p++)
				if (*p == '\t' || *p == '\n' || *p == '\r')
					*p = ' ';
		}
		snprintf(line, sizeof(line), "ADD\t%.72s%s%s", argv[2], name[0] ? "\t" : "", name);
		return request(line, false);
	}
	if (argc == 3 && (!strcmp(argv[1], "ignore") || !strcmp(argv[1], "remove"))) {
		snprintf(line, sizeof(line), "%s\t%.72s", argv[1][0] == 'i' ? "IGNORE" : "REMOVE", argv[2]);
		return request(line, false);
	}
	if ((argc == 4 && (!strcmp(argv[1], "folder-id") || !strcmp(argv[1], "use"))) ||
	    (argc == 3 && !strcmp(argv[1], "decline"))) {
		for (int i = 2; i < argc; i++)
			if (strpbrk(argv[i], "\t\n\r") || strlen(argv[i]) > 199)
				return puts("ERR value"), 1;
		if (argv[1][0] == 'f')
			snprintf(line, sizeof(line), "FOLDER_ID\t%s\t%s", argv[2], argv[3]);
		else if (argv[1][0] == 'u')
			snprintf(line, sizeof(line), "USE\t%s\t%s", argv[2], argv[3]);
		else
			snprintf(line, sizeof(line), "DECLINE\t%s", argv[2]);
		return request(line, false);
	}
	fprintf(stderr, "Usage: nuubos-syncctl status | enable | disable | folder KIND on|off | internet on|off | relays on|off |\n"
			"       add DEVICE [NAME] | ignore DEVICE | remove DEVICE |\n"
			"       folder-id KIND ID|- | use FOLDER KIND | decline FOLDER\n");
	return 2;
}
