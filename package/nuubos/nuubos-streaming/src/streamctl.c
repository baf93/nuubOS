/* SPDX-License-Identifier: MIT */
/*
 * nuubos-streamctl — command line client of nuubos-streamd (tests,
 * lifecycle hooks). Prints the reply; exits 0 on OK, 1 on ERR or no
 * service.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/nuubos/streamd.sock"
/* PRE_POWER waits for the stream to end; never hold a lifecycle transition
 * longer. */
#define REPLY_TIMEOUT_S 15

static void usage(void)
{
	fprintf(stderr,
		"Usage: nuubos-streamctl COMMAND\n"
		"  status | watch\n"
		"  discover                       look for hosts on the network\n"
		"  add ADDRESS | remove HOST | refresh [HOST]\n"
		"  pair HOST | pair-cancel\n"
		"  apps HOST                      load the host's applications\n"
		"  launch HOST APP_NAME\n"
		"  quit [close]                   end the stream (close: also the app)\n"
		"  stats\n"
		"  set resolution|fps|codec|bitrate VALUE\n"
		"  pre-power sleep|restart|poweroff\n");
}

int main(int argc, char **argv)
{
	struct sockaddr_un addr;
	struct timeval tv = { REPLY_TIMEOUT_S, 0 };
	char command[512];
	char buf[4096];
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
	} else if (!strcmp(verb, "discover") && argc == 2) {
		snprintf(command, sizeof(command), "DISCOVER\n");
	} else if (!strcmp(verb, "add") && argc == 3) {
		snprintf(command, sizeof(command), "ADD\t%s\n", argv[2]);
	} else if (!strcmp(verb, "remove") && argc == 3) {
		snprintf(command, sizeof(command), "REMOVE\t%s\n", argv[2]);
	} else if (!strcmp(verb, "refresh") && (argc == 2 || argc == 3)) {
		if (argc == 3)
			snprintf(command, sizeof(command), "REFRESH\t%s\n", argv[2]);
		else
			snprintf(command, sizeof(command), "REFRESH\n");
	} else if (!strcmp(verb, "pair") && argc == 3) {
		snprintf(command, sizeof(command), "PAIR\t%s\n", argv[2]);
	} else if (!strcmp(verb, "pair-cancel") && argc == 2) {
		snprintf(command, sizeof(command), "PAIR_CANCEL\n");
	} else if (!strcmp(verb, "apps") && argc == 3) {
		snprintf(command, sizeof(command), "APPS\t%s\n", argv[2]);
	} else if (!strcmp(verb, "launch") && argc == 4) {
		snprintf(command, sizeof(command), "LAUNCH\t%s\t%s\n", argv[2], argv[3]);
	} else if (!strcmp(verb, "quit") && argc == 2) {
		snprintf(command, sizeof(command), "QUIT\n");
	} else if (!strcmp(verb, "quit") && argc == 3 && !strcmp(argv[2], "close")) {
		snprintf(command, sizeof(command), "QUIT\tclose\n");
	} else if (!strcmp(verb, "stats") && argc == 2) {
		snprintf(command, sizeof(command), "STATS\n");
	} else if (!strcmp(verb, "set") && argc == 4) {
		snprintf(command, sizeof(command), "SET\t%s\t%s\n", argv[2], argv[3]);
	} else if (!strcmp(verb, "pre-power") && argc == 3) {
		snprintf(command, sizeof(command), "PRE_POWER\t%s\n", argv[2]);
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
		/* No streaming service: nothing streams, nothing to end. */
		if (!strcmp(verb, "pre-power"))
			return 0;
		fprintf(stderr, "nuubos-streamctl: %s\n", strerror(errno));
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
