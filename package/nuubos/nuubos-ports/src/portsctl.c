/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-portsctl — command line client of nuubos-portsd (tests,
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

#define SOCKET_PATH "/run/nuubos/portsd.sock"
/* PRE_POWER waits for the session to end; never hold a lifecycle transition
 * longer. */
#define REPLY_TIMEOUT_S 15

static void usage(void)
{
	fprintf(stderr,
		"Usage: nuubos-portsctl COMMAND\n"
		"  status | watch\n"
		"  open                           PortMaster's interface\n"
		"  launch SCRIPT                  a port (\"<name>.sh\" of the ports folder)\n"
		"  launch-game ID                 the port of a library game\n"
		"  pause | resume | quit\n"
		"  portmaster check|install|cancel|remove|rollback\n"
		"  auto-update 0|1\n"
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
	} else if (!strcmp(verb, "open") && argc == 2) {
		snprintf(command, sizeof(command), "OPEN\n");
	} else if (!strcmp(verb, "launch") && argc == 3) {
		snprintf(command, sizeof(command), "LAUNCH\t%s\n", argv[2]);
	} else if (!strcmp(verb, "launch-game") && argc == 3) {
		snprintf(command, sizeof(command), "LAUNCH_GAME\t%s\n", argv[2]);
	} else if ((!strcmp(verb, "pause") || !strcmp(verb, "resume") || !strcmp(verb, "quit")) && argc == 2) {
		snprintf(command, sizeof(command), "%s\n",
			 verb[0] == 'p' ? "PAUSE" : verb[0] == 'r' ? "RESUME" : "QUIT");
	} else if (!strcmp(verb, "portmaster") && argc == 3 &&
		   (!strcmp(argv[2], "check") || !strcmp(argv[2], "install") || !strcmp(argv[2], "cancel") ||
		    !strcmp(argv[2], "remove") || !strcmp(argv[2], "rollback"))) {
		char upper[16];
		size_t i;

		for (i = 0; argv[2][i] && i + 1 < sizeof(upper); i++)
			upper[i] = (char)(argv[2][i] - 'a' + 'A');
		upper[i] = '\0';
		snprintf(command, sizeof(command), "PM_%s\n", upper);
	} else if (!strcmp(verb, "auto-update") && argc == 3) {
		snprintf(command, sizeof(command), "PM_AUTO_UPDATE\t%s\n", argv[2]);
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
		/* No ports service: no port runs, nothing to end. */
		if (!strcmp(verb, "pre-power"))
			return 0;
		fprintf(stderr, "nuubos-portsctl: %s\n", strerror(errno));
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
