/* SPDX-License-Identifier: MIT */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BACKLIGHT_CLASS_DIR "/sys/class/backlight"
#define DRM_CLASS_DIR       "/sys/class/drm"

#define CONFIG_PATH "/state/config/nuubos.conf"
#define CONFIG_TMP  "/state/config/nuubos.conf.displayctl.tmp"
#define CONFIG_KEY  "DISPLAY_BRIGHTNESS"

#define DEFAULT_BRIGHTNESS 60

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s brightness\n"
		"  %s brightness 0..100\n"
		"  %s brightness-live 0..100\n"
		"  %s hdmi\n"
		"  %s restore\n"
		"  %s reset\n",
		argv0, argv0, argv0, argv0, argv0, argv0);
}

static int parse_percent(const char *text, unsigned int *value)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 10);

	if (errno != 0 || end == text || *end != '\0' ||
	    parsed < 0 || parsed > 100)
		return -1;

	*value = (unsigned int)parsed;
	return 0;
}

static int read_ulong_file(const char *path, unsigned long *value)
{
	FILE *file;
	char buffer[64];
	char *end;
	unsigned long parsed;

	file = fopen(path, "r");
	if (!file)
		return -1;

	if (!fgets(buffer, sizeof(buffer), file)) {
		fclose(file);
		return -1;
	}

	fclose(file);

	errno = 0;
	parsed = strtoul(buffer, &end, 10);

	if (errno != 0 || end == buffer)
		return -1;

	while (*end != '\0') {
		if (!isspace((unsigned char)*end))
			return -1;
		end++;
	}

	*value = parsed;
	return 0;
}

static int write_ulong_file(const char *path, unsigned long value)
{
	FILE *file;

	file = fopen(path, "w");
	if (!file)
		return -1;

	if (fprintf(file, "%lu\n", value) < 0) {
		fclose(file);
		return -1;
	}

	if (fclose(file) != 0)
		return -1;

	return 0;
}

static int find_variable_backlight(char *path, size_t path_size,
				   unsigned long *max_brightness)
{
	DIR *dir;
	struct dirent *entry;

	dir = opendir(BACKLIGHT_CLASS_DIR);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		char candidate[512];
		char max_path[512];
		unsigned long max;
		int n;

		if (entry->d_name[0] == '.')
			continue;

		n = snprintf(candidate, sizeof(candidate), "%s/%s",
			     BACKLIGHT_CLASS_DIR, entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(candidate))
			continue;

		n = snprintf(max_path, sizeof(max_path),
			     "%s/max_brightness", candidate);
		if (n < 0 || (size_t)n >= sizeof(max_path))
			continue;

		if (read_ulong_file(max_path, &max) != 0)
			continue;

		/*
		 * A max value of 1 is only an on/off backlight and does not
		 * satisfy the nuubOS userspace brightness contract.
		 */
		if (max <= 1)
			continue;

		n = snprintf(path, path_size, "%s", candidate);
		if (n < 0 || (size_t)n >= path_size) {
			closedir(dir);
			return -1;
		}

		*max_brightness = max;
		closedir(dir);
		return 0;
	}

	closedir(dir);
	errno = ENODEV;
	return -1;
}

static unsigned long percent_to_raw(unsigned int percent,
				    unsigned long max_brightness)
{
	unsigned long long scaled;

	scaled = (unsigned long long)percent * max_brightness;
	scaled += 50;

	return (unsigned long)(scaled / 100);
}

static unsigned int raw_to_percent(unsigned long raw,
				   unsigned long max_brightness)
{
	unsigned long long scaled;

	if (raw > max_brightness)
		raw = max_brightness;

	scaled = (unsigned long long)raw * 100;
	scaled += max_brightness / 2;

	return (unsigned int)(scaled / max_brightness);
}

static int backlight_get(unsigned int *percent)
{
	char path[512];
	char brightness_path[512];
	unsigned long max;
	unsigned long raw;
	int n;

	if (find_variable_backlight(path, sizeof(path), &max) != 0)
		return -1;

	n = snprintf(brightness_path, sizeof(brightness_path),
		     "%s/brightness", path);
	if (n < 0 || (size_t)n >= sizeof(brightness_path))
		return -1;

	if (read_ulong_file(brightness_path, &raw) != 0)
		return -1;

	*percent = raw_to_percent(raw, max);
	return 0;
}

static int backlight_set(unsigned int percent)
{
	char path[512];
	char brightness_path[512];
	unsigned long max;
	unsigned long raw;
	int n;

	if (find_variable_backlight(path, sizeof(path), &max) != 0)
		return -1;

	raw = percent_to_raw(percent, max);

	n = snprintf(brightness_path, sizeof(brightness_path),
		     "%s/brightness", path);
	if (n < 0 || (size_t)n >= sizeof(brightness_path))
		return -1;

	return write_ulong_file(brightness_path, raw);
}

/*
 * Returns:
 *   0: valid value found
 *   1: key/config missing
 *  -1: malformed value or I/O error
 */
static int read_config_brightness(unsigned int *percent)
{
	FILE *file;
	char line[256];
	const size_t key_len = strlen(CONFIG_KEY);

	file = fopen(CONFIG_PATH, "r");
	if (!file) {
		if (errno == ENOENT)
			return 1;
		return -1;
	}

	while (fgets(line, sizeof(line), file)) {
		char *value;
		char *end;
		long parsed;

		if (strncmp(line, CONFIG_KEY, key_len) != 0 ||
		    line[key_len] != '=')
			continue;

		value = line + key_len + 1;

		errno = 0;
		parsed = strtol(value, &end, 10);

		if (errno != 0 || end == value) {
			fclose(file);
			return -1;
		}

		while (*end != '\0') {
			if (!isspace((unsigned char)*end)) {
				fclose(file);
				return -1;
			}
			end++;
		}

		if (parsed < 0 || parsed > 100) {
			fclose(file);
			return -1;
		}

		*percent = (unsigned int)parsed;
		fclose(file);
		return 0;
	}

	if (ferror(file)) {
		fclose(file);
		return -1;
	}

	fclose(file);
	return 1;
}

static int update_config(unsigned int percent)
{
	FILE *in;
	FILE *out;
	char line[256];
	char value[16];
	bool replaced = false;
	const size_t key_len = strlen(CONFIG_KEY);
	int fd;

	in = fopen(CONFIG_PATH, "r");
	if (!in)
		return -1;

	out = fopen(CONFIG_TMP, "w");
	if (!out) {
		fclose(in);
		return -1;
	}

	snprintf(value, sizeof(value), "%u", percent);

	while (fgets(line, sizeof(line), in)) {
		if (strncmp(line, CONFIG_KEY, key_len) == 0 &&
		    line[key_len] == '=') {
			if (fprintf(out, "%s=%s\n", CONFIG_KEY, value) < 0)
				goto fail;
			replaced = true;
		} else {
			if (fputs(line, out) == EOF)
				goto fail;
		}
	}

	if (ferror(in))
		goto fail;

	if (!replaced &&
	    fprintf(out, "%s=%s\n", CONFIG_KEY, value) < 0)
		goto fail;

	if (fflush(out) != 0)
		goto fail;

	fd = fileno(out);
	if (fd >= 0 && fsync(fd) != 0)
		goto fail;

	if (fclose(out) != 0) {
		fclose(in);
		unlink(CONFIG_TMP);
		return -1;
	}

	fclose(in);

	if (chmod(CONFIG_TMP, 0600) != 0) {
		unlink(CONFIG_TMP);
		return -1;
	}

	if (rename(CONFIG_TMP, CONFIG_PATH) != 0) {
		unlink(CONFIG_TMP);
		return -1;
	}

	return 0;

fail:
	fclose(out);
	fclose(in);
	unlink(CONFIG_TMP);
	return -1;
}

static int command_hdmi(void)
{
	DIR *dir;
	struct dirent *entry;
	bool found = false;
	bool connected = false;

	dir = opendir(DRM_CLASS_DIR);
	if (!dir) {
		fprintf(stderr,
			"nuubos-displayctl: cannot inspect DRM connectors: %s\n",
			strerror(errno));
		return 1;
	}

	while ((entry = readdir(dir)) != NULL) {
		char status_path[512];
		char status[32];
		FILE *file;
		int n;

		if (!strstr(entry->d_name, "-HDMI-A-"))
			continue;

		n = snprintf(status_path, sizeof(status_path),
			     "%s/%s/status", DRM_CLASS_DIR, entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(status_path))
			continue;

		file = fopen(status_path, "r");
		if (!file)
			continue;

		if (!fgets(status, sizeof(status), file)) {
			fclose(file);
			continue;
		}

		fclose(file);
		found = true;

		if (strncmp(status, "connected", 9) == 0) {
			connected = true;
			break;
		}
	}

	closedir(dir);

	if (!found) {
		fprintf(stderr,
			"nuubos-displayctl: HDMI connector is unavailable\n");
		return 1;
	}

	printf("%s\n", connected ? "connected" : "disconnected");
	return 0;
}

static int command_get(void)
{
	unsigned int percent;

	if (backlight_get(&percent) != 0) {
		fprintf(stderr,
			"nuubos-displayctl: variable brightness is not supported\n");
		return 1;
	}

	printf("%u\n", percent);
	return 0;
}

static int command_set(const char *value, bool persist)
{
	unsigned int percent;
	unsigned int previous;

	if (parse_percent(value, &percent) != 0) {
		fprintf(stderr,
			"nuubos-displayctl: brightness must be 0..100\n");
		return 2;
	}

	if (backlight_get(&previous) != 0) {
		fprintf(stderr,
			"nuubos-displayctl: variable brightness is not supported\n");
		return 1;
	}

	if (backlight_set(percent) != 0) {
		fprintf(stderr,
			"nuubos-displayctl: cannot apply brightness: %s\n",
			strerror(errno));
		return 1;
	}

	if (persist && update_config(percent) != 0) {
		int saved_errno = errno;

		/*
		 * Keep runtime and persistent state consistent when persistence
		 * fails.
		 */
		(void)backlight_set(previous);

		fprintf(stderr,
			"nuubos-displayctl: cannot persist brightness: %s\n",
			strerror(saved_errno));
		return 1;
	}

	printf("%u\n", percent);
	return 0;
}

static int command_restore(void)
{
	unsigned int percent;
	char path[512];
	unsigned long max;
	int ret;

	/*
	 * Restore is deliberately a no-op on hardware that only exposes an
	 * on/off backlight. This keeps boot portable across H700 profiles
	 * while each device is qualified for variable brightness.
	 */
	if (find_variable_backlight(path, sizeof(path), &max) != 0)
		return 0;

	ret = read_config_brightness(&percent);
	if (ret == 1)
		percent = DEFAULT_BRIGHTNESS;
	else if (ret != 0) {
		fprintf(stderr,
			"nuubos-displayctl: invalid persistent brightness\n");
		return 1;
	}

	if (backlight_set(percent) != 0) {
		fprintf(stderr,
			"nuubos-displayctl: cannot restore brightness: %s\n",
			strerror(errno));
		return 1;
	}

	return 0;
}

/*
 * Reset System Settings: persist the product default brightness and apply it
 * to a variable backlight. On/off-only hardware only gets the persistent
 * value; the kernel keeps a blanked panel dark while HDMI is active.
 */
static int command_reset(void)
{
	char path[512];
	unsigned long max;

	if (update_config(DEFAULT_BRIGHTNESS) != 0) {
		fprintf(stderr,
			"nuubos-displayctl: cannot persist brightness: %s\n",
			strerror(errno));
		return 1;
	}

	if (find_variable_backlight(path, sizeof(path), &max) != 0)
		return 0;

	if (backlight_set(DEFAULT_BRIGHTNESS) != 0) {
		fprintf(stderr,
			"nuubos-displayctl: cannot apply brightness: %s\n",
			strerror(errno));
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "brightness") == 0)
		return command_get();

	if (argc == 3 && strcmp(argv[1], "brightness") == 0)
		return command_set(argv[2], true);

	if (argc == 3 && strcmp(argv[1], "brightness-live") == 0)
		return command_set(argv[2], false);

	if (argc == 2 && strcmp(argv[1], "hdmi") == 0)
		return command_hdmi();

	if (argc == 2 && strcmp(argv[1], "restore") == 0)
		return command_restore();

	if (argc == 2 && strcmp(argv[1], "reset") == 0)
		return command_reset();

	usage(argv[0]);
	return 2;
}
