/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * KMS helpers shared by nuubos-splash and nuubos-charge: panel
 * orientation, the output policy's mode, CRTC choice, the rotation of a
 * logical raster into a scan-out buffer, and holding the DRM device so the
 * last frame stays on screen.
 */
#ifndef NUUBOS_SPLASH_KMS_H
#define NUUBOS_SPLASH_KMS_H

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* Wayland transform (0 normal, 1 = 90, 2 = 180, 3 = 270) of a connector's
 * "panel orientation" property, mapped exactly as wlroots does: the RG28XX
 * panel is 480x640 mounted sideways (device tree rotation = <270>, "Left
 * Side Up", transform 90), labwc applies it to the output. */
static inline int panel_transform(int fd, uint32_t connector)
{
	drmModeObjectProperties *props;
	int t = 0;

	props = drmModeObjectGetProperties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);

		if (!p)
			continue;
		if (!strcmp(p->name, "panel orientation"))
			for (int e = 0; e < p->count_enums; e++) {
				if (p->enums[e].value != props->prop_values[i])
					continue;
				t = !strcmp(p->enums[e].name, "Left Side Up") ? 1
				  : !strcmp(p->enums[e].name, "Upside Down") ? 2
				  : !strcmp(p->enums[e].name, "Right Side Up") ? 3 : 0;
			}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return t;
}

/* Transform of the built-in panel, for the fbdev path (no master needed). */
static inline int builtin_panel_transform(void)
{
	drmModeRes *res;
	int t = 0;
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

	if (fd < 0)
		return 0;
	if ((res = drmModeGetResources(fd))) {
		for (int i = 0; i < res->count_connectors && !t; i++) {
			drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);

			if (!c)
				continue;
			if (c->connector_type != DRM_MODE_CONNECTOR_HDMIA)
				t = panel_transform(fd, c->connector_id);
			drmModeFreeConnector(c);
		}
		drmModeFreeResources(res);
	}
	close(fd);
	return t;
}

/* The HDMI mode of the UI's output policy (nuubos-wl-output-apply): the
 * preferred mode when it has at most 1080 lines, otherwise the tallest mode
 * up to 1080 lines, at the refresh closest to the preferred one. Built-in
 * panels: their preferred (native) mode. */
static inline drmModeModeInfo *pick_mode(drmModeConnector *c)
{
	drmModeModeInfo *pref = NULL, *best = NULL;
	int hdmi = c->connector_type == DRM_MODE_CONNECTOR_HDMIA || c->connector_type == DRM_MODE_CONNECTOR_HDMIB;

	for (int i = 0; i < c->count_modes; i++)
		if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			pref = &c->modes[i];
			break;
		}
	if (!pref && c->count_modes)
		pref = &c->modes[0];
	if (!hdmi || !pref || pref->vdisplay <= 1080)
		return pref;
	for (int i = 0; i < c->count_modes; i++) {
		drmModeModeInfo *m = &c->modes[i];

		if (m->vdisplay > 1080 || (m->flags & DRM_MODE_FLAG_INTERLACE))
			continue;
		if (!best || m->vdisplay > best->vdisplay ||
		    (m->vdisplay == best->vdisplay &&
		     abs((int)m->vrefresh - (int)pref->vrefresh) < abs((int)best->vrefresh - (int)pref->vrefresh)))
			best = m;
	}
	return best ? best : pref;
}

static inline uint32_t pick_crtc(int fd, drmModeRes *res, drmModeConnector *c, uint32_t used)
{
	for (int e = 0; e < c->count_encoders; e++) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, c->encoders[e]);

		if (!enc)
			continue;
		for (int i = 0; i < res->count_crtcs; i++) {
			if ((enc->possible_crtcs & (1u << i)) && !(used & (1u << i))) {
				drmModeFreeEncoder(enc);
				return (uint32_t)i + 1;
			}
		}
		drmModeFreeEncoder(enc);
	}
	return 0;
}

/* Sleeps until nuubUI is ready (or forever without a path), holding fd. */
static inline void hold_device(const char *ready)
{
	signal(SIGTERM, SIG_DFL);
	if (ready) {
		int in = inotify_init1(IN_CLOEXEC);
		char buf[4096];

		if (in >= 0 && inotify_add_watch(in, "/run/nuubos", IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE) >= 0) {
			while (access(ready, F_OK) != 0)
				if (read(in, buf, sizeof(buf)) <= 0)
					break;
			return;
		}
	}
	for (;;)
		pause();
}

/* Copies a logical lw x lh raster (t & 1: lw = h, lh = w) into a w x h
 * scan-out buffer through transform t (physical = logical through the
 * inverse transform, as wlroots renders). */
static inline void nuubos_kms_rotate_copy(uint32_t *dst, int stride, int w, int h, int t, const uint32_t *src)
{
	int lw = (t & 1) ? h : w;
	int lh = (t & 1) ? w : h;

	for (int ly = 0; ly < lh; ly++)
		for (int lx = 0; lx < lw; lx++) {
			int px = t == 1 ? ly : t == 2 ? lw - 1 - lx : t == 3 ? lh - 1 - ly : lx;
			int py = t == 1 ? lw - 1 - lx : t == 2 ? lh - 1 - ly : t == 3 ? lx : ly;

			dst[py * stride + px] = src[ly * lw + lx];
		}
}

#endif
