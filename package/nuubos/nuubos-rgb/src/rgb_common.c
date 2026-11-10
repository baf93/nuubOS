/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "rgb_common.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/*
 * Production-qualified RGB topologies.
 *
 * RG35XX Pro deliberately has no entry: RGB absence is a valid capability
 * state and must not activate any RGB hardware.
 *
 * CubeXX has 16 LEDs arranged as two independent 8-LED rings.
 *
 * Every ring: position 0 is the upper right LED and positions run
 * clockwise, 45 degrees apart (position 3 at the bottom, lightingd
 * RING_BOTTOM). CubeXX: qualified 2026-10-11 with locate 0 on both
 * rings (the two rings are wired in opposite directions).
 *
 * led_indexes lists the physical LED at each position around a ring, in
 * order: nuubos-rgbd maps logical frames (zone by zone, position by
 * position) through it, so effects never depend on the wiring order.
 * Check a ring with `nuubos-rgbctl locate N`.
 *
 * RG40XX-H: same MCU protocol and 16-LED layout as the CubeXX (ROCKNIX
 * drives both with one frame: LEDs 0-7 right ring, 8-15 left ring); the
 * ring directions are copied from the CubeXX, not hardware-checked.
 */
static const struct nuubos_rgb_topology topologies[] = {
	{
		.device_id = "rgcubexx",
		.compatible = "anbernic,rgcubexx",
		.led_count = 16,
		.zone_count = 2,
		.zones = {
			{
				.name = "left-stick",
				.type = NUUBOS_RGB_ZONE_RING,
				.led_count = 8,
				.led_indexes = { 15, 14, 13, 12, 11, 10, 9, 8 },
			},
			{
				.name = "right-stick",
				.type = NUUBOS_RGB_ZONE_RING,
				.led_count = 8,
				.led_indexes = { 1, 0, 7, 6, 5, 4, 3, 2 },
			},
		},
	},
	{
		.device_id = "rg40xx-h",
		.compatible = "anbernic,rg40xx-h",
		.led_count = 16,
		.zone_count = 2,
		.zones = {
			{
				.name = "left-stick",
				.type = NUUBOS_RGB_ZONE_RING,
				.led_count = 8,
				.led_indexes = { 15, 14, 13, 12, 11, 10, 9, 8 },
			},
			{
				.name = "right-stick",
				.type = NUUBOS_RGB_ZONE_RING,
				.led_count = 8,
				.led_indexes = { 1, 0, 7, 6, 5, 4, 3, 2 },
			},
		},
	},
	{
		.device_id = "rg40xx-v",
		.compatible = "anbernic,rg40xx-V",
		.led_count = 8,
		.zone_count = 1,
		.zones = {
			{
				.name = "stick",
				.type = NUUBOS_RGB_ZONE_RING,
				.led_count = 8,
				.led_indexes = { 0, 7, 6, 5, 4, 3, 2, 1 },
			},
		},
	},
};

static int compatible_present(const char *wanted)
{
	static const char *paths[] = {
		"/sys/firmware/devicetree/base/compatible",
		"/proc/device-tree/compatible",
	};
	char buf[4096];
	ssize_t n = -1;
	size_t p;

	for (p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
		int fd = open(paths[p], O_RDONLY);

		if (fd < 0)
			continue;

		n = read(fd, buf, sizeof(buf));
		close(fd);

		if (n > 0)
			break;
	}

	if (n <= 0)
		return 0;

	for (p = 0; p < (size_t)n;) {
		size_t left = (size_t)n - p;
		size_t len = strnlen(buf + p, left);

		if (len == 0) {
			p++;
			continue;
		}

		if (strcmp(buf + p, wanted) == 0)
			return 1;

		p += len + 1;
	}

	return 0;
}

const struct nuubos_rgb_topology *nuubos_rgb_detect_topology(void)
{
	size_t i;

	for (i = 0; i < sizeof(topologies) / sizeof(topologies[0]); i++) {
		if (compatible_present(topologies[i].compatible))
			return &topologies[i];
	}

	return NULL;
}

const char *nuubos_rgb_zone_type_name(enum nuubos_rgb_zone_type type)
{
	switch (type) {
	case NUUBOS_RGB_ZONE_RING:
		return "ring";
	case NUUBOS_RGB_ZONE_LED:
		return "led";
	case NUUBOS_RGB_ZONE_LINE:
		return "line";
	default:
		return "unknown";
	}
}
