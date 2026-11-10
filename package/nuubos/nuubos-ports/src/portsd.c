/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-portsd — Ports Service (EPIC-058).
 *
 * Owns PortMaster and the ports it installs. PortMaster (MIT) is never part
 * of the image: on the user's request nuubos-portmaster-get downloads its
 * stable release into /userdata/roms/ports/PortMaster, where ports look for
 * it (PM_CHECK, PM_INSTALL, PM_CANCEL, PM_REMOVE, PM_ROLLBACK, automatic
 * updates on a check, PM_AUTO_UPDATE). OPEN runs PortMaster's own interface,
 * LAUNCH <script> a port (a "<name>.sh" of the ports folder), LAUNCH_GAME
 * <id> the port of a library game (Home; Recently Played and play time go
 * to libraryd like emud's games): all are a
 * session like a game, with the nuubOS game gamepads on for its lifetime
 * (inputd connection); PAUSE/RESUME freeze and thaw it, QUIT ends it,
 * Restart/Power Off end it first (PRE_POWER); Sleep keeps it.
 *
 * Every session runs in a sandbox (bubblewrap) that cannot touch the rest of
 * the device, at native speed (kernel namespaces, no emulation): its own
 * user namespace (root inside, no capability at all), its own PID namespace
 * (it sees only its processes), the system read-only, of USERDATA only the
 * ports folder (/roms/ports, where port scripts expect it), a reduced /etc
 * without secrets, the GPU, the input devices, uinput, the Wayland and
 * PipeWire sockets and the network. Its processes are in their own cgroup:
 * the first the kernel kills when memory runs out, all together
 * (memory.oom.group), frozen for a pause, killed at once on quit.
 *
 * Port scripts mount PortMaster's runtimes (squashfs images) and bind their
 * configuration folders. Inside the sandbox "mount" and "umount" are
 * nuubos-ports-mount, which asks this service (/run/nuubos/ports-mount.sock,
 * reachable only from the sandbox): a worker enters the sandbox's mount
 * namespace and performs the request there if every path stays inside
 * /roms/ports or /tmp — squashfs images read-only, binds without setuid or
 * devices.
 *
 * Protocol (/run/nuubos/portsd.sock, one command per line, tab separated
 * arguments): STATUS and SUBSCRIBE return the snapshot (SUBSCRIBE pushes a
 * new one on every change); the other commands reply OK or ERR <reason> at
 * once and report progress in the snapshot; PRE_POWER
 * <sleep|restart|poweroff> replies once no session runs (at once for
 * sleep). MEDIA_ACCESS <script> [0|1] reads or sets whether that port sees
 * the Music and Videos folders (read only, /roms/music, /roms/videos).
 *
 * Idle: poll() without timeout. Timers exist only while a child process or
 * a session exit is pending.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/loop.h>
#include <linux/openat2.h>

#define RUN_ROOT "/run/nuubos"
#define SOCKET_PATH RUN_ROOT "/portsd.sock"
#define MOUNT_SOCKET RUN_ROOT "/ports-mount.sock"
#define PIDFILE RUN_ROOT "/portsd.pid"
#define SESSION_LOG RUN_ROOT "/ports.log"
#define INPUT_SOCKET RUN_ROOT "/inputd.sock"
#define LIBRARY_SOCKET RUN_ROOT "/libraryd.sock"
#define WAYLAND_SOCKET RUN_ROOT "/wayland-runtime/wayland-0"
#define PIPEWIRE_SOCKET RUN_ROOT "/pipewire/pipewire-0"

/* The console's own ports folder; the SD2 card's when the user chose it on
 * the PortMaster page (GitHub #21): ports_root, as libraryd resolves it
 * (PORTS_ROOT), "" while that card is out. PortMaster and nuubOS's
 * .nuubos live in it and move with it. */
#define CONSOLE_PORTS "/userdata/roms/ports"
#define LOCATION_FILE "/state/ports/location.conf"
static char ports_root[PATH_MAX] = CONSOLE_PORTS;
static bool ports_sd2;
/* The SD2 card's ports folder whatever the setting ("" without the card). */
static char sd2_ports[PATH_MAX];
/* A move (LOCATION): its folders, its direction; the ports the last one
 * left where they were (already at the destination), -1 before any. */
static char move_from[PATH_MAX], move_to[PATH_MAX];
static bool move_to_sd2;
static int move_conflicts = -1;

/* A path in the ports folder; a few at once are valid (rotating). */
static const char *in_ports(const char *sub)
{
	static char buf[4][PATH_MAX];
	static int k;

	k = (k + 1) % 4;
	snprintf(buf[k], sizeof(buf[k]), "%s%s", ports_root, sub);
	return buf[k];
}
#define PORTMASTER_DIR in_ports("/PortMaster")
/* nuubOS's PortMaster module in the image, and a line only its copies
 * carry (a release that ships its own module is never touched). */
#ifndef MOD_SOURCE
#define MOD_SOURCE "/usr/share/nuubos/ports/mod_nuubOS.txt"
#endif
#define MOD_INSTALLED in_ports("/PortMaster/mod_nuubOS.txt")
#define MOD_MARK "controllers are the nuubOS virtual gamepads"
/* nuubOS's own files in the ports folder (dot folder: not a port). */
#define STATE_DIR in_ports("/.nuubos")
#define HOME_DIR in_ports("/.nuubos/home")
#define PREVIOUS_DIR in_ports("/.nuubos/previous")
#define PORTS_CONF in_ports("/.nuubos/nuubos.conf")
/* The SD2 card is mounted noexec: a session's view of its ports folder is
 * a bind of it here with exec (still nosuid, nodev), for the sandbox only. */
#define SD2_PORTS_MOUNT RUN_ROOT "/ports-sd2"
#define PORTMASTER_GET "/usr/lib/nuubos/nuubos-portmaster-get"
#define PORTS_MOVE "/usr/lib/nuubos/nuubos-ports-move"

/* The sandbox's view. */
#define SB_PORTS "/roms/ports"
#define SB_HOME SB_PORTS "/.nuubos/home"
#define SB_RUNTIME "/run/user"
#define SB_MOUNT_DIR "/run/nuubos-ports"
#define SB_MOUNT_SOCKET SB_MOUNT_DIR "/mount.sock"
/* nuubos-ports-mount as mount/umount, first in the sandbox's PATH. */
#define SB_BIN "/usr/lib/nuubos/ports/bin"
#define BWRAP "/usr/bin/bwrap"
#define BASH "/bin/bash"

#define CGROUP_ROOT "/sys/fs/cgroup"
#define CGROUP_DIR CGROUP_ROOT "/nuubos-ports"
#define CGROUP_NAME "/nuubos-ports"

#define MAX_CLIENTS 16
#define MAX_LINE 1024
#define MAX_JOBS 4
#define CHECK_TIMEOUT_MS 40000
#define REMOVE_TIMEOUT_MS 60000
#define KILL_TIMEOUT_MS 3000
/* What PortMaster reads about the console (session_info()): the battery
 * level (its pugwash knows /tmp/battery.percent, not axp20x-battery) and
 * an os-release with VERSION= (else "nuubOS (0.0.0)"). */
#define BATTERY_SOURCE "/sys/class/power_supply/axp20x-battery/capacity"
#define BATTERY_FILE RUN_ROOT "/ports-battery.percent"
#define BATTERY_MS 60000
#define OS_RELEASE_FILE RUN_ROOT "/ports-os-release"
#define OUTPUT_APPLY "/usr/bin/nuubos-wl-output-apply"
/* Ports the user let read the Music and Videos folders (Game Details,
 * MEDIA_ACCESS): one script name per line, on STATE, out of the sandbox's
 * reach (the ports folder is writable from inside). */
#define MEDIA_ACCESS_DIR "/state/ports"
#define MEDIA_ACCESS_FILE MEDIA_ACCESS_DIR "/media-access.conf"
#define MUSIC_DIR "/userdata/music"
#define VIDEOS_DIR "/userdata/videos"
/* RetroArch-based ports (libretro cores run by /usr/bin/retroarch) use
 * this configuration: fullscreen (windowed, RetroArch draws its own title
 * bar). */
#define RA_CONFIG in_ports("/.nuubos/home/.config/retroarch/retroarch.cfg")
#define RA_CONFIG_TMP in_ports("/.nuubos/home/.config/retroarch/retroarch.cfg.tmp")

enum job_kind {
	JOB_NONE = 0,
	JOB_CHECK,    /* nuubos-portmaster-get check */
	JOB_INSTALL,  /* nuubos-portmaster-get install */
	JOB_REMOVE,   /* nuubos-portmaster-get remove */
	JOB_ROLLBACK, /* nuubos-portmaster-get rollback */
	JOB_MOVE,     /* nuubos-ports-move: Ports on SD2 switched */
	JOB_SESSION,  /* bwrap: PortMaster or a port */
};

struct job {
	enum job_kind kind;
	pid_t pid;
	int out_fd;
	char buf[MAX_LINE];
	size_t used;
	long long deadline_ms;
	bool term_sent;
	bool kill_sent;
	char error[32];
	char result[32];    /* @latest / @done version */
	bool ok;
};

struct client {
	int fd;
	bool subscribed;
	char buf[MAX_LINE];
	size_t used;
};

enum session_state { SESSION_IDLE = 0, SESSION_RUNNING, SESSION_EXITING };

struct session {
	enum session_state state;
	int job;            /* index in jobs[] */
	int input_fd;       /* inputd connection holding game gamepads on */
	bool portmaster;    /* PortMaster's interface, not a port */
	char port[256];     /* script name of the port */
	char game[17];      /* its library game id, "" for LAUNCH by name */
	bool paused;
	bool user_quit;
	long long kill_ms;  /* cgroup.kill after SIGTERM, 0 when none */
	long long battery_ms; /* next BATTERY_FILE refresh, 0 when none */
	/* The port script as launched (see session_relaunch()). */
	time_t script_mtime;
	off_t script_size;
	bool relaunched;
	int power_client;
};

static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];
static struct job jobs[MAX_JOBS];
static struct session session = { .power_client = -1 };
static bool cgroup_ready;

/* PortMaster (shared by all users). */
static char pm_version[32];   /* installed release, "" when absent */
static char pm_latest[32];    /* the stable channel's release, once checked */
static int pm_progress = -1;  /* download percent while installing */
static char pm_error[16];     /* last install failure */
static char pm_previous[32];  /* release kept for a rollback, "" when none */
static bool pm_auto_update = true;

static long long monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void handle_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void handle_sigchld(int sig)
{
	int saved = errno;

	(void)sig;
	if (sigchld_pipe[1] >= 0 && write(sigchld_pipe[1], "c", 1) < 0) {
		/* Pipe full: a wakeup is already pending. */
	}
	errno = saved;
}

static void copy_text(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

static void trim(char *text)
{
	size_t len = strlen(text);

	while (len > 0 && isspace((unsigned char)text[len - 1]))
		text[--len] = '\0';
}

/* Snapshot fields are line separated: never let a value break them. */
static void sanitize(char *text)
{
	for (char *p = text; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r')
			*p = ' ';
}

static int write_all(int fd, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = send(fd, data, len, MSG_NOSIGNAL);

		if (n < 0 && errno == ENOTSOCK)
			n = write(fd, data, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];

	copy_text(tmp, sizeof(tmp), path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	int rc;

	if (fd < 0)
		return -1;
	rc = write(fd, text, strlen(text)) == (ssize_t)strlen(text) ? 0 : -1;
	close(fd);
	return rc;
}

/* ------------------------------------------------------------------ */
/* PortMaster state                                                    */
/* ------------------------------------------------------------------ */

static void read_version_file(const char *dir, char *out, size_t size)
{
	char path[PATH_MAX];
	char line[64] = "";
	FILE *fp;

	out[0] = '\0';
	snprintf(path, sizeof(path), "%s/version", dir);
	fp = fopen(path, "r");
	if (!fp)
		return;
	if (fgets(line, sizeof(line), fp)) {
		trim(line);
		sanitize(line);
		copy_text(out, size, line);
	}
	fclose(fp);
}

/* A small text file, whole ("" when missing or too big). */
static size_t read_small(const char *path, char *buf, size_t size)
{
	FILE *fp = fopen(path, "rb");
	size_t n = 0;

	buf[0] = '\0';
	if (!fp)
		return 0;
	n = fread(buf, 1, size - 1, fp);
	if (!feof(fp))
		n = 0;
	fclose(fp);
	buf[n] = '\0';
	return n;
}

/* The module copied at install stays as it was: an image with a newer one
 * replaces the installed copy when it is nuubOS's own (2026-10-27). */
static void refresh_mod(void)
{
	static char shipped[65536], installed[65536];
	char tmp[PATH_MAX];
	size_t n = read_small(MOD_SOURCE, shipped, sizeof(shipped));
	FILE *fp;

	snprintf(tmp, sizeof(tmp), "%s.tmp", MOD_INSTALLED);
	if (!n || !read_small(MOD_INSTALLED, installed, sizeof(installed)) ||
	    !strstr(installed, MOD_MARK) || !strcmp(installed, shipped))
		return;
	fp = fopen(tmp, "wb");
	if (!fp)
		return;
	if (fwrite(shipped, 1, n, fp) != n || fclose(fp) != 0 || rename(tmp, MOD_INSTALLED) != 0) {
		unlink(tmp);
		log_msg("portsd: mod_nuubOS.txt update failed: %s", strerror(errno));
		return;
	}
	log_msg("portsd: mod_nuubOS.txt updated from the image");
}

static void read_portmaster(void)
{
	/* version alone (an interrupted removal) is not an installation. */
	if (ports_root[0] && access(in_ports("/PortMaster/PortMaster.sh"), R_OK) == 0) {
		read_version_file(PORTMASTER_DIR, pm_version, sizeof(pm_version));
		refresh_mod();
	} else {
		pm_version[0] = '\0';
	}
	read_version_file(PREVIOUS_DIR, pm_previous, sizeof(pm_previous));
}

static void load_conf(void)
{
	char line[64];
	FILE *fp = fopen(PORTS_CONF, "r");

	pm_auto_update = true;
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp))
		if (!strncmp(line, "AUTO_UPDATE=", 12))
			pm_auto_update = line[12] != '0';
	fclose(fp);
}

static int save_conf(void)
{
	char tmp[PATH_MAX];
	FILE *fp;

	if (!ports_root[0])
		return -1;
	snprintf(tmp, sizeof(tmp), "%s.tmp", PORTS_CONF);
	if (mkdir_p(STATE_DIR) != 0)
		return -1;
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "AUTO_UPDATE=%d\n", pm_auto_update ? 1 : 0);
	if (fclose(fp) != 0 || rename(tmp, PORTS_CONF) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* The PortMaster page's Ports on SD2 (GitHub #21). */
static bool location_sd2(void)
{
	char line[64];
	bool sd2 = false;
	FILE *fp = fopen(LOCATION_FILE, "r");

	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp))
		if (!strncmp(line, "LOCATION=sd2", 12))
			sd2 = true;
	fclose(fp);
	return sd2;
}

/* A slot for an SD2 card (the storage status says "none" without one). */
static bool sd2_slot(void)
{
	char line[128];
	bool slot = false;
	FILE *fp = fopen(RUN_ROOT "/storage/sd2.status", "r");

	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp))
		if (!strncmp(line, "SD2_STATE=", 10))
			slot = strncmp(line + 10, "none", 4) != 0;
	fclose(fp);
	return slot;
}

/* The setting, on STATE (out of the sandbox's reach). */
static int save_location(bool sd2)
{
	char tmp[] = LOCATION_FILE ".tmp";
	bool ok;
	FILE *fp;

	if (mkdir_p("/state/ports") != 0 || !(fp = fopen(tmp, "w")))
		return -1;
	fprintf(fp, "LOCATION=%s\n", sd2 ? "sd2" : "console");
	ok = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
	if (fclose(fp) != 0 || !ok || rename(tmp, LOCATION_FILE) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static bool sd2_mounted;

static void release_sd2_mount(void)
{
	if (sd2_mounted && umount2(SD2_PORTS_MOUNT, MNT_DETACH) != 0)
		log_msg("portsd: umount %s: %s", SD2_PORTS_MOUNT, strerror(errno));
	sd2_mounted = false;
}

/* What the sandbox binds as /roms/ports: the ports folder, or on the SD2
 * card (noexec) its bind with exec; NULL when that fails. */
static const char *sandbox_ports(void)
{
	if (!ports_sd2)
		return ports_root;
	if (sd2_mounted)
		return SD2_PORTS_MOUNT;
	if (mkdir(SD2_PORTS_MOUNT, 0700) != 0 && errno != EEXIST)
		return NULL;
	if (mount(ports_root, SD2_PORTS_MOUNT, NULL, MS_BIND, NULL) != 0) {
		log_msg("portsd: bind %s: %s", ports_root, strerror(errno));
		return NULL;
	}
	if (mount(NULL, SD2_PORTS_MOUNT, NULL, MS_REMOUNT | MS_BIND | MS_NOSUID | MS_NODEV, NULL) != 0) {
		log_msg("portsd: exec remount %s: %s", SD2_PORTS_MOUNT, strerror(errno));
		umount2(SD2_PORTS_MOUNT, MNT_DETACH);
		return NULL;
	}
	sd2_mounted = true;
	return SD2_PORTS_MOUNT;
}

static void library_request(const char *command, char *answer, size_t size);

/* The ports folder from libraryd (one owner of the card paths), else from
 * the setting alone; PortMaster's state and settings are the folder's. Only
 * while nothing runs in it. */
static void refresh_ports_root(void)
{
	char answer[PATH_MAX + 32], old[PATH_MAX];
	char *tab;

	copy_text(old, sizeof(old), ports_root);
	library_request("PORTS_ROOT", answer, sizeof(answer));
	if (!strncmp(answer, "OK ", 3) && (tab = strchr(answer + 3, '\t')) != NULL) {
		char *sd2 = strchr(tab + 1, '\t');

		*tab = '\0';
		if (sd2)
			*sd2++ = '\0';
		ports_sd2 = !strcmp(answer + 3, "sd2");
		copy_text(ports_root, sizeof(ports_root), tab + 1);
		copy_text(sd2_ports, sizeof(sd2_ports), sd2 ? sd2 : "");
	} else {
		ports_sd2 = location_sd2();
		copy_text(ports_root, sizeof(ports_root), ports_sd2 ? "" : CONSOLE_PORTS);
		sd2_ports[0] = '\0';
	}
	if (strcmp(old, ports_root)) {
		log_msg("portsd: ports folder %s", ports_root[0] ? ports_root : "(SD2 card out)");
		release_sd2_mount();
	}
	load_conf();
	read_portmaster();
}

/* PortMaster releases are dates: "2026.10.24-1611" > "2026.06.23-0015". */
static bool version_newer(const char *a, const char *b)
{
	while (*a || *b) {
		long x = strtol(a, (char **)&a, 10);
		long y = strtol(b, (char **)&b, 10);

		if (x != y)
			return x > y;
		if (*a == '.' || *a == '-')
			a++;
		if (*b == '.' || *b == '-')
			b++;
		if (!*a && !*b)
			break;
		if ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b)))
			break;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

static int pm_job(void)
{
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].kind == JOB_CHECK || jobs[i].kind == JOB_INSTALL ||
		    jobs[i].kind == JOB_REMOVE || jobs[i].kind == JOB_ROLLBACK || jobs[i].kind == JOB_MOVE)
			return i;
	return -1;
}

static const char *pm_job_name(void)
{
	int i = pm_job();

	if (i < 0)
		return "idle";
	switch (jobs[i].kind) {
	case JOB_CHECK:
		return "checking";
	case JOB_INSTALL:
		return "installing";
	case JOB_ROLLBACK:
		return "rolling-back";
	case JOB_MOVE:
		return "moving";
	default:
		return "removing";
	}
}

static const char *session_state_name(void)
{
	switch (session.state) {
	case SESSION_RUNNING:
		return "running";
	case SESSION_EXITING:
		return "exiting";
	default:
		return "idle";
	}
}

static char *build_status(size_t *len_out)
{
	size_t cap = 2048;
	char *out = malloc(cap);
	size_t used = 0;

#define APPEND(...) \
	do { \
		int _n = snprintf(out + used, cap - used, __VA_ARGS__); \
		if (_n > 0) \
			used += (size_t)_n < cap - used ? (size_t)_n : cap - used - 1; \
	} while (0)

	if (!out)
		return NULL;
	out[0] = '\0';
	APPEND("state=%s\n", session_state_name());
	APPEND("session=%s\n", session.state == SESSION_IDLE ? "" :
	       session.portmaster ? "portmaster" : "port");
	APPEND("session_port=%s\n", session.state == SESSION_IDLE ? "" : session.port);
	APPEND("session_game=%s\n", session.state == SESSION_IDLE ? "" : session.game);
	APPEND("paused=%d\n", session.paused ? 1 : 0);
	APPEND("portmaster=%s\n", pm_version[0] ? "installed" : "absent");
	APPEND("portmaster_version=%s\n", pm_version);
	APPEND("portmaster_latest=%s\n", pm_latest);
	APPEND("portmaster_job=%s\n", pm_job_name());
	APPEND("portmaster_progress=%d\n", pm_progress);
	APPEND("portmaster_error=%s\n", pm_error);
	APPEND("portmaster_previous=%s\n", pm_previous);
	APPEND("portmaster_auto_update=%d\n", pm_auto_update ? 1 : 0);
	/* Ports on SD2 (GitHub #21): offered with a card slot; "ready" = the
	 * folder is there (its card in). */
	APPEND("ports_location=%s\nports_ready=%d\nports_sd2_slot=%d\nports_conflicts=%d\n",
	       ports_sd2 ? "sd2" : "console", ports_root[0] ? 1 : 0, sd2_slot() ? 1 : 0, move_conflicts);
	APPEND("end=1\n");
#undef APPEND
	*len_out = used;
	return out;
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->subscribed = false;
	c->used = 0;
	if (session.power_client >= 0 && &clients[session.power_client] == c)
		session.power_client = -1;
}

static void reply(struct client *c, const char *text)
{
	if (c->fd >= 0 && write_all(c->fd, text, strlen(text)) != 0)
		close_client(c);
}

static void send_status(struct client *c)
{
	size_t len;
	char *status = build_status(&len);

	if (!status) {
		close_client(c);
		return;
	}
	if (write_all(c->fd, status, len) != 0)
		close_client(c);
	free(status);
}

static void notify_subscribers(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].subscribed)
			send_status(&clients[i]);
}

/* ------------------------------------------------------------------ */
/* Session cgroup                                                      */
/* ------------------------------------------------------------------ */

/* cgroup v2 at /sys/fs/cgroup (mounted here if nothing did), one group for
 * every process of the session. */
static void setup_cgroup(void)
{
	if (access(CGROUP_ROOT "/cgroup.controllers", F_OK) != 0 &&
	    mount("cgroup2", CGROUP_ROOT, "cgroup2", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		log_msg("portsd: cgroup2 unavailable: %s", strerror(errno));
		return;
	}
	if (mkdir(CGROUP_DIR, 0755) != 0 && errno != EEXIST) {
		log_msg("portsd: %s: %s", CGROUP_DIR, strerror(errno));
		return;
	}
	(void)write_text(CGROUP_ROOT "/cgroup.subtree_control", "+memory");
	/* Out of memory: the whole port goes, never half of it. */
	if (write_text(CGROUP_DIR "/memory.oom.group", "1") != 0)
		log_msg("portsd: memory.oom.group: %s", strerror(errno));
	cgroup_ready = true;
}

static void cgroup_freeze(bool on)
{
	if (cgroup_ready)
		(void)write_text(CGROUP_DIR "/cgroup.freeze", on ? "1" : "0");
}

/* SIGTERM (or SIGKILL) to every process of the session. */
static void cgroup_signal(int sig)
{
	char line[32];
	FILE *fp;

	if (!cgroup_ready)
		return;
	if (sig == SIGKILL && write_text(CGROUP_DIR "/cgroup.kill", "1") == 0)
		return;
	fp = fopen(CGROUP_DIR "/cgroup.procs", "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		pid_t pid = (pid_t)atoi(line);

		if (pid > 1)
			kill(pid, sig);
	}
	fclose(fp);
}

/* The process belongs to the running session. */
static bool in_session(pid_t pid)
{
	char path[64];
	char line[256];
	bool found = false;
	FILE *fp;

	if (session.state == SESSION_IDLE || pid <= 0)
		return false;
	snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
	fp = fopen(path, "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (!strcmp(line, "0::" CGROUP_NAME) || !strncmp(line, "0::" CGROUP_NAME "/", 3 + strlen(CGROUP_NAME) + 1))
			found = true;
	}
	fclose(fp);
	return found;
}

/* ------------------------------------------------------------------ */
/* Sandbox                                                             */
/* ------------------------------------------------------------------ */

static int connect_socket(const char *path, int timeout_ms)
{
	struct sockaddr_un addr;
	struct timeval tv;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	copy_text(addr.sun_path, sizeof(addr.sun_path), path);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Game gamepads stay on for as long as this connection is open: if portsd
 * dies, inputd gives the controllers back to the nuubUI by itself. */
static int input_gamepads_on(void)
{
	static const char command[] = "GAMEPADS ON\n";
	char answer[64];
	size_t used = 0;
	int fd = connect_socket(INPUT_SOCKET, 1000);

	if (fd < 0)
		return -1;
	if (write_all(fd, command, strlen(command)) != 0) {
		close(fd);
		return -1;
	}
	while (used + 1 < sizeof(answer)) {
		char c;
		ssize_t n = read(fd, &c, 1);

		if (n <= 0 || c == '\n')
			break;
		answer[used++] = c;
	}
	answer[used] = '\0';
	if (strcmp(answer, "OK") != 0) {
		close(fd);
		return -1;
	}
	(void)fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

/* One request to libraryd, its first line back ("" on failure). */
static void library_request(const char *command, char *answer, size_t size)
{
	int fd = connect_socket(LIBRARY_SOCKET, 1000);
	size_t used = 0;

	answer[0] = '\0';
	if (fd < 0)
		return;
	if (write_all(fd, command, strlen(command)) == 0 && write_all(fd, "\n", 1) == 0) {
		while (used + 1 < size) {
			char ch;

			if (read(fd, &ch, 1) != 1 || ch == '\n')
				break;
			answer[used++] = ch;
		}
	}
	answer[used] = '\0';
	close(fd);
}

/* Recently Played and play time, as for a game of emud. */
static void library_session(const char *verb, const char *game)
{
	char command[64];
	char answer[64];

	snprintf(command, sizeof(command), "%s\t%s", verb, game);
	library_request(command, answer, sizeof(answer));
	if (strcmp(answer, "OK") != 0)
		log_msg("portsd: libraryd %s %s failed: %s", verb, game, answer[0] ? answer : "no reply");
}

/* What the sandbox sees, then the command (inside paths). */
/* A port script name as LAUNCH takes it: "<name>.sh" of the ports folder. */
static bool valid_port_name(const char *port)
{
	size_t len = strlen(port);

	return len >= 4 && len < 256 && !strcmp(port + len - 3, ".sh") &&
	       !strchr(port, '/') && !strchr(port, '\n') && port[0] != '.';
}

static bool media_access(const char *port)
{
	char line[300];
	FILE *fp = fopen(MEDIA_ACCESS_FILE, "r");
	bool on = false;

	if (!fp)
		return false;
	while (!on && fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\n")] = '\0';
		on = !strcmp(line, port);
	}
	fclose(fp);
	return on;
}

static int set_media_access(const char *port, bool on)
{
	char line[300];
	FILE *in, *out;

	if (mkdir_p(MEDIA_ACCESS_DIR) != 0)
		return -1;
	out = fopen(MEDIA_ACCESS_FILE ".tmp", "w");
	if (!out)
		return -1;
	in = fopen(MEDIA_ACCESS_FILE, "r");
	while (in && fgets(line, sizeof(line), in)) {
		line[strcspn(line, "\n")] = '\0';
		if (line[0] && strcmp(line, port))
			fprintf(out, "%s\n", line);
	}
	if (in)
		fclose(in);
	if (on)
		fprintf(out, "%s\n", port);
	if (fclose(out) != 0)
		return -1;
	return rename(MEDIA_ACCESS_FILE ".tmp", MEDIA_ACCESS_FILE);
}

/* The host folder bound as /roms/ports (sandbox_ports(), set before the
 * fork). */
static char sandbox_src[PATH_MAX];

static void sandbox_exec(const char *script)
{
	const char *port = strrchr(script, '/') ? strrchr(script, '/') + 1 : script;
	static char *argv[160];
	int n = 0;

#define ARG(x) (argv[n++] = (char *)(x))
#define ARG2(a, b) do { ARG(a); ARG(b); } while (0)
#define ARG3(a, b, c) do { ARG(a); ARG(b); ARG(c); } while (0)
	ARG(BWRAP);
	/* Root inside, nothing outside: no capability survives. */
	ARG("--unshare-user");
	ARG3("--uid", "0", "--gid");
	ARG("0");
	ARG("--unshare-pid");
	ARG("--unshare-ipc");
	ARG("--unshare-uts");
	ARG("--unshare-cgroup-try");
	ARG("--die-with-parent");
	ARG2("--cap-drop", "ALL");
	/* The system, read-only. */
	ARG3("--ro-bind", "/usr", "/usr");
	ARG3("--ro-bind", "/bin", "/bin");
	ARG3("--ro-bind", "/sbin", "/sbin");
	ARG3("--ro-bind", "/lib", "/lib");
	ARG3("--symlink", "lib", "/lib64");
	/* /etc without secrets (no shadow, Wi-Fi, SSH or nuubOS state). */
	ARG2("--dir", "/etc");
	ARG3("--ro-bind-try", "/etc/alsa", "/etc/alsa");
	ARG3("--ro-bind-try", "/etc/asound.conf", "/etc/asound.conf");
	ARG3("--ro-bind-try", "/etc/fonts", "/etc/fonts");
	ARG3("--ro-bind-try", "/etc/ssl", "/etc/ssl");
	ARG3("--ro-bind", "/etc/passwd", "/etc/passwd");
	ARG3("--ro-bind", "/etc/group", "/etc/group");
	ARG3("--ro-bind-try", "/etc/hostname", "/etc/hostname");
	ARG3("--ro-bind-try", "/etc/hosts", "/etc/hosts");
	ARG3("--ro-bind-try", "/etc/nsswitch.conf", "/etc/nsswitch.conf");
	ARG3("--ro-bind", access(OS_RELEASE_FILE, R_OK) == 0 ? OS_RELEASE_FILE : "/etc/os-release", "/etc/os-release");
	ARG3("--ro-bind-try", "/etc/protocols", "/etc/protocols");
	ARG3("--ro-bind-try", "/etc/services", "/etc/services");
	ARG3("--ro-bind-try", "/etc/localtime", "/etc/localtime");
	ARG3("--ro-bind-try", "/etc/resolv.conf", "/etc/resolv.conf");
	ARG2("--proc", "/proc");
	ARG2("--dev", "/dev");
	ARG3("--dev-bind", "/dev/dri", "/dev/dri");
	ARG3("--dev-bind", "/dev/input", "/dev/input");
	ARG3("--dev-bind-try", "/dev/uinput", "/dev/uinput");
	/* SDL finds the devices through sysfs and udev's database. */
	ARG3("--ro-bind", "/sys", "/sys");
	ARG2("--tmpfs", "/run");
	ARG3("--ro-bind-try", "/run/udev", "/run/udev");
	ARG2("--dir", SB_RUNTIME);
	ARG3("--bind", WAYLAND_SOCKET, SB_RUNTIME "/wayland-0");
	/* At the system's own path: /etc/asound.conf names it (ALSA, and SDL
	 * through ALSA, reach PipeWire there). */
	ARG3("--bind-try", PIPEWIRE_SOCKET, PIPEWIRE_SOCKET);
	ARG2("--dir", SB_MOUNT_DIR);
	ARG3("--bind", MOUNT_SOCKET, SB_MOUNT_SOCKET);
	ARG2("--tmpfs", "/tmp");
	ARG3("--ro-bind-try", BATTERY_FILE, "/tmp/battery.percent");
	/* Of USERDATA, the ports folder only. */
	ARG2("--dir", "/roms");
	ARG3("--bind", sandbox_src, SB_PORTS);
	/* Read only, and only for a port the user allowed (never PortMaster's
	 * own interface). */
	if (strcmp(port, "PortMaster.sh") && media_access(port)) {
		ARG3("--ro-bind-try", MUSIC_DIR, "/roms/music");
		ARG3("--ro-bind-try", VIDEOS_DIR, "/roms/videos");
	}
	ARG2("--chdir", SB_PORTS);
	ARG(BASH);
	ARG(script);
	argv[n] = NULL;
#undef ARG
#undef ARG2
#undef ARG3

	clearenv();
	setenv("PATH", SB_BIN ":/usr/bin:/bin:/usr/sbin:/sbin", 1);
	setenv("HOME", SB_HOME, 1);
	setenv("XDG_RUNTIME_DIR", SB_RUNTIME, 1);
	setenv("WAYLAND_DISPLAY", "wayland-0", 1);
	setenv("PIPEWIRE_RUNTIME_DIR", RUN_ROOT "/pipewire", 1);
	setenv("SDL_VIDEODRIVER", "wayland", 1);
	/* No window frames (labwc draws none, rc.xml): SDL would draw its own
	 * with libdecor, a desktop title bar over the port. */
	setenv("SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR", "0", 1);
	/* Only inputd's game gamepads (see streamd: SDL's HIDAPI drivers
	 * would open the real controller past inputd's grab). */
	setenv("SDL_JOYSTICK_HIDAPI", "0", 1);
	setenv("LANG", "C.UTF-8", 1);
	setenv("TERM", "linux", 1);
	execv(BWRAP, argv);
}

/* The battery level for PortMaster, rewritten in place: the sandbox binds
 * this file, a new one would not reach it. */
static void battery_refresh(void)
{
	char level[16] = "";
	FILE *fp = fopen(BATTERY_SOURCE, "r");
	int fd;

	if (fp) {
		if (!fgets(level, sizeof(level), fp))
			level[0] = '\0';
		fclose(fp);
	}
	fd = open(BATTERY_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (level[0] && write(fd, level, strlen(level)) < 0)
		log_msg("portsd: battery level not written");
	close(fd);
}

/* The system's os-release with VERSION= for PortMaster. */
static void write_os_release(void)
{
	char line[256], version[64] = "";
	FILE *in = fopen("/etc/os-release", "r");
	FILE *out = fopen(OS_RELEASE_FILE ".tmp", "w");
	bool has_version = false;

	if (!in || !out) {
		if (in)
			fclose(in);
		if (out)
			fclose(out);
		return;
	}
	while (fgets(line, sizeof(line), in)) {
		fputs(line, out);
		if (!strncmp(line, "VERSION=", 8))
			has_version = true;
		else if (!strncmp(line, "VERSION_ID=", 11))
			sscanf(line + 11, "%63[^\n]", version);
	}
	if (!has_version && version[0])
		fprintf(out, "VERSION=\"%s\"\n", version);
	fclose(in);
	if (fclose(out) == 0)
		(void)rename(OS_RELEASE_FILE ".tmp", OS_RELEASE_FILE);
}

/* video_fullscreen = "true" in RA_CONFIG, the rest left to RetroArch. */
static void retroarch_fullscreen(void)
{
	char line[1024];
	FILE *in = fopen(RA_CONFIG, "r");
	FILE *out;
	bool found = false;

	if (mkdir_p(in_ports("/.nuubos/home/.config/retroarch")) != 0)
		return;
	out = fopen(RA_CONFIG_TMP, "w");
	if (!out) {
		if (in)
			fclose(in);
		return;
	}
	while (in && fgets(line, sizeof(line), in)) {
		if (!strncmp(line, "video_fullscreen ", 17) || !strncmp(line, "video_fullscreen=", 17)) {
			fputs("video_fullscreen = \"true\"\n", out);
			found = true;
		} else {
			fputs(line, out);
		}
	}
	if (!found)
		fputs("video_fullscreen = \"true\"\n", out);
	if (in)
		fclose(in);
	if (fclose(out) == 0)
		(void)rename(RA_CONFIG_TMP, RA_CONFIG);
}

/* The built-in panel at its native resolution while a session runs
 * (nuubos-wl-output-apply --native-panel: SDL saw 640x640 on the CubeXX's
 * 720x720 at the UI's 1.125 scale), the UI scale again after. */
static void panel_native(bool native)
{
	pid_t pid = fork();

	if (pid == 0) {
		setenv("XDG_RUNTIME_DIR", RUN_ROOT "/wayland-runtime", 1);
		setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		execl(OUTPUT_APPLY, OUTPUT_APPLY, native ? "--native-panel" : "--panel-scale", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
}

/*
 * A port whose runtime is missing: PortMaster's runtime_check downloads it
 * and rewrites the script while bash is still reading it, so the first
 * launch ended in a syntax error and only the second played. A session
 * that failed on its own (not quit, no Restart/Power Off waiting) after its
 * script changed is launched once more.
 */
static bool session_relaunch(int status)
{
	char host[PATH_MAX];
	struct stat st;

	if (session.portmaster || session.user_quit || session.relaunched ||
	    session.power_client >= 0 || status == 0 || !session.port[0])
		return false;
	snprintf(host, sizeof(host), "%s/%s", ports_root, session.port);
	if (stat(host, &st) != 0)
		return false;
	return st.st_mtime != session.script_mtime || st.st_size != session.script_size;
}

static void reset_session(void)
{
	/* A Restart/Power Off waiting for this session's end is answered
	 * after the reset (finish_power_wait()): without it the lifecycle hook
	 * waited for nuubos-portsctl's 15 s timeout. */
	int power_client = session.power_client;

	if (session.input_fd >= 0)
		close(session.input_fd);
	memset(&session, 0, sizeof(session));
	session.state = SESSION_IDLE;
	session.job = -1;
	session.input_fd = -1;
	session.power_client = power_client;
}

static int free_job(void)
{
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].kind == JOB_NONE)
			return i;
	return -1;
}

static const char *launch_session(const char *port)
{
	char script[PATH_MAX];
	char host[PATH_MAX];
	struct stat st;
	int slot;
	pid_t pid;

	if (session.state != SESSION_IDLE)
		return "busy";
	/* The folder now (an SD2 card may have come or gone). */
	if (pm_job() < 0)
		refresh_ports_root();
	if (!pm_version[0])
		return "unavailable";
	/* Install, removal or rollback change PortMaster's files; a check
	 * does not. */
	if (pm_job() >= 0 && jobs[pm_job()].kind != JOB_CHECK)
		return "unavailable";
	if (!cgroup_ready)
		return "sandbox";
	if (port) {
		size_t len = strlen(port);

		/* A script of the ports folder itself, by name. */
		if (len < 4 || len >= sizeof(session.port) || strcmp(port + len - 3, ".sh") != 0 ||
		    strchr(port, '/') || port[0] == '.')
			return "port";
		snprintf(host, sizeof(host), "%s/%s", ports_root, port);
		if (stat(host, &st) != 0 || !S_ISREG(st.st_mode))
			return "port";
		snprintf(script, sizeof(script), SB_PORTS "/%s", port);
	} else {
		snprintf(script, sizeof(script), SB_PORTS "/PortMaster/PortMaster.sh");
	}
	if (mkdir_p(HOME_DIR) != 0)
		return "write";
	slot = free_job();
	if (slot < 0)
		return "busy";
	if (!sandbox_ports())
		return "sandbox";
	copy_text(sandbox_src, sizeof(sandbox_src), sandbox_ports());

	session.input_fd = input_gamepads_on();
	if (session.input_fd < 0)
		log_msg("portsd: inputd game gamepads unavailable");
	battery_refresh();
	write_os_release();
	retroarch_fullscreen();
	panel_native(true);
	pid = fork();
	if (pid < 0) {
		reset_session();
		panel_native(false);
		return "spawn";
	}
	if (pid == 0) {
		sigset_t none;
		char text[16];
		int fd;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		snprintf(text, sizeof(text), "%d", (int)getpid());
		if (write_text(CGROUP_DIR "/cgroup.procs", text) != 0)
			_exit(126);
		/* The first process the kernel kills when memory runs out. */
		(void)write_text("/proc/self/oom_score_adj", "1000");
		fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			dup2(fd, STDIN_FILENO);
		fd = open(SESSION_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
		}
		sandbox_exec(script);
		_exit(127);
	}
	memset(&jobs[slot], 0, sizeof(jobs[slot]));
	jobs[slot].kind = JOB_SESSION;
	jobs[slot].pid = pid;
	jobs[slot].out_fd = -1;
	jobs[slot].deadline_ms = -1;
	session.state = SESSION_RUNNING;
	session.job = slot;
	session.battery_ms = monotonic_ms() + BATTERY_MS;
	if (port) {
		session.script_mtime = st.st_mtime;
		session.script_size = st.st_size;
	}
	session.portmaster = port == NULL;
	copy_text(session.port, sizeof(session.port), port ? port : "");
	log_msg("portsd: session %s pid=%d", port ? port : "PortMaster", (int)pid);
	return NULL;
}

static void request_quit(void)
{
	if (session.state != SESSION_RUNNING)
		return;
	session.user_quit = true;
	session.state = SESSION_EXITING;
	/* A frozen process cannot handle SIGTERM. */
	cgroup_freeze(false);
	session.paused = false;
	cgroup_signal(SIGTERM);
	session.kill_ms = monotonic_ms() + KILL_TIMEOUT_MS;
	notify_subscribers();
}

static void finish_power_wait(void)
{
	struct client *c;

	if (session.power_client < 0 || session.state != SESSION_IDLE)
		return;
	c = &clients[session.power_client];
	session.power_client = -1;
	reply(c, "OK\n");
}

static void process_input_link(void)
{
	static char line[64];
	static size_t used;
	char buf[128];
	ssize_t n = read(session.input_fd, buf, sizeof(buf));

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		/* inputd restarted: take the gamepads again. */
		close(session.input_fd);
		used = 0;
		session.input_fd = input_gamepads_on();
		return;
	}
	/* Quit (H + Start) ends the port like a game; the other emulation
	 * chords mean nothing to a port: ignored. */
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			line[used] = '\0';
			if (!strcmp(line, "HOTKEY quit"))
				request_quit();
			used = 0;
		} else if (used + 1 < sizeof(line)) {
			line[used++] = buf[i];
		} else {
			used = 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Mounts asked from inside the sandbox                                */
/* ------------------------------------------------------------------ */

static int make_server(const char *path, mode_t mode)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	copy_text(addr.sun_path, sizeof(addr.sun_path), path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    chmod(path, mode) != 0 || listen(fd, 8) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * Opens an inside path (absolute) only if it resolves inside /roms/ports or
 * /tmp: no "..", no link leaves the folder. Called inside the sandbox's
 * mount namespace.
 */
static int open_inside(const char *path, int flags)
{
	static const char *const roots[] = { SB_PORTS, "/tmp" };
	struct open_how how;

	for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		size_t len = strlen(roots[i]);
		const char *rel;
		int dir, fd;

		if (strncmp(path, roots[i], len) != 0 || (path[len] != '/' && path[len] != '\0'))
			continue;
		rel = path + len;
		while (*rel == '/')
			rel++;
		if (!*rel)
			rel = ".";
		dir = open(roots[i], O_PATH | O_DIRECTORY | O_CLOEXEC);
		if (dir < 0)
			return -1;
		memset(&how, 0, sizeof(how));
		how.flags = (uint64_t)(flags | O_CLOEXEC);
		how.mode = (flags & O_CREAT) ? 0644 : 0;
		how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
		fd = (int)syscall(SYS_openat2, dir, rel, &how, sizeof(how));
		close(dir);
		return fd;
	}
	errno = EPERM;
	return -1;
}

/* A free loop device backed (read-only) by the image; released with the
 * last mount that uses it. */
static int loop_attach(int image, char *dev, size_t size)
{
	struct loop_config config;
	int control = open("/dev/loop-control", O_RDWR | O_CLOEXEC);

	if (control < 0)
		return -1;
	for (int attempt = 0; attempt < 8; attempt++) {
		int n = ioctl(control, LOOP_CTL_GET_FREE);
		int fd;

		if (n < 0)
			break;
		snprintf(dev, size, "/dev/loop%d", n);
		fd = open(dev, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			break;
		memset(&config, 0, sizeof(config));
		config.fd = (uint32_t)image;
		config.info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
		if (ioctl(fd, LOOP_CONFIGURE, &config) == 0) {
			close(control);
			return fd;
		}
		close(fd);
		if (errno != EBUSY)
			break;
	}
	close(control);
	return -1;
}

/*
 * PortMaster's Ren'Py runtimes ship config.gl_enable = False: Ren'Py's
 * software renderer, one frame every ~20 s on the H700. Panfrost runs its
 * GLES2 renderer, so a renpy_*.squashfs mount gets, over its
 * renpy/config.py, a copy with gl_enable = True, config.mipmap (their
 * build reads it, their config.py lacks it; True = upstream Ren'Py) and
 * the GLES2 renderer on Panfrost itself: port scripts point SDL at gl4es
 * (SDL_VIDEO_GL/EGL_DRIVER, LD_LIBRARY_PATH), whose desktop GL drew a
 * black window, so SDL gets Mesa's libraries by their full paths.
 * Called in the sandbox's mount namespace after the mount. Best effort:
 * the port starts either way.
 */
static void renpy_enable_gl(const char *image, const char *mountpoint)
{
	static const char key[] = "\ngl_enable = False\n";
	static const char copy[] = "/tmp/.nuubos-renpy-config.py";
	static const char gles2[] =
		"\n# nuubOS: GLES2 on Panfrost, not desktop GL through gl4es.\n"
		"import os\n"
		"os.environ[\"SDL_VIDEO_GL_DRIVER\"] = \"/usr/lib/libGLESv2.so.2\"\n"
		"os.environ[\"SDL_VIDEO_EGL_DRIVER\"] = \"/usr/lib/libEGL.so.1\"\n"
		"os.environ[\"RENPY_RENDERER\"] = \"gles2\"\n";
	static char text[256 * 1024];
	const char *name = strrchr(image, '/');
	const char *at;
	char path[PATH_MAX];
	size_t n = 0;
	ssize_t got;
	int fd, src, dst, mfd;
	bool ok;

	name = name ? name + 1 : image;
	if (strncmp(name, "renpy_", 6) != 0)
		return;
	snprintf(path, sizeof(path), "%s/renpy/config.py", mountpoint);
	if ((fd = open_inside(path, O_RDONLY)) < 0)
		return;
	while (n < sizeof(text) - 1 && (got = read(fd, text + n, sizeof(text) - 1 - n)) > 0)
		n += (size_t)got;
	close(fd);
	text[n] = '\0';
	if (n >= sizeof(text) - 1 || (at = strstr(text, key)) == NULL)
		return;
	if ((fd = open_inside(copy, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return;
	ok = write_all(fd, text, (size_t)(at - text)) == 0 &&
	     write_all(fd, "\ngl_enable = True\n", 18) == 0 &&
	     write_all(fd, at + strlen(key), n - (size_t)(at - text) - strlen(key)) == 0 &&
	     (strstr(text, "\nmipmap =") || write_all(fd, "\nmipmap = True\n", 15) == 0) &&
	     write_all(fd, gles2, strlen(gles2)) == 0;
	close(fd);
	src = ok ? open_inside(copy, O_PATH) : -1;
	dst = ok ? open_inside(path, O_PATH) : -1;
	mfd = src >= 0 && dst >= 0 ?
	      open_tree(src, "", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH) : -1;
	if (mfd < 0 || move_mount(mfd, "", dst, "", MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) != 0)
		log_msg("portsd: Ren'Py OpenGL for %s failed: %s", name, strerror(errno));
	else
		log_msg("portsd: Ren'Py OpenGL on for %s", name);
	if (mfd >= 0)
		close(mfd);
	if (src >= 0)
		close(src);
	if (dst >= 0)
		close(dst);
}

/*
 * The worker: in a child, enters the mount namespace of the process that
 * asked (a member of the session) and performs one request there.
 */
static const char *mount_request(pid_t peer, char *line)
{
	char *op = line;
	char *a = NULL;
	char *b = NULL;
	char path[64];
	int host_ns, sandbox_ns;
	int src = -1, dst = -1, mfd = -1, loop = -1;
	const char *error = NULL;

	if ((a = strchr(op, '\t')) != NULL) {
		*a++ = '\0';
		if ((b = strchr(a, '\t')) != NULL)
			*b++ = '\0';
	}
	if (!a || a[0] != '/' || (b && b[0] != '/'))
		return "request";
	host_ns = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
	snprintf(path, sizeof(path), "/proc/%d/ns/mnt", (int)peer);
	sandbox_ns = open(path, O_RDONLY | O_CLOEXEC);
	if (host_ns < 0 || sandbox_ns < 0 || setns(sandbox_ns, CLONE_NEWNS) != 0)
		return "namespace";

	if (!strcmp(op, "SQUASHFS") && b) {
		char dev[32];
		struct stat st;
		size_t len = strlen(a);
		int fs;

		if (len <= 9 || strcmp(a + len - 9, ".squashfs") != 0)
			return "image";
		src = open_inside(a, O_RDONLY);
		dst = open_inside(b, O_PATH | O_DIRECTORY);
		if (src < 0 || dst < 0 || fstat(src, &st) != 0 || !S_ISREG(st.st_mode))
			return "path";
		/* The loop device and the filesystem come from the system's
		 * /dev; the mount goes back into the sandbox. */
		if (setns(host_ns, CLONE_NEWNS) != 0)
			return "namespace";
		loop = loop_attach(src, dev, sizeof(dev));
		if (loop < 0)
			return "loop";
		fs = fsopen("squashfs", FSOPEN_CLOEXEC);
		if (fs < 0 || fsconfig(fs, FSCONFIG_SET_STRING, "source", dev, 0) != 0 ||
		    fsconfig(fs, FSCONFIG_SET_FLAG, "ro", NULL, 0) != 0 ||
		    fsconfig(fs, FSCONFIG_CMD_CREATE, NULL, NULL, 0) != 0 ||
		    (mfd = fsmount(fs, FSMOUNT_CLOEXEC,
				   MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV)) < 0)
			error = "image";
		if (fs >= 0)
			close(fs);
		close(loop);
		if (error)
			return error;
		if (setns(sandbox_ns, CLONE_NEWNS) != 0)
			return "namespace";
		if (move_mount(mfd, "", dst, "", MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) != 0)
			return "mount";
		renpy_enable_gl(a, b);
		return NULL;
	}
	if (!strcmp(op, "BIND") && b) {
		struct mount_attr attr = { .attr_set = MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV };

		src = open_inside(a, O_PATH);
		dst = open_inside(b, O_PATH);
		if (src < 0 || dst < 0)
			return "path";
		mfd = open_tree(src, "", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
		if (mfd < 0 || mount_setattr(mfd, "", AT_EMPTY_PATH, &attr, sizeof(attr)) != 0 ||
		    move_mount(mfd, "", dst, "", MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) != 0)
			return "mount";
		return NULL;
	}
	if (!strcmp(op, "UMOUNT") && !b) {
		dst = open_inside(a, O_PATH);
		if (dst < 0)
			return "path";
		close(dst);
		if (umount2(a, MNT_DETACH | UMOUNT_NOFOLLOW) != 0)
			return errno == EINVAL ? "not-mounted" : "umount";
		return NULL;
	}
	return "request";
}

static void accept_mount(int server)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);
	int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC);
	pid_t pid;

	if (fd < 0)
		return;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || !in_session(cred.pid)) {
		(void)write_all(fd, "ERR denied\n", 11);
		close(fd);
		return;
	}
	pid = fork();
	if (pid == 0) {
		struct timeval tv = { 5, 0 };
		char line[2 * PATH_MAX + 32];
		const char *error;
		ssize_t n;
		char text[64];

		signal(SIGCHLD, SIG_DFL);
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		n = read(fd, line, sizeof(line) - 1);
		if (n <= 0)
			_exit(1);
		line[n] = '\0';
		line[strcspn(line, "\n")] = '\0';
		error = mount_request(cred.pid, line);
		if (error)
			snprintf(text, sizeof(text), "ERR %s\n", error);
		else
			snprintf(text, sizeof(text), "OK\n");
		(void)write_all(fd, text, strlen(text));
		_exit(error ? 1 : 0);
	}
	if (pid < 0)
		(void)write_all(fd, "ERR spawn\n", 10);
	close(fd);
}

/* ------------------------------------------------------------------ */
/* PortMaster helper jobs                                              */
/* ------------------------------------------------------------------ */

static bool start_pm_job(enum job_kind kind)
{
	char *argv[4] = { (char *)PORTMASTER_GET, NULL, NULL, NULL };
	long long timeout = -1;
	int out_pipe[2];
	int slot = free_job();
	pid_t pid;

	switch (kind) {
	case JOB_CHECK:
		argv[1] = (char *)"check";
		timeout = CHECK_TIMEOUT_MS;
		break;
	case JOB_INSTALL:
		/* No overall limit: the helper aborts a stalled download. */
		argv[1] = (char *)"install";
		argv[2] = ports_root;
		break;
	case JOB_REMOVE:
		argv[1] = (char *)"remove";
		argv[2] = ports_root;
		timeout = REMOVE_TIMEOUT_MS;
		break;
	case JOB_ROLLBACK:
		argv[1] = (char *)"rollback";
		argv[2] = ports_root;
		timeout = REMOVE_TIMEOUT_MS;
		break;
	case JOB_MOVE:
		/* No overall limit: gigabytes of ports on a slow card. */
		argv[0] = (char *)PORTS_MOVE;
		argv[1] = move_from;
		argv[2] = move_to;
		break;
	default:
		return false;
	}
	if (slot < 0 || pipe2(out_pipe, O_CLOEXEC) != 0)
		return false;
	pid = fork();
	if (pid < 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		return false;
	}
	if (pid == 0) {
		sigset_t none;
		int fd;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			dup2(fd, STDIN_FILENO);
		dup2(out_pipe[1], STDOUT_FILENO);
		clearenv();
		setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
		execv(argv[0], argv);
		_exit(127);
	}
	close(out_pipe[1]);
	memset(&jobs[slot], 0, sizeof(jobs[slot]));
	jobs[slot].kind = kind;
	jobs[slot].pid = pid;
	jobs[slot].out_fd = out_pipe[0];
	jobs[slot].deadline_ms = timeout > 0 ? monotonic_ms() + timeout : -1;
	(void)fcntl(jobs[slot].out_fd, F_SETFL, O_NONBLOCK);
	log_msg("portsd: portmaster %s pid=%d", argv[1], (int)pid);
	return true;
}

static void handle_job_line(int slot, char *line)
{
	struct job *j = &jobs[slot];

	if (line[0] != '@')
		return;
	if (!strncmp(line, "@error ", 7)) {
		size_t len = strcspn(line + 7, " ");

		if (len >= sizeof(j->error))
			len = sizeof(j->error) - 1;
		memmove(j->error, line + 7, len);
		j->error[len] = '\0';
	} else if (!strncmp(line, "@progress ", 10)) {
		int percent = atoi(line + 10);

		if (percent != pm_progress) {
			pm_progress = percent;
			notify_subscribers();
		}
	} else if (!strncmp(line, "@latest ", 8) || !strncmp(line, "@done", 5)) {
		const char *value = line[1] == 'l' ? line + 8 : line + 5;

		while (*value == ' ')
			value++;
		copy_text(j->result, sizeof(j->result), value);
		trim(j->result);
		sanitize(j->result);
		j->ok = true;
	}
}

/* Returns the bytes read; closes the pipe at end of file. */
static ssize_t read_job(int slot)
{
	struct job *j = &jobs[slot];
	char buf[1024];
	ssize_t n = read(j->out_fd, buf, sizeof(buf));

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return -1;
		close(j->out_fd);
		j->out_fd = -1;
		return 0;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			j->buf[j->used] = '\0';
			handle_job_line(slot, j->buf);
			j->used = 0;
		} else if (j->used + 1 < sizeof(j->buf)) {
			j->buf[j->used++] = buf[i];
		} else {
			j->used = 0;
		}
	}
	return n;
}

static void finish_job(int slot, int status)
{
	struct job done;
	struct job *j = &done;
	bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	/* Everything the process wrote before it exited, then free the slot
	 * (a follow-up job may need it). */
	while (jobs[slot].out_fd >= 0 && read_job(slot) > 0)
		;
	if (jobs[slot].out_fd >= 0)
		close(jobs[slot].out_fd);
	done = jobs[slot];
	memset(&jobs[slot], 0, sizeof(jobs[slot]));
	jobs[slot].kind = JOB_NONE;
	jobs[slot].out_fd = -1;

	switch (j->kind) {
	case JOB_CHECK:
		if (j->ok && exited_ok)
			copy_text(pm_latest, sizeof(pm_latest), j->result);
		/* Automatic updates: a newer release installs right away (never
		 * under a running session); the replaced files stay for a
		 * rollback. */
		if (j->ok && exited_ok && pm_auto_update && pm_version[0] &&
		    version_newer(pm_latest, pm_version) && session.state == SESSION_IDLE &&
		    start_pm_job(JOB_INSTALL)) {
			pm_progress = 0;
			pm_error[0] = '\0';
			log_msg("portsd: portmaster automatic update %s -> %s", pm_version, pm_latest);
		}
		break;
	case JOB_INSTALL:
		pm_progress = -1;
		read_portmaster();
		if (j->ok && exited_ok) {
			copy_text(pm_latest, sizeof(pm_latest), j->result);
			pm_error[0] = '\0';
		} else if (strcmp(j->error, "cancelled") != 0) {
			copy_text(pm_error, sizeof(pm_error), j->error[0] ? j->error : "write");
		}
		log_msg("portsd: portmaster install status=%d error=%s version=%s", status,
			j->error[0] ? j->error : "-", pm_version[0] ? pm_version : "-");
		break;
	case JOB_MOVE:
		pm_progress = -1;
		if (j->ok && exited_ok) {
			char answer[16];

			move_conflicts = atoi(j->result);
			/* The folder the ports are in now. */
			if (save_location(move_to_sd2) != 0)
				copy_text(pm_error, sizeof(pm_error), "write");
			pm_latest[0] = '\0';
			library_request("SCAN", answer, sizeof(answer));
			refresh_ports_root();
		} else if (strcmp(j->error, "cancelled") != 0) {
			copy_text(pm_error, sizeof(pm_error), j->error[0] ? j->error : "write");
		}
		log_msg("portsd: ports move %s -> %s status=%d error=%s conflicts=%d", move_from, move_to, status,
			j->error[0] ? j->error : "-", move_conflicts);
		break;
	case JOB_ROLLBACK:
	case JOB_REMOVE:
		read_portmaster();
		log_msg("portsd: portmaster %s status=%d version=%s",
			j->kind == JOB_REMOVE ? "remove" : "rollback", status,
			pm_version[0] ? pm_version : "-");
		break;
	case JOB_SESSION: {
		char port[sizeof(session.port)], game[sizeof(session.game)];
		bool relaunch = session_relaunch(status);
		bool portmaster = session.portmaster;

		log_msg("portsd: session %s exited status=%d user_quit=%d",
			session.portmaster ? "PortMaster" : session.port, status, session.user_quit ? 1 : 0);
		/* Nothing of the session survives it. */
		cgroup_freeze(false);
		cgroup_signal(SIGKILL);
		if (session.game[0])
			library_session("SESSION_END", session.game);
		copy_text(port, sizeof(port), session.port);
		copy_text(game, sizeof(game), session.game);
		reset_session();
		if (relaunch && !launch_session(port)) {
			log_msg("portsd: %s rewritten while it ran, launched again", port);
			session.relaunched = true;
			copy_text(session.game, sizeof(session.game), game);
			if (game[0])
				library_session("SESSION_BEGIN", game);
			break;
		}
		panel_native(false);
		release_sd2_mount();
		/* PortMaster may have updated itself. */
		read_portmaster();
		/* Ports it installed or removed reach Home now, not at the next
		 * start (community report 2026-10-26). */
		if (portmaster) {
			char answer[16];

			library_request("SCAN", answer, sizeof(answer));
			if (strcmp(answer, "OK") != 0)
				log_msg("portsd: libraryd SCAN failed: %s", answer[0] ? answer : "no reply");
		}
		finish_power_wait();
		break;
	}
	default:
		break;
	}
	notify_subscribers();
}

static void reap_children(void)
{
	char drain[32];
	int status;
	pid_t pid;

	while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
		;
	/* Mount workers are not jobs: just reaped. */
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		for (int i = 0; i < MAX_JOBS; i++) {
			if (jobs[i].kind != JOB_NONE && jobs[i].pid == pid) {
				finish_job(i, status);
				break;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void handle_command(struct client *c, char *line)
{
	char *args[2] = { NULL, NULL };
	char *p;
	int argc = 0;

	trim(line);
	p = strchr(line, '\t');
	if (p) {
		*p++ = '\0';
		while (p && argc < 2) {
			args[argc++] = p;
			p = strchr(p, '\t');
			if (p)
				*p++ = '\0';
		}
	}

	if (!strcmp(line, "STATUS")) {
		send_status(c);
	} else if (!strcmp(line, "SUBSCRIBE")) {
		c->subscribed = true;
		send_status(c);
	} else if (!strcmp(line, "PRE_POWER") && argc == 1) {
		if (strcmp(args[0], "sleep") && strcmp(args[0], "restart") && strcmp(args[0], "poweroff")) {
			reply(c, "ERR action\n");
			return;
		}
		/* A port sleeps with the console (the kernel freezes it) and
		 * goes on after the wake; Restart/Power Off end it first. */
		if (session.state == SESSION_IDLE || !strcmp(args[0], "sleep")) {
			reply(c, "OK\n");
			return;
		}
		if (session.power_client >= 0) {
			reply(c, "ERR busy\n");
			return;
		}
		session.power_client = (int)(c - clients);
		request_quit();
	} else if (!strcmp(line, "OPEN") || (!strcmp(line, "LAUNCH") && argc == 1)) {
		const char *error = launch_session(argc == 1 ? args[0] : NULL);

		if (error) {
			char text[64];

			log_msg("portsd: launch refused: %s", error);
			snprintf(text, sizeof(text), "ERR %s\n", error);
			reply(c, text);
			return;
		}
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "LAUNCH_GAME") && argc == 1) {
		/* A library game of the Ports system (Home): its script must be
		 * one of the ports folder itself. */
		char command[64], answer[PATH_MAX + 64], text[64];
		const char *error = NULL;
		const char *name = NULL;
		char *path;

		if (strlen(args[0]) != 16 || strspn(args[0], "0123456789abcdef") != 16) {
			reply(c, "ERR game\n");
			return;
		}
		/* portsd starts before the SD2 card is mounted: the folder now,
		 * as launch_session() does, or the path below never matches. */
		if (session.state == SESSION_IDLE && pm_job() < 0)
			refresh_ports_root();
		snprintf(command, sizeof(command), "RESOLVE\t%s", args[0]);
		library_request(command, answer, sizeof(answer));
		/* "unavailable" stays PortMaster's absence: a game whose card
		 * is gone is just not there. */
		if (strncmp(answer, "OK ports\t", 9) != 0)
			error = "game";
		else if (!ports_root[0] || strncmp((path = answer + 9), ports_root, strlen(ports_root)) != 0 ||
			 path[strlen(ports_root)] != '/' || strchr(path + strlen(ports_root) + 1, '/'))
			error = "port";
		else
			name = path + strlen(ports_root) + 1;
		if (!error)
			error = launch_session(name);
		if (error) {
			log_msg("portsd: launch of game %s refused: %s", args[0], error);
			snprintf(text, sizeof(text), "ERR %s\n", error);
			reply(c, text);
			return;
		}
		copy_text(session.game, sizeof(session.game), args[0]);
		library_session("SESSION_BEGIN", session.game);
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "MEDIA_ACCESS") && (argc == 1 || argc == 2)) {
		/* MEDIA_ACCESS <script> -> OK 0|1; MEDIA_ACCESS <script> 0|1 sets
		 * it (from the next launch). */
		if (!valid_port_name(args[0])) {
			reply(c, "ERR port\n");
		} else if (argc == 1) {
			reply(c, media_access(args[0]) ? "OK 1\n" : "OK 0\n");
		} else if (strcmp(args[1], "0") && strcmp(args[1], "1")) {
			reply(c, "ERR value\n");
		} else if (set_media_access(args[0], args[1][0] == '1') != 0) {
			reply(c, "ERR write\n");
		} else {
			log_msg("portsd: %s music and videos %s", args[0], args[1][0] == '1' ? "allowed" : "denied");
			reply(c, "OK\n");
		}
	} else if (!strcmp(line, "PAUSE") || !strcmp(line, "RESUME")) {
		bool pause = !strcmp(line, "PAUSE");

		if (session.state != SESSION_RUNNING) {
			reply(c, "ERR no-session\n");
			return;
		}
		cgroup_freeze(pause);
		session.paused = pause;
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "QUIT")) {
		if (session.state != SESSION_RUNNING) {
			reply(c, "ERR no-session\n");
			return;
		}
		request_quit();
		reply(c, "OK\n");
	} else if (!strcmp(line, "PM_CHECK")) {
		if (pm_job() < 0 && !start_pm_job(JOB_CHECK)) {
			reply(c, "ERR spawn\n");
			return;
		}
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "PM_INSTALL") || !strcmp(line, "PM_REMOVE") ||
		   !strcmp(line, "PM_ROLLBACK")) {
		enum job_kind kind = line[3] == 'I' ? JOB_INSTALL : line[3] == 'R' && line[4] == 'E' ?
				     JOB_REMOVE : JOB_ROLLBACK;

		if (pm_job() >= 0 || session.state != SESSION_IDLE) {
			reply(c, "ERR busy\n");
			return;
		}
		refresh_ports_root();
		/* Ports on an SD2 card that is out (GitHub #21). */
		if (!ports_root[0] || (kind == JOB_INSTALL && mkdir_p(ports_root) != 0)) {
			reply(c, "ERR card\n");
			notify_subscribers();
			return;
		}
		if (kind == JOB_ROLLBACK && !pm_previous[0]) {
			reply(c, "ERR unavailable\n");
			return;
		}
		if (!start_pm_job(kind)) {
			reply(c, "ERR spawn\n");
			return;
		}
		pm_error[0] = '\0';
		pm_progress = kind == JOB_INSTALL ? 0 : -1;
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "PM_CANCEL")) {
		int i = pm_job();

		if (i >= 0 && (jobs[i].kind == JOB_INSTALL || jobs[i].kind == JOB_MOVE) && !jobs[i].term_sent) {
			kill(-jobs[i].pid, SIGTERM);
			jobs[i].term_sent = true;
			jobs[i].deadline_ms = monotonic_ms() + KILL_TIMEOUT_MS;
		}
		reply(c, "OK\n");
	} else if (!strcmp(line, "LOCATION") && argc == 1) {
		/* LOCATION console|sd2 (PortMaster page, GitHub #21): the ports
		 * and PortMaster move to that card (nuubos-ports-move, a job like
		 * a download, PM_CANCEL stops it), then are used there. Never
		 * under a session or another job; both cards must be in. */
		bool sd2 = !strcmp(args[0], "sd2");

		if (!sd2 && strcmp(args[0], "console")) {
			reply(c, "ERR location\n");
			return;
		}
		if (pm_job() >= 0 || session.state != SESSION_IDLE) {
			reply(c, "ERR busy\n");
			return;
		}
		refresh_ports_root();
		if (sd2 == ports_sd2) {
			reply(c, "OK\n");
			return;
		}
		if (!ports_root[0] || !sd2_ports[0]) {
			reply(c, "ERR card\n");
			return;
		}
		copy_text(move_from, sizeof(move_from), ports_root);
		copy_text(move_to, sizeof(move_to), sd2 ? sd2_ports : CONSOLE_PORTS);
		move_to_sd2 = sd2;
		if (!start_pm_job(JOB_MOVE)) {
			reply(c, "ERR spawn\n");
			return;
		}
		pm_error[0] = '\0';
		pm_progress = 0;
		move_conflicts = -1;
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "REFRESH")) {
		/* The PortMaster page opens: an SD2 card may have come or gone. */
		if (pm_job() < 0 && session.state == SESSION_IDLE)
			refresh_ports_root();
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "PM_AUTO_UPDATE") && argc == 1) {
		pm_auto_update = strcmp(args[0], "0") != 0;
		reply(c, save_conf() == 0 ? "OK\n" : "ERR write\n");
		notify_subscribers();
	} else {
		reply(c, "ERR unknown\n");
	}
}

static void process_client(struct client *c)
{
	char buf[512];
	ssize_t n = read(c->fd, buf, sizeof(buf));

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close_client(c);
		return;
	}
	for (ssize_t i = 0; i < n && c->fd >= 0; i++) {
		if (buf[i] == '\n') {
			c->buf[c->used] = '\0';
			handle_command(c, c->buf);
			c->used = 0;
		} else if (c->used + 1 < sizeof(c->buf)) {
			c->buf[c->used++] = buf[i];
		} else {
			c->used = 0;
		}
	}
}

static void accept_client(int server)
{
	int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

	if (fd < 0)
		return;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd >= 0)
			continue;
		memset(&clients[i], 0, sizeof(clients[i]));
		clients[i].fd = fd;
		return;
	}
	close(fd);
}

/* ------------------------------------------------------------------ */
/* Timers: only while something is pending                             */
/* ------------------------------------------------------------------ */

static int next_timeout_ms(void)
{
	long long now = monotonic_ms();
	long long deadline = -1;

#define CONSIDER(t) do { if ((t) > 0 && (deadline < 0 || (t) < deadline)) deadline = (t); } while (0)
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].kind != JOB_NONE)
			CONSIDER(jobs[i].deadline_ms);
	CONSIDER(session.kill_ms);
	CONSIDER(session.battery_ms);
#undef CONSIDER
	if (deadline < 0)
		return -1;
	if (deadline <= now)
		return 0;
	return deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}

static void run_timers(void)
{
	long long now = monotonic_ms();

	for (int i = 0; i < MAX_JOBS; i++) {
		struct job *j = &jobs[i];

		if (j->kind == JOB_NONE || j->deadline_ms <= 0 || now < j->deadline_ms)
			continue;
		if (!j->term_sent) {
			log_msg("portsd: child pid=%d timed out, SIGTERM", (int)j->pid);
			kill(-j->pid, SIGTERM);
			j->term_sent = true;
			j->deadline_ms = now + KILL_TIMEOUT_MS;
		} else if (!j->kill_sent) {
			log_msg("portsd: child pid=%d did not terminate, SIGKILL", (int)j->pid);
			kill(-j->pid, SIGKILL);
			j->kill_sent = true;
			j->deadline_ms = -1;
		}
	}
	if (session.battery_ms > 0 && now >= session.battery_ms) {
		battery_refresh();
		session.battery_ms = now + BATTERY_MS;
	}
	if (session.kill_ms > 0 && now >= session.kill_ms) {
		log_msg("portsd: session did not end, killed");
		session.kill_ms = 0;
		cgroup_signal(SIGKILL);
	}
}

int main(void)
{
	struct sigaction sa;
	int server, mount_server;
	FILE *pid_fp;

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	for (int i = 0; i < MAX_JOBS; i++)
		jobs[i].out_fd = -1;
	reset_session();

	if (mkdir_p(RUN_ROOT) != 0 || pipe2(sigchld_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		perror("nuubos-portsd: init");
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_handler = handle_stop;
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	setup_cgroup();
	refresh_ports_root();

	server = make_server(SOCKET_PATH, 0660);
	/* Reached from the sandbox (root inside, root's socket outside);
	 * requests from anything but the session are refused. */
	mount_server = make_server(MOUNT_SOCKET, 0600);
	if (server < 0 || mount_server < 0) {
		perror("nuubos-portsd: socket");
		return 1;
	}
	pid_fp = fopen(PIDFILE, "w");
	if (pid_fp) {
		fprintf(pid_fp, "%ld\n", (long)getpid());
		fclose(pid_fp);
	}
	log_msg("portsd: started pid=%ld portmaster=%s", (long)getpid(), pm_version[0] ? pm_version : "-");

	while (!stop_requested) {
		struct pollfd pfd[4 + MAX_JOBS + MAX_CLIENTS];
		int who[4 + MAX_JOBS + MAX_CLIENTS];
		nfds_t count = 0;
		int rc;

		pfd[count] = (struct pollfd){ server, POLLIN, 0 };
		who[count++] = -1;
		pfd[count] = (struct pollfd){ sigchld_pipe[0], POLLIN, 0 };
		who[count++] = -2;
		pfd[count] = (struct pollfd){ mount_server, POLLIN, 0 };
		who[count++] = -3;
		if (session.input_fd >= 0) {
			pfd[count] = (struct pollfd){ session.input_fd, POLLIN, 0 };
			who[count++] = -4;
		}
		for (int i = 0; i < MAX_JOBS; i++) {
			if (jobs[i].kind == JOB_NONE || jobs[i].out_fd < 0)
				continue;
			pfd[count] = (struct pollfd){ jobs[i].out_fd, POLLIN, 0 };
			who[count++] = 1000 + i;
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;
			pfd[count] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
			who[count++] = i;
		}

		rc = poll(pfd, count, next_timeout_ms());
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		run_timers();
		for (nfds_t i = 0; i < count; i++) {
			if (!pfd[i].revents)
				continue;
			switch (who[i]) {
			case -1:
				accept_client(server);
				break;
			case -2:
				reap_children();
				break;
			case -3:
				accept_mount(mount_server);
				break;
			case -4:
				if (session.input_fd >= 0)
					process_input_link();
				break;
			default:
				if (who[i] >= 1000) {
					int slot = who[i] - 1000;

					if (jobs[slot].kind != JOB_NONE && jobs[slot].out_fd >= 0)
						read_job(slot);
				} else if (clients[who[i]].fd >= 0) {
					process_client(&clients[who[i]]);
				}
				break;
			}
		}
	}

	/* Service stop (shutdown): end a running session, bounded. */
	if (session.state != SESSION_IDLE && session.job >= 0) {
		long long deadline = monotonic_ms() + KILL_TIMEOUT_MS;
		pid_t pid = jobs[session.job].pid;
		bool exited = false;

		cgroup_freeze(false);
		cgroup_signal(SIGTERM);
		while (!exited && monotonic_ms() < deadline) {
			struct pollfd p = { sigchld_pipe[0], POLLIN, 0 };
			char drain[32];

			(void)poll(&p, 1, (int)(deadline - monotonic_ms()));
			while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
				;
			exited = waitpid(pid, NULL, WNOHANG) == pid;
		}
		cgroup_signal(SIGKILL);
	}
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].kind != JOB_NONE && jobs[i].kind != JOB_SESSION)
			kill(-jobs[i].pid, SIGTERM);
	unlink(SOCKET_PATH);
	unlink(MOUNT_SOCKET);
	unlink(PIDFILE);
	return 0;
}
