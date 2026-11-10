/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-medialib — the Media application's light library (EPIC-028).
 *
 * Not a media-center database: every answer is read from the folders when
 * asked (user decision 2026-10-12: "Videos" and "Music" folders on every
 * storage, plus browsing any folder).
 *
 *   sources USER      source=<videos|music|recordings>\t<path>\t<volume>
 *                     (<volume> "" = Internal Storage, <name> = /media/<name>,
 *                     share:<name> = a connected network share)
 *   videos DIR        folder=<path>\t<name>\t<poster>\t<entries>
 *                     video=<path>\t<title>\t<poster>\t<resume s>\t<size>
 *   albums DIR        album=<path>\t<title>\t<artist>\t<cover>\t<tracks>
 *                     (folders up to 3 levels down holding audio files;
 *                     audio loose in DIR is an album with an empty title)
 *   tracks DIR [all]  track=<path>\t<title>\t<number> ("all": also the
 *                     folders below, 3 levels: Shuffle All)
 *   resume USER       resume=<path>\t<seconds>\t<mtime>\t<title>\t<poster>
 *                     (local videos with a saved position, newest first)
 *   cover PATH        cover=<image> for a folder or an audio file: the
 *                     folder's picture, else the picture embedded in the
 *                     file (ID3v2 APIC, FLAC PICTURE, MP4 covr), written
 *                     once to /userdata/cache/media/covers
 *   playlists USER    playlist=<file>\t<name>\t<tracks>\t<cover>
 *   playlist FILE     track=<path>\t<title>\t<position> (missing files skipped)
 *   playlist-new USER NAME            OK <file> | ERR exists|name
 *   playlist-add FILE PATH...         folders add their audio files, sorted;
 *                                     a track already in it is not repeated
 *   playlist-remove FILE POSITION | playlist-rename FILE NAME |
 *   playlist-delete FILE
 *                     User playlists (user request 2026-10-12) are plain
 *                     M3U files, one per playlist, in the user's
 *                     appdata/media/playlists: other players read them too.
 * Every list ends with end=1. Dot-names are skipped; values never hold
 * tabs or line breaks (such names are skipped).
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef USERDATA_ROOT
#define USERDATA_ROOT "/userdata"
#endif
#ifndef MEDIA_ROOT
#define MEDIA_ROOT "/media"
#endif
#ifndef SHARES_ROOT
#define SHARES_ROOT "/run/nuubos/shares"
#endif
#ifndef COVER_CACHE
#define COVER_CACHE USERDATA_ROOT "/cache/media/covers"
#endif
#define MAX_ENTRIES 20000
#define MAX_PICTURE (16u << 20)

static const char *const video_exts[] = { "mkv", "mp4", "m4v", "avi", "webm", "mov", "ts", "mpg", "mpeg", NULL };
static const char *const audio_exts[] = { "mp3", "flac", "ogg", "opus", "m4a", "aac", "wav", "alac", NULL };
static const char *const image_exts[] = { "jpg", "jpeg", "png", NULL };
static const char *const folder_images[] = { "cover", "folder", "front", "album", "poster", NULL };

static const char *ext_of(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && dot != name ? dot + 1 : "";
}

static bool has_ext(const char *name, const char *const *exts)
{
	const char *e = ext_of(name);

	for (int i = 0; exts[i]; i++)
		if (!strcasecmp(e, exts[i]))
			return true;
	return false;
}

static bool usable_name(const char *name)
{
	return name[0] != '.' && !strpbrk(name, "\t\r\n");
}

static bool is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                   */
/* ------------------------------------------------------------------ */

struct entry {
	char *name;
	bool dir;
	long long size;
};

struct listing {
	struct entry *e;
	size_t n;
};

static int entry_cmp(const void *a, const void *b)
{
	return strcasecmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

static void list_dir(const char *dir, struct listing *out)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	size_t cap = 0;
	char path[PATH_MAX];

	out->e = NULL;
	out->n = 0;
	if (!d)
		return;
	while ((de = readdir(d)) && out->n < MAX_ENTRIES) {
		struct stat st;

		if (!usable_name(de->d_name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (out->n == cap) {
			cap = cap ? cap * 2 : 64;
			out->e = realloc(out->e, cap * sizeof(*out->e));
			if (!out->e)
				abort();
		}
		out->e[out->n].name = strdup(de->d_name);
		out->e[out->n].dir = S_ISDIR(st.st_mode);
		out->e[out->n].size = (long long)st.st_size;
		out->n++;
	}
	closedir(d);
	if (out->n)
		qsort(out->e, out->n, sizeof(*out->e), entry_cmp);
}

static void free_listing(struct listing *l)
{
	for (size_t i = 0; i < l->n; i++)
		free(l->e[i].name);
	free(l->e);
	l->e = NULL;
	l->n = 0;
}

/* ------------------------------------------------------------------ */
/* Pictures                                                            */
/* ------------------------------------------------------------------ */

/* <dir>/<base>.<jpg|jpeg|png>, case-insensitive on the base and extension. */
static bool find_image(const char *dir, const char *base, char *out, size_t size)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	size_t blen = strlen(base);
	bool found = false;

	if (!d)
		return false;
	while (!found && (de = readdir(d))) {
		if (strncasecmp(de->d_name, base, blen) || de->d_name[blen] != '.' || !has_ext(de->d_name, image_exts))
			continue;
		if (strchr(de->d_name + blen + 1, '.') || !usable_name(de->d_name))
			continue;
		snprintf(out, size, "%s/%s", dir, de->d_name);
		found = true;
	}
	closedir(d);
	return found;
}

static bool folder_image(const char *dir, char *out, size_t size)
{
	for (int i = 0; folder_images[i]; i++)
		if (find_image(dir, folder_images[i], out, size))
			return true;
	return false;
}

/* A video's poster: <name>.jpg, <name>-poster.jpg, then the folder's. */
static void video_poster(const char *dir, const char *file, char *out, size_t size)
{
	char base[NAME_MAX + 16];
	const char *dot = strrchr(file, '.');
	size_t len = dot ? (size_t)(dot - file) : strlen(file);

	out[0] = '\0';
	snprintf(base, sizeof(base), "%.*s", (int)len, file);
	if (find_image(dir, base, out, size))
		return;
	snprintf(base, sizeof(base), "%.*s-poster", (int)len, file);
	if (find_image(dir, base, out, size))
		return;
	out[0] = '\0';
}

static uint64_t fnv1a(const char *s, uint64_t h)
{
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h;
}

static uint32_t be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t syncsafe(const unsigned char *p)
{
	return (uint32_t)(p[0] & 0x7f) << 21 | (uint32_t)(p[1] & 0x7f) << 14 | (uint32_t)(p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

static unsigned char *read_at(FILE *fp, long off, size_t len)
{
	unsigned char *buf;

	if (len == 0 || len > MAX_PICTURE || fseek(fp, off, SEEK_SET) != 0)
		return NULL;
	buf = malloc(len);
	if (buf && fread(buf, 1, len, fp) != len) {
		free(buf);
		buf = NULL;
	}
	return buf;
}

/* Picture data kind by its first bytes: "jpg", "png" or NULL. */
static const char *picture_kind(const unsigned char *p, size_t len)
{
	if (len > 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff)
		return "jpg";
	if (len > 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8))
		return "png";
	return NULL;
}

/* ID3v2.2/2.3/2.4 APIC/PIC: the picture at start, len inside the returned tag buffer. */
static unsigned char *id3_picture(FILE *fp, size_t *start, size_t *len)
{
	unsigned char h[10], *tag;
	uint32_t size, pos = 0;
	int ver;

	if (fseek(fp, 0, SEEK_SET) != 0 || fread(h, 1, 10, fp) != 10 || memcmp(h, "ID3", 3))
		return NULL;
	ver = h[3];
	size = syncsafe(h + 6);
	if (ver < 2 || ver > 4 || !(tag = read_at(fp, 10, size)))
		return NULL;
	if (ver >= 3 && (h[5] & 0x40) && size > 4) {
		/* Extended header. */
		pos = ver == 4 ? syncsafe(tag) : be32(tag) + 4;
	}
	while (pos + (ver == 2 ? 6u : 10u) <= size) {
		unsigned char *f = tag + pos;
		uint32_t fsize, hdr = ver == 2 ? 6 : 10;
		bool pic;

		if (!f[0])
			break;
		if (ver == 2) {
			fsize = (uint32_t)f[3] << 16 | (uint32_t)f[4] << 8 | f[5];
			pic = !memcmp(f, "PIC", 3);
		} else {
			fsize = ver == 4 ? syncsafe(f + 4) : be32(f + 4);
			pic = !memcmp(f, "APIC", 4);
		}
		if (fsize > size - pos - hdr)
			break;
		if (pic && fsize > 4) {
			unsigned char *d = f + hdr, *end = d + fsize;
			int enc = *d++;

			if (ver == 2) {
				d += 3; /* image format "JPG"/"PNG" */
			} else {
				while (d < end && *d)
					d++;
				d++; /* MIME type */
			}
			d++; /* picture type */
			if (enc == 1 || enc == 2) {
				while (d + 1 < end && (d[0] || d[1]))
					d += 2;
				d += 2;
			} else {
				while (d < end && *d)
					d++;
				d++;
			}
			if (d < end) {
				*start = (size_t)(d - tag);
				*len = (size_t)(end - d);
				return tag;
			}
		}
		pos += hdr + fsize;
	}
	free(tag);
	return NULL;
}

static unsigned char *flac_picture(FILE *fp, size_t *start, size_t *len)
{
	unsigned char h[4];
	long off = 4;

	if (fseek(fp, 0, SEEK_SET) != 0 || fread(h, 1, 4, fp) != 4 || memcmp(h, "fLaC", 4))
		return NULL;
	for (int guard = 0; guard < 64; guard++) {
		uint32_t blen;
		bool last;

		if (fseek(fp, off, SEEK_SET) != 0 || fread(h, 1, 4, fp) != 4)
			return NULL;
		last = h[0] & 0x80;
		blen = (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
		if ((h[0] & 0x7f) == 6) {
			unsigned char *b = read_at(fp, off + 4, blen);
			uint32_t p = 4, n;

			if (!b)
				return NULL;
			/* type, MIME, description, 4 x 32-bit sizes, data length. */
			if (p + 4 <= blen && (n = be32(b + p)) <= blen - p - 4) {
				p += 4 + n;
				if (p + 4 <= blen && (n = be32(b + p)) <= blen - p - 4) {
					p += 4 + n + 16;
					if (p + 4 <= blen && (n = be32(b + p)) <= blen - p - 4) {
						*start = p + 4;
						*len = n;
						return b;
					}
				}
			}
			free(b);
			return NULL;
		}
		if (last)
			return NULL;
		off += 4 + (long)blen;
	}
	return NULL;
}

/* MP4: moov/udta/meta/ilst/covr/data. */
static bool mp4_find(const unsigned char *b, size_t len, const char *type, size_t *start, size_t *size)
{
	size_t p = 0;

	while (p + 8 <= len) {
		uint32_t n = be32(b + p);

		if (n < 8 || n > len - p)
			return false;
		if (!memcmp(b + p + 4, type, 4)) {
			*start = p + 8;
			*size = n - 8;
			return true;
		}
		p += n;
	}
	return false;
}

static unsigned char *mp4_picture(FILE *fp, size_t *start, size_t *len)
{
	unsigned char h[8], *moov;
	long off = 0;

	for (int guard = 0; guard < 64; guard++) {
		uint32_t n;

		if (fseek(fp, off, SEEK_SET) != 0 || fread(h, 1, 8, fp) != 8)
			return NULL;
		n = be32(h);
		if (n < 8)
			return NULL;
		if (!memcmp(h + 4, "moov", 4))
			break;
		off += n;
	}
	if (memcmp(h + 4, "moov", 4) || !(moov = read_at(fp, off + 8, be32(h) - 8)))
		return NULL;
	{
		size_t a = 0, an = be32(h) - 8, sa, na;

		if (!mp4_find(moov + a, an, "udta", &sa, &na))
			goto none;
		a += sa;
		an = na;
		if (!mp4_find(moov + a, an, "meta", &sa, &na) || na <= 4)
			goto none;
		a += sa + 4;
		an = na - 4;
		if (!mp4_find(moov + a, an, "ilst", &sa, &na))
			goto none;
		a += sa;
		an = na;
		if (!mp4_find(moov + a, an, "covr", &sa, &na))
			goto none;
		a += sa;
		an = na;
		if (!mp4_find(moov + a, an, "data", &sa, &na) || na <= 8)
			goto none;
		*start = a + sa + 8;
		*len = na - 8;
		return moov;
	}
none:
	free(moov);
	return NULL;
}

/* The embedded picture of `file`, written once to the cover cache. */
static bool embedded_cover(const char *file, char *out, size_t size)
{
	struct stat st;
	char key[64], tmp[PATH_MAX];
	unsigned char *buf = NULL;
	size_t start = 0, len = 0;
	const char *kind;
	uint64_t h;
	FILE *fp;

	if (stat(file, &st) != 0)
		return false;
	h = fnv1a(file, 1469598103934665603ULL);
	snprintf(key, sizeof(key), "%lld:%lld", (long long)st.st_mtime, (long long)st.st_size);
	h = fnv1a(key, h);
	for (int i = 0; i < 2; i++) {
		snprintf(out, size, COVER_CACHE "/%016llx.%s", (unsigned long long)h, i ? "png" : "jpg");
		if (is_file(out))
			return true;
	}
	/* A file known to have no picture. */
	snprintf(out, size, COVER_CACHE "/%016llx.none", (unsigned long long)h);
	if (is_file(out))
		return false;
	fp = fopen(file, "rb");
	if (!fp)
		return false;
	buf = id3_picture(fp, &start, &len);
	if (!buf)
		buf = flac_picture(fp, &start, &len);
	if (!buf)
		buf = mp4_picture(fp, &start, &len);
	fclose(fp);
	kind = buf ? picture_kind(buf + start, len) : NULL;
	{
		/* mkdir -p of the cache. */
		char dir[] = COVER_CACHE;

		for (char *p = dir + 1; *p; p++)
			if (*p == '/') {
				*p = '\0';
				mkdir(dir, 0755);
				*p = '/';
			}
		mkdir(dir, 0755);
	}
	if (!kind) {
		FILE *none = fopen(out, "w");

		if (none)
			fclose(none);
		free(buf);
		return false;
	}
	snprintf(out, size, COVER_CACHE "/%016llx.%s", (unsigned long long)h, kind);
	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", out, (long)getpid());
	fp = fopen(tmp, "wb");
	if (!fp || fwrite(buf + start, 1, len, fp) != len || fclose(fp) != 0 || rename(tmp, out) != 0) {
		if (fp)
			unlink(tmp);
		free(buf);
		return false;
	}
	free(buf);
	return true;
}

static void cover_of(const char *path, char *out, size_t size)
{
	char dir[PATH_MAX];
	char *slash;

	out[0] = '\0';
	if (is_dir(path)) {
		if (folder_image(path, out, size))
			return;
		out[0] = '\0';
		return;
	}
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (!slash)
		return;
	*slash = '\0';
	if (folder_image(dir, out, size))
		return;
	if (!embedded_cover(path, out, size))
		out[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Titles                                                              */
/* ------------------------------------------------------------------ */

/* "03 - Song.mp3" → "Song", number 3; "Movie.Name.2019.mkv" keeps dots
 * except the extension. */
static void title_of(const char *name, char *out, size_t size, int *number)
{
	const char *p = name, *dot = strrchr(name, '.');
	size_t len = dot && dot != name ? (size_t)(dot - name) : strlen(name);
	int n = 0, digits = 0;

	while (digits < 3 && (size_t)(p - name) < len && isdigit((unsigned char)*p)) {
		n = n * 10 + (*p - '0');
		p++;
		digits++;
	}
	if (digits && (size_t)(p - name) < len && (*p == ' ' || *p == '.' || *p == '-' || *p == '_')) {
		while ((size_t)(p - name) < len && (*p == ' ' || *p == '.' || *p == '-' || *p == '_'))
			p++;
		if ((size_t)(p - name) >= len)
			p = name, n = 0;
	} else {
		p = name;
		n = 0;
	}
	if (number)
		*number = n;
	snprintf(out, size, "%.*s", (int)(len - (size_t)(p - name)), p);
	for (char *c = out; *c; c++)
		if (*c == '_')
			*c = ' ';
}

/* ------------------------------------------------------------------ */
/* Resume positions (mpv watch-later, written with the file name)      */
/* ------------------------------------------------------------------ */

struct resume {
	char *path;
	double seconds;
	long long mtime;
};

static struct resume *resumes;
static size_t resume_count;

static bool valid_user(const char *id)
{
	size_t n = strlen(id);

	if (n < 8 || n > 36)
		return false;
	for (const char *p = id; *p; p++)
		if (!(isxdigit((unsigned char)*p) || *p == '-'))
			return false;
	return true;
}

static void load_resumes(const char *user)
{
	char dir[PATH_MAX], path[PATH_MAX], line[PATH_MAX + 16];
	DIR *d;
	struct dirent *de;

	if (!user || !valid_user(user))
		return;
	snprintf(dir, sizeof(dir), USERDATA_ROOT "/users/%s/appdata/media/watch-later", user);
	d = opendir(dir);
	if (!d)
		return;
	while ((de = readdir(d))) {
		struct stat st;
		char file[PATH_MAX] = "";
		double seconds = -1;
		FILE *fp;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || !(fp = fopen(path, "r")))
			continue;
		while (fgets(line, sizeof(line), fp)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (!file[0] && !strncmp(line, "# ", 2) && line[2] == '/')
				snprintf(file, sizeof(file), "%s", line + 2);
			else if (!strncmp(line, "start=", 6))
				seconds = atof(line + 6);
		}
		fclose(fp);
		if (!file[0] || seconds < 1 || strpbrk(file, "\t"))
			continue;
		resumes = realloc(resumes, (resume_count + 1) * sizeof(*resumes));
		if (!resumes)
			abort();
		resumes[resume_count].path = strdup(file);
		resumes[resume_count].seconds = seconds;
		resumes[resume_count].mtime = (long long)st.st_mtime;
		resume_count++;
	}
	closedir(d);
}

static double resume_of(const char *path)
{
	for (size_t i = 0; i < resume_count; i++)
		if (!strcmp(resumes[i].path, path))
			return resumes[i].seconds;
	return 0;
}

static int resume_cmp(const void *a, const void *b)
{
	long long x = ((const struct resume *)a)->mtime, y = ((const struct resume *)b)->mtime;

	return x < y ? 1 : x > y ? -1 : 0;
}

static int cmd_resume(const char *user)
{
	char dir[PATH_MAX], poster[PATH_MAX], title[NAME_MAX + 1];
	size_t shown = 0;

	load_resumes(user);
	if (resume_count)
		qsort(resumes, resume_count, sizeof(*resumes), resume_cmp);
	for (size_t i = 0; i < resume_count && shown < 30; i++) {
		const char *path = resumes[i].path, *slash = strrchr(path, '/');

		if (!slash || !is_file(path) || !has_ext(path, video_exts))
			continue;
		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
		video_poster(dir, slash + 1, poster, sizeof(poster));
		if (!poster[0] && !folder_image(dir, poster, sizeof(poster)))
			poster[0] = '\0';
		title_of(slash + 1, title, sizeof(title), NULL);
		printf("resume=%s\t%.0f\t%lld\t%s\t%s\n", path, resumes[i].seconds, resumes[i].mtime, title, poster);
		shown++;
	}
	puts("end=1");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Sources                                                             */
/* ------------------------------------------------------------------ */

static const char *const video_names[] = { "videos", "video", "movies", "films", "film", NULL };
static const char *const music_names[] = { "music", "musica", "musik", "musique", NULL };

static bool name_in(const char *name, const char *const *names)
{
	for (int i = 0; names[i]; i++)
		if (!strcasecmp(name, names[i]))
			return true;
	return false;
}

static void volume_sources(const char *root, const char *volume)
{
	struct listing l;

	list_dir(root, &l);
	for (size_t i = 0; i < l.n; i++) {
		if (!l.e[i].dir)
			continue;
		if (name_in(l.e[i].name, video_names))
			printf("source=videos\t%s/%s\t%s\n", root, l.e[i].name, volume);
		else if (name_in(l.e[i].name, music_names))
			printf("source=music\t%s/%s\t%s\n", root, l.e[i].name, volume);
	}
	free_listing(&l);
}

static int cmd_sources(const char *user)
{
	char path[PATH_MAX];
	struct listing l;

	volume_sources(USERDATA_ROOT, "");
	if (user && valid_user(user)) {
		snprintf(path, sizeof(path), USERDATA_ROOT "/users/%s/recordings", user);
		if (is_dir(path))
			printf("source=recordings\t%s\t\n", path);
	}
	list_dir(MEDIA_ROOT, &l);
	for (size_t i = 0; i < l.n; i++) {
		if (!l.e[i].dir)
			continue;
		snprintf(path, sizeof(path), MEDIA_ROOT "/%s", l.e[i].name);
		volume_sources(path, l.e[i].name);
	}
	free_listing(&l);
	/* Network shares (EPIC-036): only connected ones are directories with
	 * content; a NAS often keeps media at the top, so a share without
	 * Videos/Music folders is still reachable from Browse. */
	list_dir(SHARES_ROOT, &l);
	for (size_t i = 0; i < l.n; i++) {
		char volume[NAME_MAX + 8];

		if (!l.e[i].dir)
			continue;
		snprintf(path, sizeof(path), SHARES_ROOT "/%s", l.e[i].name);
		snprintf(volume, sizeof(volume), "share:%s", l.e[i].name);
		volume_sources(path, volume);
	}
	free_listing(&l);
	puts("end=1");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Videos, albums, tracks                                              */
/* ------------------------------------------------------------------ */

/* Media entries of a folder (for its tile caption). */
static size_t media_count(const char *dir, const char *const *exts)
{
	struct listing l;
	size_t n = 0;

	list_dir(dir, &l);
	for (size_t i = 0; i < l.n; i++)
		if (l.e[i].dir || has_ext(l.e[i].name, exts))
			n++;
	free_listing(&l);
	return n;
}

static int cmd_videos(const char *dir, const char *user)
{
	char path[PATH_MAX], poster[PATH_MAX], title[NAME_MAX + 1];
	struct listing l;

	load_resumes(user);
	list_dir(dir, &l);
	for (size_t i = 0; i < l.n; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, l.e[i].name);
		if (l.e[i].dir) {
			size_t n = media_count(path, video_exts);

			if (!n)
				continue;
			if (!folder_image(path, poster, sizeof(poster)))
				poster[0] = '\0';
			printf("folder=%s\t%s\t%s\t%zu\n", path, l.e[i].name, poster, n);
		}
	}
	for (size_t i = 0; i < l.n; i++) {
		if (l.e[i].dir || !has_ext(l.e[i].name, video_exts))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, l.e[i].name);
		video_poster(dir, l.e[i].name, poster, sizeof(poster));
		title_of(l.e[i].name, title, sizeof(title), NULL);
		printf("video=%s\t%s\t%s\t%.0f\t%lld\n", path, title, poster, resume_of(path), l.e[i].size);
	}
	free_listing(&l);
	puts("end=1");
	return 0;
}

static void albums_in(const char *dir, const char *top, const char *parent_name, int depth)
{
	char path[PATH_MAX], cover[PATH_MAX];
	struct listing l;
	size_t tracks = 0;
	const char *first = NULL;

	list_dir(dir, &l);
	for (size_t i = 0; i < l.n; i++)
		if (!l.e[i].dir && has_ext(l.e[i].name, audio_exts)) {
			if (!first)
				first = l.e[i].name;
			tracks++;
		}
	if (tracks) {
		const char *name = strrchr(dir, '/');
		bool loose = !strcmp(dir, top);

		cover[0] = '\0';
		if (!folder_image(dir, cover, sizeof(cover))) {
			snprintf(path, sizeof(path), "%s/%s", dir, first);
			if (!embedded_cover(path, cover, sizeof(cover)))
				cover[0] = '\0';
		}
		printf("album=%s\t%s\t%s\t%s\t%zu\n", dir, loose ? "" : name ? name + 1 : dir,
		       loose ? "" : parent_name, cover, tracks);
	}
	if (depth < 3)
		for (size_t i = 0; i < l.n; i++) {
			if (!l.e[i].dir)
				continue;
			snprintf(path, sizeof(path), "%s/%s", dir, l.e[i].name);
			/* Music/<Artist>/<Album>: the artist is the parent folder. */
			albums_in(path, top, strcmp(dir, top) ? strrchr(dir, '/') + 1 : "", depth + 1);
		}
	free_listing(&l);
}

static int cmd_albums(const char *dir)
{
	albums_in(dir, dir, "", 0);
	puts("end=1");
	return 0;
}

static void tracks_in(const char *dir, int depth)
{
	char title[NAME_MAX + 1], path[PATH_MAX];
	struct listing l;

	list_dir(dir, &l);
	for (size_t i = 0; i < l.n; i++) {
		int n = 0;

		if (l.e[i].dir || !has_ext(l.e[i].name, audio_exts))
			continue;
		title_of(l.e[i].name, title, sizeof(title), &n);
		printf("track=%s/%s\t%s\t%d\n", dir, l.e[i].name, title, n);
	}
	for (size_t i = 0; depth > 0 && i < l.n; i++)
		if (l.e[i].dir) {
			snprintf(path, sizeof(path), "%s/%s", dir, l.e[i].name);
			tracks_in(path, depth - 1);
		}
	free_listing(&l);
}

static int cmd_tracks(const char *dir, bool all)
{
	tracks_in(dir, all ? 3 : 0);
	puts("end=1");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Playlists                                                           */
/* ------------------------------------------------------------------ */

struct plist {
	char **path;
	size_t n;
};

static void plist_free(struct plist *p)
{
	for (size_t i = 0; i < p->n; i++)
		free(p->path[i]);
	free(p->path);
	p->path = NULL;
	p->n = 0;
}

static void plist_push(struct plist *p, const char *path)
{
	for (size_t i = 0; i < p->n; i++)
		if (!strcmp(p->path[i], path))
			return;
	p->path = realloc(p->path, (p->n + 1) * sizeof(*p->path));
	if (!p->path || !(p->path[p->n] = strdup(path)))
		abort();
	p->n++;
}

static bool plist_load(const char *file, struct plist *p)
{
	char line[PATH_MAX + 64];
	FILE *fp = fopen(file, "r");

	p->path = NULL;
	p->n = 0;
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '/' && !strchr(line, '\t'))
			plist_push(p, line);
	}
	fclose(fp);
	return true;
}

static bool plist_save(const char *file, const struct plist *p)
{
	char tmp[PATH_MAX + 16], title[NAME_MAX + 1];
	FILE *fp;

	snprintf(tmp, sizeof(tmp), "%s.tmp", file);
	fp = fopen(tmp, "w");
	if (!fp)
		return false;
	fputs("#EXTM3U\n", fp);
	for (size_t i = 0; i < p->n; i++) {
		const char *slash = strrchr(p->path[i], '/');

		title_of(slash ? slash + 1 : p->path[i], title, sizeof(title), NULL);
		fprintf(fp, "#EXTINF:-1,%s\n%s\n", title, p->path[i]);
	}
	/* fsync: /userdata is exFAT, an unsynced file renamed into place comes
	 * back empty after a crash. */
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0 || fclose(fp) != 0 ||
	    rename(tmp, file) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

static void playlist_dir(const char *user, char *out, size_t size)
{
	snprintf(out, size, USERDATA_ROOT "/users/%s/appdata/media/playlists", user);
}

/* A playlist file of some user: <users>/<id>/appdata/media/playlists/x.m3u */
static bool valid_playlist_file(const char *file)
{
	const char *prefix = USERDATA_ROOT "/users/", *rest, *slash;
	size_t n = strlen(prefix);

	if (strncmp(file, prefix, n) || strstr(file, "/../") || strstr(file, "/./"))
		return false;
	rest = file + n;
	slash = strchr(rest, '/');
	if (!slash || strncmp(slash, "/appdata/media/playlists/", 25))
		return false;
	slash += 25;
	return slash[0] && slash[0] != '.' && !strchr(slash, '/') && strlen(slash) > 4 &&
	       !strcasecmp(slash + strlen(slash) - 4, ".m3u");
}

static bool valid_playlist_name(const char *name)
{
	size_t n = strlen(name);

	if (!n || n > 80 || name[0] == '.' || name[0] == ' ')
		return false;
	for (const char *c = name; *c; c++)
		if (*c == '/' || *c == '\\' || (unsigned char)*c < 32)
			return false;
	return true;
}

static void playlist_name(const char *file, char *out, size_t size)
{
	const char *slash = strrchr(file, '/');

	snprintf(out, size, "%s", slash ? slash + 1 : file);
	if (strlen(out) > 4)
		out[strlen(out) - 4] = '\0';
}

static int cmd_playlists(const char *user)
{
	char dir[PATH_MAX], file[PATH_MAX], name[NAME_MAX + 1], cover[PATH_MAX];
	struct listing l;

	if (!user || !valid_user(user)) {
		puts("ERR user");
		return 1;
	}
	playlist_dir(user, dir, sizeof(dir));
	list_dir(dir, &l);
	for (size_t i = 0; i < l.n; i++) {
		struct plist p;

		if (l.e[i].dir || strlen(l.e[i].name) <= 4 || strcasecmp(l.e[i].name + strlen(l.e[i].name) - 4, ".m3u"))
			continue;
		snprintf(file, sizeof(file), "%s/%s", dir, l.e[i].name);
		plist_load(file, &p);
		cover[0] = '\0';
		for (size_t k = 0; k < p.n && !cover[0]; k++)
			if (is_file(p.path[k]))
				cover_of(p.path[k], cover, sizeof(cover));
		playlist_name(file, name, sizeof(name));
		printf("playlist=%s\t%s\t%zu\t%s\n", file, name, p.n, cover);
		plist_free(&p);
	}
	free_listing(&l);
	puts("end=1");
	return 0;
}

static int cmd_playlist(const char *file)
{
	char title[NAME_MAX + 1];
	struct plist p;

	if (!valid_playlist_file(file) || !plist_load(file, &p)) {
		puts("ERR playlist");
		return 1;
	}
	for (size_t i = 0; i < p.n; i++) {
		const char *slash = strrchr(p.path[i], '/');

		if (!is_file(p.path[i]))
			continue;
		title_of(slash + 1, title, sizeof(title), NULL);
		printf("track=%s\t%s\t%zu\n", p.path[i], title, i);
	}
	plist_free(&p);
	puts("end=1");
	return 0;
}

static bool allowed(const char *path, char *real, size_t size);

static int playlist_edit(int argc, char **argv)
{
	const char *cmd = argv[1];
	char file[PATH_MAX], dir[PATH_MAX], real[PATH_MAX];
	struct plist p;
	bool ok;

	if (!strcmp(cmd, "playlist-new")) {
		if (argc != 4 || !valid_user(argv[2]) || !valid_playlist_name(argv[3]))
			return puts("ERR name"), 1;
		playlist_dir(argv[2], dir, sizeof(dir));
		snprintf(file, sizeof(file), "%s/%s.m3u", dir, argv[3]);
		{
			/* mkdir -p of the playlists folder. */
			char tmp[PATH_MAX];

			snprintf(tmp, sizeof(tmp), "%s", dir);
			for (char *c = tmp + 1; *c; c++)
				if (*c == '/') {
					*c = '\0';
					mkdir(tmp, 0755);
					*c = '/';
				}
			mkdir(tmp, 0755);
		}
		if (is_file(file))
			return puts("ERR exists"), 1;
		p.path = NULL;
		p.n = 0;
		if (!plist_save(file, &p))
			return puts("ERR storage"), 1;
		printf("OK %s\n", file);
		return 0;
	}
	if (argc < 3 || !valid_playlist_file(argv[2]) || !is_file(argv[2]))
		return puts("ERR playlist"), 1;
	snprintf(file, sizeof(file), "%s", argv[2]);
	if (!strcmp(cmd, "playlist-delete"))
		return unlink(file) == 0 ? (puts("OK"), 0) : (puts("ERR storage"), 1);
	if (!strcmp(cmd, "playlist-rename")) {
		char to[PATH_MAX];

		if (argc != 4 || !valid_playlist_name(argv[3]))
			return puts("ERR name"), 1;
		snprintf(dir, sizeof(dir), "%s", file);
		*strrchr(dir, '/') = '\0';
		snprintf(to, sizeof(to), "%s/%s.m3u", dir, argv[3]);
		if (is_file(to))
			return puts("ERR exists"), 1;
		return rename(file, to) == 0 ? (printf("OK %s\n", to), 0) : (puts("ERR storage"), 1);
	}
	plist_load(file, &p);
	if (!strcmp(cmd, "playlist-remove") && argc == 4) {
		size_t at = (size_t)strtoul(argv[3], NULL, 10);

		if (at < p.n) {
			free(p.path[at]);
			memmove(p.path + at, p.path + at + 1, (p.n - at - 1) * sizeof(*p.path));
			p.n--;
		}
	} else if (!strcmp(cmd, "playlist-add") && argc >= 4) {
		for (int i = 3; i < argc; i++) {
			if (!allowed(argv[i], real, sizeof(real)))
				continue;
			if (is_dir(real)) {
				struct listing l;
				char path[PATH_MAX];

				list_dir(real, &l);
				for (size_t k = 0; k < l.n; k++)
					if (!l.e[k].dir && has_ext(l.e[k].name, audio_exts)) {
						snprintf(path, sizeof(path), "%s/%s", real, l.e[k].name);
						plist_push(&p, path);
					}
				free_listing(&l);
			} else if (has_ext(real, audio_exts)) {
				plist_push(&p, real);
			}
		}
	} else {
		plist_free(&p);
		return puts("ERR usage"), 2;
	}
	ok = plist_save(file, &p);
	printf(ok ? "OK %zu\n" : "ERR storage\n", p.n);
	plist_free(&p);
	return ok ? 0 : 1;
}

/* Only user storage: the same roots as nuubos-filesctl. */
static bool allowed(const char *path, char *real, size_t size)
{
	char buf[PATH_MAX];

	if (!path || path[0] != '/' || !realpath(path, buf))
		return false;
	snprintf(real, size, "%s", buf);
	return !strncmp(buf, USERDATA_ROOT "/", strlen(USERDATA_ROOT) + 1) || !strcmp(buf, USERDATA_ROOT) ||
	       !strncmp(buf, MEDIA_ROOT "/", strlen(MEDIA_ROOT) + 1) ||
	       !strncmp(buf, SHARES_ROOT "/", strlen(SHARES_ROOT) + 1);
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	char real[PATH_MAX], cover[PATH_MAX];

	if (!strcmp(cmd, "sources"))
		return cmd_sources(argc > 2 ? argv[2] : NULL);
	if (!strcmp(cmd, "resume"))
		return cmd_resume(argc > 2 ? argv[2] : NULL);
	if (!strcmp(cmd, "playlists"))
		return cmd_playlists(argc > 2 ? argv[2] : NULL);
	if (!strcmp(cmd, "playlist") && argc == 3)
		return cmd_playlist(argv[2]);
	if (!strncmp(cmd, "playlist-", 9))
		return playlist_edit(argc, argv);
	if (argc < 3) {
		fprintf(stderr, "usage: nuubos-medialib sources USER | resume USER | videos DIR [USER] | "
				"albums DIR | tracks DIR | cover PATH | playlists USER | playlist FILE |\n"
				"  playlist-new USER NAME | playlist-add FILE PATH... | playlist-remove FILE N |\n"
				"  playlist-rename FILE NAME | playlist-delete FILE\n");
		return 2;
	}
	if (!allowed(argv[2], real, sizeof(real))) {
		puts("ERR path");
		return 1;
	}
	if (!strcmp(cmd, "videos"))
		return cmd_videos(real, argc > 3 ? argv[3] : NULL);
	if (!strcmp(cmd, "albums"))
		return cmd_albums(real);
	if (!strcmp(cmd, "tracks"))
		return cmd_tracks(real, argc > 3 && !strcmp(argv[3], "all"));
	if (!strcmp(cmd, "cover")) {
		cover_of(real, cover, sizeof(cover));
		printf("cover=%s\n", cover);
		return 0;
	}
	puts("ERR unknown");
	return 2;
}
