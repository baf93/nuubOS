/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-mediad — Media playback service (EPIC-027, EPIC-028).
 *
 * Owns the one playback session, controlled over a JSON IPC socket pair
 * inherited by mpv (--input-ipc-client=fd://3), so there is no connect
 * retry. nuubUI and the Quick Menu send the user's actions here; nothing
 * else talks to mpv. Two kinds of session:
 *
 *   video  mpv fullscreen on Wayland with Cedrus decoding (--hwdec=
 *          v4l2request, mpv patch 0001: the DRM PRIME frames go to the GPU
 *          output without a copy, or copied for widths Panfrost cannot
 *          import, hwdec_fit(); codecs Cedrus lacks are decoded in software
 *          and shown by the same output), one item, per-user resume (watch-later,
 *          with the file name written so "Continue Watching" can list it).
 *   audio  music without a window (user request 2026-10-12: the user's own
 *          music also plays behind nuubUI and behind games). mpv idles and
 *          mediad owns the queue: it loads one track at a time and moves on
 *          at end-of-file, so shuffle, repeat and jumps are its own state.
 *          Its PipeWire stream is "nuubOS Music": the Applications (game)
 *          volume does not scale it (93-nuubos-stream-policy.conf); the
 *          Music volume is the player's own (VOLUME, per user, kept in
 *          appdata/media/settings.conf), so the Quick Menu mixes a game
 *          and the music.
 *
 * Protocol (/run/nuubos/mediad.sock, tab separated):
 *   STATUS | SUBSCRIBE   state=idle|playing|paused|stopping kind=video|audio
 *                        title= artist= album= item= index= count=
 *                        position= duration= shuffle=0|1 repeat=off|all|one
 *                        volume= codec= rate= bits= queue_id= last_item=
 *                        last_position= end=1
 *   PLAY <path|url> [start seconds] [audio]   one item (video unless audio;
 *                                             a start, even 0, overrides the
 *                                             saved position)
 *   PLAYDIR <dir> <file>                      the folder's audio files
 *   PLAYLIST <start> <count> [shuffle 0|1]    then <count> lines
 *                                             "<path|url>[\t<title>]"
 *   QUEUE                                     track=<i>\t<path>\t<title>
 *                                             in play order, end=1
 *   PAUSE | SEEK <+-seconds> | SEEKTO <seconds> | NEXT | PREV | JUMP <i> |
 *   SHUFFLE | REPEAT | VOLUME <0..100> | SUB | AUDIO | STOP | PRE_POWER
 *   KEY <action>         a controller action during a video (nuubUI): the
 *                        first one only shows the on-screen controls, the
 *                        next ones act while they are shown
 *   LABELS <pause> <play> <subtitles> <audio> <stop> <off> <confirm pos>
 *          <back pos> <north pos> <west pos>   the OSD's localized words and
 *                        face-button positions (0 top, 1 right, 2 bottom,
 *                        3 left), from nuubUI
 *
 * Video OSD (user request 2026-10-12: "un'interfaccia a comparsa"): drawn by
 * mpv as an ASS osd-overlay over the picture: title, audio/subtitle track,
 * progress, times and the controller hints (face buttons as four dots).
 * It shows for OSD_MS after a key or at the start, stays while paused, and
 * is redrawn once a second only while shown.
 * Replies OK / ERR <reason>. A new PLAY* while something plays replaces it
 * (the running session is stopped first, then the new one starts).
 *
 * Position: published when it changes discontinuously (start, seek, pause,
 * track change); clients extrapolate while state=playing. Nothing here
 * ticks: idle = poll() without timeout; a timer exists only while a stop
 * waits for mpv to quit.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <nuubos/notify.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef MPV_BIN
#define MPV_BIN "/usr/bin/mpv"
#endif
#ifndef USERDATA_USERS
#define USERDATA_USERS "/userdata/users"
#endif
#define SOCKET_PATH RUN_ROOT "/mediad.sock"
#define PIDFILE RUN_ROOT "/mediad.pid"
#define ACTIVE_USER_FILE RUN_ROOT "/user/active"
#define MPV_LOG RUN_ROOT "/mpv.log"
/* Exists while something plays (not paused): nuubos-systemd does not enter
 * idle Sleep then (music with the screen off, a film). */
#define PLAYING_MARK RUN_ROOT "/media-playing"
#define DISPLAYCTL "/usr/bin/nuubos-displayctl"

#define MAX_CLIENTS 16
#define MAX_LINE 4096
#define MAX_QUEUE 4000
#define QUIT_TIMEOUT_MS 5000
#define POSITION_TIMEOUT_MS 1000
/* PREV restarts the track when it played longer than this. */
#define PREV_RESTART_SECONDS 3.0
#define DEFAULT_MUSIC_VOLUME 80

#define OSD_MS 4500
/* inputd repeats a held direction every 90 ms after 350 ms. */
#define HOLD_MS 250
#define SEEK_STUCK_MS 2000
#define OSD_START_MS 3500

#define REQ_STOP_POSITION 100
#define REQ_POSITION 101

enum state { IDLE, PLAYING, PAUSED, STOPPING };
enum repeat { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };

struct track {
	char *path;
	char *title;
};

/* A list of tracks and the order they play in. */
struct queue {
	struct track *t;
	size_t n, cap;
	size_t *order;
	size_t pos;
	bool shuffle;
};

struct client {
	int fd;
	bool subscribed;
	char buf[MAX_LINE];
	size_t used;
	/* PLAYLIST being received: lines still expected. */
	long expect;
	long start;
	struct queue incoming;
};

/* A session to start once the running one has quit. */
struct pending {
	bool set;
	bool audio;
	char target[PATH_MAX];
	double seconds;
	struct queue q;
};

struct session {
	enum state state;
	pid_t pid;
	int ipc;
	char in[65536];
	size_t in_used;
	bool audio;
	char title[512];
	char artist[256];
	char album[256];
	char item[PATH_MAX];
	bool video;
	long index;
	long count;
	/* Position at pos_at_ms (monotonic); it runs on while playing. */
	double position;
	long long pos_at_ms;
	double duration;
	/* The track's format: codec name, sample rate, bits (0 = unknown). */
	char codec[32];
	long rate;
	int bits;
	bool started;
	int errors;
	/* Video OSD: shown until osd_until_ms (0 = hidden). */
	long long osd_until_ms;
	long long osd_tick_ms;
	bool osd_intro;
	/* Video at its end (--keep-open): held on the last frame. */
	bool eof;
	/* OSD size in pixels (its aspect gives the canvas width). */
	int osd_w, osd_h;
	/* Seeks: one at a time; keys arriving meanwhile add up. A held
	 * direction (repeats closer than HOLD_MS) seeks in growing steps. */
	bool seeking;
	long long seek_ms;
	double seek_accum;
	char last_key[24];
	long long last_key_ms;
	int held;
	char audio_track[128];
	char sub_track[128];
	bool has_sub;
	/* Decoded width and whether Cedrus frames are being copied (see
	 * hwdec_fit()). */
	int video_w;
	bool hwdec_copy;
	bool hwdec_settled;
	long long deadline_ms;
	bool term_sent;
	int power_client;
};

static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];
static struct session s = { .ipc = -1, .pid = -1, .power_client = -1, .deadline_ms = -1 };
static struct queue q;
static enum repeat repeat_mode = REPEAT_OFF;
static long queue_id;
static struct pending pending;
static int music_volume = -1;
/* OSD words and face positions (LABELS). */
static char osd_label[6][64] = { "Pause", "Play", "Subtitles", "Audio", "Stop", "Off" };
static int osd_pos[4] = { 2, 1, 0, 3 };
static char last_item[PATH_MAX];
static double last_position = -1;

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	fputs("mediad: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void on_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void on_sigchld(int sig)
{
	int saved = errno;
	char b = 1;

	(void)sig;
	(void)!write(sigchld_pipe[1], &b, 1);
	errno = saved;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void copy_text(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

/* Values for the line protocol: no tabs or line breaks. */
static void flatten(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

static void mkdir_p(const char *path)
{
	char tmp[PATH_MAX];

	copy_text(tmp, sizeof(tmp), path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

static bool read_user(char *out, size_t size)
{
	FILE *fp = fopen(ACTIVE_USER_FILE, "r");

	out[0] = '\0';
	if (!fp)
		return false;
	if (fgets(out, (int)size, fp))
		out[strcspn(out, "\r\n")] = '\0';
	fclose(fp);
	for (char *p = out; *p; p++)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-'))
			return false;
	return out[0] != '\0';
}

/* Bits per sample of a local FLAC file (STREAMINFO), 0 if unknown: mpv
 * decodes 24-bit FLAC to 32-bit samples and cannot tell them apart. */
static int flac_bits(const char *path)
{
	unsigned char h[4 + 4 + 34];
	FILE *fp = fopen(path, "rb");
	size_t n;

	if (!fp)
		return 0;
	n = fread(h, 1, sizeof(h), fp);
	fclose(fp);
	if (n != sizeof(h) || memcmp(h, "fLaC", 4) || (h[4] & 0x7f) != 0)
		return 0;
	/* STREAMINFO: 3 bits channels-1, then 5 bits bits-per-sample-1. */
	return (((h[8 + 12] & 0x01) << 4) | (h[8 + 13] >> 4)) + 1;
}

static void settings_path(char *out, size_t size)
{
	char user[64];

	out[0] = '\0';
	if (read_user(user, sizeof(user)))
		snprintf(out, size, USERDATA_USERS "/%s/appdata/media/settings.conf", user);
}

/* The active user's Music volume (read once per user switch is not
 * needed: every session start reads it). */
static int load_music_volume(void)
{
	char path[PATH_MAX], line[128];
	int v = DEFAULT_MUSIC_VOLUME;
	FILE *fp;

	settings_path(path, sizeof(path));
	if (path[0] && (fp = fopen(path, "r"))) {
		while (fgets(line, sizeof(line), fp))
			if (!strncmp(line, "MUSIC_VOLUME=", 13))
				v = atoi(line + 13);
		fclose(fp);
	}
	return v < 0 ? 0 : v > 100 ? 100 : v;
}

static bool save_music_volume(int v)
{
	char path[PATH_MAX], tmp[PATH_MAX + 16], dir[PATH_MAX];
	FILE *fp;

	settings_path(path, sizeof(path));
	if (!path[0])
		return false;
	copy_text(dir, sizeof(dir), path);
	*strrchr(dir, '/') = '\0';
	mkdir_p(dir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return false;
	fprintf(fp, "MUSIC_VOLUME=%d\n", v);
	/* fsync: /userdata is exFAT, an unsynced file renamed into place comes
	 * back empty after a crash. */
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0 || fclose(fp) != 0 ||
	    rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

static double current_position(void)
{
	if (s.state != PLAYING)
		return s.position;
	return s.position + (double)(now_ms() - s.pos_at_ms) / 1000.0;
}

static void set_position(double seconds)
{
	s.position = seconds < 0 ? 0 : seconds;
	s.pos_at_ms = now_ms();
}

/* Video goes to HDMI when it is connected (display policy), otherwise to
 * the built-in panel; asked from the display contract like Home does. */
static bool display_is_internal(void)
{
	char line[32] = "";
	FILE *fp = popen(DISPLAYCTL " hdmi 2>/dev/null", "r");

	if (!fp)
		return true;
	if (!fgets(line, sizeof(line), fp))
		line[0] = '\0';
	pclose(fp);
	return strncmp(line, "connected", 9) != 0;
}

static bool is_url(const char *s)
{
	return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8);
}

static bool audio_name(const char *name)
{
	static const char *const exts[] = { ".mp3", ".flac", ".ogg", ".opus", ".m4a", ".aac", ".wav", ".alac", NULL };
	size_t len = strlen(name);

	for (int i = 0; exts[i]; i++) {
		size_t e = strlen(exts[i]);

		if (len > e && !strcasecmp(name + len - e, exts[i]))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Queue                                                               */
/* ------------------------------------------------------------------ */

static void queue_clear(struct queue *x)
{
	for (size_t i = 0; i < x->n; i++) {
		free(x->t[i].path);
		free(x->t[i].title);
	}
	free(x->t);
	free(x->order);
	memset(x, 0, sizeof(*x));
}

static bool queue_add(struct queue *x, const char *path, const char *title)
{
	if (x->n >= MAX_QUEUE)
		return false;
	if (x->n == x->cap) {
		size_t cap = x->cap ? x->cap * 2 : 64;
		struct track *t = realloc(x->t, cap * sizeof(*t));

		if (!t)
			return false;
		x->t = t;
		x->cap = cap;
	}
	x->t[x->n].path = strdup(path);
	x->t[x->n].title = strdup(title ? title : "");
	if (!x->t[x->n].path || !x->t[x->n].title)
		abort();
	flatten(x->t[x->n].title);
	x->n++;
	return true;
}

static void queue_move(struct queue *dst, struct queue *src)
{
	queue_clear(dst);
	*dst = *src;
	memset(src, 0, sizeof(*src));
}

static size_t random_below(size_t n)
{
	return n ? (size_t)((unsigned long)random() % n) : 0;
}

/* Play order: identity, or shuffled with the track `first` played first. */
static void queue_order(struct queue *x, size_t first)
{
	free(x->order);
	x->order = malloc((x->n ? x->n : 1) * sizeof(*x->order));
	if (!x->order)
		abort();
	for (size_t i = 0; i < x->n; i++)
		x->order[i] = i;
	x->pos = first < x->n ? first : 0;
	if (x->shuffle && x->n > 1) {
		x->order[0] = x->pos;
		x->order[x->pos] = 0;
		for (size_t i = x->n - 1; i > 1; i--) {
			size_t j = 1 + random_below(i);
			size_t tmp = x->order[i];

			x->order[i] = x->order[j];
			x->order[j] = tmp;
		}
		x->pos = 0;
	}
	queue_id++;
}

static const struct track *current_track(void)
{
	return q.n && q.pos < q.n ? &q.t[q.order[q.pos]] : NULL;
}

static int name_cmp(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* The folder's audio files, sorted; *start = index of `first`. */
static bool queue_from_dir(struct queue *x, const char *dir, const char *first, size_t *start)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char **names = NULL, path[PATH_MAX];
	size_t n = 0, cap = 0;

	*start = 0;
	if (!d)
		return false;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || !audio_name(e->d_name) || strpbrk(e->d_name, "\t\r\n"))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			names = realloc(names, cap * sizeof(*names));
			if (!names)
				abort();
		}
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	if (n)
		qsort(names, n, sizeof(*names), name_cmp);
	for (size_t i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		if (!strcmp(names[i], first))
			*start = x->n;
		queue_add(x, path, "");
		free(names[i]);
	}
	free(names);
	return x->n > 0;
}

/* ------------------------------------------------------------------ */
/* Clients                                                             */
/* ------------------------------------------------------------------ */

static const char *state_name(void)
{
	switch (s.state) {
	case PLAYING: return "playing";
	case PAUSED: return "paused";
	case STOPPING: return "stopping";
	default: return "idle";
	}
}

static const char *repeat_name(void)
{
	return repeat_mode == REPEAT_ALL ? "all" : repeat_mode == REPEAT_ONE ? "one" : "off";
}

static size_t build_status(char *out, size_t size)
{
	bool idle = s.state == IDLE;
	int n = snprintf(out, size,
			 "state=%s\nkind=%s\ntitle=%s\nartist=%s\nalbum=%s\nitem=%s\nindex=%ld\ncount=%ld\n"
			 "position=%.1f\nduration=%.1f\nshuffle=%d\nrepeat=%s\nvolume=%d\n"
			 "codec=%s\nrate=%ld\nbits=%d\nqueue_id=%ld\n"
			 "last_item=%s\nlast_position=%.0f\nend=1\n",
			 state_name(), idle ? "" : (s.video ? "video" : "audio"),
			 idle ? "" : s.title, idle ? "" : s.artist, idle ? "" : s.album,
			 idle ? "" : s.item, s.index, s.count,
			 idle ? 0.0 : current_position(), idle ? 0.0 : s.duration,
			 q.shuffle ? 1 : 0, repeat_name(),
			 music_volume < 0 ? (music_volume = load_music_volume()) : music_volume,
			 idle ? "" : s.codec, idle ? 0 : s.rate, idle ? 0 : s.bits, queue_id,
			 last_item, last_position < 0 ? -1.0 : last_position);

	return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->subscribed = false;
	c->used = 0;
	c->expect = 0;
	queue_clear(&c->incoming);
}

static void reply(struct client *c, const char *text)
{
	if (c->fd >= 0 && send(c->fd, text, strlen(text), MSG_NOSIGNAL) < 0)
		close_client(c);
}

static void playing_mark(void)
{
	static int marked = -1;
	int now = s.state == PLAYING;

	if (now == marked)
		return;
	marked = now;
	if (now) {
		FILE *fp = fopen(PLAYING_MARK, "w");

		if (fp)
			fclose(fp);
	} else {
		unlink(PLAYING_MARK);
	}
}

static void publish(void)
{
	char buf[3 * PATH_MAX + 2048];
	size_t len;

	playing_mark();
	len = build_status(buf, sizeof(buf));

	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].subscribed &&
		    send(clients[i].fd, buf, len, MSG_NOSIGNAL) < 0)
			close_client(&clients[i]);
}

/* ------------------------------------------------------------------ */
/* mpv                                                                 */
/* ------------------------------------------------------------------ */

static void mpv_send(const char *json)
{
	size_t len = strlen(json);

	if (s.ipc < 0)
		return;
	if (write(s.ipc, json, len) != (ssize_t)len || write(s.ipc, "\n", 1) != 1)
		log_msg("ipc write failed: %s", strerror(errno));
}

static void mpv_command_id(cJSON *args, int request_id)
{
	cJSON *root = cJSON_CreateObject();
	char *text;

	cJSON_AddItemToObject(root, "command", args);
	if (request_id)
		cJSON_AddNumberToObject(root, "request_id", request_id);
	text = cJSON_PrintUnformatted(root);
	if (text)
		mpv_send(text);
	free(text);
	cJSON_Delete(root);
}

static void mpv_command(cJSON *args)
{
	mpv_command_id(args, 0);
}

static void observe(int id, const char *property)
{
	cJSON *a = cJSON_CreateArray();

	cJSON_AddItemToArray(a, cJSON_CreateString("observe_property"));
	cJSON_AddItemToArray(a, cJSON_CreateNumber(id));
	cJSON_AddItemToArray(a, cJSON_CreateString(property));
	mpv_command(a);
}

/*
 * Cedrus writes NV12 with a 32-aligned stride (the video engine reads its
 * reference frames that way, kernel 0109); Panfrost samples a plane of a
 * linear dma-buf (mpv's R8/GR88 import) only with a 64-byte aligned one.
 * Widths in between (1440, 1480...) were rejected and played as a blue
 * screen (GitHub #2). A video starts with the frames copied (any width,
 * ~20 % CPU at 1080p24, no dropped frames on the CubeXX) and moves to the
 * zero-copy import once its width allows it; the others stay copied. A
 * stride of 64 broke the engine's reference frames (streaks), and
 * Panfrost given 16 faulted (black video).
 */
static void hwdec_fit(void)
{
	if (!s.hwdec_copy || s.hwdec_settled || s.video_w <= 0)
		return;
	s.hwdec_settled = true;
	if (((s.video_w + 31) & ~31) % 64 != 0) {
		log_msg("width %d: hardware decoding with a frame copy", s.video_w);
		return;
	}
	mpv_send("{\"command\":[\"set_property\",\"hwdec\",\"v4l2request\"]}");
}

static void ask_position(void)
{
	mpv_send("{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":101}");
}

static void notify_failed(void)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "media", "media.failed");
	(void)nuubos_notify_send(&n);
}

/* ------------------------------------------------------------------ */
/* Video OSD                                                           */
/* ------------------------------------------------------------------ */

/* Text for ASS: no override braces, no backslashes, no line breaks. */
static void ass_text(char *out, size_t size, const char *in)
{
	size_t n = 0;

	for (; *in && n + 2 < size; in++) {
		char c = *in;

		if (c == '{' || c == '}' || c == '\\')
			c = c == '{' ? '(' : c == '}' ? ')' : '/';
		out[n++] = c == '\n' || c == '\t' ? ' ' : c;
	}
	out[n] = '\0';
}

static void appendf(char *buf, size_t size, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;

	if (len + 1 >= size)
		return;
	va_start(ap, fmt);
	vsnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);
}

/* A circle as an ASS drawing path. */
static void ass_circle(char *buf, size_t size, double cx, double cy, double r)
{
	double k = r * 0.5523;

	appendf(buf, size, "m %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f "
		"b %.1f %.1f %.1f %.1f %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f ",
		cx - r, cy, cx - r, cy - k, cx - k, cy - r, cx, cy - r,
		cx + k, cy - r, cx + r, cy - k, cx + r, cy,
		cx + r, cy + k, cx + k, cy + r, cx, cy + r,
		cx - k, cy + r, cx - r, cy + k, cx - r, cy);
}

/* A circle drawn the other way round: inside another one it cuts a hole
 * (libass fills by winding). */
static void ass_circle_rev(char *buf, size_t size, double cx, double cy, double r)
{
	double k = r * 0.5523;

	appendf(buf, size, "m %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f "
		"b %.1f %.1f %.1f %.1f %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f ",
		cx - r, cy, cx - r, cy + k, cx - k, cy + r, cx, cy + r,
		cx + k, cy + r, cx + r, cy + k, cx + r, cy,
		cx + r, cy - k, cx + k, cy - r, cx, cy - r,
		cx - k, cy - r, cx - r, cy - k, cx - r, cy);
}

#define ASS_SHAPE "{\\an7\\pos(0,0)\\bord0\\shad0\\1c&H%s&\\p1}"
#define INK_DIM "B4B4B4"
#define INK_ACCENT "FFA873"

/* Face buttons at (x, y), a 40 px box: three rings and the button to
 * press as a big accent dot (the dots of nuubUI's hint bar). */
static void ass_face(char *buf, size_t size, double x, double y, int pos)
{
	static const double at[4][2] = { { 20, 6 }, { 34, 20 }, { 20, 34 }, { 6, 20 } };

	appendf(buf, size, ASS_SHAPE, INK_DIM);
	for (int i = 0; i < 4; i++)
		if (i != pos) {
			ass_circle(buf, size, x + at[i][0], y + at[i][1], 5.5);
			ass_circle_rev(buf, size, x + at[i][0], y + at[i][1], 3.4);
		}
	appendf(buf, size, "{\\p0}\n" ASS_SHAPE, INK_ACCENT);
	ass_circle(buf, size, x + at[pos & 3][0], y + at[pos & 3][1], 7.5);
	appendf(buf, size, "{\\p0}\n");
}

/* D-pad at (x, y), 40 px: the arms that act in the accent colour. */
static void ass_dpad(char *buf, size_t size, double x, double y, bool horizontal)
{
	appendf(buf, size, ASS_SHAPE "m %.0f %.0f l %.0f %.0f l %.0f %.0f l %.0f %.0f "
		"m %.0f %.0f l %.0f %.0f l %.0f %.0f l %.0f %.0f{\\p0}\n", INK_DIM,
		x + 14, y + 14, x + 26, y + 14, x + 26, y + 26, x + 14, y + 26,
		horizontal ? x + 14 : x, horizontal ? y : y + 14, horizontal ? x + 26 : x + 40, horizontal ? y : y + 14,
		horizontal ? x + 26 : x + 40, horizontal ? y + 40 : y + 26, horizontal ? x + 14 : x, horizontal ? y + 40 : y + 26);
	if (horizontal)
		appendf(buf, size, ASS_SHAPE "m %.0f %.0f l %.0f %.0f l %.0f %.0f l %.0f %.0f "
			"m %.0f %.0f l %.0f %.0f l %.0f %.0f l %.0f %.0f{\\p0}\n", INK_ACCENT,
			x, y + 14, x + 13, y + 14, x + 13, y + 26, x, y + 26,
			x + 27, y + 14, x + 40, y + 14, x + 40, y + 26, x + 27, y + 26);
	else
		appendf(buf, size, ASS_SHAPE "m %.0f %.0f l %.0f %.0f l %.0f %.0f l %.0f %.0f "
			"m %.0f %.0f l %.0f %.0f l %.0f %.0f l %.0f %.0f{\\p0}\n", INK_ACCENT,
			x + 14, y, x + 26, y, x + 26, y + 13, x + 14, y + 13,
			x + 14, y + 27, x + 26, y + 27, x + 26, y + 40, x + 14, y + 40);
}

/* A hint word smaller when it would run into the next cell (long
 * translations on square screens): ~0.5 em per character. */
static void ass_label(char *buf, size_t size, double x, double y, int fs, double room,
		      const char *text)
{
	size_t chars = 0;

	for (const char *p = text; *p; p++)
		chars += ((unsigned char)*p & 0xC0) != 0x80;
	if (chars > 0 && chars * 0.5 * fs > room)
		fs = (int)(room / (chars * 0.5));
	if (fs < 18)
		fs = 18;
	appendf(buf, size, "{\\an4\\pos(%.0f,%.0f)\\fs%d\\bord0\\shad0\\1c&HFFFFFF&\\q2}%s\n", x, y, fs, text);
}

static void clock_text(char *out, size_t size, double t)
{
	long v = t < 0 ? 0 : (long)t;

	if (v >= 3600)
		snprintf(out, size, "%ld:%02ld:%02ld", v / 3600, (v / 60) % 60, v % 60);
	else
		snprintf(out, size, "%ld:%02ld", v / 60, v % 60);
}

static void osd_hide(void)
{
	if (!s.osd_until_ms)
		return;
	s.osd_until_ms = 0;
	mpv_send("{\"command\":{\"name\":\"osd-overlay\",\"id\":1,\"format\":\"none\",\"data\":\"\"}}");
}

static void osd_draw(void)
{
	char ev[8192] = "", title[600], a[160], sub[160], t1[16], t2[16];
	double pos = current_position(), dur = s.duration;
	double frac = dur > 0 ? pos / dur : 0;
	/* The virtual canvas is 720 high and as wide as the screen's aspect:
	 * a fixed 1280x720 was stretched on square screens (CubeXX). */
	double W = s.osd_w > 0 && s.osd_h > 0 ? 720.0 * s.osd_w / s.osd_h : 1280;
	bool narrow = W < 1000;
	double m = narrow ? 36 : 56, cell = (W - 2 * m) / 3;
	int fs = narrow ? 28 : 34;
	cJSON *cmd, *root;
	char *text;

	if (frac > 1)
		frac = 1;
	ass_text(title, sizeof(title), s.title);
	ass_text(a, sizeof(a), s.audio_track);
	ass_text(sub, sizeof(sub), s.has_sub ? s.sub_track : osd_label[5]);
	clock_text(t1, sizeof(t1), pos);
	clock_text(t2, sizeof(t2), dur);
	/* Shade behind the controls (two steps: a soft edge). */
	appendf(ev, sizeof(ev), "{\\an7\\pos(0,0)\\bord0\\shad0\\1c&H000000&\\1a&HB0&\\p1}m 0 %d l %.0f %d l %.0f 720 l 0 720{\\p0}\n",
		narrow ? 340 : 380, W, narrow ? 340 : 380, W);
	appendf(ev, sizeof(ev), "{\\an7\\pos(0,0)\\bord0\\shad0\\1c&H000000&\\1a&H50&\\p1}m 0 %d l %.0f %d l %.0f 720 l 0 720{\\p0}\n",
		narrow ? 390 : 430, W, narrow ? 390 : 430, W);
	/* Paused: a big pause sign in the middle. */
	if (s.state == PAUSED || s.eof)
		appendf(ev, sizeof(ev), "{\\an7\\pos(0,0)\\bord0\\shad0\\1c&HFFFFFF&\\1a&H30&\\p1}"
			"m %.0f 200 l %.0f 200 l %.0f 300 l %.0f 300 m %.0f 200 l %.0f 200 l %.0f 300 l %.0f 300{\\p0}\n",
			W / 2 - 44, W / 2 - 14, W / 2 - 14, W / 2 - 44, W / 2 + 14, W / 2 + 44, W / 2 + 44, W / 2 + 14);
	/* Title (wraps upwards when long), audio/subtitles beside it or, on
	 * narrow screens, on the times line. */
	appendf(ev, sizeof(ev), "{\\an1\\pos(%.0f,%d)\\fs%d\\b1\\bord0\\shad1\\1c&HFFFFFF&\\q0}%s\n",
		m, 506, narrow ? 38 : 44, title);
	if (narrow)
		appendf(ev, sizeof(ev), "{\\an8\\pos(%.0f,536)\\fs24\\bord0\\shad0\\1c&HC8C8C8&\\q2}%s: %s   %s: %s\n",
			W / 2, osd_label[3], a[0] ? a : "-", osd_label[2], sub);
	else
		appendf(ev, sizeof(ev), "{\\an3\\pos(%.0f,504)\\fs28\\bord0\\shad0\\1c&HC8C8C8&\\q2}%s: %s   %s: %s\n",
			W - m, osd_label[3], a[0] ? a : "-", osd_label[2], sub);
	/* Progress. */
	appendf(ev, sizeof(ev), "{\\an7\\pos(0,0)\\bord0\\shad0\\1c&HFFFFFF&\\1a&HA0&\\p1}m %.0f 520 l %.0f 520 l %.0f 528 l %.0f 528{\\p0}\n",
		m, W - m, W - m, m);
	appendf(ev, sizeof(ev), "{\\an7\\pos(0,0)\\bord0\\shad0\\1c&HFFA873&\\p1}m %.0f 520 l %.0f 520 l %.0f 528 l %.0f 528{\\p0}\n",
		m, m + frac * (W - 2 * m), m + frac * (W - 2 * m), m);
	appendf(ev, sizeof(ev), "{\\an7\\pos(%.0f,536)\\fs28\\bord0\\shad0\\1c&HE6E6E6&}%s\n", m, t1);
	appendf(ev, sizeof(ev), "{\\an9\\pos(%.0f,536)\\fs28\\bord0\\shad0\\1c&HE6E6E6&}%s\n", W - m, t2);
	/* Controller hints: two rows of three cells, icon then word (one row
	 * of six was too crowded, user report 2026-10-13). */
	{
		const double y1 = 590, y2 = 650;
		const char *play = s.eof || s.state == PAUSED ? osd_label[1] : osd_label[0];

		ass_face(ev, sizeof(ev), m, y1, osd_pos[0]);
		ass_label(ev, sizeof(ev), m + 50, y1 + 20, fs, cell - 58, play);
		ass_dpad(ev, sizeof(ev), m + cell, y1, true);
		ass_label(ev, sizeof(ev), m + cell + 50, y1 + 20, fs, cell - 58, "±1 s");
		ass_dpad(ev, sizeof(ev), m + 2 * cell, y1, false);
		ass_label(ev, sizeof(ev), m + 2 * cell + 50, y1 + 20, fs, cell - 58, "±10 s");
		ass_face(ev, sizeof(ev), m, y2, osd_pos[2]);
		ass_label(ev, sizeof(ev), m + 50, y2 + 20, fs, cell - 58, osd_label[2]);
		ass_face(ev, sizeof(ev), m + cell, y2, osd_pos[3]);
		ass_label(ev, sizeof(ev), m + cell + 50, y2 + 20, fs, cell - 58, osd_label[3]);
		ass_face(ev, sizeof(ev), m + 2 * cell, y2, osd_pos[1]);
		ass_label(ev, sizeof(ev), m + 2 * cell + 50, y2 + 20, fs, cell - 58, osd_label[4]);
	}

	root = cJSON_CreateObject();
	cmd = cJSON_CreateObject();
	cJSON_AddStringToObject(cmd, "name", "osd-overlay");
	cJSON_AddNumberToObject(cmd, "id", 1);
	cJSON_AddStringToObject(cmd, "format", "ass-events");
	cJSON_AddStringToObject(cmd, "data", ev);
	cJSON_AddNumberToObject(cmd, "res_x", (int)W);
	cJSON_AddNumberToObject(cmd, "res_y", 720);
	cJSON_AddItemToObject(root, "command", cmd);
	text = cJSON_PrintUnformatted(root);
	if (text)
		mpv_send(text);
	free(text);
	cJSON_Delete(root);
	s.osd_tick_ms = now_ms() + 1000;
}

static void osd_show(long long ms)
{
	if (s.audio || (s.state != PLAYING && s.state != PAUSED))
		return;
	s.osd_until_ms = now_ms() + ms;
	osd_draw();
}

/* Timer: refresh the shown OSD once a second, hide it when it expires
 * (never while paused). */
static void osd_timer(void)
{
	long long now = now_ms();

	if (!s.osd_until_ms || s.audio)
		return;
	if (s.state == PAUSED || s.eof)
		s.osd_until_ms = now + OSD_MS;
	if (now >= s.osd_until_ms)
		osd_hide();
	else if (now >= s.osd_tick_ms)
		osd_draw();
}

static long long osd_deadline(void)
{
	if (!s.osd_until_ms)
		return -1;
	return s.osd_tick_ms < s.osd_until_ms ? s.osd_tick_ms : s.osd_until_ms;
}

static void track_name(char *out, size_t size, const cJSON *data, const char *fallback)
{
	copy_text(out, size, cJSON_IsString(data) && data->valuestring[0] ? data->valuestring : fallback);
	flatten(out);
}

/* A relative seek, one at a time (playback-restart ends it). exact: a
 * single short step lands precisely; held steps go to keyframes (fast,
 * light on the Cedrus decoder). */
static void seek_by(double d, bool exact)
{
	cJSON *a;

	if (s.seeking && now_ms() - s.seek_ms < SEEK_STUCK_MS) {
		s.seek_accum += d;
		return;
	}
	a = cJSON_CreateArray();
	cJSON_AddItemToArray(a, cJSON_CreateString("seek"));
	cJSON_AddItemToArray(a, cJSON_CreateNumber(d));
	cJSON_AddItemToArray(a, cJSON_CreateString(exact ? "relative+exact" : "relative+keyframes"));
	mpv_command(a);
	s.seeking = true;
	s.seek_ms = now_ms();
}

/* Audio: load the track at q.pos (mpv stays idle between tracks). The
 * title, when the caller gave one (a server stream), is forced; mpv's
 * %n% quoting keeps commas and equals signs in it. */
static void load_current(void)
{
	const struct track *t = current_track();
	cJSON *a;

	if (!t)
		return;
	s.index = (long)q.pos;
	s.count = (long)q.n;
	set_position(0);
	s.duration = 0;
	s.artist[0] = s.album[0] = s.codec[0] = '\0';
	s.rate = 0;
	s.bits = is_url(t->path) ? 0 : flac_bits(t->path);
	copy_text(s.item, sizeof(s.item), t->path);
	flatten(s.item);
	a = cJSON_CreateArray();
	cJSON_AddItemToArray(a, cJSON_CreateString("loadfile"));
	cJSON_AddItemToArray(a, cJSON_CreateString(t->path));
	cJSON_AddItemToArray(a, cJSON_CreateString("replace"));
	if (t->title[0]) {
		char opt[700];

		snprintf(opt, sizeof(opt), "force-media-title=%%%zu%%%s", strlen(t->title), t->title);
		cJSON_AddItemToArray(a, cJSON_CreateNumber(-1));
		cJSON_AddItemToArray(a, cJSON_CreateString(opt));
		copy_text(s.title, sizeof(s.title), t->title);
	} else {
		const char *slash = strrchr(t->path, '/');

		copy_text(s.title, sizeof(s.title), slash ? slash + 1 : t->path);
	}
	if (s.state == PAUSED) {
		/* A track chosen while paused plays. */
		mpv_send("{\"command\":[\"set_property\",\"pause\",false]}");
	}
	mpv_command(a);
	publish();
}

static void finish_audio(void)
{
	/* End of the queue: quit mpv; the session ends when it exits. */
	s.state = STOPPING;
	mpv_send("{\"command\":[\"quit\"]}");
	s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
	publish();
}

/* Audio end-of-file / error: the next track per repeat mode. */
static void track_ended(bool error)
{
	if (!s.audio || s.state == STOPPING || s.state == IDLE)
		return;
	if (error) {
		if (++s.errors >= (int)q.n || s.errors >= 20) {
			log_msg("no playable track");
			notify_failed();
			finish_audio();
			return;
		}
	} else {
		s.errors = 0;
		if (repeat_mode == REPEAT_ONE) {
			load_current();
			return;
		}
	}
	if (q.pos + 1 < q.n) {
		q.pos++;
	} else if (repeat_mode != REPEAT_OFF) {
		if (q.shuffle)
			queue_order(&q, q.order[random_below(q.n)]);
		else
			q.pos = 0;
	} else {
		finish_audio();
		return;
	}
	load_current();
}

static void metadata_tags(const cJSON *data)
{
	const cJSON *it;

	s.artist[0] = s.album[0] = '\0';
	if (!cJSON_IsObject(data))
		return;
	cJSON_ArrayForEach(it, data) {
		if (!cJSON_IsString(it) || !it->string)
			continue;
		if (!strcasecmp(it->string, "artist") || (!s.artist[0] && !strcasecmp(it->string, "album_artist")))
			copy_text(s.artist, sizeof(s.artist), it->valuestring);
		else if (!strcasecmp(it->string, "album"))
			copy_text(s.album, sizeof(s.album), it->valuestring);
	}
	flatten(s.artist);
	flatten(s.album);
}

static void handle_mpv_line(const char *line)
{
	cJSON *msg = cJSON_Parse(line);
	const cJSON *ev, *name, *data, *id;
	bool changed = false;

	if (!msg)
		return;
	ev = cJSON_GetObjectItemCaseSensitive(msg, "event");
	id = cJSON_GetObjectItemCaseSensitive(msg, "request_id");
	data = cJSON_GetObjectItemCaseSensitive(msg, "data");
	if (cJSON_IsNumber(id) && id->valueint == REQ_STOP_POSITION) {
		/* Position asked by STOP: publish it, then quit. */
		if (cJSON_IsNumber(data)) {
			last_position = data->valuedouble;
			copy_text(last_item, sizeof(last_item), s.item);
		}
		mpv_send("{\"command\":[\"quit-watch-later\"]}");
		s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
	} else if (cJSON_IsNumber(id) && id->valueint == REQ_POSITION) {
		if (cJSON_IsNumber(data) && s.state != STOPPING) {
			set_position(data->valuedouble);
			if (s.osd_until_ms)
				osd_draw();
			changed = true;
		}
	} else if (cJSON_IsString(ev) && !strcmp(ev->valuestring, "playback-restart")) {
		/* Started or seeked: where it is now. The first start of a video
		 * shows the controls for a moment, so the user knows they exist. */
		if (!s.osd_intro && !s.audio) {
			s.osd_intro = true;
			osd_show(OSD_START_MS);
		}
		s.seeking = false;
		if (s.seek_accum != 0) {
			double d = s.seek_accum;

			s.seek_accum = 0;
			seek_by(d, false);
		}
		s.started = true;
		ask_position();
	} else if (cJSON_IsString(ev) && !strcmp(ev->valuestring, "end-file")) {
		const cJSON *reason = cJSON_GetObjectItemCaseSensitive(msg, "reason");

		if (cJSON_IsString(reason) && !strcmp(reason->valuestring, "eof"))
			track_ended(false);
		else if (cJSON_IsString(reason) && !strcmp(reason->valuestring, "error"))
			track_ended(true);
	} else if (cJSON_IsString(ev) && !strcmp(ev->valuestring, "property-change")) {
		name = cJSON_GetObjectItemCaseSensitive(msg, "name");
		if (!cJSON_IsString(name)) {
			cJSON_Delete(msg);
			return;
		}
		if (!strcmp(name->valuestring, "pause") && cJSON_IsBool(data) && s.state != STOPPING) {
			/* Freeze or restart the running position at the switch. */
			set_position(current_position());
			s.state = cJSON_IsTrue(data) ? PAUSED : PLAYING;
			ask_position();
			if (!s.audio && s.state == PAUSED)
				osd_show(OSD_MS);
			else if (s.osd_until_ms)
				osd_draw();
			changed = true;
		} else if (!strcmp(name->valuestring, "media-title") && cJSON_IsString(data)) {
			copy_text(s.title, sizeof(s.title), data->valuestring);
			flatten(s.title);
			changed = true;
		} else if (!strcmp(name->valuestring, "metadata")) {
			metadata_tags(data);
			changed = true;
		} else if (!strcmp(name->valuestring, "audio-codec-name")) {
			copy_text(s.codec, sizeof(s.codec), cJSON_IsString(data) ? data->valuestring : "");
			flatten(s.codec);
			changed = true;
		} else if (!strcmp(name->valuestring, "audio-params/samplerate")) {
			s.rate = cJSON_IsNumber(data) ? (long)data->valuedouble : 0;
			changed = true;
		} else if (!strcmp(name->valuestring, "duration")) {
			s.duration = cJSON_IsNumber(data) ? data->valuedouble : 0;
			changed = true;
		} else if (!strcmp(name->valuestring, "osd-width") || !strcmp(name->valuestring, "osd-height")) {
			int v = cJSON_IsNumber(data) ? data->valueint : 0;

			if (name->valuestring[4] == 'w')
				s.osd_w = v;
			else
				s.osd_h = v;
			if (s.osd_until_ms)
				osd_draw();
		} else if (!strcmp(name->valuestring, "video-params/w")) {
			s.video_w = cJSON_IsNumber(data) ? data->valueint : 0;
			hwdec_fit();
		} else if (!strcmp(name->valuestring, "hwdec-current")) {
			s.hwdec_copy = cJSON_IsString(data) && !strcmp(data->valuestring, "v4l2request-copy");
			hwdec_fit();
		} else if (!strcmp(name->valuestring, "eof-reached")) {
			s.eof = cJSON_IsTrue(data);
			if (s.eof && !s.audio)
				osd_show(OSD_MS);
		} else if (!strcmp(name->valuestring, "current-tracks/audio/lang") ||
			   !strcmp(name->valuestring, "current-tracks/audio/title")) {
			/* Language first; a title only when there is no language. */
			if (name->valuestring[21] == 'l' || !s.audio_track[0])
				track_name(s.audio_track, sizeof(s.audio_track), data, s.audio_track);
			if (s.osd_until_ms)
				osd_draw();
		} else if (!strcmp(name->valuestring, "current-tracks/sub/lang") ||
			   !strcmp(name->valuestring, "current-tracks/sub/title")) {
			if (cJSON_IsNull(data) || !data) {
				if (name->valuestring[19] == 'l')
					s.has_sub = false, s.sub_track[0] = '\0';
			} else {
				s.has_sub = true;
				if (name->valuestring[19] == 'l' || !s.sub_track[0])
					track_name(s.sub_track, sizeof(s.sub_track), data, "#");
			}
			if (s.osd_until_ms)
				osd_draw();
		} else if (s.audio) {
			/* The queue is ours: mpv's playlist and path say nothing new. */
		} else if (!strcmp(name->valuestring, "path") && cJSON_IsString(data)) {
			copy_text(s.item, sizeof(s.item), data->valuestring);
			flatten(s.item);
			s.started = true;
			changed = true;
		} else if (!strcmp(name->valuestring, "playlist-pos") && cJSON_IsNumber(data)) {
			s.index = (long)data->valuedouble;
			changed = true;
		} else if (!strcmp(name->valuestring, "playlist-count") && cJSON_IsNumber(data)) {
			s.count = (long)data->valuedouble;
			changed = true;
		} else if (!strcmp(name->valuestring, "vid")) {
			/* Album art counts as audio: only a real video track is video. */
			s.video = cJSON_IsNumber(data) || (cJSON_IsString(data) && strcmp(data->valuestring, "no"));
			changed = true;
		}
	}
	cJSON_Delete(msg);
	if (changed)
		publish();
}

static void process_ipc(void)
{
	ssize_t n = read(s.ipc, s.in + s.in_used, sizeof(s.in) - s.in_used - 1);
	char *nl;

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close(s.ipc);
		s.ipc = -1;
		return;
	}
	s.in_used += (size_t)n;
	s.in[s.in_used] = '\0';
	while ((nl = strchr(s.in, '\n'))) {
		*nl = '\0';
		handle_mpv_line(s.in);
		memmove(s.in, nl + 1, s.in_used - (size_t)(nl + 1 - s.in) + 1);
		s.in_used -= (size_t)(nl + 1 - s.in);
	}
	if (s.in_used >= sizeof(s.in) - 1)
		s.in_used = 0;
}

/* video: `target` played fullscreen from `start_seconds`; audio: the queue
 * already in `q`, no window. */
/* start_seconds < 0: no --start (mpv resumes from watch-later). */
static const char *start_mpv(bool audio, const char *target, double start_seconds)
{
	char user[64], home[PATH_MAX], later[PATH_MAX], arg_later[PATH_MAX + 32];
	char arg_start[64], arg_volume[32];
	bool video_top;
	int sv[2];
	pid_t pid;

	if (s.state != IDLE)
		return "busy";
	if (!read_user(user, sizeof(user)))
		return "no-user";
	snprintf(home, sizeof(home), USERDATA_USERS "/%s/appdata/media", user);
	snprintf(later, sizeof(later), "%s/watch-later", home);
	mkdir_p(later);
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
		return "spawn";
	snprintf(arg_later, sizeof(arg_later), "--watch-later-dir=%s", later);
	snprintf(arg_start, sizeof(arg_start), "--start=%.0f", start_seconds);
	music_volume = load_music_volume();
	snprintf(arg_volume, sizeof(arg_volume), "--volume=%d", music_volume);
	video_top = display_is_internal();
	pid = fork();
	if (pid < 0) {
		close(sv[0]);
		close(sv[1]);
		return "spawn";
	}
	if (pid == 0) {
		char *argv[40];
		int argc = 0, log;
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		dup2(sv[1], 3);
		unlink(MPV_LOG);
		log = open("/dev/null", O_WRONLY);
		if (log >= 0) {
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
		}
		clearenv();
		setenv("PATH", "/usr/bin:/bin", 1);
		setenv("HOME", home, 1);
		setenv("XDG_RUNTIME_DIR", RUN_ROOT "/wayland-runtime", 1);
		setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		setenv("PIPEWIRE_RUNTIME_DIR", RUN_ROOT "/pipewire", 1);
		/* mpv has no PipeWire output here: it plays through the ALSA
		 * PipeWire plugin, which takes the stream name from this. */
		if (audio)
			setenv("PIPEWIRE_PROPS", "{ application.name = \"nuubOS Music\" media.role = Music }", 1);
		argv[argc++] = (char *)MPV_BIN;
		argv[argc++] = (char *)"--input-ipc-client=fd://3";
		argv[argc++] = (char *)"--no-config";
		argv[argc++] = (char *)"--no-terminal";
		/* mpv's own log (--no-terminal silences stderr): the cause of a
		 * player crash (seen once 2026-10-13, status 11) stays readable. */
		argv[argc++] = (char *)"--log-file=" MPV_LOG;
		argv[argc++] = (char *)"--ao=pipewire,alsa";
		argv[argc++] = (char *)"--input-default-bindings=no";
		argv[argc++] = (char *)"--input-vo-keyboard=no";
		if (audio) {
			argv[argc++] = (char *)"--idle=yes";
			argv[argc++] = (char *)"--audio-client-name=nuubOS Music";
			argv[argc++] = arg_volume;
			argv[argc++] = (char *)"--force-window=no";
			argv[argc++] = (char *)"--vid=no";
			argv[argc++] = (char *)"--audio-display=no";
			argv[argc++] = (char *)"--save-position-on-quit=no";
			argv[argc++] = (char *)"--resume-playback=no";
			argv[argc++] = (char *)"--gapless-audio=weak";
		} else {
			argv[argc++] = (char *)"--fullscreen";
			argv[argc++] = (char *)"--force-window=yes";
			/* --hwdec=drm is not a hwdec for the v4l2request hwaccel
			 * (software decoding, and dmabuf-wayland cannot show software
			 * frames: black video, found 2026-10-12). */
			/* Copied until the width is known (hwdec_fit()). */
			argv[argc++] = (char *)"--hwdec=v4l2request-copy";
			argv[argc++] = (char *)"--vo=gpu";
			argv[argc++] = (char *)"--gpu-context=wayland";
			/* Bilinear scaling, no dither or linear-light passes: the
			 * default scalers cost the Mali-G31 frames (1080p24 H.264
			 * on the CubeXX dropped ~5 frames/s in hardware, ~12/s for
			 * software-decoded 10-bit; with this, none; GitHub #2). */
			argv[argc++] = (char *)"--profile=fast";
			/* Full native resolution on HDMI (wlroots 0002,
			 * NUUBOS_CLIENT_FULL_SCALE_APP_IDS in nuubos-ui-launch). */
			argv[argc++] = (char *)"--wayland-app-id=nuubos-video";
			/* Built-in panel: a letterboxed video sits at the top,
			 * the room below it is free (user request 2026-10-14). */
			if (video_top)
				argv[argc++] = (char *)"--video-align-y=-1";
			argv[argc++] = (char *)"--osd-level=1";
			argv[argc++] = (char *)"--save-position-on-quit=yes";
			argv[argc++] = (char *)"--resume-playback=yes";
			argv[argc++] = (char *)"--write-filename-in-watch-later-config=yes";
			argv[argc++] = arg_later;
			argv[argc++] = (char *)"--sub-auto=fuzzy";
			argv[argc++] = (char *)"--idle=no";
			/* The end of a video does not close it (user request
			 * 2026-10-13): last frame, controls shown, Back closes. */
			argv[argc++] = (char *)"--keep-open=yes";
			if (start_seconds >= 0)
				argv[argc++] = arg_start;
			argv[argc++] = (char *)"--";
			argv[argc++] = (char *)target;
		}
		argv[argc] = NULL;
		execv(MPV_BIN, argv);
		_exit(127);
	}
	close(sv[1]);
	s.pid = pid;
	s.ipc = sv[0];
	(void)fcntl(s.ipc, F_SETFL, O_NONBLOCK);
	s.state = PLAYING;
	s.audio = audio;
	s.in_used = 0;
	s.title[0] = s.item[0] = s.artist[0] = s.album[0] = s.codec[0] = '\0';
	s.rate = 0;
	s.bits = 0;
	s.osd_until_ms = 0;
	s.osd_intro = false;
	s.eof = false;
	s.audio_track[0] = s.sub_track[0] = '\0';
	s.has_sub = false;
	s.video_w = 0;
	s.hwdec_copy = s.hwdec_settled = false;
	s.video = false;
	s.index = 0;
	s.count = 1;
	set_position(0);
	s.duration = 0;
	s.started = false;
	s.errors = 0;
	s.term_sent = false;
	s.deadline_ms = -1;
	last_item[0] = '\0';
	last_position = -1;
	observe(1, "pause");
	observe(2, "media-title");
	observe(3, "duration");
	observe(4, "metadata");
	observe(9, "audio-codec-name");
	observe(10, "audio-params/samplerate");
	if (!audio) {
		observe(5, "path");
		observe(6, "playlist-pos");
		observe(7, "playlist-count");
		observe(8, "vid");
		observe(11, "current-tracks/audio/lang");
		observe(15, "eof-reached");
		observe(16, "osd-width");
		observe(17, "osd-height");
		observe(12, "current-tracks/sub/lang");
		observe(13, "current-tracks/audio/title");
		observe(14, "current-tracks/sub/title");
		observe(18, "video-params/w");
		observe(19, "hwdec-current");
	}
	log_msg("playing %s (pid %d)", audio ? "music" : target, (int)pid);
	if (audio)
		load_current();
	else
		publish();
	return NULL;
}

static void stop_session(void)
{
	if (s.state == IDLE || s.state == STOPPING)
		return;
	osd_hide();
	s.state = STOPPING;
	if (s.audio) {
		/* Nothing to resume for music. */
		mpv_send("{\"command\":[\"quit\"]}");
		s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
		last_position = 0;
	} else if (s.eof) {
		/* Watched to the end: nothing to resume. */
		mpv_send("{\"command\":[\"delete-watch-later-config\"]}");
		mpv_send("{\"command\":[\"quit\"]}");
		s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
		last_position = 0;
		copy_text(last_item, sizeof(last_item), s.item);
	} else {
		mpv_send("{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":100}");
		/* No answer (nothing playing yet): quit anyway after a short wait. */
		s.deadline_ms = now_ms() + POSITION_TIMEOUT_MS;
	}
	publish();
}

/* Starts now, or after the running session has quit. */
static const char *request_start(bool audio, const char *target, double seconds, struct queue *src)
{
	if (s.state == IDLE) {
		if (audio)
			queue_move(&q, src);
		return start_mpv(audio, target, seconds);
	}
	pending.set = true;
	pending.audio = audio;
	copy_text(pending.target, sizeof(pending.target), target);
	pending.seconds = seconds;
	if (audio)
		queue_move(&pending.q, src);
	stop_session();
	return NULL;
}

static void child_exited(int status)
{
	bool failed = !s.started || (WIFEXITED(status) && WEXITSTATUS(status) != 0 &&
				     WEXITSTATUS(status) != 4) || WIFSIGNALED(status);

	log_msg("mpv exited status=%d", status);
	if (failed && s.state != STOPPING)
		notify_failed();
	if (s.ipc >= 0)
		close(s.ipc);
	s.ipc = -1;
	s.pid = -1;
	s.state = IDLE;
	s.deadline_ms = -1;
	if (s.power_client >= 0) {
		pending.set = false;
		publish();
		reply(&clients[s.power_client], "OK\n");
		s.power_client = -1;
		return;
	}
	if (pending.set) {
		const char *err;

		pending.set = false;
		if (pending.audio)
			queue_move(&q, &pending.q);
		err = start_mpv(pending.audio, pending.target, pending.seconds);
		if (!err)
			return;
		log_msg("pending start failed: %s", err);
		notify_failed();
	}
	publish();
}

static void timers(void)
{
	if (s.pid <= 0)
		s.deadline_ms = -1;
	if (s.deadline_ms < 0 || now_ms() < s.deadline_ms)
		return;
	if (s.state == STOPPING && !s.term_sent && s.ipc >= 0 && last_position < 0) {
		/* Position never came: ask mpv to quit normally. */
		mpv_send("{\"command\":[\"quit-watch-later\"]}");
		s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
		s.term_sent = false;
		last_position = 0;
		return;
	}
	if (!s.term_sent) {
		kill(s.pid, SIGTERM);
		s.term_sent = true;
		s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
	} else {
		kill(-s.pid, SIGKILL);
		s.deadline_ms = -1;
	}
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static bool playing(struct client *c)
{
	if (s.state == PLAYING || s.state == PAUSED)
		return true;
	reply(c, "ERR no-session\n");
	return false;
}

static void reply_result(struct client *c, const char *err)
{
	char line[64];

	snprintf(line, sizeof(line), err ? "ERR %s\n" : "OK\n", err);
	reply(c, line);
}

static bool valid_source(const char *p)
{
	return is_url(p) || (p[0] == '/' && access(p, R_OK) == 0);
}

/* One line of a PLAYLIST body. */
static void playlist_line(struct client *c, char *line)
{
	char *title = strchr(line, '\t');

	if (title)
		*title++ = '\0';
	if (line[0] && valid_source(line))
		queue_add(&c->incoming, line, title);
	if (--c->expect > 0)
		return;
	if (!c->incoming.n) {
		queue_clear(&c->incoming);
		reply(c, "ERR file\n");
		return;
	}
	queue_order(&c->incoming, c->start >= 0 && (size_t)c->start < c->incoming.n ? (size_t)c->start : 0);
	reply_result(c, request_start(true, "", 0, &c->incoming));
}

static void send_queue(struct client *c)
{
	char line[PATH_MAX + 700];

	for (size_t i = 0; i < q.n && c->fd >= 0 && s.audio && s.state != IDLE; i++) {
		const struct track *t = &q.t[q.order[i]];

		snprintf(line, sizeof(line), "track=%zu\t%s\t%s\n", i, t->path, t->title);
		reply(c, line);
	}
	reply(c, "end=1\n");
}

static void handle(struct client *c, char *line)
{
	char *arg = strchr(line, '\t'), *arg2 = NULL, *arg3 = NULL;
	char status[3 * PATH_MAX + 2048];

	line[strcspn(line, "\r")] = '\0';
	if (c->expect > 0) {
		playlist_line(c, line);
		return;
	}
	if (arg) {
		*arg++ = '\0';
		if ((arg2 = strchr(arg, '\t'))) {
			*arg2++ = '\0';
			if ((arg3 = strchr(arg2, '\t')))
				*arg3++ = '\0';
		}
	}
	if (!strcmp(line, "STATUS") || !strcmp(line, "SUBSCRIBE")) {
		size_t len = build_status(status, sizeof(status));

		c->subscribed = c->subscribed || line[1] == 'U';
		if (send(c->fd, status, len, MSG_NOSIGNAL) < 0)
			close_client(c);
	} else if (!strcmp(line, "PLAY") && arg && arg[0]) {
		bool audio = (arg3 && !strcmp(arg3, "audio")) || (!is_url(arg) && audio_name(arg));

		if (!valid_source(arg)) {
			reply(c, "ERR file\n");
			return;
		}
		if (audio) {
			struct queue one = { 0 };

			queue_add(&one, arg, "");
			queue_order(&one, 0);
			reply_result(c, request_start(true, "", 0, &one));
		} else {
			/* An explicit start (0 = from the beginning) overrides the
			 * saved position. */
			reply_result(c, request_start(false, arg, arg2 && arg2[0] ? atof(arg2) : -1, NULL));
		}
	} else if (!strcmp(line, "PLAYDIR") && arg && arg2) {
		struct queue dir = { 0 };
		size_t start;

		if (arg[0] != '/' || strchr(arg2, '/') || !queue_from_dir(&dir, arg, arg2, &start)) {
			queue_clear(&dir);
			reply(c, "ERR file\n");
			return;
		}
		queue_order(&dir, start);
		reply_result(c, request_start(true, "", 0, &dir));
	} else if (!strcmp(line, "PLAYLIST") && arg && arg2) {
		long count = atol(arg2);

		if (count <= 0 || count > MAX_QUEUE) {
			reply(c, "ERR value\n");
			return;
		}
		queue_clear(&c->incoming);
		c->incoming.shuffle = arg3 && !strcmp(arg3, "1");
		c->start = atol(arg);
		c->expect = count;
	} else if (!strcmp(line, "QUEUE")) {
		send_queue(c);
	} else if (!strcmp(line, "PAUSE")) {
		if (!playing(c))
			return;
		mpv_send("{\"command\":[\"cycle\",\"pause\"]}");
		reply(c, "OK\n");
	} else if ((!strcmp(line, "SEEK") || !strcmp(line, "SEEKTO")) && arg) {
		cJSON *a;

		if (!playing(c))
			return;
		a = cJSON_CreateArray();
		cJSON_AddItemToArray(a, cJSON_CreateString("seek"));
		cJSON_AddItemToArray(a, cJSON_CreateNumber(atof(arg)));
		cJSON_AddItemToArray(a, cJSON_CreateString(line[4] ? "absolute" : "relative"));
		mpv_command(a);
		reply(c, "OK\n");
	} else if (!strcmp(line, "NEXT") || !strcmp(line, "PREV")) {
		if (!playing(c))
			return;
		reply(c, "OK\n");
		if (!s.audio) {
			mpv_send(line[0] == 'N' ? "{\"command\":[\"playlist-next\",\"weak\"]}"
						: "{\"command\":[\"playlist-prev\",\"weak\"]}");
		} else if (line[0] == 'N') {
			if (q.pos + 1 < q.n)
				q.pos++;
			else if (repeat_mode != REPEAT_OFF)
				q.pos = 0;
			else
				return;
			load_current();
		} else {
			/* Like every player: back to the start first. */
			double at = current_position();

			if (at <= PREV_RESTART_SECONDS && q.pos > 0)
				q.pos--;
			else if (at <= PREV_RESTART_SECONDS && repeat_mode != REPEAT_OFF)
				q.pos = q.n - 1;
			load_current();
		}
	} else if (!strcmp(line, "JUMP") && arg) {
		long i = atol(arg);

		if (!playing(c))
			return;
		if (!s.audio || i < 0 || (size_t)i >= q.n) {
			reply(c, "ERR value\n");
			return;
		}
		reply(c, "OK\n");
		q.pos = (size_t)i;
		load_current();
	} else if (!strcmp(line, "SHUFFLE")) {
		if (!playing(c))
			return;
		if (!s.audio) {
			reply(c, "ERR value\n");
			return;
		}
		reply(c, "OK\n");
		q.shuffle = !q.shuffle;
		/* The current track keeps playing; the rest is reordered. */
		queue_order(&q, q.n ? q.order[q.pos] : 0);
		s.index = (long)q.pos;
		publish();
	} else if (!strcmp(line, "REPEAT")) {
		if (!playing(c))
			return;
		repeat_mode = repeat_mode == REPEAT_OFF ? REPEAT_ALL : repeat_mode == REPEAT_ALL ? REPEAT_ONE : REPEAT_OFF;
		reply(c, "OK\n");
		publish();
	} else if (!strcmp(line, "LABELS") && arg) {
		/* Tab-separated words then positions; arg/arg2/arg3 were split. */
		char all[MAX_LINE], *f[10] = { 0 };
		int n = 0;

		snprintf(all, sizeof(all), "%s\t%s\t%s", arg, arg2 ? arg2 : "", arg3 ? arg3 : "");
		for (char *t = strtok(all, "\t"); t && n < 10; t = strtok(NULL, "\t"))
			f[n++] = t;
		for (int i = 0; i < 6 && i < n; i++) {
			copy_text(osd_label[i], sizeof(osd_label[i]), f[i]);
			ass_text(osd_label[i], sizeof(osd_label[i]), osd_label[i]);
		}
		for (int i = 0; i < 4 && 6 + i < n; i++)
			osd_pos[i] = atoi(f[6 + i]) & 3;
		if (s.osd_until_ms)
			osd_draw();
		reply(c, "OK\n");
	} else if (!strcmp(line, "KEY") && arg) {
		bool shown = s.osd_until_ms && now_ms() < s.osd_until_ms;

		if (!playing(c))
			return;
		reply(c, "OK\n");
		if (s.audio)
			return;
		/* Hidden controls: the first key only shows them. */
		if (!shown) {
			osd_show(OSD_MS);
			return;
		}
		if (!strcmp(arg, "menu_confirm") && s.eof) {
			/* At the end: from the beginning. */
			mpv_send("{\"command\":[\"seek\",0,\"absolute\"]}");
			mpv_send("{\"command\":[\"set_property\",\"pause\",false]}");
			set_position(0);
		} else if (!strcmp(arg, "menu_confirm")) {
			mpv_send("{\"command\":[\"cycle\",\"pause\"]}");
		} else if (!strcmp(arg, "menu_left") || !strcmp(arg, "menu_right") ||
			   !strcmp(arg, "menu_up") || !strcmp(arg, "menu_down")) {
			/* ←/→ 1 s, ↑/↓ 10 s (user request 2026-10-13); held, the
			 * step grows: fast forward / rewind. */
			bool horizontal = arg[5] == 'l' || arg[5] == 'r';
			int sign = arg[5] == 'l' || arg[5] == 'd' ? -1 : 1;
			long long now = now_ms();
			double step;

			s.held = !strcmp(arg, s.last_key) && now - s.last_key_ms < HOLD_MS ? s.held + 1 : 0;
			copy_text(s.last_key, sizeof(s.last_key), arg);
			s.last_key_ms = now;
			if (horizontal)
				step = !s.held ? 1 : s.held < 8 ? 2 : s.held < 20 ? 5 : 10;
			else
				step = !s.held ? 10 : s.held < 8 ? 10 : s.held < 20 ? 30 : 60;
			seek_by(sign * step, !s.held);
			/* Shown right away at the new position. */
			set_position(current_position() + sign * step);
		} else if (!strcmp(arg, "face_north")) {
			mpv_send("{\"command\":[\"cycle\",\"sub\"]}");
		} else if (!strcmp(arg, "face_west")) {
			mpv_send("{\"command\":[\"cycle\",\"audio\"]}");
		} else if (!strcmp(arg, "menu_back")) {
			osd_hide();
			stop_session();
			return;
		}
		osd_show(OSD_MS);
	} else if (!strcmp(line, "VOLUME") && arg) {
		int v = atoi(arg);

		if (v < 0 || v > 100 || !save_music_volume(v)) {
			reply(c, "ERR value\n");
			return;
		}
		music_volume = v;
		if (s.audio && (s.state == PLAYING || s.state == PAUSED)) {
			char json[96];

			snprintf(json, sizeof(json), "{\"command\":[\"set_property\",\"volume\",%d]}", v);
			mpv_send(json);
		}
		reply(c, "OK\n");
		publish();
	} else if (!strcmp(line, "SUB") || !strcmp(line, "AUDIO")) {
		if (!playing(c))
			return;
		mpv_send(line[0] == 'S' ? "{\"command\":[\"cycle\",\"sub\"]}"
					: "{\"command\":[\"cycle\",\"audio\"]}");
		reply(c, "OK\n");
	} else if (!strcmp(line, "STOP")) {
		pending.set = false;
		queue_clear(&pending.q);
		stop_session();
		reply(c, "OK\n");
	} else if (!strcmp(line, "PRE_POWER")) {
		pending.set = false;
		queue_clear(&pending.q);
		if (s.state == IDLE) {
			reply(c, "OK\n");
			return;
		}
		s.power_client = (int)(c - clients);
		stop_session();
	} else {
		reply(c, "ERR unknown\n");
	}
}

static void process_client(struct client *c)
{
	ssize_t n = recv(c->fd, c->buf + c->used, sizeof(c->buf) - c->used - 1, 0);
	char *nl;

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		if (s.power_client == (int)(c - clients))
			s.power_client = -1;
		close_client(c);
		return;
	}
	c->used += (size_t)n;
	c->buf[c->used] = '\0';
	while (c->fd >= 0 && (nl = strchr(c->buf, '\n'))) {
		size_t len = (size_t)(nl - c->buf) + 1;

		*nl = '\0';
		handle(c, c->buf);
		if (c->fd < 0)
			return;
		memmove(c->buf, c->buf + len, c->used - len + 1);
		c->used -= len;
	}
	if (c->used >= sizeof(c->buf) - 1)
		close_client(c);
}

int main(void)
{
	struct sigaction sa;
	struct sockaddr_un addr;
	int server;
	FILE *fp;

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	srandom((unsigned)(time(NULL) ^ getpid()));
	if (pipe2(sigchld_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_handler = on_stop;
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	server = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	unlink(SOCKET_PATH);
	if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(server, 8) != 0) {
		perror("nuubos-mediad");
		return 1;
	}
	chmod(SOCKET_PATH, 0666);
	fp = fopen(PIDFILE, "w");
	if (fp) {
		fprintf(fp, "%ld\n", (long)getpid());
		fclose(fp);
	}

	while (!stop_requested) {
		struct pollfd pfd[3 + MAX_CLIENTS];
		int who[3 + MAX_CLIENTS];
		nfds_t count = 0;
		int timeout = -1;

		pfd[count] = (struct pollfd){ server, POLLIN, 0 };
		who[count++] = -1;
		pfd[count] = (struct pollfd){ sigchld_pipe[0], POLLIN, 0 };
		who[count++] = -2;
		if (s.ipc >= 0) {
			pfd[count] = (struct pollfd){ s.ipc, POLLIN, 0 };
			who[count++] = -3;
		}
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd >= 0) {
				pfd[count] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
				who[count++] = i;
			}
		{
			long long due = s.deadline_ms, osd = osd_deadline();

			if (osd >= 0 && (due < 0 || osd < due))
				due = osd;
			if (due >= 0) {
				long long left = due - now_ms();

				timeout = left < 0 ? 0 : (int)left;
			}
		}
		if (poll(pfd, count, timeout) < 0 && errno != EINTR)
			break;
		timers();
		osd_timer();
		for (nfds_t i = 0; i < count; i++) {
			if (!pfd[i].revents)
				continue;
			if (who[i] == -1) {
				int fd;

				while ((fd = accept4(server, NULL, NULL, SOCK_CLOEXEC)) >= 0) {
					int slot = -1;

					for (int k = 0; k < MAX_CLIENTS; k++)
						if (clients[k].fd < 0) {
							slot = k;
							break;
						}
					if (slot < 0) {
						close(fd);
						continue;
					}
					clients[slot].fd = fd;
					clients[slot].subscribed = false;
					clients[slot].used = 0;
					clients[slot].expect = 0;
				}
			} else if (who[i] == -2) {
				char drain[32];
				int status;
				pid_t pid;

				while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
					;
				while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
					if (pid == s.pid)
						child_exited(status);
			} else if (who[i] == -3) {
				if (s.ipc >= 0)
					process_ipc();
			} else if (clients[who[i]].fd >= 0) {
				process_client(&clients[who[i]]);
			}
		}
	}
	if (s.pid > 0) {
		mpv_send("{\"command\":[\"quit-watch-later\"]}");
		kill(s.pid, SIGTERM);
	}
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	unlink(PLAYING_MARK);
	return 0;
}
