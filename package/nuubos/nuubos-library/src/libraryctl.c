/* SPDX-License-Identifier: MIT */
/*
 * nuubos-libraryctl — command-line client of nuubos-libraryd.
 *
 * Used by tooling, tests and future launchers; the UI talks to the socket
 * directly with the same commands.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/nuubos/libraryd.sock"

static void usage(void)
{
	fprintf(stderr,
		"Usage: nuubos-libraryctl COMMAND\n"
		"  status                         Home snapshot\n"
		"  watch                          print every snapshot change\n"
		"  scan                           rescan ROM storage\n"
		"  games SCOPE                    system:<id> | favorites | collection:<id> | recent\n"
		"  session-begin GAME             a game session started\n"
		"  session-end GAME               the running session ended\n"
		"  played GAME SECONDS            record a finished session\n"
		"  favorite GAME on|off\n"
		"  collection-create NAME\n"
		"  collection-rename ID NAME\n"
		"  collection-delete ID\n"
		"  collection-add ID GAME\n"
		"  collection-remove ID GAME\n");
}

static int connect_service(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Multi-line replies end with "end=1"; every other reply is one line. */
static int run(const char *command, bool multi, bool forever)
{
	int fd = connect_service();
	char buf[8192];
	size_t used = 0;
	int status = 0;

	if (fd < 0) {
		fprintf(stderr, "nuubos-libraryctl: cannot connect to %s: %s\n",
			SOCKET_PATH, strerror(errno));
		return 1;
	}
	if (!forever) {
		struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	if (write(fd, command, strlen(command)) < 0 || write(fd, "\n", 1) < 0) {
		close(fd);
		return 1;
	}
	for (;;) {
		ssize_t n = read(fd, buf + used, sizeof(buf) - used - 1);
		char *line, *nl;

		if (n <= 0)
			break;
		used += (size_t)n;
		buf[used] = '\0';
		line = buf;
		while ((nl = strchr(line, '\n')) != NULL) {
			*nl = '\0';
			puts(line);
			if (!strncmp(line, "ERR", 3))
				status = 1;
			if ((!multi || !strcmp(line, "end=1") || !strncmp(line, "ERR", 3)) &&
			    !forever) {
				close(fd);
				return status;
			}
			line = nl + 1;
		}
		used = strlen(line);
		memmove(buf, line, used + 1);
		if (used >= sizeof(buf) - 1)
			used = 0;
	}
	close(fd);
	return forever ? 0 : 1;
}

int main(int argc, char **argv)
{
	char command[1024];
	const char *verb;

	if (argc < 2) {
		usage();
		return 2;
	}
	verb = argv[1];
	if (!strcmp(verb, "status") && argc == 2)
		return run("STATUS", true, false);
	if (!strcmp(verb, "watch") && argc == 2)
		return run("SUBSCRIBE", true, true);
	if (!strcmp(verb, "scan") && argc == 2)
		return run("SCAN", false, false);
	if (!strcmp(verb, "games") && argc == 3) {
		snprintf(command, sizeof(command), "GAMES\t%s", argv[2]);
		return run(command, true, false);
	}
	if (!strcmp(verb, "session-begin") && argc == 3) {
		snprintf(command, sizeof(command), "SESSION_BEGIN\t%s", argv[2]);
		return run(command, false, false);
	}
	if (!strcmp(verb, "session-end") && argc == 3) {
		snprintf(command, sizeof(command), "SESSION_END\t%s", argv[2]);
		return run(command, false, false);
	}
	if (!strcmp(verb, "played") && argc == 4) {
		snprintf(command, sizeof(command), "SESSION_RECORD\t%s\t%s", argv[2], argv[3]);
		return run(command, false, false);
	}
	if (!strcmp(verb, "favorite") && argc == 4 &&
	    (!strcmp(argv[3], "on") || !strcmp(argv[3], "off"))) {
		snprintf(command, sizeof(command), "FAVORITE\t%s\t%d", argv[2],
			 !strcmp(argv[3], "on"));
		return run(command, false, false);
	}
	if (!strcmp(verb, "collection-create") && argc == 3) {
		snprintf(command, sizeof(command), "COLLECTION_CREATE\t%s", argv[2]);
		return run(command, false, false);
	}
	if (!strcmp(verb, "collection-rename") && argc == 4) {
		snprintf(command, sizeof(command), "COLLECTION_RENAME\t%s\t%s", argv[2], argv[3]);
		return run(command, false, false);
	}
	if (!strcmp(verb, "collection-delete") && argc == 3) {
		snprintf(command, sizeof(command), "COLLECTION_DELETE\t%s", argv[2]);
		return run(command, false, false);
	}
	if ((!strcmp(verb, "collection-add") || !strcmp(verb, "collection-remove")) && argc == 4) {
		snprintf(command, sizeof(command), "%s\t%s\t%s",
			 !strcmp(verb, "collection-add") ? "COLLECTION_ADD" : "COLLECTION_REMOVE",
			 argv[2], argv[3]);
		return run(command, false, false);
	}
	usage();
	return 2;
}
