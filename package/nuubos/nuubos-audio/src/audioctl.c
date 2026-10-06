/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/nuubos/audiod.sock"

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s status\n"
		"  %s route\n"
"  %s output\n"
		"  %s output auto|bluetooth|analog|hdmi\n"
		"  %s volume\n"
		"  %s volume speaker|headphones|bluetooth|hdmi\n"
		"  %s volume speaker|headphones|bluetooth 0..100\n"
		"  %s volume adjust -100..100\n"
		"  %s system-volume [0..100|adjust DELTA]\n"
		"  %s home-music-volume [0..100|adjust DELTA]\n"
		"  %s music start|stop|next\n"
		"  %s sfx navigation|select|back|quick-settings\n"
		"  %s test-sound start|stop|status\n"
		"  %s reset-defaults\n",
		argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
		argv0, argv0, argv0, argv0, argv0, argv0);
}

static int connect_daemon(void)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
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

int main(int argc, char **argv)
{
	char command[256];
	char reply[2048];
	ssize_t n;
	int fd;

	if (argc == 2 && strcmp(argv[1], "status") == 0) {
		snprintf(command, sizeof(command), "STATUS");
	} else if (argc == 2 && strcmp(argv[1], "route") == 0) {
		snprintf(command, sizeof(command), "ROUTE GET");
	} else if (argc == 2 && strcmp(argv[1], "output") == 0) {
		snprintf(command, sizeof(command), "OUTPUT GET");
	} else if (argc == 3 && strcmp(argv[1], "output") == 0) {
		snprintf(command, sizeof(command),
			 "OUTPUT SET %s", argv[2]);
	} else if (argc == 2 && strcmp(argv[1], "volume") == 0) {
		snprintf(command, sizeof(command), "VOLUME GET");
	} else if (argc == 3 && strcmp(argv[1], "volume") == 0) {
		snprintf(command, sizeof(command),
			 "VOLUME GET %s", argv[2]);
	} else if (argc == 4 && strcmp(argv[1], "volume") == 0 &&
		   strcmp(argv[2], "adjust") == 0) {
		snprintf(command, sizeof(command),
			 "VOLUME ADJUST %s", argv[3]);
	} else if (argc == 4 && strcmp(argv[1], "volume") == 0) {
		snprintf(command, sizeof(command),
			 "VOLUME SET %s %s", argv[2], argv[3]);
	} else if (argc == 2 && strcmp(argv[1], "system-volume") == 0) {
		snprintf(command, sizeof(command), "SYSTEM VOLUME GET");
	} else if (argc == 3 && strcmp(argv[1], "system-volume") == 0) {
		snprintf(command, sizeof(command), "SYSTEM VOLUME SET %s", argv[2]);
	} else if (argc == 4 && strcmp(argv[1], "system-volume") == 0 &&
		   strcmp(argv[2], "adjust") == 0) {
		snprintf(command, sizeof(command), "SYSTEM VOLUME ADJUST %s", argv[3]);
	} else if (argc == 2 && strcmp(argv[1], "home-music-volume") == 0) {
		snprintf(command, sizeof(command), "MUSIC VOLUME GET");
	} else if (argc == 3 && strcmp(argv[1], "home-music-volume") == 0) {
		snprintf(command, sizeof(command), "MUSIC VOLUME SET %s", argv[2]);
	} else if (argc == 4 && strcmp(argv[1], "home-music-volume") == 0 &&
		   strcmp(argv[2], "adjust") == 0) {
		snprintf(command, sizeof(command), "MUSIC VOLUME ADJUST %s", argv[3]);
	} else if (argc == 3 && strcmp(argv[1], "music") == 0) {
		if (strcmp(argv[2], "start") == 0)
			snprintf(command, sizeof(command), "MUSIC START");
		else if (strcmp(argv[2], "stop") == 0)
			snprintf(command, sizeof(command), "MUSIC STOP");
		else if (strcmp(argv[2], "next") == 0)
			snprintf(command, sizeof(command), "MUSIC NEXT");
		else {
			usage(argv[0]);
			return 2;
		}
	} else if (argc == 3 && strcmp(argv[1], "sfx") == 0) {
		snprintf(command, sizeof(command), "SFX PLAY %s", argv[2]);
	} else if (argc == 3 && strcmp(argv[1], "test-sound") == 0) {
		if (strcmp(argv[2], "start") == 0)
			snprintf(command, sizeof(command), "TEST START");
		else if (strcmp(argv[2], "stop") == 0)
			snprintf(command, sizeof(command), "TEST STOP");
		else if (strcmp(argv[2], "status") == 0)
			snprintf(command, sizeof(command), "TEST STATUS");
		else {
			usage(argv[0]);
			return 2;
		}
	} else if (argc == 2 && strcmp(argv[1], "reset-defaults") == 0) {
		snprintf(command, sizeof(command), "RESET DEFAULTS");
	} else {
		usage(argv[0]);
		return 2;
	}

	fd = connect_daemon();
	if (fd < 0) {
		fprintf(stderr,
			"nuubos-audioctl: audio service is not running: %s\n",
			strerror(errno));
		return 1;
	}

	if (write(fd, command, strlen(command)) !=
	    (ssize_t)strlen(command)) {
		perror("nuubos-audioctl write");
		close(fd);
		return 1;
	}

	n = read(fd, reply, sizeof(reply) - 1);
	close(fd);

	if (n < 0) {
		perror("nuubos-audioctl read");
		return 1;
	}

	reply[n] = '\0';
	fputs(reply, stdout);

	return strncmp(reply, "ERR ", 4) == 0 ? 1 : 0;
}
