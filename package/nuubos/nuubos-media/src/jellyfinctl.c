/* SPDX-License-Identifier: MIT */
/*
 * nuubos-jellyfinctl — Jellyfin media server client (EPIC-027).
 *
 *   status USER                      server= name= signed=0|1
 *   login USER SERVER NAME           password on stdin; stores the token only
 *   logout USER
 *   views USER                       item=<id>\t<name>\t<type>\t1\t0\t0 ... end=1
 *   items USER PARENT                item=<id>\t<name>\t<type>\t<folder>\t<resume s>\t<length s>
 *   stream USER ID                   direct-play URL for nuubos-mediad
 *   progress USER ID SECONDS         report the stop position (resume)
 *
 * Jellyfin is self-hosted and open source; no cloud account is involved.
 * Accounts are per nuubOS user: /state/users/<id>/secrets/jellyfin.conf
 * (0600: SERVER, USER_ID, NAME, TOKEN). Media is direct-played (no server
 * transcoding): a format nuubOS cannot decode fails visibly in the player.
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#ifndef STATE_USERS
#define STATE_USERS "/state/users"
#endif
#define CLIENT_NAME "nuubOS"
#define DEVICE_ID_FILE "/etc/machine-id"

struct account {
	char server[256];
	char user_id[64];
	char name[64];
	char token[128];
};

struct buffer {
	char *data;
	size_t len;
};

static bool valid_user(const char *id)
{
	if (strlen(id) != 36)
		return false;
	for (const char *p = id; *p; p++)
		if (!isxdigit((unsigned char)*p) && *p != '-')
			return false;
	return true;
}

static bool valid_item(const char *id)
{
	if (!id[0] || strlen(id) > 64)
		return false;
	for (const char *p = id; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '-')
			return false;
	return true;
}

static bool safe_value(const char *v)
{
	for (; *v; v++)
		if ((unsigned char)*v < 0x20 || *v == '=')
			return false;
	return true;
}

static void path_for(const char *user, char *out, size_t size)
{
	snprintf(out, size, STATE_USERS "/%s/secrets/jellyfin.conf", user);
}

static bool load(const char *user, struct account *a)
{
	char path[512], line[512];
	FILE *fp;

	memset(a, 0, sizeof(*a));
	path_for(user, path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		line[strcspn(line, "\r\n")] = '\0';
		if (!(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
		if (!strcmp(line, "SERVER"))
			snprintf(a->server, sizeof(a->server), "%s", eq);
		else if (!strcmp(line, "USER_ID"))
			snprintf(a->user_id, sizeof(a->user_id), "%s", eq);
		else if (!strcmp(line, "NAME"))
			snprintf(a->name, sizeof(a->name), "%s", eq);
		else if (!strcmp(line, "TOKEN"))
			snprintf(a->token, sizeof(a->token), "%s", eq);
	}
	fclose(fp);
	return a->server[0] && a->token[0] && valid_item(a->user_id);
}

static int save(const char *user, const struct account *a)
{
	char dir[512], path[512], tmp[600];
	FILE *fp;
	int rc = 0;

	snprintf(dir, sizeof(dir), STATE_USERS "/%s/secrets", user);
	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return -1;
	path_for(user, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	umask(077);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "SERVER=%s\nUSER_ID=%s\nNAME=%s\nTOKEN=%s\n", a->server, a->user_id, a->name, a->token);
	if (fclose(fp) != 0 || rename(tmp, path) != 0)
		rc = -1;
	if (rc)
		unlink(tmp);
	return rc;
}

static size_t collect(char *ptr, size_t size, size_t n, void *userdata)
{
	struct buffer *b = userdata;
	size_t len = size * n;
	char *p;

	if (b->len + len > 8 * 1024 * 1024)
		return 0;
	p = realloc(b->data, b->len + len + 1);
	if (!p)
		return 0;
	b->data = p;
	memcpy(b->data + b->len, ptr, len);
	b->len += len;
	b->data[b->len] = '\0';
	return len;
}

static void device_id(char *out, size_t size)
{
	FILE *fp = fopen(DEVICE_ID_FILE, "r");

	snprintf(out, size, "nuubos");
	if (fp) {
		if (fgets(out, (int)size, fp))
			out[strcspn(out, "\r\n")] = '\0';
		fclose(fp);
	}
}

/* Request with the Jellyfin authorization header; returns HTTP status. */
static long request(const char *method, const char *url, const char *token, const char *body,
		    struct buffer *out)
{
	CURL *curl = curl_easy_init();
	struct curl_slist *h = NULL;
	char auth[512], dev[64];
	long status = -1;

	if (!curl)
		return -1;
	device_id(dev, sizeof(dev));
	snprintf(auth, sizeof(auth),
		 "X-Emby-Authorization: MediaBrowser Client=\"" CLIENT_NAME "\", Device=\"nuubOS\", "
		 "DeviceId=\"%s\", Version=\"1\"%s%s%s", dev,
		 token ? ", Token=\"" : "", token ? token : "", token ? "\"" : "");
	h = curl_slist_append(h, auth);
	h = curl_slist_append(h, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	if (body)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	if (curl_easy_perform(curl) == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(h);
	curl_easy_cleanup(curl);
	return status;
}

static const char *jstr(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

	return cJSON_IsString(v) ? v->valuestring : "";
}

static void print_clean(const char *s)
{
	for (; *s; s++)
		putchar(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
}

static int fail(const char *reason)
{
	printf("ERR %s\n", reason);
	return 1;
}

static int list(const char *url, const char *token)
{
	struct buffer b = { 0 };
	long status = request("GET", url, token, NULL, &b);
	cJSON *root, *items, *it;

	if (status == 401)
		return free(b.data), fail("auth");
	if (status != 200)
		return free(b.data), fail("unreachable");
	root = cJSON_Parse(b.data);
	free(b.data);
	items = root ? cJSON_GetObjectItemCaseSensitive(root, "Items") : NULL;
	cJSON_ArrayForEach(it, items) {
		const cJSON *folder = cJSON_GetObjectItemCaseSensitive(it, "IsFolder");
		const cJSON *ud = cJSON_GetObjectItemCaseSensitive(it, "UserData");
		const cJSON *pos = ud ? cJSON_GetObjectItemCaseSensitive(ud, "PlaybackPositionTicks") : NULL;
		const cJSON *len = cJSON_GetObjectItemCaseSensitive(it, "RunTimeTicks");

		if (!valid_item(jstr(it, "Id")))
			continue;
		printf("item=%s\t", jstr(it, "Id"));
		print_clean(jstr(it, "Name"));
		printf("\t%s\t%d\t%.0f\t%.0f\n", jstr(it, "Type"), cJSON_IsTrue(folder) ? 1 : 0,
		       cJSON_IsNumber(pos) ? pos->valuedouble / 1e7 : 0.0,
		       cJSON_IsNumber(len) ? len->valuedouble / 1e7 : 0.0);
	}
	cJSON_Delete(root);
	puts("end=1");
	return 0;
}

int main(int argc, char **argv)
{
	struct account a;
	char url[1024];
	int rc;

	if (argc < 3 || !valid_user(argv[2])) {
		fprintf(stderr, "Usage: nuubos-jellyfinctl status|logout|views USER | login USER SERVER NAME | "
				"items USER PARENT | stream USER ID | progress USER ID SECONDS\n");
		return 2;
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	if (!strcmp(argv[1], "status") && argc == 3) {
		bool ok = load(argv[2], &a);

		printf("server=%s\nname=%s\nsigned=%d\nend=1\n", ok ? a.server : "", ok ? a.name : "", ok);
		rc = 0;
	} else if (!strcmp(argv[1], "logout") && argc == 3) {
		char path[512];

		path_for(argv[2], path, sizeof(path));
		rc = (unlink(path) == 0 || errno == ENOENT) ? (puts("OK"), 0) : fail("storage");
	} else if (!strcmp(argv[1], "login") && argc == 5) {
		char pass[256];
		struct buffer b = { 0 };
		cJSON *body = cJSON_CreateObject(), *res;
		char *text;
		long status;

		if (strncmp(argv[3], "http://", 7) && strncmp(argv[3], "https://", 8))
			return fail("server");
		if (!safe_value(argv[3]) || !safe_value(argv[4]) || strlen(argv[3]) >= sizeof(a.server))
			return fail("server");
		if (!fgets(pass, sizeof(pass), stdin))
			pass[0] = '\0';
		pass[strcspn(pass, "\r\n")] = '\0';
		cJSON_AddStringToObject(body, "Username", argv[4]);
		cJSON_AddStringToObject(body, "Pw", pass);
		text = cJSON_PrintUnformatted(body);
		cJSON_Delete(body);
		memset(pass, 0, sizeof(pass));
		snprintf(url, sizeof(url), "%s/Users/AuthenticateByName", argv[3]);
		status = request("POST", url, NULL, text, &b);
		free(text);
		if (status == 401 || status == 400)
			return free(b.data), fail("auth");
		if (status != 200)
			return free(b.data), fail("unreachable");
		res = cJSON_Parse(b.data);
		free(b.data);
		memset(&a, 0, sizeof(a));
		snprintf(a.server, sizeof(a.server), "%s", argv[3]);
		if (a.server[strlen(a.server) - 1] == '/')
			a.server[strlen(a.server) - 1] = '\0';
		snprintf(a.token, sizeof(a.token), "%s", jstr(res, "AccessToken"));
		snprintf(a.user_id, sizeof(a.user_id), "%s",
			 jstr(cJSON_GetObjectItemCaseSensitive(res, "User"), "Id"));
		snprintf(a.name, sizeof(a.name), "%s", argv[4]);
		cJSON_Delete(res);
		if (!a.token[0] || !valid_item(a.user_id) || !safe_value(a.token))
			return fail("auth");
		rc = save(argv[2], &a) == 0 ? (puts("OK"), 0) : fail("storage");
	} else if (!load(argv[2], &a)) {
		rc = fail("not-signed-in");
	} else if (!strcmp(argv[1], "views") && argc == 3) {
		snprintf(url, sizeof(url), "%s/Users/%s/Views", a.server, a.user_id);
		rc = list(url, a.token);
	} else if (!strcmp(argv[1], "items") && argc == 4 && valid_item(argv[3])) {
		snprintf(url, sizeof(url),
			 "%s/Users/%s/Items?ParentId=%s&SortBy=SortName&Fields=UserData&Limit=500",
			 a.server, a.user_id, argv[3]);
		rc = list(url, a.token);
	} else if (!strcmp(argv[1], "stream") && argc == 4 && valid_item(argv[3])) {
		/* Static direct play; the player decides from the container. */
		printf("%s/Items/%s/Download?api_key=%s\n", a.server, argv[3], a.token);
		rc = 0;
	} else if (!strcmp(argv[1], "progress") && argc == 5 && valid_item(argv[3])) {
		char body[256];
		struct buffer b = { 0 };
		long status;

		snprintf(body, sizeof(body), "{\"ItemId\":\"%s\",\"PositionTicks\":%lld}", argv[3],
			 (long long)(atof(argv[4]) * 1e7));
		snprintf(url, sizeof(url), "%s/Sessions/Playing/Stopped", a.server);
		status = request("POST", url, a.token, body, &b);
		free(b.data);
		rc = (status >= 200 && status < 300) ? (puts("OK"), 0) : fail("unreachable");
	} else {
		rc = fail("usage");
	}
	curl_global_cleanup();
	return rc;
}
