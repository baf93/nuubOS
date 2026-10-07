/* SPDX-License-Identifier: MIT */
/*
 * nuubos-webctl — command line client of nuubos-webd (tests, the
 * pre-power hook): sends one command line, prints the reply.
 *   nuubos-webctl status | watch | open ADDRESS | back | forward | reload |
 *                 zoom-in | zoom-out | zoom-reset | quit | bookmark-add [URL [TITLE]] |
 *                 bookmark-remove URL | history-clear | clear-data | pre-power ACTION
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif

int main(int argc, char **argv)
{
	struct sockaddr_un addr;
	struct timeval tv = { 12, 0 };
	char cmd[4096] = "", buf[4096];
	const char *verb = argc > 1 ? argv[1] : "";
	int multi = !strcmp(verb, "status") || !strcmp(verb, "watch");
	int watch = !strcmp(verb, "watch");
	int fd, ok = 0;

	if (!verb[0]) {
		fprintf(stderr, "Usage: nuubos-webctl status|watch|open ADDRESS|back|forward|reload|"
				"zoom-in|zoom-out|zoom-reset|quit|bookmark-add [URL [TITLE]]|bookmark-remove URL|history-clear|clear-data|pre-power ACTION\n");
		return 2;
	}
	if (watch)
		snprintf(cmd, sizeof(cmd), "SUBSCRIBE");
	else
		for (const char *p = verb; *p; p++) {
			char c = *p == '-' ? '_' : (*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
			size_t n = strlen(cmd);

			cmd[n] = c;
			cmd[n + 1] = '\0';
		}
	for (int i = 2; i < argc; i++) {
		strncat(cmd, "\t", sizeof(cmd) - strlen(cmd) - 1);
		strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
	}
	strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), RUN_ROOT "/webd.sock");
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		/* No Web Mode service: no browser runs, nothing to stop. */
		return !strcmp(verb, "pre-power") ? 0 : 1;
	}
	if (!watch)
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd))
		return 1;
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);

		if (n <= 0)
			break;
		buf[n] = '\0';
		fputs(buf, stdout);
		fflush(stdout);
		if (watch)
			continue;
		if (!multi) {
			ok = !strncmp(buf, "OK", 2);
			break;
		}
		if (strstr(buf, "end=1\n")) {
			ok = 1;
			break;
		}
	}
	close(fd);
	return ok ? 0 : 1;
}
