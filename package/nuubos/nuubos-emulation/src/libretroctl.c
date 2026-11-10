/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-libretroctl — online libretro content for nuubOS emulation:
 * emulators (cores) from the libretro buildbot and the libretro cheat
 * collection. Nothing of it is part of the image: the user downloads it
 * (Game Settings → Get More Emulators, Quick Menu → Cheats, Settings →
 * Gaming → Cheat Collection).
 *
 *   cores SYSTEM      emulators of the buildbot for SYSTEM that are not on
 *                     the console: "core=<id>\t<name>\t<core-info display
 *                     name>" lines, "end=1"; a
 *                     core is for a system when its libretro core-info
 *                     names one of the system's databases (libretro.conf),
 *                     minus unsupported-cores.conf. The buildbot index is
 *                     cached a day.
 *   install CORE      downloads CORE into /userdata/retroarch/cores
 *                     (job core-install); nuubos-emud then offers it for
 *                     the system. A core nuubOS cannot run (listed, or
 *                     needing a library it lacks) fails "invalid".
 *   cheats status     "cheats=0|1", "updated=<epoch>", "end=1".
 *   cheats update     downloads the collection (~37 MB) into
 *                     /userdata/cheats/libretro/cheats.zip with index.txt,
 *                     its files of the nuubOS systems' databases (job
 *                     cheats-update), replacing the previous copy;
 *                     nuubos-emud extracts the files of the game played
 *                     into /userdata/cheats/libretro/<database>.
 *
 * As a nuubos-jobd worker (NUUBOS_JOB_ID set) it reports @progress,
 * @result and @error and stops on SIGTERM, removing partial files.
 * Errors: network, space, invalid (also a file the buildbot does not
 * have), cancelled.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#ifndef USERDATA
#define USERDATA "/userdata"
#endif
#ifndef SHARE_DIR
#define SHARE_DIR "/usr/share/nuubos/emulation"
#endif
#ifndef CORE_INFO_DIR
#define CORE_INFO_DIR "/usr/share/libretro/info"
#endif
#ifndef BUNDLED_CORES
#define BUNDLED_CORES "/usr/lib/libretro"
#endif
#ifndef BUILDBOT_CORES
#define BUILDBOT_CORES "https://buildbot.libretro.com/nightly/linux/aarch64/latest/"
#endif
#ifndef CHEATS_URL
#define CHEATS_URL "https://buildbot.libretro.com/assets/frontend/cheats.zip"
#endif
#define LIBRETRO_CONF SHARE_DIR "/libretro.conf"

#include "core-check.h"
#define DOWNLOADED_CORES USERDATA "/retroarch/cores"
#define CACHE_DIR USERDATA "/cache/retroarch"
#define CORE_INDEX CACHE_DIR "/core-index.txt"
#define CHEATS_DIR USERDATA "/cheats"
#define CHEATS_LIBRETRO CHEATS_DIR "/libretro"
#define CHEATS_STAMP CHEATS_LIBRETRO "/.updated"
#define USER_AGENT "nuubOS libretroctl"
#define INDEX_MAX_AGE (24 * 3600)

static volatile sig_atomic_t cancelled;
static bool as_job;
/* Progress of a download: the part of the job it covers. */
static int progress_from;
static int progress_to = 100;
static int progress_last = -1;

static void on_term(int sig)
{
	(void)sig;
	cancelled = 1;
}

static void copy_text(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

static void trim(char *text)
{
	size_t len = strlen(text);

	while (len > 0 && isspace((unsigned char)text[len - 1]))
		text[--len] = '\0';
}

static void job_progress(int percent)
{
	if (as_job && percent != progress_last) {
		printf("@progress %d\n", percent);
		fflush(stdout);
		progress_last = percent;
	}
}

static int fail(const char *reason)
{
	printf(as_job ? "@error %s\n" : "ERR %s\n", reason);
	fflush(stdout);
	return 1;
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];

	copy_text(tmp, sizeof(tmp), path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(tmp, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

static bool file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Runs a program and waits; true when it exits 0. */
static bool run(char *const argv[])
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		return false;
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void remove_tree(const char *path)
{
	char *argv[] = { (char *)"rm", (char *)"-rf", (char *)path, NULL };

	(void)run(argv);
}

/* ------------------------------------------------------------------ */
/* Downloads                                                           */
/* ------------------------------------------------------------------ */

static int progress_cb(void *p, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
	(void)p;
	(void)ut;
	(void)un;
	if (total > 0)
		job_progress(progress_from + (int)((progress_to - progress_from) * now / total));
	return cancelled ? 1 : 0;
}

/* Downloads url into path (through path.part). Returns NULL or the error
 * reason. */
static const char *download(const char *url, const char *path, long timeout)
{
	char part[PATH_MAX];
	CURL *curl;
	FILE *fp;
	CURLcode rc;
	long status = 0;
	bool written;

	snprintf(part, sizeof(part), "%s.part", path);
	fp = fopen(part, "wb");
	if (!fp)
		return "space";
	curl = curl_easy_init();
	if (!curl) {
		fclose(fp);
		unlink(part);
		return "network";
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	/* A stalled transfer gives up; a slow one may take its time. */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
	rc = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	written = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
	if (fclose(fp) != 0)
		written = false;
	if (cancelled || rc != CURLE_OK || !written || rename(part, path) != 0) {
		unlink(part);
		if (cancelled || rc == CURLE_ABORTED_BY_CALLBACK)
			return "cancelled";
		if (rc == CURLE_WRITE_ERROR || !written)
			return "space";
		if (rc == CURLE_HTTP_RETURNED_ERROR && status == 404)
			return "invalid";
		return "network";
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Emulators                                                           */
/* ------------------------------------------------------------------ */

/* A plain core name ("snes9x", "mesen-s", "bsnes_mercury_accuracy"). */
static bool valid_core(const char *core)
{
	if (!core[0] || strlen(core) > 64)
		return false;
	for (const char *p = core; *p; p++)
		if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && !strchr("_-+", *p))
			return false;
	return true;
}

static void core_info_value(const char *core, const char *key, char *out, size_t size)
{
	char path[PATH_MAX];
	char line[1024];
	size_t klen = strlen(key);
	FILE *fp;

	out[0] = '\0';
	snprintf(path, sizeof(path), CORE_INFO_DIR "/%s_libretro.info", core);
	if (!(fp = fopen(path, "r")))
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		char *end;

		if (strncmp(p, key, klen))
			continue;
		p += klen;
		while (*p == ' ')
			p++;
		if (*p++ != '=')
			continue;
		while (*p == ' ')
			p++;
		if (*p == '"')
			p++;
		trim(p);
		if ((end = strrchr(p, '"')))
			*end = '\0';
		copy_text(out, size, p);
		break;
	}
	fclose(fp);
}

static bool in_pipe_list(const char *list, const char *name, size_t len)
{
	for (const char *p = list; *p;) {
		const char *bar = strchr(p, '|');
		size_t n = bar ? (size_t)(bar - p) : strlen(p);

		if (n == len && !strncmp(p, name, len))
			return true;
		if (!bar)
			break;
		p = bar + 1;
	}
	return false;
}

/* The libretro databases of a system (libretro.conf), "a|b". */
static void system_databases(const char *system, char *out, size_t size)
{
	char line[1024];
	size_t len = strlen(system);
	FILE *fp = fopen(LIBRETRO_CONF, "r");

	out[0] = '\0';
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (line[0] != '#' && !strncmp(line, system, len) && line[len] == '|') {
			copy_text(out, size, line + len + 1);
			break;
		}
	}
	fclose(fp);
}

static bool core_for_databases(const char *core, const char *dbs)
{
	char info[1024];

	core_info_value(core, "database", info, sizeof(info));
	for (const char *p = info; *p;) {
		const char *bar = strchr(p, '|');
		size_t n = bar ? (size_t)(bar - p) : strlen(p);

		if (n && in_pipe_list(dbs, p, n))
			return true;
		if (!bar)
			break;
		p = bar + 1;
	}
	return false;
}

static bool core_installed(const char *core)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), BUNDLED_CORES "/%s_libretro.so", core);
	if (file_exists(path))
		return true;
	snprintf(path, sizeof(path), DOWNLOADED_CORES "/%s_libretro.so", core);
	return file_exists(path);
}

static int cmd_cores(const char *system)
{
	char dbs[1024];
	char line[512];
	struct stat st;
	FILE *fp;

	system_databases(system, dbs, sizeof(dbs));
	if (!dbs[0]) {
		printf("end=1\n");
		return 0;
	}
	/* The buildbot index (".index-extended": date crc name.zip), a day. */
	if (stat(CORE_INDEX, &st) != 0 || time(NULL) - st.st_mtime > INDEX_MAX_AGE) {
		if (mkdir_p(CACHE_DIR) != 0 ||
		    (download(BUILDBOT_CORES ".index-extended", CORE_INDEX, 30) && !file_exists(CORE_INDEX)))
			return fail("network");
	}
	if (!(fp = fopen(CORE_INDEX, "r")))
		return fail("network");
	while (fgets(line, sizeof(line), fp)) {
		char date[32], crc[32], file[256];
		char name[300];
		size_t len;

		if (sscanf(line, "%31s %31s %255s", date, crc, file) != 3)
			continue;
		len = strlen(file);
		if (len <= 16 || strcmp(file + len - 16, "_libretro.so.zip"))
			continue;
		file[len - 16] = '\0';
		if (!valid_core(file) || core_installed(file) || core_unsupported(file) ||
		    !core_for_databases(file, dbs))
			continue;
		core_info_value(file, "display_name", name, sizeof(name));
		for (char *p = name; *p; p++)
			if (*p == '\t' || (unsigned char)*p < 0x20)
				*p = ' ';
		/* The emulator's own name: the end of "System (Name)". */
		{
			char *open = strrchr(name, '(');
			char *close = open ? strchr(open, ')') : NULL;
			char shown[128];

			if (open && close && close > open + 1)
				snprintf(shown, sizeof(shown), "%.*s", (int)(close - open - 1), open + 1);
			else
				copy_text(shown, sizeof(shown), name[0] ? name : file);
			printf("core=%s\t%s\t%s\n", file, shown, name[0] ? name : file);
		}
	}
	fclose(fp);
	printf("end=1\n");
	return 0;
}

static int cmd_install(const char *core)
{
	char url[512];
	char zip[PATH_MAX];
	char dir[PATH_MAX];
	char so[PATH_MAX];
	char dest[PATH_MAX];
	const char *error;

	if (!valid_core(core) || core_unsupported(core))
		return fail("invalid");
	if (core_installed(core)) {
		printf(as_job ? "@result installed\n" : "OK installed\n");
		return 0;
	}
	if (mkdir_p(DOWNLOADED_CORES) != 0)
		return fail("space");
	snprintf(url, sizeof(url), BUILDBOT_CORES "%s_libretro.so.zip", core);
	snprintf(zip, sizeof(zip), DOWNLOADED_CORES "/.%s.zip", core);
	snprintf(dir, sizeof(dir), DOWNLOADED_CORES "/.install-%s", core);
	job_progress(0);
	progress_from = 0;
	progress_to = 90;
	if ((error = download(url, zip, 900)))
		return fail(error);
	remove_tree(dir);
	{
		char *argv[] = { (char *)"unzip", (char *)"-o", (char *)"-q", zip, (char *)"-d", dir, NULL };

		if (mkdir_p(dir) != 0 || !run(argv)) {
			unlink(zip);
			remove_tree(dir);
			return fail(cancelled ? "cancelled" : "space");
		}
	}
	unlink(zip);
	snprintf(so, sizeof(so), "%s/%s_libretro.so", dir, core);
	snprintf(dest, sizeof(dest), DOWNLOADED_CORES "/%s_libretro.so", core);
	/* A build that needs libraries nuubOS lacks would only close the game
	 * at once (GitHub #22). */
	if (!file_exists(so) || !core_libraries_present(so) || chmod(so, 0755) != 0 ||
	    rename(so, dest) != 0) {
		remove_tree(dir);
		return fail("invalid");
	}
	remove_tree(dir);
	job_progress(100);
	printf(as_job ? "@result installed\n" : "OK installed\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Cheats                                                              */
/* ------------------------------------------------------------------ */

static int cmd_cheats_status(void)
{
	struct stat st;
	bool present = stat(CHEATS_STAMP, &st) == 0;

	printf("cheats=%d\nupdated=%lld\nend=1\n", present ? 1 : 0, present ? (long long)st.st_mtime : 0LL);
	return 0;
}

static int cmd_cheats_update(void)
{
	char zip[] = CACHE_DIR "/cheats.zip";
	char fresh[] = CHEATS_DIR "/.libretro.new";
	char old[] = CHEATS_DIR "/.libretro.old";
	char dbs[128][128];
	char line[1024];
	char list[PATH_MAX + 32];
	int count = 0;
	int entries = 0;
	const char *error;
	FILE *fp;
	FILE *ls;
	FILE *index;

	if (mkdir_p(CACHE_DIR) != 0 || mkdir_p(CHEATS_DIR) != 0)
		return fail("space");
	job_progress(0);
	progress_from = 0;
	progress_to = 90;
	if ((error = download(CHEATS_URL, zip, 1800)))
		return fail(error);
	/* The nuubOS systems' databases. */
	if ((fp = fopen(LIBRETRO_CONF, "r"))) {
		while (fgets(line, sizeof(line), fp) && count < 128) {
			char *bar;

			trim(line);
			if (line[0] == '#' || !(bar = strchr(line, '|')))
				continue;
			for (char *db = strtok(bar + 1, "|"); db && count < 128; db = strtok(NULL, "|")) {
				bool known = false;

				for (int i = 0; i < count && !known; i++)
					known = !strcmp(dbs[i], db);
				if (!known)
					snprintf(dbs[count++], sizeof(dbs[0]), "%s", db);
			}
		}
		fclose(fp);
	}
	/* The collection stays an archive (tens of thousands of small files
	 * would take gigabytes of exFAT clusters); index.txt lists its .cht
	 * files of those databases and nuubos-emud extracts the ones of the
	 * game being played. "unzip -l": length, date, time, name. */
	remove_tree(fresh);
	if (mkdir_p(fresh) != 0) {
		unlink(zip);
		return fail("space");
	}
	snprintf(list, sizeof(list), "%s/index.txt", fresh);
	index = fopen(list, "w");
	snprintf(list, sizeof(list), "unzip -l '%s'", zip);
	ls = index ? popen(list, "r") : NULL;
	while (ls && fgets(line, sizeof(line), ls)) {
		char *name = line;
		char *slash;
		size_t len;

		for (int field = 0; field < 3; field++) {
			while (*name == ' ')
				name++;
			while (*name && *name != ' ')
				name++;
		}
		while (*name == ' ')
			name++;
		trim(name);
		len = strlen(name);
		if (len < 5 || strcasecmp(name + len - 4, ".cht") || !(slash = strchr(name, '/')) ||
		    strchr(slash + 1, '/'))
			continue;
		for (int i = 0; i < count; i++) {
			if ((size_t)(slash - name) == strlen(dbs[i]) && !strncmp(name, dbs[i], strlen(dbs[i]))) {
				fprintf(index, "%s\n", name);
				entries++;
				break;
			}
		}
	}
	if (ls)
		pclose(ls);
	snprintf(list, sizeof(list), "%s/cheats.zip", fresh);
	if (!index || fclose(index) != 0 || !entries || cancelled || rename(zip, list) != 0) {
		unlink(zip);
		remove_tree(fresh);
		return fail(cancelled ? "cancelled" : !entries ? "invalid" : "space");
	}
	job_progress(95);
	/* The new copy replaces the old one (and the files extracted from
	 * it) in one step. */
	remove_tree(old);
	if (rename(CHEATS_LIBRETRO, old) != 0 && errno != ENOENT) {
		remove_tree(fresh);
		return fail("space");
	}
	if (rename(fresh, CHEATS_LIBRETRO) != 0) {
		(void)rename(old, CHEATS_LIBRETRO);
		remove_tree(fresh);
		return fail("space");
	}
	remove_tree(old);
	if ((fp = fopen(CHEATS_STAMP, "w")))
		fclose(fp);
	job_progress(100);
	printf(as_job ? "@result updated\n" : "OK updated\n");
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: nuubos-libretroctl cores SYSTEM | install CORE | cheats status|update\n");
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int rc;

	as_job = getenv("NUUBOS_JOB_ID") != NULL;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	if (argc != 3) {
		usage();
		return 2;
	}
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
		return fail("network");
	if (!strcmp(argv[1], "cores"))
		rc = cmd_cores(argv[2]);
	else if (!strcmp(argv[1], "install"))
		rc = cmd_install(argv[2]);
	else if (!strcmp(argv[1], "cheats") && !strcmp(argv[2], "status"))
		rc = cmd_cheats_status();
	else if (!strcmp(argv[1], "cheats") && !strcmp(argv[2], "update"))
		rc = cmd_cheats_update();
	else {
		usage();
		rc = 2;
	}
	curl_global_cleanup();
	return rc;
}
