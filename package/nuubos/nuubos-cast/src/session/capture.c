/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Screen capture: one wlr-screencopy frame at a time into shared memory
 * buffers the session converts and encodes. Two buffers alternate, so the
 * compositor copies frame N+1 while frame N is converted and encoded. The first frame is a plain copy,
 * the following ones copy_with_damage, so a static screen costs nothing
 * (the session repeats the last picture itself when a stream needs it).
 *
 * The captured output: HDMI when it is on (nuubOS drives only HDMI then),
 * otherwise the first output. Output hotplug selects again; frames keep
 * their buffer size and the converter adapts.
 */
#include "capture.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

#define MAX_OUTPUTS 4

struct output {
	struct wl_output *wl;
	uint32_t global;
	int transform;
	char name[32];
	bool hdmi;
};

struct capture {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct zwlr_screencopy_manager_v1 *manager;
	struct output outputs[MAX_OUTPUTS];
	int output_count;
	int target; /* index in outputs[], -1 = none */

	struct zwlr_screencopy_frame_v1 *frame;
	bool frame_damage;
	uint32_t shm_format;
	int shm_width, shm_height, shm_stride;
	bool have_shm_format;
	uint32_t flags;
	bool ready;
	bool done;
	int64_t ready_time_us;
	int frame_transform;

	struct wl_buffer *buffer[2];
	void *data;     /* both buffers, one mapping */
	size_t size;    /* one buffer */
	int cur;        /* buffer the pending copy writes */
	int done_index; /* buffer of the last finished frame */
	int buf_width, buf_height, buf_stride;
	uint32_t buf_format;

	bool broken;
	bool reading;

	/* Test pattern instead of the compositor (host tests):
	 * NUUBOS_CAST_FAKE_SCREEN=WxH. */
	bool fake;
	int fake_pipe[2];
	unsigned fake_frame;
};

/* ------------------------------------------------------------------ */

static void output_geometry(void *data, struct wl_output *wl, int32_t x, int32_t y,
			    int32_t pw, int32_t ph, int32_t subpixel, const char *make,
			    const char *model, int32_t transform)
{
	struct output *o = data;

	(void)wl; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel; (void)make; (void)model;
	o->transform = transform;
}

static void output_mode(void *data, struct wl_output *wl, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
	(void)data; (void)wl; (void)flags; (void)w; (void)h; (void)r;
}

static void output_done(void *data, struct wl_output *wl)
{
	(void)data; (void)wl;
}

static void output_scale(void *data, struct wl_output *wl, int32_t factor)
{
	(void)data; (void)wl; (void)factor;
}

static void output_name(void *data, struct wl_output *wl, const char *name)
{
	struct output *o = data;

	(void)wl;
	copy_text(o->name, sizeof(o->name), name);
	o->hdmi = strncmp(name, "HDMI", 4) == 0;
}

static void output_description(void *data, struct wl_output *wl, const char *description)
{
	(void)data; (void)wl; (void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void select_target(struct capture *c)
{
	c->target = -1;
	for (int i = 0; i < c->output_count; i++)
		if (c->outputs[i].hdmi) {
			c->target = i;
			return;
		}
	if (c->output_count > 0)
		c->target = 0;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	struct capture *c = data;

	if (!strcmp(interface, wl_shm_interface.name)) {
		c->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name) && version >= 3) {
		c->manager = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 3);
	} else if (!strcmp(interface, wl_output_interface.name) && c->output_count < MAX_OUTPUTS) {
		struct output *o = &c->outputs[c->output_count++];

		memset(o, 0, sizeof(*o));
		o->global = name;
		o->wl = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
		wl_output_add_listener(o->wl, &output_listener, o);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	struct capture *c = data;

	(void)registry;
	for (int i = 0; i < c->output_count; i++) {
		if (c->outputs[i].global != name)
			continue;
		wl_output_destroy(c->outputs[i].wl);
		memmove(&c->outputs[i], &c->outputs[i + 1],
			(size_t)(c->output_count - i - 1) * sizeof(c->outputs[0]));
		c->output_count--;
		/* listeners hold pointers into outputs[]: rebind them */
		for (int j = 0; j < c->output_count; j++)
			wl_output_set_user_data(c->outputs[j].wl, &c->outputs[j]);
		if (c->frame) {
			zwlr_screencopy_frame_v1_destroy(c->frame);
			c->frame = NULL;
		}
		select_target(c);
		log_msg("output removed, capturing %s", c->target >= 0 ? c->outputs[c->target].name : "nothing");
		return;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
			 uint32_t width, uint32_t height, uint32_t stride)
{
	struct capture *c = data;

	(void)frame;
	/* 32-bit RGB formats only: the converter handles them all. */
	if (format != WL_SHM_FORMAT_XRGB8888 && format != WL_SHM_FORMAT_ARGB8888 &&
	    format != WL_SHM_FORMAT_XBGR8888 && format != WL_SHM_FORMAT_ABGR8888)
		return;
	if (c->have_shm_format)
		return;
	c->have_shm_format = true;
	c->shm_format = format;
	c->shm_width = (int)width;
	c->shm_height = (int)height;
	c->shm_stride = (int)stride;
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags)
{
	struct capture *c = data;

	(void)frame;
	c->flags = flags;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
			uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	struct capture *c = data;

	(void)sec_hi; (void)sec_lo; (void)nsec;
	zwlr_screencopy_frame_v1_destroy(frame);
	c->frame = NULL;
	c->ready = true;
	c->done_index = c->cur;
	c->cur ^= 1;
	/* The presentation timestamp uses the compositor's clock domain
	 * (CLOCK_MONOTONIC for wlroots); the arrival time is close enough
	 * and always in ours. */
	c->ready_time_us = now_us();
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	struct capture *c = data;

	zwlr_screencopy_frame_v1_destroy(frame);
	c->frame = NULL;
	c->have_shm_format = false;
	log_msg("screencopy frame failed");
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
			 uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	(void)data; (void)frame; (void)x; (void)y; (void)w; (void)h;
}

static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
			       uint32_t format, uint32_t width, uint32_t height)
{
	(void)data; (void)frame; (void)format; (void)width; (void)height;
}

static int ensure_buffer(struct capture *c)
{
	size_t size = (size_t)c->shm_stride * (size_t)c->shm_height;
	struct wl_shm_pool *pool;
	int fd;

	if (c->buffer[0] && c->buf_width == c->shm_width && c->buf_height == c->shm_height &&
	    c->buf_stride == c->shm_stride && c->buf_format == c->shm_format)
		return 0;
	if (c->buffer[0]) {
		wl_buffer_destroy(c->buffer[0]);
		wl_buffer_destroy(c->buffer[1]);
		munmap(c->data, c->size * 2);
		c->buffer[0] = c->buffer[1] = NULL;
	}
	fd = memfd_create("nuubos-cast", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size * 2) != 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	c->data = mmap(NULL, size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (c->data == MAP_FAILED) {
		close(fd);
		c->data = NULL;
		return -1;
	}
	pool = wl_shm_create_pool(c->shm, fd, (int32_t)(size * 2));
	for (int i = 0; i < 2; i++)
		c->buffer[i] = wl_shm_pool_create_buffer(pool, (int32_t)(size * (size_t)i), c->shm_width,
							 c->shm_height, c->shm_stride, c->shm_format);
	wl_shm_pool_destroy(pool);
	close(fd);
	c->size = size;
	c->buf_width = c->shm_width;
	c->buf_height = c->shm_height;
	c->buf_stride = c->shm_stride;
	c->buf_format = c->shm_format;
	log_msg("capture buffer %dx%d stride %d format 0x%x", c->buf_width, c->buf_height,
		c->buf_stride, c->buf_format);
	return 0;
}

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	struct capture *c = data;

	if (!c->have_shm_format || ensure_buffer(c) != 0) {
		log_msg("no usable shm capture format");
		zwlr_screencopy_frame_v1_destroy(frame);
		c->frame = NULL;
		c->broken = true;
		return;
	}
	if (c->frame_damage)
		zwlr_screencopy_frame_v1_copy_with_damage(frame, c->buffer[c->cur]);
	else
		zwlr_screencopy_frame_v1_copy(frame, c->buffer[c->cur]);
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.damage = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done = frame_buffer_done,
};

/* ------------------------------------------------------------------ */

static struct capture *fake_open(struct capture *c, const char *size)
{
	int w = 0, h = 0;

	if (sscanf(size, "%dx%d", &w, &h) != 2 || w < 16 || h < 16 || pipe2(c->fake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		free(c);
		return NULL;
	}
	c->fake = true;
	c->buf_width = w;
	c->buf_height = h;
	c->buf_stride = w * 4;
	c->buf_format = WL_SHM_FORMAT_XRGB8888;
	c->size = (size_t)c->buf_stride * h;
	c->data = calloc(1, c->size);
	c->target = 0;
	log_msg("fake screen %dx%d", w, h);
	return c;
}

/* Moving bars and a frame counter block: any decoder output shows them. */
static void fake_draw(struct capture *c)
{
	uint32_t *px = c->data;
	unsigned f = c->fake_frame++;

	for (int y = 0; y < c->buf_height; y++)
		for (int x = 0; x < c->buf_width; x++) {
			unsigned bar = (unsigned)(x + (int)f * 4) / 40 % 6;
			static const uint32_t colors[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x00ffff, 0xffffff };

			px[(size_t)y * c->buf_width + x] = colors[bar];
		}
	for (int y = 8; y < 40 && y < c->buf_height; y++)
		for (int x = 8; x < 8 + (int)(f % 64) * 4 && x < c->buf_width; x++)
			px[(size_t)y * c->buf_width + x] = 0x000000;
}

struct capture *capture_open(void)
{
	struct capture *c = calloc(1, sizeof(*c));
	const char *fake = getenv("NUUBOS_CAST_FAKE_SCREEN");

	if (!c)
		return NULL;
	c->target = -1;
	if (fake && fake[0])
		return fake_open(c, fake);
	c->display = wl_display_connect(NULL);
	if (!c->display) {
		log_msg("cannot connect to the Wayland compositor");
		free(c);
		return NULL;
	}
	c->registry = wl_display_get_registry(c->display);
	wl_registry_add_listener(c->registry, &registry_listener, c);
	/* globals, then the output names/geometry */
	wl_display_roundtrip(c->display);
	wl_display_roundtrip(c->display);
	if (!c->shm || !c->manager || c->output_count == 0) {
		log_msg("compositor lacks wl_shm, screencopy v3 or an output");
		capture_close(c);
		return NULL;
	}
	select_target(c);
	log_msg("capturing output %s (transform %d)", c->outputs[c->target].name,
		c->outputs[c->target].transform);
	return c;
}

void capture_close(struct capture *c)
{
	if (!c)
		return;
	if (c->fake) {
		close(c->fake_pipe[0]);
		close(c->fake_pipe[1]);
		free(c->data);
		free(c);
		return;
	}
	if (c->reading)
		wl_display_cancel_read(c->display);
	if (c->frame)
		zwlr_screencopy_frame_v1_destroy(c->frame);
	if (c->buffer[0]) {
		wl_buffer_destroy(c->buffer[0]);
		wl_buffer_destroy(c->buffer[1]);
	}
	if (c->data)
		munmap(c->data, c->size * 2);
	for (int i = 0; i < c->output_count; i++)
		wl_output_destroy(c->outputs[i].wl);
	if (c->manager)
		zwlr_screencopy_manager_v1_destroy(c->manager);
	if (c->shm)
		wl_shm_destroy(c->shm);
	if (c->registry)
		wl_registry_destroy(c->registry);
	if (c->display)
		wl_display_disconnect(c->display);
	free(c);
}

int capture_fd(struct capture *c)
{
	if (c->fake)
		return c->fake_pipe[0];
	return wl_display_get_fd(c->display);
}

int capture_prepare(struct capture *c)
{
	if (c->fake)
		return 0;
	if (c->reading)
		return 0;
	while (wl_display_prepare_read(c->display) != 0) {
		if (wl_display_dispatch_pending(c->display) < 0) {
			c->broken = true;
			return -1;
		}
	}
	c->reading = true;
	if (wl_display_flush(c->display) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(c->display);
		c->reading = false;
		c->broken = true;
		return -1;
	}
	return 0;
}

int capture_dispatch(struct capture *c, bool readable)
{
	if (c->fake) {
		char drain[16];

		if (readable && read(c->fake_pipe[0], drain, sizeof(drain)) > 0) {
			fake_draw(c);
			c->ready = true;
			c->ready_time_us = now_us();
			c->frame = NULL;
		}
		return 0;
	}
	if (c->reading) {
		c->reading = false;
		if (readable) {
			if (wl_display_read_events(c->display) < 0) {
				c->broken = true;
				return -1;
			}
		} else {
			wl_display_cancel_read(c->display);
		}
	}
	if (wl_display_dispatch_pending(c->display) < 0) {
		c->broken = true;
		return -1;
	}
	return 0;
}

int capture_request(struct capture *c, bool damage)
{
	if (c->fake) {
		(void)damage;
		if (c->ready)
			return -1;
		/* any non-NULL marker: busy until dispatched */
		c->frame = (struct zwlr_screencopy_frame_v1 *)c;
		return write(c->fake_pipe[1], "f", 1) == 1 ? 0 : -1;
	}
	if (c->frame || c->target < 0)
		return -1;
	c->have_shm_format = false;
	c->ready = false;
	c->flags = 0;
	c->frame_damage = damage && c->buffer[0] != NULL;
	c->frame_transform = c->outputs[c->target].transform;
	c->frame = zwlr_screencopy_manager_v1_capture_output(c->manager, 0, c->outputs[c->target].wl);
	zwlr_screencopy_frame_v1_add_listener(c->frame, &frame_listener, c);
	return 0;
}

void capture_flush(struct capture *c)
{
	if (!c->fake)
		wl_display_flush(c->display);
}

bool capture_busy(const struct capture *c)
{
	return c->frame != NULL;
}

bool capture_take(struct capture *c, struct capture_image *out)
{
	if (!c->ready)
		return false;
	c->ready = false;
	out->data = c->fake ? c->data : (const uint8_t *)c->data + c->size * (size_t)c->done_index;
	out->width = c->buf_width;
	out->height = c->buf_height;
	out->stride = c->buf_stride;
	out->format = c->buf_format;
	out->y_invert = (c->flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
	out->transform = c->fake ? 0 : c->frame_transform;
	out->time_us = c->ready_time_us;
	return true;
}

bool capture_broken(const struct capture *c)
{
	return c->broken || c->target < 0;
}
