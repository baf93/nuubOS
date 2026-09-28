/* SPDX-License-Identifier: MIT */
#ifndef NUUBOS_RGB_COMMON_H
#define NUUBOS_RGB_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define NUUBOS_RGB_MAX_LEDS   32
#define NUUBOS_RGB_MAX_ZONES   4

enum nuubos_rgb_zone_type {
	NUUBOS_RGB_ZONE_RING,
	NUUBOS_RGB_ZONE_LED,
	NUUBOS_RGB_ZONE_LINE,
};

struct nuubos_rgb_zone {
	const char *name;
	enum nuubos_rgb_zone_type type;
	unsigned int led_count;
	uint8_t led_indexes[NUUBOS_RGB_MAX_LEDS];
};

struct nuubos_rgb_topology {
	const char *device_id;
	const char *compatible;
	unsigned int led_count;
	unsigned int zone_count;
	struct nuubos_rgb_zone zones[NUUBOS_RGB_MAX_ZONES];
};

const struct nuubos_rgb_topology *nuubos_rgb_detect_topology(void);
const char *nuubos_rgb_zone_type_name(enum nuubos_rgb_zone_type type);

#endif
