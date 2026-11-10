/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-charge — the charge screen (user request 2026-10-11).
 *
 * Run by S08nuubos-charge after the state partition, the display
 * brightness and rgbd are up. When the PMIC reports that the console was
 * switched on by plugging in the charger (AXP717 REG 20: VBUS insert, not
 * the power key; a software reboot reads 0), a battery is present and no
 * HDMI display is connected (a docked console boots straight to the TV:
 * this screen is drawn on the built-in panel only; 2026-10-16), it
 * shows a minimal screen instead of booting: the nuubOS wordmark, the
 * splash track as the charge gauge (the part above the user's charge limit
 * dimmed, the limit under its edge), the percentage and the battery
 * voltage. Gauge colour: red → orange → green relative to the limit, the
 * same on the RGB LEDs (when the device has them).
 *
 *   - the screen goes dark after 20 s; any key turns it back on;
 *   - Power (any time) or Start (screen on) boots nuubOS: the boot splash
 *     raster comes back and init continues;
 *   - unplugging the charger powers the console off;
 *   - the LEDs go dark with the screen, except when charging is complete
 *     (limit reached or 100 %): then they stay on.
 *
 * The charge limit itself is enforced by the kernel (AXP717 patch 0102,
 * charge_control_end_threshold); this program only writes the user's
 * setting (nuubos-system.conf CHARGE_LIMIT, owned by nuubos-systemd, which
 * is not running yet) and shows it. Exit 0 = continue booting. No polling:
 * power_supply uevents, key events and the screen timeout; a 5 s refresh
 * of the voltage only while the screen is on.
 */

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "nuubos-splash-render.h"
#include "nuubos-splash-kms.h"
#include "nuubos-charge-font.h"

#define SYSTEM_CONFIG "/state/config/nuubos-system.conf"
#define RGB_SOCKET "/run/nuubos/rgbd.sock"
#define UI_READY "/run/nuubos/ui-ready"
#define SCREEN_TIMEOUT_S 20
#define REFRESH_S 5
#define MAX_OUTPUTS 4
#define MAX_KEYS 16
#define MAX_LEDS 32

/* AXP717 REG 20 (PWRON status). */
#define AXP717_PWRON_STATUS 0x20
#define AXP717_PWRON_KEY 0x01
#define AXP717_PWRON_VBUS 0x04

/* Gauge colours (lightingd ROLE_ERROR / ROLE_WARNING / ROLE_SUCCESS). */
static const float GAUGE_RGB[3][3] = {
	{ 1.00f, 0.10f, 0.08f },
	{ 1.00f, 0.60f, 0.06f },
	{ 0.20f, 0.95f, 0.42f },
};
/* LEDs at lightingd's default brightness (50, perceptual gain). */
#define LED_GAIN 0.218f

struct battery {
	int percent;    /* -1 unknown */
	int voltage_mv; /* -1 unknown */
	int limit;      /* 50..100 */
	bool full;      /* charging stopped: limit reached or 100 % */
	bool external;  /* charger connected */
};

struct output {
	uint32_t connector, crtc, fb, handle;
	drmModeModeInfo mode;
	uint32_t *map;
	size_t size;
	int stride, transform;
};

static char battery_dir[256];
static char usb_dir[256];
static int drm_fd = -1;
static struct output outputs[MAX_OUTPUTS];
static int output_count;
static struct nuubos_splash_palette palette;
/* Localized lines (nuubOS strings 901-903). */
static char msg_charging[192] = "Recharging extra lives...";
static char msg_full[192] = "Full power. Let's play!";
static char msg_hint[192] = "Press POWER or START to start nuubOS";

/* ------------------------------------------------------------------ */
/* Power-on source and battery                                         */

static int read_line(const char *path, char *buf, size_t size)
{
	FILE *fp = fopen(path, "r");

	if (!fp)
		return -1;
	if (!fgets(buf, (int)size, fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	buf[strcspn(buf, "\r\n")] = '\0';
	return 0;
}

static int read_int(const char *dir, const char *name, int fallback)
{
	char path[320], buf[64];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return read_line(path, buf, sizeof(buf)) == 0 ? atoi(buf) : fallback;
}

/* The AXP717's I2C bus and address from sysfs (<bus>-<addr>, name axp717). */
static int pmic_read(int reg)
{
	glob_t g;
	int value = -1;

	if (glob("/sys/bus/i2c/devices/*-00*/name", 0, NULL, &g) != 0)
		return -1;
	for (size_t i = 0; i < g.gl_pathc && value < 0; i++) {
		char name[32], dev[32];
		unsigned int bus, addr;
		const char *dir;
		int fd;

		if (read_line(g.gl_pathv[i], name, sizeof(name)) != 0 || strcmp(name, "axp717"))
			continue;
		dir = g.gl_pathv[i] + strlen("/sys/bus/i2c/devices/");
		if (sscanf(dir, "%u-%x", &bus, &addr) != 2)
			continue;
		snprintf(dev, sizeof(dev), "/dev/i2c-%u", bus);
		fd = open(dev, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		/* The kernel driver owns the device: a forced single-byte read. */
		if (ioctl(fd, I2C_SLAVE_FORCE, addr) == 0) {
			union i2c_smbus_data data;
			struct i2c_smbus_ioctl_data args = {
				.read_write = I2C_SMBUS_READ,
				.command = (uint8_t)reg,
				.size = I2C_SMBUS_BYTE_DATA,
				.data = &data,
			};

			if (ioctl(fd, I2C_SMBUS, &args) == 0)
				value = data.byte;
		}
		close(fd);
	}
	globfree(&g);
	return value;
}

/* The console battery and the charger input (power_supply class). */
static void find_supplies(void)
{
	glob_t g;

	battery_dir[0] = usb_dir[0] = '\0';
	if (glob("/sys/class/power_supply/*", 0, NULL, &g) != 0)
		return;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		char path[320], type[32], scope[32];

		snprintf(path, sizeof(path), "%s/type", g.gl_pathv[i]);
		if (read_line(path, type, sizeof(type)) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/scope", g.gl_pathv[i]);
		if (read_line(path, scope, sizeof(scope)) == 0 && !strcmp(scope, "Device"))
			continue;
		if (!strcmp(type, "Battery") && !battery_dir[0])
			snprintf(battery_dir, sizeof(battery_dir), "%s", g.gl_pathv[i]);
		else if ((!strcmp(type, "USB") || !strcmp(type, "Mains")) && !usb_dir[0])
			snprintf(usb_dir, sizeof(usb_dir), "%s", g.gl_pathv[i]);
	}
	globfree(&g);
}

/* A TV on HDMI: the console is docked, boot instead of charging (the
 * screen would be on the panel, the TV stayed black: RG35XX Pro report
 * 2026-10-16). HPD is up even with the TV in standby. */
static bool hdmi_connected(void)
{
	glob_t g;
	bool on = false;

	if (glob("/sys/class/drm/card*-HDMI-A-*/status", 0, NULL, &g))
		return false;
	for (size_t i = 0; i < g.gl_pathc && !on; i++) {
		FILE *fp = fopen(g.gl_pathv[i], "r");
		char st[32] = "";

		if (fp) {
			if (fgets(st, sizeof(st), fp))
				on = !strncmp(st, "connected", 9);
			fclose(fp);
		}
	}
	globfree(&g);
	return on;
}

/* REG 00 bit 3 alone read 1 right after a charger-plug boot with no battery
 * (RG35XX Pro, 2026-10-16; once running: present 0, health Dead, 0.9 V):
 * also require a plausible cell voltage and a health that is not Dead. */
static bool battery_present(void)
{
	char health[32] = "";
	FILE *fp;
	char path[300];

	if (!battery_dir[0] || read_int(battery_dir, "present", 0) != 1)
		return false;
	if (read_int(battery_dir, "voltage_now", 0) < 2500000)
		return false;
	snprintf(path, sizeof(path), "%s/health", battery_dir);
	fp = fopen(path, "r");
	if (fp) {
		if (!fgets(health, sizeof(health), fp))
			health[0] = '\0';
		fclose(fp);
	}
	return strncmp(health, "Dead", 4) != 0;
}

static bool charger_online(void)
{
	return usb_dir[0] && read_int(usb_dir, "online", 0) == 1;
}

static bool read_key(const char *path, const char *key, char *out, size_t size)
{
	FILE *fp = fopen(path, "r");
	char line[256];
	size_t n = strlen(key);
	bool found = false;

	if (!fp)
		return false;
	while (!found && fgets(line, sizeof(line), fp))
		if (!strncmp(line, key, n) && line[n] == '=') {
			line[strcspn(line, "\r\n")] = '\0';
			snprintf(out, size, "%s", line + n + 1);
			found = out[0] != '\0';
		}
	fclose(fp);
	return found;
}

/* The lines in the language of the last user (their own choice, else the
 * device language, as localizationd decides), from the nuubOS strings. */
static void load_strings(const char *lang_file)
{
	char user[64], path[160], lang[16] = "en", file[192];
	const char *f = lang_file;

	if (!f) {
		if (!read_key("/state/config/nuubos.conf", "LAST_USER", user, sizeof(user)) ||
		    strchr(user, '/') ||
		    (snprintf(path, sizeof(path), "/state/users/%s/localization.conf", user),
		     !read_key(path, "LANGUAGE", lang, sizeof(lang))))
			(void)read_key("/state/users/default/localization.conf", "LANGUAGE", lang, sizeof(lang));
		if (strchr(lang, '/'))
			snprintf(lang, sizeof(lang), "en");
		snprintf(file, sizeof(file), "/usr/share/nuubos/i18n/%s.lang", lang);
		f = file;
	}
	/* English first, the language over it (a missing line stays English). */
	if (strcmp(f, "/usr/share/nuubos/i18n/en.lang"))
		load_strings("/usr/share/nuubos/i18n/en.lang");
	(void)read_key(f, "901", msg_charging, sizeof(msg_charging));
	(void)read_key(f, "902", msg_full, sizeof(msg_full));
	(void)read_key(f, "903", msg_hint, sizeof(msg_hint));
}

/* The user's charge limit (nuubos-systemd's setting) into the kernel. */
static void apply_charge_limit(void)
{
	FILE *fp = fopen(SYSTEM_CONFIG, "r");
	char line[128], path[320];
	int limit = -1;

	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp))
		if (!strncmp(line, "CHARGE_LIMIT=", 13))
			limit = atoi(line + 13);
	fclose(fp);
	if (limit < 80 || limit > 100)
		return;
	snprintf(path, sizeof(path), "%s/charge_control_end_threshold", battery_dir);
	fp = fopen(path, "w");
	if (!fp)
		return;
	fprintf(fp, "%d\n", limit);
	fclose(fp);
}

static void read_battery(struct battery *b)
{
	char path[320], status[32];

	b->percent = read_int(battery_dir, "capacity", -1);
	b->voltage_mv = read_int(battery_dir, "voltage_now", -1000) / 1000;
	b->limit = read_int(battery_dir, "charge_control_end_threshold", 100);
	if (b->limit < 50 || b->limit > 100)
		b->limit = 100;
	snprintf(path, sizeof(path), "%s/status", battery_dir);
	if (read_line(path, status, sizeof(status)) != 0)
		status[0] = '\0';
	b->external = charger_online();
	b->full = !strcmp(status, "Full") || (b->percent >= 100 && b->external);
}

/* Charge relative to the limit, 0..1. */
static float gauge_fraction(const struct battery *b)
{
	float f;

	if (b->full)
		return 1.0f;
	if (b->percent < 0)
		return 0.0f;
	f = (float)b->percent / (float)b->limit;
	return f > 1.0f ? 1.0f : f;
}

/* Red → orange → green over 0..1. */
static void gauge_color(float f, float out[3])
{
	int seg = f < 0.5f ? 0 : 1;
	float k = f < 0.5f ? f * 2.0f : (f - 0.5f) * 2.0f;

	for (int i = 0; i < 3; i++)
		out[i] = GAUGE_RGB[seg][i] + (GAUGE_RGB[seg + 1][i] - GAUGE_RGB[seg][i]) * k;
}

static uint32_t to_pixel(const float c[3])
{
	uint32_t p = 0;

	for (int i = 0; i < 3; i++) {
		float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];

		p = (p << 8) | (uint32_t)(v * 255.0f + 0.5f);
	}
	return p;
}

/* ------------------------------------------------------------------ */
/* Raster                                                              */

static const struct nuubos_glyph *glyph_for(unsigned int cp)
{
	for (size_t i = 0; i < sizeof(NUUBOS_FONT) / sizeof(NUUBOS_FONT[0]); i++)
		if (NUUBOS_FONT[i].cp == cp)
			return &NUUBOS_FONT[i];
	return NULL;
}

/* Next UTF-8 codepoint of *s (advanced); 0 at the end. */
static unsigned int next_cp(const char **s)
{
	const unsigned char *u = (const unsigned char *)*s;
	unsigned int cp;
	int n;

	if (!*u)
		return 0;
	if (*u < 0x80) {
		cp = *u;
		n = 1;
	} else if ((*u & 0xe0) == 0xc0) {
		cp = *u & 0x1f;
		n = 2;
	} else if ((*u & 0xf0) == 0xe0) {
		cp = *u & 0x0f;
		n = 3;
	} else {
		cp = *u & 0x07;
		n = 4;
	}
	for (int i = 1; i < n; i++) {
		if ((u[i] & 0xc0) != 0x80) {
			*s += i;
			return '?';
		}
		cp = (cp << 6) | (u[i] & 0x3f);
	}
	*s += n;
	return cp;
}

/* The text as a dot bitmap (cols x NUUBOS_FONT_ROWS, glyphs one column
 * apart; unknown characters are skipped). Caller frees. */
static unsigned char *text_bitmap(const char *s, int *cols_out)
{
	const char *p = s;
	unsigned int cp;
	int cols = 0, x = 0;
	unsigned char *bits;

	while ((cp = next_cp(&p))) {
		const struct nuubos_glyph *g = glyph_for(cp);

		if (g)
			cols += g->width + 1;
	}
	if (cols)
		cols--;
	*cols_out = cols;
	if (!cols || !(bits = calloc((size_t)cols * NUUBOS_FONT_ROWS, 1)))
		return NULL;
	for (p = s; (cp = next_cp(&p));) {
		const struct nuubos_glyph *g = glyph_for(cp);

		if (!g)
			continue;
		for (int r = 0; r < NUUBOS_FONT_ROWS; r++)
			for (int c = 0; c < g->width && g->rows[r]; c++)
				bits[r * cols + x + c] = g->rows[r][c] == '#';
		x += g->width + 1;
	}
	return bits;
}

static int text_cols(const char *s)
{
	int cols;
	unsigned char *bits = text_bitmap(s, &cols);

	free(bits);
	return cols;
}

/* Dot text with pitch p, top-left at (x, y) px, in colour c over the
 * existing pixels: rounded dots like the motto (a gap of p/4, none up to
 * 3 px), 4x4 samples per pixel. Row 0 is the top of capitals. */
static void draw_text(uint32_t *px, int w, int h, const char *s, int x, int y, int p, uint32_t c)
{
	int cols;
	unsigned char *bits = text_bitmap(s, &cols);
	int64_t p8 = (int64_t)p * 8;
	int64_t dot8 = (int64_t)(p - p / 4) * 8;
	int64_t r = dot8 / 4;

	if (!bits)
		return;
	for (int yy = y; yy < y + p * NUUBOS_FONT_ROWS && yy < h; yy++) {
		if (yy < 0)
			continue;
		for (int xx = x; xx < x + p * cols && xx < w; xx++) {
			int n = 0;

			if (xx < 0)
				continue;
			for (int j = 0; j < 4; j++)
				for (int i = 0; i < 4; i++) {
					int64_t sx = ((int64_t)xx - x) * 8 + 1 + 2 * i;
					int64_t sy = ((int64_t)yy - y) * 8 + 1 + 2 * j;
					int64_t col = sx / p8, row = sy / p8;
					int64_t dx = sx - col * p8, dy = sy - row * p8, cx, cy;

					if (col >= cols || row >= NUUBOS_FONT_ROWS || !bits[row * cols + col])
						continue;
					if (dx >= dot8 || dy >= dot8)
						continue;
					cx = dx < r ? r : dx > dot8 - r ? dot8 - r : dx;
					cy = dy < r ? r : dy > dot8 - r ? dot8 - r : dy;
					if ((dx - cx) * (dx - cx) + (dy - cy) * (dy - cy) <= r * r)
						n++;
				}
			if (n)
				px[(int64_t)yy * w + xx] = nuubos_splash_mix(px[(int64_t)yy * w + xx], c, n, 16);
		}
	}
	free(bits);
}

/* Pitch p for a line of s, smaller when it would not fit 90 % of w. */
static int line_pitch(const char *s, int w, int p)
{
	int cols = text_cols(s);

	if (p < 1)
		p = 1;
	while (p > 1 && cols * p > w * 9 / 10)
		p--;
	return p;
}

/* One centred line at pitch p. */
static void draw_line(uint32_t *px, int w, int h, const char *s, int y, int p, uint32_t c)
{
	draw_text(px, w, h, s, (w - text_cols(s) * p) / 2, y, p, c);
}

/* The charge screen at the logical size w x h (stride w). */
static void render_charge(uint32_t *px, int w, int h, const struct battery *b)
{
	struct nuubos_splash_geometry g;
	float rgb[3];
	uint32_t fill, tail, dim_text;
	int64_t fill_x8, limit_x8;
	int track_bottom, info_pitch, small_pitch, phrase_pitch, hint_pitch, hint_y, y;
	int word_top, block, top, fixed, gaps, scale;
	int gap[4];
	const char *phrase = b->full ? msg_full : msg_charging;
	char percent[16], volts[32], limit[16];

	/* Wordmark and empty track: the splash raster, then moved up so the
	 * whole block (wordmark to the line) is centred above the hint. */
	nuubos_splash_render(px, w, w, h, "reboot", &palette);
	nuubos_splash_geometry(w, h, &g);
	small_pitch = g.cell / 3 > 1 ? g.cell / 3 : 1;
	info_pitch = g.cell * 3 / 4 > 2 ? g.cell * 3 / 4 : 2;
	phrase_pitch = line_pitch(phrase, w, g.cell / 2);
	hint_pitch = line_pitch(msg_hint, w, g.cell / 4);
	hint_y = h - g.cell * 2 - hint_pitch * NUUBOS_FONT_ROWS;
	word_top = (int)(g.word_y8 / 8);
	track_bottom = (int)((g.track_y8 + g.track_h8) / 8) + 1;
	/* Gaps after the track, the limit, the percentage, the voltage and
	 * before the line, grown (up to twice) toward 80 % of the height. */
	fixed = track_bottom - word_top + small_pitch * 7 + info_pitch * 7 + small_pitch * 7 +
		phrase_pitch * NUUBOS_FONT_ROWS;
	gaps = g.cell * 15 / 2;
	scale = (hint_y * 80 / 100 - fixed) * 100 / gaps;
	scale = scale < 100 ? 100 : scale > 200 ? 200 : scale;
	gap[0] = g.cell * scale / 100;
	gap[1] = g.cell * 2 * scale / 100;
	gap[2] = g.cell * 3 / 2 * scale / 100;
	gap[3] = g.cell * 3 * scale / 100;
	block = fixed + gap[0] + gap[1] + gap[2] + gap[3];
	top = (hint_y - block) / 2;
	if (top > 0 && top < word_top) {
		int up = word_top - top;

		memmove(px, px + (size_t)up * (size_t)w, (size_t)(h - up) * (size_t)w * 4);
		for (int i = (h - up) * w; i < h * w; i++)
			px[i] = palette.background;
		g.word_y8 -= (int64_t)up * 8;
		g.track_y8 -= (int64_t)up * 8;
	}

	gauge_color(gauge_fraction(b), rgb);
	fill = to_pixel(rgb);
	tail = nuubos_splash_mix(palette.surface, palette.background, 55, 100);
	dim_text = nuubos_splash_mix(palette.background, palette.text, 55, 100);
	fill_x8 = g.track_x8 + (b->percent > 0 ? g.track_w8 * (b->percent > 100 ? 100 : b->percent) / 100 : 0);
	limit_x8 = g.track_x8 + g.track_w8 * b->limit / 100;

	/* Track: filled to the charge, surface up to the limit, dimmed above. */
	track_bottom = (int)((g.track_y8 + g.track_h8) / 8) + 1;
	for (y = (int)(g.track_y8 / 8); y < track_bottom && y < h; y++)
		for (int x = (int)(g.track_x8 / 8); x <= (int)((g.track_x8 + g.track_w8) / 8) && x < w; x++) {
			int nf = 0, nt = 0, ns = 0;

			for (int j = 0; j < 4; j++)
				for (int i = 0; i < 4; i++) {
					int64_t sx = (int64_t)x * 8 + 1 + 2 * i, sy = (int64_t)y * 8 + 1 + 2 * j;

					if (!nuubos_splash_track_hit(&g, sx, sy))
						continue;
					if (sx < fill_x8)
						nf++;
					else if (sx >= limit_x8)
						nt++;
					else
						ns++;
				}
			if (nf || nt) {
				/* One coverage sum over the background. */
				uint32_t c = 0;

				for (int sh = 0; sh <= 16; sh += 8) {
					int v = (int)((palette.background >> sh) & 0xff) * (16 - nf - nt - ns) +
						(int)((palette.surface >> sh) & 0xff) * ns +
						(int)((fill >> sh) & 0xff) * nf + (int)((tail >> sh) & 0xff) * nt;

					c |= (uint32_t)((v + 8) / 16) << sh;
				}
				px[(int64_t)y * w + x] = c;
			}
		}

	y = track_bottom + gap[0];

	/* The limit, centred under its edge of the track. */
	if (b->limit < 100) {
		int cols;

		snprintf(limit, sizeof(limit), "%d%%", b->limit);
		cols = text_cols(limit);
		draw_text(px, w, h, limit, (int)(limit_x8 / 8) - cols * small_pitch / 2, y, small_pitch, dim_text);
	}
	y += small_pitch * 7 + gap[1];

	/* The percentage, large, and the voltage under it, centred. */
	if (b->percent >= 0) {
		snprintf(percent, sizeof(percent), "%d%%", b->percent);
		draw_text(px, w, h, percent, (w - text_cols(percent) * info_pitch) / 2, y, info_pitch, palette.text);
	}
	y += info_pitch * 7 + gap[2];
	if (b->voltage_mv > 0) {
		snprintf(volts, sizeof(volts), "%d.%02d V", b->voltage_mv / 1000, (b->voltage_mv % 1000) / 10);
		draw_text(px, w, h, volts, (w - text_cols(volts) * small_pitch) / 2, y, small_pitch, dim_text);
	}
	y += small_pitch * 7 + gap[3];

	/* The line of the moment, and how to start nuubOS at the bottom. */
	draw_line(px, w, h, phrase, y, phrase_pitch,
		  nuubos_splash_mix(palette.background, palette.text, 80, 100));
	draw_line(px, w, h, msg_hint, hint_y, hint_pitch,
		  nuubos_splash_mix(palette.background, palette.text, 45, 100));
}

/* ------------------------------------------------------------------ */
/* Outputs                                                             */

static int setup_outputs(void)
{
	drmModeRes *res;
	uint32_t used = 0;

	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0)
		return -1;
	res = drmModeGetResources(drm_fd);
	if (!res)
		return -1;
	for (int i = 0; i < res->count_connectors && output_count < MAX_OUTPUTS; i++) {
		drmModeConnector *c = drmModeGetConnector(drm_fd, res->connectors[i]);
		struct drm_mode_create_dumb cd = { 0 };
		struct drm_mode_map_dumb md = { 0 };
		struct output *o = &outputs[output_count];
		drmModeModeInfo *m;
		uint32_t crtc_index;
		void *map;

		if (!c)
			continue;
		if (c->connection != DRM_MODE_CONNECTED || !(m = pick_mode(c)) ||
		    !(crtc_index = pick_crtc(drm_fd, res, c, used))) {
			drmModeFreeConnector(c);
			continue;
		}
		cd.width = m->hdisplay;
		cd.height = m->vdisplay;
		cd.bpp = 32;
		if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0 &&
		    drmModeAddFB(drm_fd, cd.width, cd.height, 24, 32, cd.pitch, cd.handle, &o->fb) == 0) {
			md.handle = cd.handle;
			if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0 &&
			    (map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, md.offset)) != MAP_FAILED) {
				o->connector = c->connector_id;
				o->crtc = res->crtcs[crtc_index - 1];
				o->handle = cd.handle;
				o->mode = *m;
				o->map = map;
				o->size = cd.size;
				o->stride = (int)(cd.pitch / 4);
				o->transform = panel_transform(drm_fd, c->connector_id);
				used |= 1u << (crtc_index - 1);
				output_count++;
			}
		}
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	return output_count ? 0 : -1;
}

/* Draws the charge screen (b) or a splash raster (mode) on every output. */
static void draw_outputs(const struct battery *b, const char *mode)
{
	for (int i = 0; i < output_count; i++) {
		struct output *o = &outputs[i];
		int w = o->mode.hdisplay, h = o->mode.vdisplay;
		int lw = (o->transform & 1) ? h : w, lh = (o->transform & 1) ? w : h;
		uint32_t *buf = malloc((size_t)lw * (size_t)lh * 4);

		if (!buf)
			continue;
		if (b)
			render_charge(buf, lw, lh, b);
		else
			nuubos_splash_render(buf, lw, lw, lh, mode, &palette);
		nuubos_kms_rotate_copy(o->map, o->stride, w, h, o->transform, buf);
		free(buf);
	}
}

static void backlight_power(bool on)
{
	glob_t g;

	if (glob("/sys/class/backlight/*/bl_power", 0, NULL, &g) != 0)
		return;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		FILE *fp = fopen(g.gl_pathv[i], "w");

		if (fp) {
			fputs(on ? "0\n" : "4\n", fp);
			fclose(fp);
		}
	}
	globfree(&g);
}

static void screen_on(void)
{
	for (int i = 0; i < output_count; i++)
		(void)drmModeSetCrtc(drm_fd, outputs[i].crtc, outputs[i].fb, 0, 0,
				     &outputs[i].connector, 1, &outputs[i].mode);
	backlight_power(true);
}

static void screen_off(void)
{
	backlight_power(false);
	for (int i = 0; i < output_count; i++)
		(void)drmModeSetCrtc(drm_fd, outputs[i].crtc, 0, 0, 0, NULL, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* RGB LEDs (nuubos-rgbd frame sink; lightingd is not running yet)      */

static int rgb_fd = -1;
static int led_zone_count;
static int led_zone_len[8];

static int rgb_command(const char *cmd, char *reply, size_t size)
{
	size_t used = 0;

	if (rgb_fd < 0 || write(rgb_fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd))
		return -1;
	while (used + 1 < size) {
		ssize_t n = read(rgb_fd, reply + used, 1);

		if (n <= 0)
			return -1;
		if (reply[used++] == '\n')
			break;
	}
	reply[used] = '\0';
	return 0;
}

static void rgb_open(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	char reply[512];
	const char *p;

	rgb_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (rgb_fd < 0)
		return;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", RGB_SOCKET);
	if (connect(rgb_fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
	    rgb_command("STATUS\n", reply, sizeof(reply)) != 0 || !strstr(reply, "supported=1")) {
		close(rgb_fd);
		rgb_fd = -1;
		return;
	}
	/* zone<N>=<name>:<type>:<count>: the count follows the last colon. */
	for (p = strstr(reply, " zone"); p && led_zone_count < 8; p = strstr(p + 1, " zone")) {
		const char *next = strstr(p + 1, " zone");
		const char *last = NULL;

		for (const char *c = p; *c && (!next || c < next); c++)
			if (*c == ':')
				last = c;
		if (last)
			led_zone_len[led_zone_count++] = atoi(last + 1);
	}
}

/* LEDs respond linearly: gamma 2.2, as lightingd. */
static unsigned int led_byte(float v)
{
	float x;

	v = v < 0 ? 0 : v > 1 ? 1 : v;
	x = powf(v, 2.2f) * LED_GAIN;
	return (unsigned int)(x * 255.0f + 0.5f);
}

/* Ring position at the bottom of the stick, in turns from logical
 * position 0 (lightingd RING_BOTTOM). */
#define RING_BOTTOM 0.375f
/* Charging: the rings fill up from the bottom along both sides to the
 * charge (relative to the limit) in FILL_MS, hold HOLD_MS, start again. */
#define FILL_MS 1600
#define HOLD_MS 1000
#define FRAME_MS 33

static float ring_distance(float a, float b)
{
	float d = fabsf(a - b);

	d -= floorf(d);
	return d > 0.5f ? 1.0f - d : d;
}

/* The rings at time t_ms of the charging cycle. on = false: dark (rgbd
 * powers the MCU off). Complete: full rings, static. */
static void leds_show(const struct battery *b, bool on, long long t_ms)
{
	char cmd[MAX_LEDS * 12 + 16], reply[128];
	size_t used;
	float rgb[3], f = gauge_fraction(b), level = f;

	if (rgb_fd < 0)
		return;
	if (!on) {
		(void)rgb_command("OFF\n", reply, sizeof(reply));
		return;
	}
	if (!b->full) {
		long long c = t_ms % (FILL_MS + HOLD_MS);
		float k = c < FILL_MS ? (float)c / FILL_MS : 1.0f;

		level = f * (1.0f - (1.0f - k) * (1.0f - k)); /* ease out */
	}
	if (f < 0.06f)
		f = 0.06f;
	gauge_color(f, rgb);
	used = (size_t)snprintf(cmd, sizeof(cmd), "FRAME");
	for (int z = 0; z < led_zone_count; z++) {
		int n = led_zone_len[z];

		for (int i = 0; i < n && used + 40 < sizeof(cmd); i++) {
			/* Coverage of this LED by the filled arc around the bottom. */
			float d = ring_distance((float)i / (float)n, RING_BOTTOM);
			float k = (0.5f * level - d) * (float)n + 0.5f;

			k = k < 0 ? 0 : k > 1 ? 1 : k;
			used += (size_t)snprintf(cmd + used, sizeof(cmd) - used, " %u %u %u",
						 led_byte(rgb[0] * k), led_byte(rgb[1] * k), led_byte(rgb[2] * k));
		}
	}
	snprintf(cmd + used, sizeof(cmd) - used, "\n");
	(void)rgb_command(cmd, reply, sizeof(reply));
}

/* ------------------------------------------------------------------ */
/* Input and events                                                    */

static int key_fds[MAX_KEYS];
static int key_count;

static void open_keys(void)
{
	glob_t g;

	if (glob("/dev/input/event*", 0, NULL, &g) != 0)
		return;
	for (size_t i = 0; i < g.gl_pathc && key_count < MAX_KEYS; i++) {
		unsigned long evbits = 0;
		int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);

		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0 || !(evbits & (1UL << EV_KEY))) {
			close(fd);
			continue;
		}
		key_fds[key_count++] = fd;
	}
	globfree(&g);
}

static int uevent_socket(void)
{
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);

	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void arm(int fd, int first_s, int interval_s)
{
	struct itimerspec t = { .it_value.tv_sec = first_s, .it_interval.tv_sec = interval_s };

	(void)timerfd_settime(fd, 0, &t, NULL);
}

static long long anim_epoch;

static long long now_ms(void);

/* The boot key's release is consumed here (at most 3 s) so nothing that
 * starts later sees half a key press. */
static void wait_key_released(unsigned short code)
{
	struct pollfd pfd[MAX_KEYS];
	long long end = now_ms() + 3000;

	for (int i = 0; i < key_count; i++)
		pfd[i] = (struct pollfd){ .fd = key_fds[i], .events = POLLIN };
	while (now_ms() < end) {
		if (poll(pfd, (nfds_t)key_count, (int)(end - now_ms())) <= 0)
			return;
		for (int i = 0; i < key_count; i++) {
			struct input_event ev;

			if (!pfd[i].revents)
				continue;
			while (read(key_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
				if (ev.type == EV_KEY && ev.code == code && ev.value == 0)
					return;
		}
	}
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The LED frame timer runs only while the rings animate. */
static void update_anim(int fd, bool on, const struct battery *b)
{
	struct itimerspec t = { 0 };

	if (on && !b->full && rgb_fd >= 0) {
		t.it_value.tv_nsec = FRAME_MS * 1000000L;
		t.it_interval.tv_nsec = FRAME_MS * 1000000L;
	}
	(void)timerfd_settime(fd, 0, &t, NULL);
}

/* Boot: the boot splash raster stays on screen (the device is held open
 * without master until nuubUI is ready, as S00 does) and init continues. */
static void continue_boot(void)
{
	draw_outputs(NULL, "boot");
	screen_on();
	if (rgb_fd >= 0) {
		char reply[64];

		(void)rgb_command("OFF\n", reply, sizeof(reply));
	}
	drmDropMaster(drm_fd);
	if (fork() == 0) {
		setsid();
		hold_device(UI_READY);
		_exit(0);
	}
}

static void power_off(void)
{
	char reply[64];

	draw_outputs(NULL, "poweroff");
	if (rgb_fd >= 0)
		(void)rgb_command("OFF\n", reply, sizeof(reply));
	sync();
	execl("/sbin/poweroff", "poweroff", (char *)NULL);
	_exit(1);
}

int main(int argc, char **argv)
{
	struct battery b;
	struct pollfd pfd[MAX_KEYS + 4];
	int pwron, uev, screen_timer, refresh_timer, anim_timer;
	bool on = true;

	/* --ppm FILE W H PERCENT MV LIMIT FULL [LANGFILE]: host test raster. */
	if (argc >= 9 && !strcmp(argv[1], "--ppm")) {
		int w = atoi(argv[3]), h = atoi(argv[4]);
		uint32_t *px = malloc((size_t)w * (size_t)h * 4);
		FILE *fp = fopen(argv[2], "wb");

		if (!px || !fp)
			return 1;
		b = (struct battery){ atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), atoi(argv[8]) != 0, true };
		nuubos_splash_load_palette(&palette, "/nonexistent");
		if (argc >= 10) {
			(void)read_key(argv[9], "901", msg_charging, sizeof(msg_charging));
			(void)read_key(argv[9], "902", msg_full, sizeof(msg_full));
			(void)read_key(argv[9], "903", msg_hint, sizeof(msg_hint));
		}
		render_charge(px, w, h, &b);
		fprintf(fp, "P6\n%d %d\n255\n", w, h);
		for (int i = 0; i < w * h; i++) {
			unsigned char rgb[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i] };

			fwrite(rgb, 1, 3, fp);
		}
		fclose(fp);
		return 0;
	}
	/* --force: show the screen whatever started the console (testing). */
	pwron = argc > 1 && !strcmp(argv[1], "--force") ? AXP717_PWRON_VBUS : pmic_read(AXP717_PWRON_STATUS);
	if (pwron < 0 || !(pwron & AXP717_PWRON_VBUS) || (pwron & AXP717_PWRON_KEY))
		return 0;
	if (!(argc > 1 && !strcmp(argv[1], "--force")) && hdmi_connected()) {
		fprintf(stderr, "nuubos-charge: HDMI connected, booting\n");
		return 0;
	}
	find_supplies();
	if (!battery_present() || !charger_online())
		return 0;

	apply_charge_limit();
	load_strings(NULL);
	nuubos_splash_load_palette(&palette, NULL);
	if (setup_outputs() != 0)
		return 0;
	rgb_open();
	open_keys();
	uev = uevent_socket();
	screen_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	refresh_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	anim_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	read_battery(&b);
	draw_outputs(&b, NULL);
	screen_on();
	arm(screen_timer, SCREEN_TIMEOUT_S, 0);
	arm(refresh_timer, REFRESH_S, REFRESH_S);
	anim_epoch = now_ms();
	leds_show(&b, true, 0);
	update_anim(anim_timer, on, &b);

	for (;;) {
		int n = 0;
		bool changed = false, wake = false;

		pfd[n++] = (struct pollfd){ .fd = screen_timer, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = refresh_timer, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = anim_timer, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = uev, .events = POLLIN };
		for (int i = 0; i < key_count; i++)
			pfd[n++] = (struct pollfd){ .fd = key_fds[i], .events = POLLIN };
		if (poll(pfd, (nfds_t)n, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (pfd[0].revents) {
			uint64_t x;

			if (read(screen_timer, &x, sizeof(x)) < 0)
				x = 0;
			if (on) {
				/* Dark with the screen, except a complete charge. */
				on = false;
				screen_off();
				arm(refresh_timer, 0, 0);
				leds_show(&b, b.full, 0);
				update_anim(anim_timer, on, &b);
			}
		}
		if (pfd[1].revents) {
			uint64_t x;

			if (read(refresh_timer, &x, sizeof(x)) < 0)
				x = 0;
			changed = true;
		}
		if (pfd[2].revents) {
			uint64_t x;

			if (read(anim_timer, &x, sizeof(x)) < 0)
				x = 0;
			if (on && !b.full)
				leds_show(&b, true, now_ms() - anim_epoch);
		}
		if (pfd[3].revents) {
			char buf[4096];
			ssize_t r;

			while ((r = recv(uev, buf, sizeof(buf) - 1, 0)) > 0) {
				buf[r] = '\0';
				if (memmem(buf, (size_t)r, "SUBSYSTEM=power_supply", 22))
					changed = true;
			}
		}
		for (int i = 4; i < n; i++) {
			struct input_event ev;

			if (!pfd[i].revents)
				continue;
			while (read(pfd[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (ev.type != EV_KEY || ev.value != 1)
					continue;
				if (ev.code == KEY_POWER || (ev.code == BTN_START && on)) {
					wait_key_released(ev.code);
					continue_boot();
					return 0;
				}
				wake = true;
			}
		}

		if (changed) {
			struct battery next;

			read_battery(&next);
			if (!next.external)
				power_off();
			if (memcmp(&next, &b, sizeof(b)) != 0) {
				bool gauge = next.percent != b.percent || next.full != b.full || next.limit != b.limit;

				b = next;
				if (on)
					draw_outputs(&b, NULL);
				if (gauge) {
					leds_show(&b, on || b.full, now_ms() - anim_epoch);
					update_anim(anim_timer, on, &b);
				}
			}
		}
		if (wake) {
			if (!on) {
				on = true;
				draw_outputs(&b, NULL);
				screen_on();
				arm(refresh_timer, REFRESH_S, REFRESH_S);
				anim_epoch = now_ms();
				leds_show(&b, true, 0);
				update_anim(anim_timer, on, &b);
			}
			arm(screen_timer, SCREEN_TIMEOUT_S, 0);
		}
	}
	continue_boot();
	return 0;
}
