/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-castctl — command line client of nuubos-castd (tests, lifecycle
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

#define SOCKET_PATH "/run/nuubos/castd.sock"
/* PRE_POWER waits for the receiver to be released; bounded by castd. */
#define REPLY_TIMEOUT_S 10

static void usage(void)
{
	fprintf(stderr,
		"Usage: nuubos-castctl COMMAND\n"
		"  status | watch\n"
		"  discover                     look for receivers (TVs, Chromecast, PCs)\n"
		"  start RECEIVER | stop        cast the screen to a receiver id | stop\n"
		"  set audio 0|1 | set latency low|normal|smooth\n"
		"  h264 install|cancel|remove   Cisco OpenH264 for Miracast\n"
		"  pre-power sleep|restart|poweroff\n");
}

int main(int argc, char **argv)
{
	struct sockaddr_un addr;
	struct timeval tv = { REPLY_TIMEOUT_S, 0 };
	char command[512], buf[4096];
	const char *verb = argc > 1 ? argv[1] : "";
	int multi = 0, watch = 0, fd, ok = 0;

	if (!strcmp(verb, "status") && argc == 2) {
		snprintf(command, sizeof(command), "STATUS\n");
		multi = 1;
	} else if (!strcmp(verb, "watch") && argc == 2) {
		snprintf(command, sizeof(command), "SUBSCRIBE\n");
		watch = 1;
	} else if (!strcmp(verb, "discover") && argc == 2) {
		snprintf(command, sizeof(command), "DISCOVER\n");
	} else if (!strcmp(verb, "start") && argc == 3) {
		snprintf(command, sizeof(command), "START\t%s\n", argv[2]);
	} else if (!strcmp(verb, "stop") && argc == 2) {
		snprintf(command, sizeof(command), "STOP\n");
	} else if (!strcmp(verb, "set") && argc == 4) {
		snprintf(command, sizeof(command), "SET\t%s\t%s\n", argv[2], argv[3]);
	} else if (!strcmp(verb, "h264") && argc == 3 && !strcmp(argv[2], "install")) {
		snprintf(command, sizeof(command), "H264_INSTALL\n");
	} else if (!strcmp(verb, "h264") && argc == 3 && !strcmp(argv[2], "cancel")) {
		snprintf(command, sizeof(command), "H264_CANCEL\n");
	} else if (!strcmp(verb, "h264") && argc == 3 && !strcmp(argv[2], "remove")) {
		snprintf(command, sizeof(command), "H264_REMOVE\n");
	} else if (!strcmp(verb, "pre-power") && argc == 3) {
		snprintf(command, sizeof(command), "PRE_POWER\t%s\n", argv[2]);
	} else {
		usage();
		return 2;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "nuubos-castctl: castd not running\n");
		return 1;
	}
	if (!watch)
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (write(fd, command, strlen(command)) < 0) {
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
		if (multi) {
			if (strstr(buf, "end=1\n")) {
				ok = 1;
				break;
			}
			continue;
		}
		ok = !strncmp(buf, "OK", 2);
		break;
	}
	close(fd);
	return ok ? 0 : 1;
}
