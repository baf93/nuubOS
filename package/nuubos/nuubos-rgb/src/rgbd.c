/* SPDX-License-Identifier: MIT */
/*
 * nuubos-rgbd — H700 RGB hardware backend.
 *
 * A frame sink with no product policy: it powers the RGB MCU and the LED
 * rail, maps logical frames onto the physical LED order of the detected
 * topology and writes them to the MCU UART. Effects, brightness and
 * preferences belong to nuubos-lightingd (the only product client).
 *
 * Protocol (/run/nuubos/rgbd.sock, persistent connections, one command per
 * line, one reply line each):
 *   STATUS           supported=1 device= leds= zones= backend=active|off
 *                    zone<N>=<name>:<type>:<count> ...
 *   FRAME R G B ...  leds*3 values 0..255 in logical order: every zone in
 *                    topology order, each zone from its first position
 *                    around the ring. Powers the hardware on when needed.
 *   OFF              LEDs dark and MCU/LED rail powered off.
 *
 * Idle: poll() without timeout; nothing runs between commands.
 */

#include "rgb_common.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/gpio.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#define RGB_SOCKET_PATH "/run/nuubos/rgbd.sock"

#define H700_GPIO_LABEL "300b000.pinctrl"
#define H700_MCU_PWR     133 /* PE5 */
#define H700_LED_PWR     263 /* PI7 */
#define H700_UART_NODE   "serial@5001400"

#define MAX_CLIENTS 4
#define LINE_MAX_LEN 1024

struct h700_backend {
	int gpio_fd;
	int uart_fd;
	int active;
};

struct client {
	int fd;
	size_t used;
	char buf[LINE_MAX_LEN];
};

static const struct nuubos_rgb_topology *topology;
static struct h700_backend hw = { .gpio_fd = -1, .uart_fd = -1 };
static uint8_t last_frame[NUUBOS_RGB_MAX_LEDS * 3];
static int last_valid;
static struct client clients[MAX_CLIENTS];
static volatile sig_atomic_t stopping;

static void signal_handler(int sig)
{
	(void)sig;
	stopping = 1;
}

static int write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		p += n;
		len -= (size_t)n;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* H700 hardware                                                       */
/* ------------------------------------------------------------------ */

static int find_gpiochip(char *out, size_t out_size)
{
	unsigned int i;

	for (i = 0; i < 32; i++) {
		char path[64];
		struct gpiochip_info info = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/gpiochip%u", i);

		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 &&
		    strcmp(info.label, H700_GPIO_LABEL) == 0) {
			close(fd);
			snprintf(out, out_size, "%s", path);
			return 0;
		}

		close(fd);
	}

	errno = ENODEV;
	return -1;
}

static int request_power_lines(void)
{
	char chip_path[64];
	struct gpio_v2_line_request req;
	int chip;

	if (find_gpiochip(chip_path, sizeof(chip_path)) < 0)
		return -1;

	chip = open(chip_path, O_RDWR);
	if (chip < 0)
		return -1;

	memset(&req, 0, sizeof(req));

	req.offsets[0] = H700_MCU_PWR;
	req.offsets[1] = H700_LED_PWR;
	req.num_lines = 2;

	snprintf(req.consumer, sizeof(req.consumer), "nuubos-rgbd");

	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	req.config.num_attrs = 1;
	req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	req.config.attrs[0].attr.values = 0;
	req.config.attrs[0].mask = 0x3;

	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		close(chip);
		return -1;
	}

	close(chip);
	return req.fd;
}

static int set_power(int fd, int mcu, int leds)
{
	struct gpio_v2_line_values values = {
		.mask = 0x3,
		.bits = (mcu ? 0x1 : 0) | (leds ? 0x2 : 0),
	};

	return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
}

static int find_rgb_uart(char *out, size_t out_size)
{
	glob_t g;
	size_t i;
	int ret = -1;

	if (glob("/sys/class/tty/ttyS*/device/of_node", 0, NULL, &g) != 0) {
		errno = ENODEV;
		return -1;
	}

	for (i = 0; i < g.gl_pathc; i++) {
		char resolved[4096];
		char path[4096];
		char *tty;
		char *slash;

		if (!realpath(g.gl_pathv[i], resolved))
			continue;

		if (!strstr(resolved, H700_UART_NODE))
			continue;

		snprintf(path, sizeof(path), "%s", g.gl_pathv[i]);

		tty = strstr(path, "/tty/");
		if (!tty)
			continue;

		tty += 5;
		slash = strchr(tty, '/');
		if (slash)
			*slash = '\0';

		snprintf(out, out_size, "/dev/%s", tty);
		ret = 0;
		break;
	}

	globfree(&g);

	if (ret < 0)
		errno = ENODEV;

	return ret;
}

static int open_rgb_uart(void)
{
	char path[128];
	struct termios tio;
	int fd;

	if (find_rgb_uart(path, sizeof(path)) < 0)
		return -1;

	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (tcgetattr(fd, &tio) < 0) {
		close(fd);
		return -1;
	}

	cfmakeraw(&tio);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);

	tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
	tio.c_cflag |= CS8 | CLOCAL | CREAD;

	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		close(fd);
		return -1;
	}

	tcflush(fd, TCIOFLUSH);
	return fd;
}

static int send_init_frame(int fd)
{
	uint8_t frame[51] = { 0 };

	frame[0] = 0x01;
	frame[1] = 0xff;

	if (write_all(fd, frame, sizeof(frame)) < 0)
		return -1;

	return tcdrain(fd);
}

static int backend_open(void)
{
	if (hw.active)
		return 0;

	hw.gpio_fd = request_power_lines();
	if (hw.gpio_fd < 0)
		return -1;

	/*
	 * Hardware-qualified H700 RGB power topology:
	 *   PE5 -> RGB MCU
	 *   PI7 -> LED rail
	 */
	if (set_power(hw.gpio_fd, 1, 0) < 0 ||
	    set_power(hw.gpio_fd, 1, 1) < 0)
		goto fail_gpio;

	hw.uart_fd = open_rgb_uart();
	if (hw.uart_fd < 0)
		goto fail_power;

	if (send_init_frame(hw.uart_fd) < 0)
		goto fail_uart;

	hw.active = 1;
	return 0;

fail_uart:
	close(hw.uart_fd);
	hw.uart_fd = -1;
fail_power:
	set_power(hw.gpio_fd, 0, 0);
fail_gpio:
	close(hw.gpio_fd);
	hw.gpio_fd = -1;
	return -1;
}

/* rgb is in physical LED order. */
static int backend_write(const uint8_t *rgb, unsigned int led_count)
{
	uint8_t packet[2 + NUUBOS_RGB_MAX_LEDS * 3 + 1];
	unsigned int i;
	unsigned int sum = 0;
	size_t payload = led_count * 3;
	size_t len = 2 + payload + 1;

	packet[0] = 0x01;
	packet[1] = 0xff;

	memcpy(packet + 2, rgb, payload);

	for (i = 0; i < 2 + payload; i++)
		sum += packet[i];

	packet[len - 1] = (uint8_t)(sum & 0xff);

	if (write_all(hw.uart_fd, packet, len) < 0)
		return -1;

	return tcdrain(hw.uart_fd);
}

static void backend_close(void)
{
	if (!hw.active)
		return;

	if (hw.uart_fd >= 0) {
		uint8_t off[NUUBOS_RGB_MAX_LEDS * 3] = { 0 };

		backend_write(off, topology->led_count);
		close(hw.uart_fd);
	}

	if (hw.gpio_fd >= 0) {
		set_power(hw.gpio_fd, 0, 0);
		close(hw.gpio_fd);
	}

	hw.gpio_fd = -1;
	hw.uart_fd = -1;
	hw.active = 0;
	last_valid = 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int parse_byte(const char *text, uint8_t *value)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(text, &end, 10);

	if (errno || *text == '\0' || *end != '\0' || v < 0 || v > 255)
		return -1;

	*value = (uint8_t)v;
	return 0;
}

/* Logical order (zones, then positions around each ring) to physical. */
static void map_frame(const uint8_t *logical, uint8_t *physical)
{
	unsigned int z, i, j = 0;

	memset(physical, 0, NUUBOS_RGB_MAX_LEDS * 3);
	for (z = 0; z < topology->zone_count; z++) {
		const struct nuubos_rgb_zone *zone = &topology->zones[z];

		for (i = 0; i < zone->led_count; i++, j++) {
			unsigned int p = zone->led_indexes[i];

			if (p >= topology->led_count)
				continue;
			memcpy(physical + p * 3, logical + j * 3, 3);
		}
	}
}

static int show_frame(const uint8_t *logical)
{
	uint8_t physical[NUUBOS_RGB_MAX_LEDS * 3];

	if (backend_open() < 0)
		return -1;

	map_frame(logical, physical);
	if (last_valid &&
	    memcmp(physical, last_frame, topology->led_count * 3) == 0)
		return 0;

	if (backend_write(physical, topology->led_count) < 0)
		return -1;

	memcpy(last_frame, physical, topology->led_count * 3);
	last_valid = 1;
	return 0;
}

static void status_response(char *reply, size_t reply_size)
{
	size_t used;
	unsigned int i;

	used = (size_t)snprintf(
		reply, reply_size,
		"supported=1 device=%s leds=%u zones=%u backend=%s",
		topology->device_id,
		topology->led_count,
		topology->zone_count,
		hw.active ? "active" : "off");

	for (i = 0; i < topology->zone_count && used < reply_size; i++) {
		const struct nuubos_rgb_zone *z = &topology->zones[i];
		int n = snprintf(reply + used, reply_size - used,
				 " zone%u=%s:%s:%u",
				 i, z->name,
				 nuubos_rgb_zone_type_name(z->type),
				 z->led_count);

		if (n < 0)
			break;

		used += (size_t)n;
	}

	if (used + 2 < reply_size) {
		reply[used++] = '\n';
		reply[used] = '\0';
	}
}

static void handle_command(char *command, char *reply, size_t reply_size)
{
	if (strcmp(command, "STATUS") == 0) {
		status_response(reply, reply_size);
		return;
	}

	if (strcmp(command, "OFF") == 0) {
		backend_close();
		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strncmp(command, "FRAME ", 6) == 0) {
		uint8_t frame[NUUBOS_RGB_MAX_LEDS * 3];
		char *save = NULL;
		char *tok;
		unsigned int count = 0;
		unsigned int expected = topology->led_count * 3;

		for (tok = strtok_r(command + 6, " ", &save);
		     tok;
		     tok = strtok_r(NULL, " ", &save)) {
			if (count >= expected ||
			    parse_byte(tok, &frame[count]) < 0) {
				snprintf(reply, reply_size, "ERR invalid frame\n");
				return;
			}

			count++;
		}

		if (count != expected) {
			snprintf(reply, reply_size,
				 "ERR frame requires %u values\n", expected);
			return;
		}

		if (show_frame(frame) < 0) {
			fprintf(stderr, "nuubos-rgbd: hardware write failed: %s\n",
				strerror(errno));
			backend_close();
			snprintf(reply, reply_size, "ERR hardware\n");
			return;
		}

		snprintf(reply, reply_size, "OK\n");
		return;
	}

	snprintf(reply, reply_size, "ERR unknown command\n");
}

/* ------------------------------------------------------------------ */
/* Socket                                                              */
/* ------------------------------------------------------------------ */

static int create_server_socket(void)
{
	struct sockaddr_un addr;
	int fd;

	if (mkdir("/run/nuubos", 0755) < 0 && errno != EEXIST)
		return -1;

	unlink(RGB_SOCKET_PATH);

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", RGB_SOCKET_PATH);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 4) < 0) {
		close(fd);
		unlink(RGB_SOCKET_PATH);
		return -1;
	}

	return fd;
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->used = 0;
}

static void service_client(struct client *c)
{
	ssize_t n;
	char *nl;

	n = read(c->fd, c->buf + c->used, sizeof(c->buf) - 1 - c->used);
	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close_client(c);
		return;
	}
	c->used += (size_t)n;
	c->buf[c->used] = '\0';

	while (c->fd >= 0 && (nl = memchr(c->buf, '\n', c->used)) != NULL) {
		char line[LINE_MAX_LEN];
		char reply[1024];
		size_t len = (size_t)(nl - c->buf);

		memcpy(line, c->buf, len);
		line[len] = '\0';
		if (len > 0 && line[len - 1] == '\r')
			line[len - 1] = '\0';
		memmove(c->buf, nl + 1, c->used - len - 1);
		c->used -= len + 1;

		handle_command(line, reply, sizeof(reply));
		if (write_all(c->fd, reply, strlen(reply)) < 0)
			close_client(c);
	}

	/* A line longer than the buffer is a protocol violation. */
	if (c->fd >= 0 && c->used >= sizeof(c->buf) - 1)
		close_client(c);
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int server;
	int i;

	topology = nuubos_rgb_detect_topology();

	if (argc == 2 && strcmp(argv[1], "--supported") == 0)
		return topology ? 0 : 1;

	if (!topology)
		return 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	for (i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;

	server = create_server_socket();
	if (server < 0) {
		perror("nuubos-rgbd socket");
		return 1;
	}

	while (!stopping) {
		struct pollfd pfd[1 + MAX_CLIENTS];
		int idx[1 + MAX_CLIENTS];
		nfds_t count = 0;
		nfds_t k;

		pfd[count].fd = server;
		pfd[count].events = POLLIN;
		idx[count++] = -1;
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;
			pfd[count].fd = clients[i].fd;
			pfd[count].events = POLLIN;
			idx[count++] = i;
		}

		if (poll(pfd, count, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		for (k = 1; k < count; k++)
			if (pfd[k].revents)
				service_client(&clients[idx[k]]);

		if (pfd[0].revents & POLLIN) {
			int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC);

			if (fd >= 0) {
				for (i = 0; i < MAX_CLIENTS; i++)
					if (clients[i].fd < 0)
						break;
				if (i == MAX_CLIENTS) {
					close(fd);
				} else {
					clients[i].fd = fd;
					clients[i].used = 0;
				}
			}
		}
	}

	backend_close();
	for (i = 0; i < MAX_CLIENTS; i++)
		close_client(&clients[i]);
	close(server);
	unlink(RGB_SOCKET_PATH);

	return 0;
}
