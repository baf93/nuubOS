/* SPDX-License-Identifier: MIT */

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

#define FRAME_INTERVAL_MS 33

enum rgb_mode {
	RGB_MODE_OFF,
	RGB_MODE_STATIC,
	RGB_MODE_BREATHE,
	RGB_MODE_RAINBOW,
	RGB_MODE_FRAME,
};

struct h700_backend {
	int gpio_fd;
	int uart_fd;
	int active;
};

struct rgb_state {
	const struct nuubos_rgb_topology *topology;
	struct h700_backend hw;
	enum rgb_mode mode;
	unsigned int brightness;
	uint8_t color[3];
	uint8_t frame[NUUBOS_RGB_MAX_LEDS * 3];
	unsigned int phase;
	int dirty;
};

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

	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
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

static int backend_open(struct h700_backend *hw)
{
	if (hw->active)
		return 0;

	hw->gpio_fd = request_power_lines();
	if (hw->gpio_fd < 0)
		return -1;

	/*
	 * Hardware-qualified H700 RGB power topology:
	 *   PE5 -> RGB MCU
	 *   PI7 -> LED rail
	 */
	if (set_power(hw->gpio_fd, 1, 0) < 0 ||
	    set_power(hw->gpio_fd, 1, 1) < 0)
		goto fail_gpio;

	hw->uart_fd = open_rgb_uart();
	if (hw->uart_fd < 0)
		goto fail_power;

	if (send_init_frame(hw->uart_fd) < 0)
		goto fail_uart;

	hw->active = 1;
	return 0;

fail_uart:
	close(hw->uart_fd);
	hw->uart_fd = -1;
fail_power:
	set_power(hw->gpio_fd, 0, 0);
fail_gpio:
	close(hw->gpio_fd);
	hw->gpio_fd = -1;
	return -1;
}

static int backend_write(struct h700_backend *hw,
			 const uint8_t *rgb,
			 unsigned int led_count)
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

	if (write_all(hw->uart_fd, packet, len) < 0)
		return -1;

	return tcdrain(hw->uart_fd);
}

static void backend_close(struct h700_backend *hw, unsigned int led_count)
{
	if (!hw->active)
		return;

	if (hw->uart_fd >= 0) {
		uint8_t off[NUUBOS_RGB_MAX_LEDS * 3] = { 0 };

		backend_write(hw, off, led_count);
		close(hw->uart_fd);
	}

	if (hw->gpio_fd >= 0) {
		set_power(hw->gpio_fd, 0, 0);
		close(hw->gpio_fd);
	}

	hw->gpio_fd = -1;
	hw->uart_fd = -1;
	hw->active = 0;
}

static const char *mode_name(enum rgb_mode mode)
{
	switch (mode) {
	case RGB_MODE_OFF:
		return "off";
	case RGB_MODE_STATIC:
		return "static";
	case RGB_MODE_BREATHE:
		return "breathe";
	case RGB_MODE_RAINBOW:
		return "rainbow";
	case RGB_MODE_FRAME:
		return "frame";
	default:
		return "unknown";
	}
}

static int mode_is_animated(enum rgb_mode mode)
{
	return mode == RGB_MODE_BREATHE ||
	       mode == RGB_MODE_RAINBOW;
}

static uint8_t scale8(uint8_t value, unsigned int scale)
{
	return (uint8_t)(((unsigned int)value * scale) / 100);
}

static unsigned int breathe_level(unsigned int phase)
{
	unsigned int x;
	unsigned int t;
	unsigned int smooth;

	phase %= 120;

	x = phase <= 60 ? phase : 120 - phase;
	t = (x * 1000) / 60;

	/* integer smoothstep: 3t^2 - 2t^3 */
	smooth = (t * t * (3000 - 2 * t)) / 1000000;

	return smooth / 10;
}

static void rainbow_pixel(unsigned int pos,
			  uint8_t *r, uint8_t *g, uint8_t *b)
{
	pos &= 0xff;

	if (pos < 85) {
		*r = (uint8_t)(255 - pos * 3);
		*g = (uint8_t)(pos * 3);
		*b = 0;
	} else if (pos < 170) {
		pos -= 85;
		*r = 0;
		*g = (uint8_t)(255 - pos * 3);
		*b = (uint8_t)(pos * 3);
	} else {
		pos -= 170;
		*r = (uint8_t)(pos * 3);
		*g = 0;
		*b = (uint8_t)(255 - pos * 3);
	}
}

static int render(struct rgb_state *s)
{
	uint8_t out[NUUBOS_RGB_MAX_LEDS * 3] = { 0 };
	unsigned int i;

	if (s->mode == RGB_MODE_OFF) {
		backend_close(&s->hw, s->topology->led_count);
		s->dirty = 0;
		return 0;
	}

	if (backend_open(&s->hw) < 0)
		return -1;

	switch (s->mode) {
	case RGB_MODE_STATIC:
		for (i = 0; i < s->topology->led_count; i++) {
			out[i * 3 + 0] = s->color[0];
			out[i * 3 + 1] = s->color[1];
			out[i * 3 + 2] = s->color[2];
		}
		break;

	case RGB_MODE_BREATHE: {
		unsigned int level = breathe_level(s->phase++);

		for (i = 0; i < s->topology->led_count; i++) {
			out[i * 3 + 0] = scale8(s->color[0], level);
			out[i * 3 + 1] = scale8(s->color[1], level);
			out[i * 3 + 2] = scale8(s->color[2], level);
		}
		break;
	}

	case RGB_MODE_RAINBOW:
		for (i = 0; i < s->topology->led_count; i++) {
			unsigned int pos =
				(s->phase * 2 +
				 (i * 256 / s->topology->led_count)) & 0xff;

			rainbow_pixel(pos,
				      &out[i * 3 + 0],
				      &out[i * 3 + 1],
				      &out[i * 3 + 2]);
		}
		s->phase++;
		break;

	case RGB_MODE_FRAME:
		memcpy(out, s->frame, s->topology->led_count * 3);
		break;

	default:
		break;
	}

	for (i = 0; i < s->topology->led_count * 3; i++)
		out[i] = scale8(out[i], s->brightness);

	if (backend_write(&s->hw, out, s->topology->led_count) < 0)
		return -1;

	s->dirty = 0;
	return 0;
}

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

static void status_response(const struct rgb_state *s,
			    char *reply, size_t reply_size)
{
	size_t used;
	unsigned int i;

	used = (size_t)snprintf(
		reply, reply_size,
		"supported=1 device=%s leds=%u zones=%u mode=%s brightness=%u backend=%s",
		s->topology->device_id,
		s->topology->led_count,
		s->topology->zone_count,
		mode_name(s->mode),
		s->brightness,
		s->hw.active ? "active" : "off");

	for (i = 0; i < s->topology->zone_count && used < reply_size; i++) {
		const struct nuubos_rgb_zone *z = &s->topology->zones[i];
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

static void handle_command(struct rgb_state *s,
			   const char *command,
			   char *reply, size_t reply_size)
{
	unsigned int value;

	if (strcmp(command, "STATUS") == 0) {
		status_response(s, reply, reply_size);
		return;
	}

	if (strcmp(command, "OFF") == 0) {
		s->mode = RGB_MODE_OFF;
		s->dirty = 1;
		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (sscanf(command, "BRIGHTNESS %u", &value) == 1) {
		if (value > 100) {
			snprintf(reply, reply_size, "ERR brightness must be 0..100\n");
			return;
		}

		s->brightness = value;
		s->dirty = 1;
		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strncmp(command, "COLOR ", 6) == 0) {
		unsigned int r, g, b;

		if (sscanf(command + 6, "%u %u %u", &r, &g, &b) != 3 ||
		    r > 255 || g > 255 || b > 255) {
			snprintf(reply, reply_size, "ERR color requires R G B in 0..255\n");
			return;
		}

		s->color[0] = (uint8_t)r;
		s->color[1] = (uint8_t)g;
		s->color[2] = (uint8_t)b;
		s->mode = RGB_MODE_STATIC;
		s->phase = 0;
		s->dirty = 1;

		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strncmp(command, "MODE ", 5) == 0) {
		const char *mode = command + 5;

		if (strcmp(mode, "static") == 0)
			s->mode = RGB_MODE_STATIC;
		else if (strcmp(mode, "breathe") == 0)
			s->mode = RGB_MODE_BREATHE;
		else if (strcmp(mode, "rainbow") == 0)
			s->mode = RGB_MODE_RAINBOW;
		else {
			snprintf(reply, reply_size, "ERR unknown mode\n");
			return;
		}

		s->phase = 0;
		s->dirty = 1;
		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strncmp(command, "FRAME ", 6) == 0) {
		char copy[1024];
		char *save = NULL;
		char *tok;
		unsigned int count = 0;
		unsigned int expected = s->topology->led_count * 3;

		snprintf(copy, sizeof(copy), "%s", command + 6);

		for (tok = strtok_r(copy, " ", &save);
		     tok;
		     tok = strtok_r(NULL, " ", &save)) {
			if (count >= expected ||
			    parse_byte(tok, &s->frame[count]) < 0) {
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

		s->mode = RGB_MODE_FRAME;
		s->phase = 0;
		s->dirty = 1;

		snprintf(reply, reply_size, "OK\n");
		return;
	}

	snprintf(reply, reply_size, "ERR unknown command\n");
}

static int create_server_socket(void)
{
	struct sockaddr_un addr;
	int fd;

	if (mkdir("/run/nuubos", 0755) < 0 && errno != EEXIST)
		return -1;

	unlink(RGB_SOCKET_PATH);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
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

static void serve_client(int client, struct rgb_state *state)
{
	char command[1024];
	char reply[1024];
	ssize_t n;

	n = read(client, command, sizeof(command) - 1);
	if (n <= 0)
		return;

	command[n] = '\0';

	while (n > 0 &&
	       (command[n - 1] == '\n' || command[n - 1] == '\r'))
		command[--n] = '\0';

	handle_command(state, command, reply, sizeof(reply));
	write_all(client, reply, strlen(reply));
}

int main(int argc, char **argv)
{
	const struct nuubos_rgb_topology *topology;
	struct rgb_state state;
	struct pollfd pfd;
	int server;

	topology = nuubos_rgb_detect_topology();

	if (argc == 2 && strcmp(argv[1], "--supported") == 0)
		return topology ? 0 : 1;

	if (!topology)
		return 0;

	memset(&state, 0, sizeof(state));

	state.topology = topology;
	state.hw.gpio_fd = -1;
	state.hw.uart_fd = -1;
	state.mode = RGB_MODE_OFF;
	state.brightness = 50;
	state.color[0] = 255;
	state.color[1] = 255;
	state.color[2] = 255;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	server = create_server_socket();
	if (server < 0) {
		perror("nuubos-rgbd socket");
		return 1;
	}

	pfd.fd = server;
	pfd.events = POLLIN;

	while (!stopping) {
		int timeout = mode_is_animated(state.mode) ?
			      FRAME_INTERVAL_MS : -1;
		int ret = poll(&pfd, 1, timeout);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (ret > 0 && (pfd.revents & POLLIN)) {
			int client = accept(server, NULL, NULL);

			if (client >= 0) {
				serve_client(client, &state);
				close(client);
			}
		}

		if (state.dirty || mode_is_animated(state.mode)) {
			if (render(&state) < 0) {
				fprintf(stderr,
					"nuubos-rgbd: hardware render failed: %s\n",
					strerror(errno));
				state.mode = RGB_MODE_OFF;
				backend_close(&state.hw,
					      state.topology->led_count);
			}
		}
	}

	backend_close(&state.hw, state.topology->led_count);

	close(server);
	unlink(RGB_SOCKET_PATH);

	return 0;
}
