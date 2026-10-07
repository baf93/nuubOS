/* SPDX-License-Identifier: MIT */
/*
 * nuubos-syncctl — Syncthing settings of the active user (EPIC-034).
 *
 *   status                    enabled= running= failed= device_id=
 *                             folder=<saves|states|screenshots>\t0|1
 *                             device=<id>\t<name>\t<connected 0|1>
 *                             pending=<id>\t<name>   ... end=1
 *   enable | disable          per user; nuubos-syncd starts/stops Syncthing
 *   folder saves|states|screenshots on|off
 *   accept DEVICE [NAME]      add a device that asked to connect; share the
 *                             enabled folders with it
 *   remove DEVICE
 *
 * Syncthing is synchronization, not backup: conflicts are kept as
 * ".sync-conflict" copies by Syncthing itself, never deleted here. ROMs are
 * not synchronized. Folder ids are "nuubos-<kind>" so another device sees
 * the same folder for the same kind.
 */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef USERDATA_USERS
#define USERDATA_USERS "/userdata/users"
#endif
#define GUI_SOCKET RUN_ROOT "/syncthing.sock"

static const char *const KINDS[] = { "saves", "states", "screenshots" };

struct buffer {
	char *data;
	size_t len;
};

static char user[64];

static int fail(const char *reason)
{
	printf("ERR %s\n", reason);
	return 1;
}

static bool read_line(const char *path, char *out, size_t size)
{
	FILE *fp = fopen(path, "r");

	out[0] = '\0';
	if (!fp)
		return false;
	if (fgets(out, (int)size, fp))
		out[strcspn(out, "\r\n")] = '\0';
	fclose(fp);
	return out[0] != '\0';
}

static void conf_path(char *out, size_t size)
{
	snprintf(out, size, USERDATA_USERS "/%s/appdata/syncthing/nuubos.conf", user);
}

static bool conf_get(const char *key, char *out, size_t size)
{
	char path[512], line[256];
	size_t klen = strlen(key);
	FILE *fp;
	bool found = false;

	out[0] = '\0';
	conf_path(path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			snprintf(out, size, "%s", line + klen + 1);
			found = true;
		}
	}
	fclose(fp);
	return found;
}

static int conf_write(bool enabled, const char *key)
{
	char dir[512], path[512], tmp[600];
	FILE *fp;

	snprintf(dir, sizeof(dir), USERDATA_USERS "/%s/appdata", user);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), USERDATA_USERS "/%s/appdata/syncthing", user);
	mkdir(dir, 0700);
	conf_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "ENABLED=%d\nAPIKEY=%s\n", enabled, key);
	if (fclose(fp) != 0 || rename(tmp, path) != 0)
		return -1;
	return 0;
}

static void signal_syncd(void)
{
	char pid[32];

	if (read_line(RUN_ROOT "/syncd.pid", pid, sizeof(pid)) && atoi(pid) > 1)
		kill(atoi(pid), SIGHUP);
}

static size_t collect(char *ptr, size_t size, size_t n, void *userdata)
{
	struct buffer *b = userdata;
	size_t len = size * n;
	char *p = realloc(b->data, b->len + len + 1);

	if (!p)
		return 0;
	b->data = p;
	memcpy(b->data + b->len, ptr, len);
	b->len += len;
	b->data[b->len] = '\0';
	return len;
}

/* REST call on the unix socket; returns the parsed JSON (or NULL). */
static cJSON *rest(const char *method, const char *path, const char *body, long *status)
{
	char key[128], url[512], header[200];
	struct buffer b = { 0 };
	struct curl_slist *h = NULL;
	CURL *curl;
	cJSON *json = NULL;

	*status = -1;
	if (!conf_get("APIKEY", key, sizeof(key)) || !(curl = curl_easy_init()))
		return NULL;
	snprintf(url, sizeof(url), "http://localhost%s", path);
	snprintf(header, sizeof(header), "X-API-Key: %s", key);
	h = curl_slist_append(h, header);
	h = curl_slist_append(h, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, GUI_SOCKET);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	if (body)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	if (curl_easy_perform(curl) == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
	curl_slist_free_all(h);
	curl_easy_cleanup(curl);
	if (b.data)
		json = cJSON_Parse(b.data);
	free(b.data);
	return json;
}

static const char *jstr(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

	return cJSON_IsString(v) ? v->valuestring : "";
}

static bool valid_device(const char *id)
{
	size_t len = strlen(id);

	if (len < 50 || len > 72)
		return false;
	for (; *id; id++)
		if (!isupper((unsigned char)*id) && !isdigit((unsigned char)*id) && *id != '-')
			return false;
	return true;
}

static void clean(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

/* All configured device ids except our own, as a JSON array of
 * {"deviceID": ...} for a folder's sharing list. */
static cJSON *folder_devices(const char *self)
{
	long status;
	cJSON *devices = rest("GET", "/rest/config/devices", NULL, &status);
	cJSON *out = cJSON_CreateArray(), *d;

	cJSON_ArrayForEach(d, devices) {
		cJSON *e = cJSON_CreateObject();

		cJSON_AddStringToObject(e, "deviceID", jstr(d, "deviceID"));
		cJSON_AddItemToArray(out, e);
	}
	(void)self;
	cJSON_Delete(devices);
	return out;
}

static int set_folder(const char *kind, bool on)
{
	char path[256], dir[512];
	long status;
	char *text;

	snprintf(path, sizeof(path), "/rest/config/folders/nuubos-%s", kind);
	if (!on) {
		/* The folder stops syncing; its files stay on the device. */
		cJSON_Delete(rest("DELETE", path, NULL, &status));
		return status == 200 || status == 404 ? (puts("OK"), 0) : fail("syncthing");
	} else {
		cJSON *f = cJSON_CreateObject();

		snprintf(dir, sizeof(dir), USERDATA_USERS "/%s/%s", user, kind);
		mkdir(dir, 0755);
		cJSON_AddStringToObject(f, "id", path + strlen("/rest/config/folders/"));
		cJSON_AddStringToObject(f, "label", kind);
		cJSON_AddStringToObject(f, "path", dir);
		cJSON_AddStringToObject(f, "type", "sendreceive");
		cJSON_AddItemToObject(f, "devices", folder_devices(NULL));
		text = cJSON_PrintUnformatted(f);
		cJSON_Delete(f);
		cJSON_Delete(rest("PUT", path, text, &status));
		free(text);
		return status == 200 ? (puts("OK"), 0) : fail("syncthing");
	}
}

static int cmd_status(void)
{
	char enabled[8], state[256];
	FILE *fp;
	long status;
	cJSON *sys, *folders, *devices, *conns, *pending, *it;

	conf_get("ENABLED", enabled, sizeof(enabled));
	printf("enabled=%s\n", !strcmp(enabled, "1") ? "1" : "0");
	fp = fopen(RUN_ROOT "/syncd.state", "r");
	if (fp) {
		while (fgets(state, sizeof(state), fp))
			if (!strncmp(state, "running=", 8) || !strncmp(state, "failed=", 7))
				fputs(state, stdout);
		fclose(fp);
	}
	sys = rest("GET", "/rest/system/status", NULL, &status);
	if (status != 200) {
		cJSON_Delete(sys);
		puts("end=1");
		return 0;
	}
	printf("device_id=%s\n", jstr(sys, "myID"));
	folders = rest("GET", "/rest/config/folders", NULL, &status);
	for (size_t i = 0; i < 3; i++) {
		char id[64];
		bool on = false;

		snprintf(id, sizeof(id), "nuubos-%s", KINDS[i]);
		cJSON_ArrayForEach(it, folders)
			if (!strcmp(jstr(it, "id"), id))
				on = true;
		printf("folder=%s\t%d\n", KINDS[i], on);
	}
	devices = rest("GET", "/rest/config/devices", NULL, &status);
	conns = rest("GET", "/rest/system/connections", NULL, &status);
	cJSON_ArrayForEach(it, devices) {
		const char *id = jstr(it, "deviceID");
		cJSON *c = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(conns, "connections"), id);
		char name[128];

		if (!strcmp(id, jstr(sys, "myID")))
			continue;
		snprintf(name, sizeof(name), "%s", jstr(it, "name"));
		clean(name);
		printf("device=%s\t%s\t%d\n", id, name, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "connected")));
	}
	pending = rest("GET", "/rest/cluster/pending/devices", NULL, &status);
	for (it = pending ? pending->child : NULL; it; it = it->next) {
		char name[128];

		snprintf(name, sizeof(name), "%s", jstr(it, "name"));
		clean(name);
		if (valid_device(it->string))
			printf("pending=%s\t%s\n", it->string, name);
	}
	cJSON_Delete(sys);
	cJSON_Delete(folders);
	cJSON_Delete(devices);
	cJSON_Delete(conns);
	cJSON_Delete(pending);
	puts("end=1");
	return 0;
}

int main(int argc, char **argv)
{
	int rc = 2;

	if (!read_line(RUN_ROOT "/user/active", user, sizeof(user)) || strlen(user) != 36 || strchr(user, '/'))
		return fail("no-user");
	curl_global_init(CURL_GLOBAL_DEFAULT);
	if (argc == 2 && !strcmp(argv[1], "status")) {
		rc = cmd_status();
	} else if (argc == 2 && (!strcmp(argv[1], "enable") || !strcmp(argv[1], "disable"))) {
		char key[128];

		if (!conf_get("APIKEY", key, sizeof(key)) || !key[0]) {
			/* A per-user API key, created once. */
			FILE *fp = fopen("/proc/sys/kernel/random/uuid", "r");

			if (!fp || !fgets(key, sizeof(key), fp)) {
				if (fp)
					fclose(fp);
				return fail("random");
			}
			fclose(fp);
			key[strcspn(key, "\n")] = '\0';
		}
		if (conf_write(argv[1][0] == 'e', key) != 0)
			return fail("storage");
		signal_syncd();
		puts("OK");
		rc = 0;
	} else if (argc == 4 && !strcmp(argv[1], "folder") &&
		   (!strcmp(argv[3], "on") || !strcmp(argv[3], "off"))) {
		bool known = false;

		for (size_t i = 0; i < 3; i++)
			known = known || !strcmp(argv[2], KINDS[i]);
		rc = known ? set_folder(argv[2], argv[3][1] == 'n') : fail("folder");
	} else if ((argc == 3 || argc == 4) && !strcmp(argv[1], "accept") && valid_device(argv[2])) {
		cJSON *d = cJSON_CreateObject(), *folders, *f;
		char path[160], *text;
		long status;

		cJSON_AddStringToObject(d, "deviceID", argv[2]);
		cJSON_AddStringToObject(d, "name", argc == 4 ? argv[3] : "");
		cJSON_AddItemToObject(d, "addresses", cJSON_CreateStringArray((const char *[]){ "dynamic" }, 1));
		text = cJSON_PrintUnformatted(d);
		cJSON_Delete(d);
		snprintf(path, sizeof(path), "/rest/config/devices/%s", argv[2]);
		cJSON_Delete(rest("PUT", path, text, &status));
		free(text);
		if (status != 200)
			return fail("syncthing");
		/* Share every enabled nuubOS folder with the new device. */
		folders = rest("GET", "/rest/config/folders", NULL, &status);
		cJSON_ArrayForEach(f, folders) {
			if (strncmp(jstr(f, "id"), "nuubos-", 7))
				continue;
			cJSON_DeleteItemFromObject(f, "devices");
			cJSON_AddItemToObject(f, "devices", folder_devices(NULL));
			text = cJSON_PrintUnformatted(f);
			snprintf(path, sizeof(path), "/rest/config/folders/%s", jstr(f, "id"));
			cJSON_Delete(rest("PUT", path, text, &status));
			free(text);
		}
		cJSON_Delete(folders);
		puts("OK");
		rc = 0;
	} else if (argc == 3 && !strcmp(argv[1], "remove") && valid_device(argv[2])) {
		char path[160];
		long status;

		snprintf(path, sizeof(path), "/rest/config/devices/%s", argv[2]);
		cJSON_Delete(rest("DELETE", path, NULL, &status));
		rc = status == 200 ? (puts("OK"), 0) : fail("syncthing");
	} else {
		fprintf(stderr, "Usage: nuubos-syncctl status | enable | disable | folder KIND on|off | "
				"accept DEVICE [NAME] | remove DEVICE\n");
	}
	curl_global_cleanup();
	return rc;
}
