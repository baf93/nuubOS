/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-ports-mount — the mount and umount commands inside the ports
 * sandbox (EPIC-058).
 *
 * Ports run without any privilege, so they cannot mount anything. Their
 * scripts still mount PortMaster's runtimes (squashfs images) and bind
 * their configuration folders: this program, first in the sandbox's PATH as
 * "mount" and "umount", passes those requests to nuubos-portsd, which
 * checks them and performs them inside the sandbox only:
 *
 *   mount [-t squashfs] [-o loop,ro] <file.squashfs> <dir>
 *   mount --bind|-o bind <dir> <dir>
 *   umount [-l|-f] <dir>
 *
 * Paths are made absolute here (the sandbox's view); portsd resolves them
 * again inside the sandbox and refuses anything outside /roms/ports and
 * /tmp. Exit status 0 on success, 32 like mount(8) on failure.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/nuubos-ports/mount.sock"
#define REPLY_TIMEOUT_S 30
#define MOUNT_FAILURE 32

static bool has_option(const char *list, const char *name)
{
	size_t len = strlen(name);

	for (const char *p = list; p && *p; ) {
		const char *end = strchr(p, ',');
		size_t n = end ? (size_t)(end - p) : strlen(p);

		if (n == len && !strncmp(p, name, len))
			return true;
		p = end ? end + 1 : NULL;
	}
	return false;
}

/* Absolute, without resolving links: portsd resolves inside the sandbox. */
static bool absolute(const char *path, char *out, size_t size)
{
	char cwd[PATH_MAX];

	if (!path[0])
		return false;
	if (path[0] == '/')
		return snprintf(out, size, "%s", path) < (int)size;
	if (!getcwd(cwd, sizeof(cwd)))
		return false;
	return snprintf(out, size, "%s/%s", cwd, path) < (int)size;
}

static int request(const char *line, const char *name)
{
	struct sockaddr_un addr;
	struct timeval tv = { REPLY_TIMEOUT_S, 0 };
	char answer[128];
	ssize_t n;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return MOUNT_FAILURE;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    write(fd, line, strlen(line)) != (ssize_t)strlen(line)) {
		fprintf(stderr, "%s: %s\n", name, strerror(errno));
		close(fd);
		return MOUNT_FAILURE;
	}
	n = read(fd, answer, sizeof(answer) - 1);
	close(fd);
	if (n <= 0) {
		fprintf(stderr, "%s: no answer\n", name);
		return MOUNT_FAILURE;
	}
	answer[n] = '\0';
	answer[strcspn(answer, "\n")] = '\0';
	if (!strcmp(answer, "OK"))
		return 0;
	fprintf(stderr, "%s: %s\n", name, !strncmp(answer, "ERR ", 4) ? answer + 4 : answer);
	return MOUNT_FAILURE;
}

static int do_umount(int argc, char **argv)
{
	char target[PATH_MAX];
	char line[PATH_MAX + 16];
	const char *path = NULL;

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-')
			continue; /* -l, -f, -v: always a lazy unmount */
		if (path) {
			fprintf(stderr, "umount: one path only\n");
			return MOUNT_FAILURE;
		}
		path = argv[i];
	}
	if (!path || !absolute(path, target, sizeof(target)) || strchr(target, '\t') || strchr(target, '\n')) {
		fprintf(stderr, "umount: bad path\n");
		return MOUNT_FAILURE;
	}
	snprintf(line, sizeof(line), "UMOUNT\t%s\n", target);
	return request(line, "umount");
}

static int do_mount(int argc, char **argv)
{
	char source[PATH_MAX], target[PATH_MAX];
	char line[2 * PATH_MAX + 32];
	const char *type = NULL;
	const char *paths[2] = { NULL, NULL };
	int count = 0;
	bool bind = false;
	bool remount = false;
	size_t len;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "--bind") || !strcmp(a, "-B") || !strcmp(a, "--rbind") || !strcmp(a, "-R")) {
			bind = true;
		} else if (!strcmp(a, "-t") && i + 1 < argc) {
			type = argv[++i];
		} else if (!strcmp(a, "-o") && i + 1 < argc) {
			const char *opts = argv[++i];

			bind |= has_option(opts, "bind") || has_option(opts, "rbind");
			remount |= has_option(opts, "remount");
			if (has_option(opts, "loop") && !type)
				type = "squashfs";
		} else if (a[0] == '-') {
			/* -r, -v, -n...: read-only is always applied */
		} else if (count < 2) {
			paths[count++] = a;
		} else {
			count++;
		}
	}
	if (remount || count != 2) {
		fprintf(stderr, "mount: not supported in the ports sandbox\n");
		return MOUNT_FAILURE;
	}
	if (!absolute(paths[0], source, sizeof(source)) || !absolute(paths[1], target, sizeof(target)) ||
	    strpbrk(source, "\t\n") || strpbrk(target, "\t\n")) {
		fprintf(stderr, "mount: bad path\n");
		return MOUNT_FAILURE;
	}
	len = strlen(source);
	if (bind) {
		snprintf(line, sizeof(line), "BIND\t%s\t%s\n", source, target);
	} else if ((type && !strcmp(type, "squashfs")) || (len > 9 && !strcmp(source + len - 9, ".squashfs"))) {
		snprintf(line, sizeof(line), "SQUASHFS\t%s\t%s\n", source, target);
	} else {
		fprintf(stderr, "mount: only squashfs images and bind mounts in the ports sandbox\n");
		return MOUNT_FAILURE;
	}
	return request(line, "mount");
}

int main(int argc, char **argv)
{
	const char *name = strrchr(argv[0], '/');

	name = name ? name + 1 : argv[0];
	if (!strcmp(name, "umount"))
		return do_umount(argc, argv);
	return do_mount(argc, argv);
}
