/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef NUUBOS_AUDIO_POLICY_H
#define NUUBOS_AUDIO_POLICY_H

#include <stdbool.h>
#include <string.h>

struct audio_outputs {
	bool cast;
	bool bluetooth;
	bool usb;
	bool headphones;
	bool hdmi;
};

static inline const char *audio_fallback_output(const struct audio_outputs *out)
{
	if (out->usb)
		return "usb";
	if (out->headphones)
		return "headphones";
	if (out->hdmi)
		return "hdmi";
	return "speaker";
}

/* Keep the requested mode when an accessory disappears. The resolved output
 * falls back immediately and returns to that accessory when it reappears. */
static inline const char *audio_select_output(const char *mode,
					     const struct audio_outputs *out)
{
	if (out->cast)
		return "cast";
	if (strcmp(mode, "analog") == 0 || strcmp(mode, "headphones") == 0)
		return out->headphones ? "headphones" : "speaker";
	if (strcmp(mode, "speaker") == 0)
		return "speaker";
	if (strcmp(mode, "hdmi") == 0 && out->hdmi)
		return "hdmi";
	if (strcmp(mode, "usb") == 0 && out->usb)
		return "usb";
	if ((strcmp(mode, "auto") == 0 || strcmp(mode, "bluetooth") == 0) &&
	    out->bluetooth)
		return "bluetooth";
	return audio_fallback_output(out);
}

#endif
