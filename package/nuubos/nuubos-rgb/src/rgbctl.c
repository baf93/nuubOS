/* SPDX-License-Identifier: MIT */

#include "rgb_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define RGB_SOCKET_PATH "/run/nuubos/rgbd.sock"

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s status\n"
		"  %s off\n"
		"  %s brightness 0..100\n"
		"  %s color R G B\n"
		"  %s mode static|breathe|rainbow\n"
		"  %s frame R G B [R G B ...]\n",
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
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", RGB_SOCKET_PATH);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static int append_arg(char *command, size_t size, const char *arg)
{
	size_t used = strlen(command);
	int n;

	n = snprintf(command + used, size - used, "%s%s",
		     used ? " " : "", arg);

	if (n < 0 || (size_t)n >= size - used)
		return -1;

	return 0;
}

int main(int argc, char **argv)
{
	const struct nuubos_rgb_topology *topology;
	char command[1024] = { 0 };
	char reply[1024];
	ssize_t n;
	int fd;
	int i;

	topology = nuubos_rgb_detect_topology();

	if (!topology) {
		puts("RGB lighting is not supported on this device");
		return 0;
	}

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "status") == 0 && argc == 2) {
		snprintf(command, sizeof(command), "STATUS");
	} else if (strcmp(argv[1], "off") == 0 && argc == 2) {
		snprintf(command, sizeof(command), "OFF");
	} else if (strcmp(argv[1], "brightness") == 0 && argc == 3) {
		snprintf(command, sizeof(command), "BRIGHTNESS %s", argv[2]);
	} else if (strcmp(argv[1], "color") == 0 && argc == 5) {
		snprintf(command, sizeof(command),
			 "COLOR %s %s %s", argv[2], argv[3], argv[4]);
	} else if (strcmp(argv[1], "mode") == 0 && argc == 3) {
		snprintf(command, sizeof(command), "MODE %s", argv[2]);
	} else if (strcmp(argv[1], "frame") == 0 && argc >= 5) {
		snprintf(command, sizeof(command), "FRAME");

		for (i = 2; i < argc; i++) {
			if (append_arg(command, sizeof(command), argv[i]) < 0) {
				fprintf(stderr, "frame command too long\n");
				return 2;
			}
		}
	} else {
		usage(argv[0]);
		return 2;
	}

	fd = connect_daemon();
	if (fd < 0) {
		fprintf(stderr,
			"nuubos-rgbctl: RGB service is not running: %s\n",
			strerror(errno));
		return 1;
	}

	if (write(fd, command, strlen(command)) != (ssize_t)strlen(command)) {
		perror("nuubos-rgbctl write");
		close(fd);
		return 1;
	}

	n = read(fd, reply, sizeof(reply) - 1);
	close(fd);

	if (n < 0) {
		perror("nuubos-rgbctl read");
		return 1;
	}

	reply[n] = '\0';
	fputs(reply, stdout);

	return strncmp(reply, "ERR ", 4) == 0 ? 1 : 0;
}
