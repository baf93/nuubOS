/* SPDX-License-Identifier: MIT */
/*
 * nuubos-rgbctl — development client of nuubos-rgbd. Product lighting
 * goes through nuubos-lightingd, which overwrites a frame set here as soon
 * as it renders again.
 */

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
		"  %s color R G B          every LED\n"
		"  %s locate N             position N of every ring white, N+1 dim\n"
		"  %s frame R G B [R G B ...]   logical order\n",
		argv0, argv0, argv0, argv0, argv0);
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

static int append(char *command, size_t size, const char *text)
{
	size_t used = strlen(command);
	int n = snprintf(command + used, size - used, " %s", text);

	return n < 0 || (size_t)n >= size - used ? -1 : 0;
}

static int append_rgb(char *command, size_t size, int r, int g, int b)
{
	char text[16];

	snprintf(text, sizeof(text), "%d %d %d", r, g, b);
	return append(command, size, text);
}

int main(int argc, char **argv)
{
	const struct nuubos_rgb_topology *topology;
	char command[1024] = { 0 };
	char reply[1024];
	unsigned int z, i;
	ssize_t n;
	int fd;

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
	} else if (strcmp(argv[1], "color") == 0 && argc == 5) {
		snprintf(command, sizeof(command), "FRAME");
		for (i = 0; i < topology->led_count; i++)
			if (append_rgb(command, sizeof(command), atoi(argv[2]),
				       atoi(argv[3]), atoi(argv[4])) < 0)
				return 2;
	} else if (strcmp(argv[1], "locate") == 0 && argc == 3) {
		unsigned int pos = (unsigned int)atoi(argv[2]);

		snprintf(command, sizeof(command), "FRAME");
		for (z = 0; z < topology->zone_count; z++) {
			unsigned int count = topology->zones[z].led_count;

			for (i = 0; i < count; i++) {
				int v = i == pos % count ? 255
					: i == (pos + 1) % count ? 24 : 0;

				if (append_rgb(command, sizeof(command), v, v, v) < 0)
					return 2;
			}
		}
	} else if (strcmp(argv[1], "frame") == 0 && argc >= 5) {
		snprintf(command, sizeof(command), "FRAME");

		for (i = 2; i < (unsigned int)argc; i++) {
			if (append(command, sizeof(command), argv[i]) < 0) {
				fprintf(stderr, "frame command too long\n");
				return 2;
			}
		}
	} else {
		usage(argv[0]);
		return 2;
	}

	if (strlen(command) + 1 >= sizeof(command))
		return 2;
	strcat(command, "\n");

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
