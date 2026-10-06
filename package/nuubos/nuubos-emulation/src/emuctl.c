/* SPDX-License-Identifier: MIT */
/*
 * nuubos-emuctl — command line client of nuubos-emud (tests, lifecycle
 * hooks). Prints the reply; exits 0 on OK, 1 on ERR or no service.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/nuubos/emud.sock"
/* PRE_POWER waits for the automatic state and, before restart/poweroff,
 * for RetroArch to exit; never hold a lifecycle transition longer. */
#define REPLY_TIMEOUT_S 25

static void usage(void)
{
	fprintf(stderr,
		"Usage: nuubos-emuctl COMMAND\n"
		"  status | watch\n"
		"  launch GAME_ID\n"
		"  pause | resume | save | load | reset | fast-forward\n"
		"  slot N|+1|-1\n"
		"  advanced                       open RetroArch's own menu\n"
		"  quit\n"
		"  pre-power sleep|restart|poweroff\n");
}

int main(int argc, char **argv)
{
	struct sockaddr_un addr;
	struct timeval tv = { REPLY_TIMEOUT_S, 0 };
	char command[256];
	char buf[1024];
	const char *verb = argc > 1 ? argv[1] : "";
	int multi = 0;
	int watch = 0;
	int fd;
	int ok = 0;

	if (!strcmp(verb, "status") && argc == 2) {
		snprintf(command, sizeof(command), "STATUS\n");
		multi = 1;
	} else if (!strcmp(verb, "watch") && argc == 2) {
		snprintf(command, sizeof(command), "SUBSCRIBE\n");
		watch = 1;
	} else if (!strcmp(verb, "launch") && argc == 3) {
		snprintf(command, sizeof(command), "LAUNCH\t%s\n", argv[2]);
	} else if (!strcmp(verb, "slot") && argc == 3) {
		snprintf(command, sizeof(command), "SLOT\t%s\n", argv[2]);
	} else if (!strcmp(verb, "pre-power") && argc == 3) {
		snprintf(command, sizeof(command), "PRE_POWER\t%s\n", argv[2]);
	} else if (argc == 2) {
		static const char *const simple[][2] = {
			{ "pause", "PAUSE" }, { "resume", "RESUME" },
			{ "save", "SAVE_STATE" }, { "load", "LOAD_STATE" },
			{ "reset", "RESET" }, { "fast-forward", "FAST_FORWARD" },
			{ "advanced", "ADVANCED" }, { "quit", "QUIT" },
		};
		size_t i;

		for (i = 0; i < sizeof(simple) / sizeof(simple[0]); i++)
			if (!strcmp(verb, simple[i][0]))
				break;
		if (i == sizeof(simple) / sizeof(simple[0])) {
			usage();
			return 2;
		}
		snprintf(command, sizeof(command), "%s\n", simple[i][1]);
	} else {
		usage();
		return 2;
	}

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		/* No emulation service: nothing runs, nothing to protect. */
		if (!strcmp(verb, "pre-power"))
			return 0;
		fprintf(stderr, "nuubos-emuctl: %s\n", strerror(errno));
		return 1;
	}
	if (!watch)
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (write(fd, command, strlen(command)) != (ssize_t)strlen(command)) {
		close(fd);
		return 1;
	}

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
			ok = strncmp(buf, "OK", 2) == 0;
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
