/* SPDX-License-Identifier: MIT */
/*
 * nuubos-jobctl — command line client of nuubos-jobd (tests, scripts).
 *
 *   status | watch
 *   submit TYPE [ARG...]     prints the job id
 *   run TYPE [ARG...]        submit and wait; prints the result, exit 0 on success
 *   cancel ID | retry ID
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#define SOCKET_PATH RUN_ROOT "/jobd.sock"

static int connect_jobd(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "nuubos-jobctl: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int send_line(int fd, const char *line)
{
	size_t len = strlen(line);

	return write(fd, line, len) == (ssize_t)len ? 0 : -1;
}

/* Read one reply line into out. */
static int read_line(int fd, char *out, size_t size)
{
	size_t n = 0;

	while (n + 1 < size) {
		char ch;
		ssize_t r = read(fd, &ch, 1);

		if (r <= 0)
			return -1;
		if (ch == '\n')
			break;
		out[n++] = ch;
	}
	out[n] = '\0';
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "Usage: nuubos-jobctl status | watch | submit TYPE [ARG...] | "
			"run TYPE [ARG...] | cancel ID | retry ID\n");
}

int main(int argc, char **argv)
{
	char cmd[1400];
	char line[1024];
	const char *verb = argc > 1 ? argv[1] : "";
	int fd;

	if ((!strcmp(verb, "status") || !strcmp(verb, "watch")) && argc == 2) {
		int watch = !strcmp(verb, "watch");

		if ((fd = connect_jobd()) < 0 || send_line(fd, watch ? "SUBSCRIBE\n" : "STATUS\n"))
			return 1;
		while (read_line(fd, line, sizeof(line)) == 0) {
			puts(line);
			fflush(stdout);
			if (!watch && !strcmp(line, "end=1"))
				break;
		}
		close(fd);
		return 0;
	}
	if ((!strcmp(verb, "submit") || !strcmp(verb, "run")) && argc >= 3) {
		char id[32];
		int rc = 1;

		snprintf(cmd, sizeof(cmd), "SUBMIT\t%s", argv[2]);
		for (int i = 3; i < argc; i++) {
			strncat(cmd, "\t", sizeof(cmd) - strlen(cmd) - 1);
			strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
		}
		strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);
		if ((fd = connect_jobd()) < 0 || send_line(fd, cmd) || read_line(fd, line, sizeof(line)))
			return 1;
		if (strncmp(line, "OK ", 3)) {
			puts(line);
			close(fd);
			return 1;
		}
		snprintf(id, sizeof(id), "%.31s", line + 3);
		if (!strcmp(verb, "submit")) {
			puts(id);
			close(fd);
			return 0;
		}
		/* run: follow snapshots until this job is finished. */
		if (send_line(fd, "SUBSCRIBE\n")) {
			close(fd);
			return 1;
		}
		while (read_line(fd, line, sizeof(line)) == 0) {
			char *f[8];
			int n = 0;
			char *p = line;

			if (strncmp(line, "job=", 4))
				continue;
			p += 4;
			while (n < 8) {
				f[n++] = p;
				p = strchr(p, '\t');
				if (!p)
					break;
				*p++ = '\0';
			}
			if (n < 8 || strcmp(f[0], id))
				continue;
			if (!strcmp(f[2], "succeeded")) {
				puts(f[6]);
				rc = 0;
				break;
			}
			if (!strcmp(f[2], "failed") || !strcmp(f[2], "cancelled")) {
				printf("ERR %s %s\n", f[2], f[7]);
				break;
			}
		}
		close(fd);
		return rc;
	}
	if ((!strcmp(verb, "cancel") || !strcmp(verb, "retry")) && argc == 3) {
		snprintf(cmd, sizeof(cmd), "%s\t%s\n", !strcmp(verb, "cancel") ? "CANCEL" : "RETRY", argv[2]);
		if ((fd = connect_jobd()) < 0 || send_line(fd, cmd) || read_line(fd, line, sizeof(line)))
			return 1;
		puts(line);
		close(fd);
		return strncmp(line, "OK", 2) ? 1 : 0;
	}
	usage();
	return 2;
}
