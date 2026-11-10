/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-tsctl — command line of the Tailscale capability, a client of
 * nuubos-tsd (/run/nuubos/tsd.sock).
 *
 *   status                 the snapshot (see tsd.c), end=1
 *   install | check | cancel | remove
 *   enable | disable
 *   login | logout
 *
 * Prints OK or ERR <reason>; exit status 0 only for OK.
 */

#include <ctype.h>
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
#define SOCKET_PATH RUN_ROOT "/tsd.sock"

/* One request; prints the reply (until end=1 for a snapshot). */
static int request(const char *line, bool multi)
{
	struct sockaddr_un a;
	/* LOGIN/LOGOUT/REMOVE wait for tailscaled (a few local API calls). */
	struct timeval tv = { .tv_sec = 45 };
	char buf[4096], reply[8192] = "";
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
		if (got + (size_t)n < sizeof(reply)) {
			memcpy(reply + got, buf, (size_t)n);
			got += (size_t)n;
			reply[got] = '\0';
		}
		if (multi ? strstr(reply, "end=1\n") != NULL || !strncmp(reply, "ERR", 3) : strchr(reply, '\n') != NULL)
			break;
	}
	close(fd);
	fflush(stdout);
	if (!strncmp(reply, "ERR", 3))
		return 1;
	return multi ? strstr(reply, "end=1\n") == NULL : strncmp(reply, "OK", 2) != 0;
}

int main(int argc, char **argv)
{
	static const char *const verbs[] = { "install", "check", "cancel", "remove", "enable", "disable", "login", "logout" };
	char line[32];

	if (argc == 2 && !strcmp(argv[1], "status"))
		return request("STATUS", true);
	for (size_t i = 0; argc == 2 && i < sizeof(verbs) / sizeof(verbs[0]); i++)
		if (!strcmp(argv[1], verbs[i])) {
			size_t n = 0;

			for (const char *c = verbs[i]; *c; c++)
				line[n++] = (char)toupper((unsigned char)*c);
			line[n] = '\0';
			return request(line, false);
		}
	fprintf(stderr, "Usage: nuubos-tsctl status | install | check | cancel | remove | enable | disable | login | logout\n");
	return 2;
}
