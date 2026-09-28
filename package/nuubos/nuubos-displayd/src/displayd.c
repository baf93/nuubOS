#include "kms.h"

#include <dirent.h>
#include <errno.h>
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define DRM_CLASS_DIR "/sys/class/drm"
#define STATE_DIR "/run/nuubos"
#define STATE_FILE STATE_DIR "/displayd.state"
#define STATE_TMP STATE_DIR "/displayd.state.tmp"
#define UEVENT_BUFFER_SIZE 8192
#define HDMI_CONNECT_SETTLE_MS 300
#define HDMI_DISCONNECT_SETTLE_MS 1500

static volatile sig_atomic_t stop_requested;
static unsigned long sequence;

static void handle_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static int install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGTERM, &sa, NULL) < 0)
		return -1;

	if (sigaction(SIGINT, &sa, NULL) < 0)
		return -1;

	return 0;
}

static int find_hdmi_connector(char *path, size_t path_size,
			       char *name, size_t name_size)
{
	DIR *dir;
	struct dirent *entry;
	int ret = -1;

	dir = opendir(DRM_CLASS_DIR);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		int n;

		if (!strstr(entry->d_name, "-HDMI-A-"))
			continue;

		n = snprintf(path, path_size, "%s/%s",
			     DRM_CLASS_DIR, entry->d_name);
		if (n < 0 || (size_t)n >= path_size)
			continue;

		n = snprintf(name, name_size, "%s", entry->d_name);
		if (n < 0 || (size_t)n >= name_size)
			continue;

		ret = 0;
		break;
	}

	closedir(dir);
	return ret;
}

static int read_connector_connected(const char *connector_path,
				    bool *connected)
{
	char status_path[512];
	char status[32];
	FILE *file;
	int n;

	n = snprintf(status_path, sizeof(status_path),
		     "%s/status", connector_path);
	if (n < 0 || (size_t)n >= sizeof(status_path))
		return -1;

	file = fopen(status_path, "r");
	if (!file)
		return -1;

	if (!fgets(status, sizeof(status), file)) {
		fclose(file);
		return -1;
	}

	fclose(file);

	*connected = strncmp(status, "connected", 9) == 0;
	return 0;
}

static int write_state(const char *connector_name, bool connected,
		       const char *reason, const char *mode)
{
	FILE *file;

	if (mkdir(STATE_DIR, 0755) < 0 && errno != EEXIST)
		return -1;

	file = fopen(STATE_TMP, "w");
	if (!file)
		return -1;

	sequence++;

	fprintf(file, "sequence=%lu\n", sequence);
	fprintf(file, "connector=%s\n", connector_name);
	fprintf(file, "hdmi=%s\n", connected ? "connected" : "disconnected");
	fprintf(file, "output=%s\n", connected ? "hdmi" : "internal");
	fprintf(file, "mode=%s\n", mode);
	fprintf(file, "reason=%s\n", reason);

	if (fclose(file) != 0) {
		unlink(STATE_TMP);
		return -1;
	}

	if (rename(STATE_TMP, STATE_FILE) < 0) {
		unlink(STATE_TMP);
		return -1;
	}

	return 0;
}

static int reconcile(const char *connector_path,
		     const char *connector_name,
		     const char *reason,
		     bool *last_connected,
		     bool *have_last)
{
	char mode[64];
	bool connected;

	if (read_connector_connected(connector_path, &connected) < 0) {
		syslog(LOG_ERR, "cannot read HDMI connector status: %s",
		       strerror(errno));
		return -1;
	}

	/*
	 * DRM may emit more than one hotplug uevent for one physical
	 * transition. Apply policy only when the physical state changes.
	 */
	if (*have_last && connected == *last_connected)
		return 0;

	if (nuubos_kms_apply(connected, mode, sizeof(mode)) < 0) {
		syslog(LOG_ERR, "KMS switch to %s failed: %s",
		       connected ? "HDMI" : "internal",
		       strerror(errno));
		return -1;
	}

	syslog(LOG_INFO, "output=%s mode=%s (%s)",
	       connected ? "HDMI" : "internal",
	       mode, reason);

	if (write_state(connector_name, connected, reason, mode) < 0) {
		syslog(LOG_ERR, "cannot write state file: %s",
		       strerror(errno));
		return -1;
	}

	*last_connected = connected;
	*have_last = true;

	return 0;
}

static int open_uevent_socket(void)
{
	struct sockaddr_nl addr;
	int fd;
	int rcvbuf = 256 * 1024;

	fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (fd < 0)
		return -1;

	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
			 &rcvbuf, sizeof(rcvbuf));

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = (unsigned int)getpid();
	addr.nl_groups = 1;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static bool is_drm_hotplug_event(const char *buf, ssize_t len)
{
	bool drm = false;
	bool change = false;
	bool hotplug = false;
	ssize_t offset = 0;

	while (offset < len) {
		const char *field = buf + offset;
		size_t remaining = (size_t)(len - offset);
		size_t field_len = strnlen(field, remaining);

		if (field_len == remaining)
			break;

		if (strcmp(field, "SUBSYSTEM=drm") == 0)
			drm = true;
		else if (strcmp(field, "ACTION=change") == 0)
			change = true;
		else if (strcmp(field, "HOTPLUG=1") == 0)
			hotplug = true;

		offset += (ssize_t)field_len + 1;
	}

	return drm && change && hotplug;
}

static int set_hotplug_timer(int fd, unsigned int delay_ms)
{
	struct itimerspec timer;

	memset(&timer, 0, sizeof(timer));

	if (delay_ms > 0) {
		timer.it_value.tv_sec = delay_ms / 1000;
		timer.it_value.tv_nsec =
			(delay_ms % 1000) * 1000000L;
	}

	return timerfd_settime(fd, 0, &timer, NULL);
}

int main(int argc, char **argv)
{
	char connector_path[512];
	char connector_name[256];
	char buffer[UEVENT_BUFFER_SIZE];
	bool last_connected = false;
	bool have_last = false;
	int fd;
	int timer_fd;

	if (find_hdmi_connector(connector_path, sizeof(connector_path),
				connector_name, sizeof(connector_name)) < 0) {
		if (argc == 2 && strcmp(argv[1], "--supported") == 0)
			return EXIT_FAILURE;

		fprintf(stderr, "nuubos-displayd: no HDMI DRM connector\n");
		return EXIT_FAILURE;
	}

	if (argc == 2 && strcmp(argv[1], "--supported") == 0)
		return EXIT_SUCCESS;

	if (argc != 1) {
		fprintf(stderr, "Usage: %s [--supported]\n", argv[0]);
		return EXIT_FAILURE;
	}

	openlog("nuubos-displayd", LOG_PID, LOG_DAEMON);

	if (install_signal_handlers() < 0) {
		syslog(LOG_ERR, "cannot install signal handlers: %s",
		       strerror(errno));
		closelog();
		return EXIT_FAILURE;
	}

	/*
	 * Subscribe first, then inspect current state. This avoids the
	 * scan-before-subscribe race where a hotplug could otherwise be lost.
	 */
	fd = open_uevent_socket();
	if (fd < 0) {
		syslog(LOG_ERR, "cannot open uevent socket: %s",
		       strerror(errno));
		closelog();
		return EXIT_FAILURE;
	}

	timer_fd = timerfd_create(CLOCK_MONOTONIC,
				  TFD_CLOEXEC | TFD_NONBLOCK);
	if (timer_fd < 0) {
		syslog(LOG_ERR, "cannot create hotplug timer: %s",
		       strerror(errno));
		close(fd);
		closelog();
		return EXIT_FAILURE;
	}

	if (reconcile(connector_path, connector_name, "startup",
		      &last_connected, &have_last) < 0) {
		close(fd);
		closelog();
		return EXIT_FAILURE;
	}

	while (!stop_requested) {
		struct pollfd fds[2];
		int poll_ret;

		memset(fds, 0, sizeof(fds));

		fds[0].fd = fd;
		fds[0].events = POLLIN;

		fds[1].fd = timer_fd;
		fds[1].events = POLLIN;

		poll_ret = poll(fds, 2, -1);
		if (poll_ret < 0) {
			if (errno == EINTR)
				continue;

			syslog(LOG_ERR, "poll failed: %s",
			       strerror(errno));
			break;
		}

		if (fds[0].revents & POLLIN) {
			ssize_t len;

			len = recv(fd, buffer, sizeof(buffer), 0);
			if (len < 0) {
				if (errno == EINTR)
					continue;

				syslog(LOG_ERR,
				       "uevent receive failed: %s",
				       strerror(errno));
				break;
			}

			if (is_drm_hotplug_event(buffer, len)) {
				bool observed_connected;
				unsigned int settle_ms;

				/*
				 * HDMI HPD may bounce during EDID/link setup.
				 *
				 * Every event rearms the one-shot timer.
				 * Never cancel it here: when it expires,
				 * reconcile() reads the actual final state.
				 *
				 * Disconnect gets a longer grace period so
				 * transient link drops caused by HDMI setup
				 * do not switch the internal panel back on.
				 */
				if (read_connector_connected(
					    connector_path,
					    &observed_connected) < 0) {
					syslog(LOG_ERR,
					       "cannot read HDMI status after hotplug: %s",
					       strerror(errno));
					break;
				}

				settle_ms = observed_connected ?
					HDMI_CONNECT_SETTLE_MS :
					HDMI_DISCONNECT_SETTLE_MS;

				if (set_hotplug_timer(timer_fd,
						      settle_ms) < 0) {
					syslog(LOG_ERR,
					       "cannot arm hotplug timer: %s",
					       strerror(errno));
					break;
				}
			}
		}

		if (fds[1].revents & POLLIN) {
			uint64_t expirations;

			if (read(timer_fd, &expirations,
				 sizeof(expirations)) !=
			    (ssize_t)sizeof(expirations)) {
				if (errno == EAGAIN ||
				    errno == EINTR)
					continue;

				syslog(LOG_ERR,
				       "hotplug timer read failed: %s",
				       strerror(errno));
				break;
			}

			if (reconcile(connector_path,
				      connector_name,
				      "hotplug-settled",
				      &last_connected,
				      &have_last) < 0)
				break;
		}
	}

	close(timer_fd);
	close(fd);
	closelog();

	return EXIT_SUCCESS;
}
