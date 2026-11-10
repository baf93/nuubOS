/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubOS lifecycle splash (boot, restart, power off): the one raster every
 * stage of the handoff shows, so kernel → compositor → nuubUI → shutdown
 * never jumps.
 *
 * Renderers that MUST stay pixel-identical (tools/splash-check compares
 * them):
 *   - this header: nuubos-splash (kernel fbdev) and, as a copy added by
 *     package/nuubos/labwc/0001, labwc's first frame;
 *   - src/splash_core.rs in nuubui-home and nuubos-quick-menu (identical
 *     copies) for the animated stages.
 *
 * Design (2026-10-10, logo chosen by the user): the nuubOS wordmark as a
 * dot matrix (rounded LED dots on a 7-row grid, aligned to whole pixels),
 * "nuub" in the Mood's text colour and "OS" in its accent, a pill track
 * below it in the Mood's surface colour, on the Mood's background. The
 * same dot table is the logo everywhere: nuubUI's NuubLogo (home.slint)
 * and the Web administration (www/app.js) keep copies of it. Modes:
 *   boot, reboot   wordmark + empty track
 *   poweroff       wordmark dimmed toward the background, no track
 *   full           wordmark + track filled with the accent (first frame of
 *                  the power-off animation; nuubUI only)
 * The animations (track filling, sweeping, draining; RGB in step) are
 * drawn by nuubUI on top of these rasters and end on one of them.
 *
 * Everything is integer arithmetic (positions in 1/8 px, 4x4 samples per
 * pixel, exact squared-distance tests), so C and Rust agree to the bit.
 * Palette: /etc/nuubos/splash.palette (written by nuubos-themectl from the
 * active user's Mood: background=, text=, accent=, surface= as #rrggbb);
 * built-in nuubOS colours when missing.
 */
#ifndef NUUBOS_SPLASH_RENDER_H
#define NUUBOS_SPLASH_RENDER_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NUUBOS_SPLASH_PALETTE "/etc/nuubos/splash.palette"

struct nuubos_splash_palette {
	uint32_t background, text, accent, surface;
};

struct nuubos_splash_geometry {
	int cell;           /* dot grid pitch, px */
	int cap;            /* wordmark height (7 cells), px */
	int64_t word_x8, word_y8, word_w8;   /* 1/8 px */
	int64_t track_x8, track_y8, track_w8, track_h8;
	int motto_pitch;    /* px */
	int64_t motto_x8, motto_y8;
};

/* The logo: 7 rows x 37 columns, '#' = a dot. Columns 0-23 are "nuub",
 * 25-36 "OS" (accent). Copies: splash_core.rs, home.slint NuubLogo,
 * www/app.js LOGO. */
#define NUUBOS_LOGO_COLS 37
#define NUUBOS_LOGO_ROWS 7
#define NUUBOS_LOGO_ACCENT_COL 25
static const char *const nuubos_logo[NUUBOS_LOGO_ROWS] = {
	"                  #       ####   ####",
	"                  #      ##  ## #    ",
	"####  #   # #   # ####   #    # #    ",
	"#   # #   # #   # #   #  #    #  ### ",
	"#   # #   # #   # #   #  #    #     #",
	"#   # #   # #   # #   #  ##  ##     #",
	"#   #  ####  #### ####    ####  #### ",
};

/* The motto, 7 rows x 173 columns (5x7 letters, 1 column apart). Copy:
 * splash_core.rs MOTTO. */
#define NUUBOS_MOTTO_COLS 173
static const char *const nuubos_motto[NUUBOS_LOGO_ROWS] = {
	" ####              ##    ##          ####              #                            ####    #                ###      #                    #                                 ",
	"#                   #     #         #                  #                            #   #        ####       #   #     #                    #                                 ",
	"#     ## ##  ###    #     #         #     #   #  #### ###    ###  ## ##             #   #  ##   #   #       #   #  #### #   #  ###  ####  ###   #   # # ##   ###   ####      ",
	" ###  # # #     #   #     #          ###  #   # #      #    #   # # # #             ####    #   #   #       ##### #   # #   # #   # #   #  #    #   # ##  # #   # #          ",
	"    # # # #  ####   #     #             #  ####  ###   #    ##### # # #             #   #   #    ####       #   # #   # #   # ##### #   #  #    #   # #     #####  ###       ",
	"    # #   # #   #   #     #             #     #     #  #  # #     #   #  ##         #   #   #       #       #   # #   #  # #  #     #   #  #  # #   # #     #         #  ##  ",
	"####  #   #  ####  ###   ###        ####   ###  ####    ##   #### #   #  ##         ####   ###   ###        #   #  ####   #    #### #   #   ##   #### #      #### ####   ##  ",
};

static uint32_t nuubos_splash_hex(const char *s, uint32_t fallback)
{
	uint32_t v = 0;

	if (s[0] != '#')
		return fallback;
	for (int i = 1; i <= 6; i++) {
		char c = s[i];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;

		if (d < 0)
			return fallback;
		v = v * 16 + (uint32_t)d;
	}
	return v;
}

static void nuubos_splash_load_palette(struct nuubos_splash_palette *p, const char *path)
{
	char line[128];
	FILE *fp;

	p->background = 0x0d0e10;
	p->text = 0xf5f6f8;
	p->accent = 0x3a86ff;
	p->surface = 0x24272d;
	fp = fopen(path ? path : NUUBOS_SPLASH_PALETTE, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *eq = strchr(line, '=');

		if (!eq)
			continue;
		*eq++ = '\0';
		if (!strcmp(line, "background"))
			p->background = nuubos_splash_hex(eq, p->background);
		else if (!strcmp(line, "text"))
			p->text = nuubos_splash_hex(eq, p->text);
		else if (!strcmp(line, "accent"))
			p->accent = nuubos_splash_hex(eq, p->accent);
		else if (!strcmp(line, "surface"))
			p->surface = nuubos_splash_hex(eq, p->surface);
	}
	fclose(fp);
}

static void nuubos_splash_geometry(int width, int height, struct nuubos_splash_geometry *g)
{
	int cell = height * 11 / 100 / NUUBOS_LOGO_ROWS;
	int word_w, top, th;

	if (cell > width * 80 / 100 / NUUBOS_LOGO_COLS)
		cell = width * 80 / 100 / NUUBOS_LOGO_COLS;
	if (cell < 3)
		cell = 3;
	g->cell = cell;
	g->cap = cell * NUUBOS_LOGO_ROWS;
	word_w = cell * NUUBOS_LOGO_COLS;
	g->word_w8 = (int64_t)word_w * 8;
	g->word_x8 = (int64_t)((width - word_w) / 2) * 8;
	top = height * 43 / 100 - g->cap / 2;
	g->word_y8 = (int64_t)top * 8;
	th = cell * 3 / 4;
	if (th < 3)
		th = 3;
	g->track_w8 = g->word_w8;
	g->track_x8 = g->word_x8;
	g->track_y8 = (int64_t)(top + g->cap + cell * 3) * 8;
	g->track_h8 = (int64_t)th * 8;
	g->motto_pitch = cell / 3;
	if (g->motto_pitch * NUUBOS_MOTTO_COLS > width * 90 / 100)
		g->motto_pitch = width * 90 / 100 / NUUBOS_MOTTO_COLS;
	if (g->motto_pitch < 1)
		g->motto_pitch = 1;
	g->motto_x8 = (int64_t)((width - g->motto_pitch * NUUBOS_MOTTO_COLS) / 2) * 8;
	g->motto_y8 = (int64_t)(top + g->cap + cell * 3 + th + cell * 2) * 8;
}

/* One motto sample: dots of pitch p with a gap of p/4 (none up to 3 px:
 * small letters stay solid) and rounded corners. */
static int nuubos_splash_motto_hit(const struct nuubos_splash_geometry *g, int64_t sx, int64_t sy)
{
	int64_t p8 = (int64_t)g->motto_pitch * 8;
	int64_t dot8 = (int64_t)(g->motto_pitch - g->motto_pitch / 4) * 8;
	int64_t r = dot8 / 4;
	int64_t x = sx - g->motto_x8, y = sy - g->motto_y8;
	int64_t col, row, dx, dy, cx, cy;

	if (x < 0 || y < 0 || x >= p8 * NUUBOS_MOTTO_COLS || y >= p8 * NUUBOS_LOGO_ROWS)
		return 0;
	col = x / p8;
	row = y / p8;
	if (nuubos_motto[row][col] != '#')
		return 0;
	dx = x - col * p8;
	dy = y - row * p8;
	if (dx >= dot8 || dy >= dot8)
		return 0;
	cx = dx < r ? r : dx > dot8 - r ? dot8 - r : dx;
	cy = dy < r ? r : dy > dot8 - r ? dot8 - r : dy;
	return (dx - cx) * (dx - cx) + (dy - cy) * (dy - cy) <= r * r;
}

/* 0 = outside, 1 = "nuub", 2 = "OS" for one sample (1/8 px). A dot fills
 * its cell but a gap of cell/6 px (at least 1) on the right and bottom,
 * with corners rounded to a quarter of its size. */
static int nuubos_splash_word_hit(const struct nuubos_splash_geometry *g, int64_t sx, int64_t sy)
{
	int64_t x = sx - g->word_x8, y = sy - g->word_y8;
	int64_t cell8 = (int64_t)g->cell * 8;
	int gap = g->cell / 6 > 1 ? g->cell / 6 : 1;
	int64_t dot8 = (int64_t)(g->cell - gap) * 8;
	int64_t r = dot8 / 4;
	int64_t col, row, dx, dy, cx, cy;

	if (x < 0 || y < 0 || x >= g->word_w8 || y >= cell8 * NUUBOS_LOGO_ROWS)
		return 0;
	col = x / cell8;
	row = y / cell8;
	if (nuubos_logo[row][col] != '#')
		return 0;
	dx = x - col * cell8;
	dy = y - row * cell8;
	if (dx >= dot8 || dy >= dot8)
		return 0;
	cx = dx < r ? r : dx > dot8 - r ? dot8 - r : dx;
	cy = dy < r ? r : dy > dot8 - r ? dot8 - r : dy;
	if ((dx - cx) * (dx - cx) + (dy - cy) * (dy - cy) > r * r)
		return 0;
	return col >= NUUBOS_LOGO_ACCENT_COL ? 2 : 1;
}

static int nuubos_splash_track_hit(const struct nuubos_splash_geometry *g, int64_t sx, int64_t sy)
{
	int64_t r = g->track_h8 / 2;
	int64_t x0 = g->track_x8, x1 = g->track_x8 + g->track_w8, y0 = g->track_y8;
	int64_t cx, dy;

	if (sy < y0 || sy >= y0 + g->track_h8 || sx < x0 || sx >= x1)
		return 0;
	if (sx >= x0 + r && sx < x1 - r)
		return 1;
	cx = sx < x0 + r ? x0 + r : x1 - r;
	dy = sy - (y0 + r);
	return (sx - cx) * (sx - cx) + dy * dy <= r * r;
}

static uint32_t nuubos_splash_mix(uint32_t from, uint32_t to, int k, int n)
{
	uint32_t out = 0;

	for (int s = 0; s <= 16; s += 8) {
		int a = (int)((from >> s) & 0xff), b = (int)((to >> s) & 0xff);

		out |= (uint32_t)((a * (n - k) + b * k + n / 2) / n) << s;
	}
	return out;
}

/* Renders into 0x00RRGGBB pixels (stride in pixels). */
static void nuubos_splash_render(uint32_t *px, int stride, int width, int height, const char *mode,
				 const struct nuubos_splash_palette *p)
{
	struct nuubos_splash_geometry g;
	int dim = mode && !strcmp(mode, "poweroff");
	int full = mode && !strcmp(mode, "full");
	uint32_t fg = dim ? nuubos_splash_mix(p->background, p->text, 35, 100) : p->text;
	uint32_t ac = dim ? nuubos_splash_mix(p->background, p->accent, 35, 100) : p->accent;
	uint32_t track = full ? p->accent : p->surface;
	int y0, y1, x0, x1;

	nuubos_splash_geometry(width, height, &g);
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++)
			px[(int64_t)y * stride + x] = p->background;

	y0 = (int)(g.word_y8 / 8);
	y1 = (int)((g.track_y8 + g.track_h8) / 8) + 1;
	x0 = (int)(g.word_x8 / 8);
	x1 = (int)((g.word_x8 + g.word_w8) / 8) + 1;
	if (y0 < 0) y0 = 0;
	if (x0 < 0) x0 = 0;
	if (y1 > height) y1 = height;
	if (x1 > width) x1 = width;
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			int nw = 0, no = 0, nt = 0;

			for (int j = 0; j < 4; j++) {
				for (int i = 0; i < 4; i++) {
					int64_t sx = (int64_t)x * 8 + 1 + 2 * i, sy = (int64_t)y * 8 + 1 + 2 * j;
					int w = nuubos_splash_word_hit(&g, sx, sy);

					if (w == 1)
						nw++;
					else if (w == 2)
						no++;
					else if (!dim && nuubos_splash_track_hit(&g, sx, sy))
						nt++;
				}
			}
			if (nw || no || nt) {
				/* Wordmark and track never overlap: one coverage sum
				 * over the background. */
				uint32_t c = 0;

				for (int sh = 0; sh <= 16; sh += 8) {
					int v = (int)((p->background >> sh) & 0xff) * (16 - nw - no - nt) +
						(int)((fg >> sh) & 0xff) * nw + (int)((ac >> sh) & 0xff) * no +
						(int)((track >> sh) & 0xff) * nt;

					c |= (uint32_t)((v + 8) / 16) << sh;
				}
				px[(int64_t)y * stride + x] = c;
			}
		}
	}

	/* The motto, at boot only. */
	if (!mode || strcmp(mode, "boot"))
		return;
	{
		uint32_t mc = nuubos_splash_mix(p->background, p->text, 55, 100);
		int pitch = g.motto_pitch;

		y0 = (int)(g.motto_y8 / 8);
		y1 = y0 + pitch * NUUBOS_LOGO_ROWS;
		x0 = (int)(g.motto_x8 / 8);
		x1 = x0 + pitch * NUUBOS_MOTTO_COLS;
		if (y0 < 0) y0 = 0;
		if (x0 < 0) x0 = 0;
		if (y1 > height) y1 = height;
		if (x1 > width) x1 = width;
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) {
				int nm = 0;

				for (int j = 0; j < 4; j++)
					for (int i = 0; i < 4; i++)
						nm += nuubos_splash_motto_hit(&g, (int64_t)x * 8 + 1 + 2 * i,
									      (int64_t)y * 8 + 1 + 2 * j);
				if (nm)
					px[(int64_t)y * stride + x] = nuubos_splash_mix(p->background, mc, nm, 16);
			}
		}
	}
}

#endif
