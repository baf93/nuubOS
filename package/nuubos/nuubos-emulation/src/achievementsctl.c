/* SPDX-License-Identifier: MIT */
/*
 * nuubos-achievementsctl — per-user RetroAchievements account (EPIC-017).
 *
 *   status USER                       user=, enabled=, hardcore= (never the token)
 *   login USER NAME                   password on stdin (one line); stores only
 *                                     the API token returned by the service
 *   logout USER                       removes the local credentials
 *   set USER enabled|hardcore 0|1
 *
 * USER is the nuubOS user id. Credentials live in STATE, readable by root
 * only: /state/users/<id>/secrets/retroachievements.conf. nuubos-emud gives
 * them to RetroArch for that user's sessions. Login uses the same endpoint
 * as rcheevos ("login2"); the password is sent once over TLS and never
 * written anywhere. Replies: OK ... / ERR <reason>.
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>

#ifndef STATE_USERS
#define STATE_USERS "/state/users"
#endif
#define FILE_NAME "secrets/retroachievements.conf"
#define LOGIN_URL "https://retroachievements.org/dorequest.php"
#define USER_AGENT "nuubOS/" NUUBOS_VERSION " rcheevos"
#ifndef NUUBOS_VERSION
#define NUUBOS_VERSION "0"
#endif

struct account {
	char username[64];
	char token[128];
	bool enabled;
	bool hardcore;
};

struct body {
	char data[8192];
	size_t len;
};

static bool valid_user(const char *id)
{
	size_t len = strlen(id);

	if (len != 36)
		return false;
	for (size_t i = 0; i < len; i++)
		if (!isxdigit((unsigned char)id[i]) && id[i] != '-')
			return false;
	return true;
}

/* Values written to a key=value file and into RetroArch's configuration. */
static bool safe_value(const char *v)
{
	if (!v[0])
		return false;
	for (const char *p = v; *p; p++)
		if (*p == '"' || *p == '\\' || *p == '=' || (unsigned char)*p < 0x20)
			return false;
	return true;
}

static void account_path(const char *user, char *out, size_t size)
{
	snprintf(out, size, STATE_USERS "/%s/" FILE_NAME, user);
}

static void load(const char *user, struct account *a)
{
	char path[512];
	char line[256];
	FILE *fp;

	memset(a, 0, sizeof(*a));
	account_path(user, path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		line[strcspn(line, "\r\n")] = '\0';
		if (!(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
		if (!strcmp(line, "USERNAME"))
			snprintf(a->username, sizeof(a->username), "%s", eq);
		else if (!strcmp(line, "TOKEN"))
			snprintf(a->token, sizeof(a->token), "%s", eq);
		else if (!strcmp(line, "ENABLED"))
			a->enabled = !strcmp(eq, "1");
		else if (!strcmp(line, "HARDCORE"))
			a->hardcore = !strcmp(eq, "1");
	}
	fclose(fp);
}

static int save(const char *user, const struct account *a)
{
	char dir[512];
	char path[512];
	char tmp[600];
	FILE *fp;
	int rc = 0;

	snprintf(dir, sizeof(dir), STATE_USERS "/%s/secrets", user);
	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return -1;
	account_path(user, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	umask(077);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "USERNAME=%s\nTOKEN=%s\nENABLED=%d\nHARDCORE=%d\n",
		a->username, a->token, a->enabled, a->hardcore);
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0)
		rc = -1;
	if (fclose(fp) != 0)
		rc = -1;
	if (rc == 0 && (chmod(tmp, 0600) != 0 || rename(tmp, path) != 0))
		rc = -1;
	if (rc != 0)
		unlink(tmp);
	return rc;
}

static size_t collect(char *ptr, size_t size, size_t n, void *userdata)
{
	struct body *b = userdata;
	size_t len = size * n;

	if (b->len + len >= sizeof(b->data))
		len = sizeof(b->data) - b->len - 1;
	memcpy(b->data + b->len, ptr, len);
	b->len += len;
	b->data[b->len] = '\0';
	return size * n;
}

/* "key":"value" from a flat JSON object; false when absent. */
static bool json_string(const char *json, const char *key, char *out, size_t size)
{
	char pattern[64];
	const char *p, *end;

	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	p = strstr(json, pattern);
	if (!p)
		return false;
	p += strlen(pattern);
	end = strchr(p, '"');
	if (!end || (size_t)(end - p) >= size)
		return false;
	snprintf(out, size, "%.*s", (int)(end - p), p);
	return true;
}

static int login(const char *user, const char *name)
{
	char password[256];
	char post[1024];
	struct body body = { .len = 0 };
	struct account a;
	CURL *curl;
	CURLcode res;
	long status = 0;
	char *eu, *ep;

	if (!fgets(password, sizeof(password), stdin)) {
		puts("ERR password");
		return 1;
	}
	password[strcspn(password, "\r\n")] = '\0';
	if (!password[0]) {
		puts("ERR password");
		return 1;
	}
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0 || !(curl = curl_easy_init())) {
		puts("ERR network");
		return 1;
	}
	eu = curl_easy_escape(curl, name, 0);
	ep = curl_easy_escape(curl, password, 0);
	snprintf(post, sizeof(post), "r=login2&u=%s&p=%s", eu ? eu : "", ep ? ep : "");
	curl_free(eu);
	curl_free(ep);
	memset(password, 0, sizeof(password));
	curl_easy_setopt(curl, CURLOPT_URL, LOGIN_URL);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	memset(post, 0, sizeof(post));
	curl_easy_cleanup(curl);
	curl_global_cleanup();
	if (res != CURLE_OK) {
		puts("ERR network");
		return 1;
	}
	load(user, &a);
	if (!strstr(body.data, "\"Success\":true") ||
	    !json_string(body.data, "Token", a.token, sizeof(a.token)) ||
	    !json_string(body.data, "User", a.username, sizeof(a.username)) ||
	    !safe_value(a.token) || !safe_value(a.username)) {
		puts(status >= 500 ? "ERR service" : "ERR auth");
		return 1;
	}
	a.enabled = true;
	if (save(user, &a) != 0) {
		puts("ERR storage");
		return 1;
	}
	printf("OK %s\n", a.username);
	return 0;
}

int main(int argc, char **argv)
{
	struct account a;
	char path[512];

	if (argc < 3 || !valid_user(argv[2])) {
		fprintf(stderr, "Usage: nuubos-achievementsctl status|logout USER | login USER NAME | set USER enabled|hardcore 0|1\n");
		return 2;
	}
	if (!strcmp(argv[1], "status") && argc == 3) {
		load(argv[2], &a);
		printf("user=%s\nenabled=%d\nhardcore=%d\nend=1\n",
		       a.token[0] ? a.username : "", a.token[0] && a.enabled, a.hardcore);
		return 0;
	}
	if (!strcmp(argv[1], "login") && argc == 4) {
		if (!safe_value(argv[3]) || strlen(argv[3]) >= 64) {
			puts("ERR name");
			return 1;
		}
		return login(argv[2], argv[3]);
	}
	if (!strcmp(argv[1], "logout") && argc == 3) {
		account_path(argv[2], path, sizeof(path));
		if (unlink(path) != 0 && errno != ENOENT) {
			puts("ERR storage");
			return 1;
		}
		puts("OK");
		return 0;
	}
	if (!strcmp(argv[1], "set") && argc == 5 &&
	    (!strcmp(argv[4], "0") || !strcmp(argv[4], "1"))) {
		load(argv[2], &a);
		if (!a.token[0]) {
			puts("ERR not-logged-in");
			return 1;
		}
		if (!strcmp(argv[3], "enabled"))
			a.enabled = argv[4][0] == '1';
		else if (!strcmp(argv[3], "hardcore"))
			a.hardcore = argv[4][0] == '1';
		else {
			puts("ERR key");
			return 1;
		}
		if (save(argv[2], &a) != 0) {
			puts("ERR storage");
			return 1;
		}
		puts("OK");
		return 0;
	}
	fprintf(stderr, "nuubos-achievementsctl: bad arguments\n");
	return 2;
}
