/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-cast-session — the screen and sound of the console on a TV.
 *
 *   nuubos-cast-session discover [SECONDS]
 *   nuubos-cast-session cast ADDRESS PORT NAME [OPTIONS]   Google Cast
 *   nuubos-cast-session dlna LOCATION NAME [OPTIONS]       DLNA renderer
 *   nuubos-cast-session mice ADDRESS NAME [OPTIONS]        Miracast (MS-MICE)
 *   OPTIONS: --audio  --latency MS  --openh264 PATH  --console NAME
 *
 * One process per session (started and supervised by nuubos-castd): the
 * capture (wlr-screencopy), the converter, the encoders and the protocol
 * all run in this poll loop; PipeWire runs its own thread for the sound.
 */
#include "session.h"
#include "common.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PFD 32
#define AUDIO_BLOCK 480 /* 10 ms */

static volatile sig_atomic_t terminate;
/* Diagnostics: NUUBOS_CAST_NO_DAMAGE=1 copies every frame. */
static bool no_damage;
static bool stop_flag;
static bool key_flag;
static char stdin_buf[256];
static size_t stdin_len;
static bool stdin_closed;

static void on_signal(int sig)
{
	(void)sig;
	terminate = 1;
}

void session_fail(struct session *s, const char *reason)
{
	if (s->error[0])
		return;
	copy_text(s->error, sizeof(s->error), reason);
	log_msg("session failed: %s", reason);
}

void session_screen_size(struct session *s, int *w, int *h)
{
	*w = s->screen_w;
	*h = s->screen_h;
}

static void read_stdin(void)
{
	char buf[128];
	ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		/* castd went away: never cast on unattended */
		stdin_closed = true;
		stop_flag = true;
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] != '\n') {
			if (stdin_len + 1 < sizeof(stdin_buf))
				stdin_buf[stdin_len++] = buf[i];
			continue;
		}
		stdin_buf[stdin_len] = '\0';
		if (!strcmp(stdin_buf, "STOP"))
			stop_flag = true;
		else if (!strcmp(stdin_buf, "KEYFRAME"))
			key_flag = true;
		stdin_len = 0;
	}
}

bool session_stop_requested(void)
{
	struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };

	if (!stdin_closed && poll(&pfd, 1, 0) > 0)
		read_stdin();
	return stop_flag || terminate;
}

static int parse_options(struct session *s, int argc, char **argv, int first)
{
	for (int i = first; i < argc; i++) {
		if (!strcmp(argv[i], "--audio")) {
			s->with_audio = true;
		} else if (!strcmp(argv[i], "--latency") && i + 1 < argc) {
			s->latency_ms = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--openh264") && i + 1 < argc) {
			copy_text(s->openh264, sizeof(s->openh264), argv[++i]);
		} else if (!strcmp(argv[i], "--console") && i + 1 < argc) {
			copy_text(s->console_name, sizeof(s->console_name), argv[++i]);
		} else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return -1;
		}
	}
	if (s->latency_ms < 80)
		s->latency_ms = 80;
	if (s->latency_ms > 1500)
		s->latency_ms = 1500;
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: nuubos-cast-session discover [SECONDS]\n"
		"       nuubos-cast-session cast ADDRESS PORT NAME [OPTIONS]\n"
		"       nuubos-cast-session dlna LOCATION NAME [OPTIONS]\n"
		"       nuubos-cast-session mice ADDRESS NAME [OPTIONS]\n"
		"OPTIONS: --audio --latency MS --openh264 PATH --console NAME\n");
}

/* One plain capture before negotiating: the offer needs the screen size. */
static int first_capture(struct session *s)
{
	struct capture_image img;
	int64_t deadline = now_ms() + 3000;

	if (capture_request(s->capture, false) != 0)
		return -1;
	while (now_ms() < deadline) {
		struct pollfd pfd = { capture_fd(s->capture), POLLIN, 0 };
		int rc;

		if (capture_prepare(s->capture) != 0)
			return -1;
		rc = poll(&pfd, 1, 200);
		if (capture_dispatch(s->capture, rc > 0 && (pfd.revents & POLLIN)) != 0)
			return -1;
		if (capture_take(s->capture, &img)) {
			capture_logical_size(&img, &s->screen_w, &s->screen_h);
			log_msg("screen %dx%d", s->screen_w, s->screen_h);
			return 0;
		}
		if (capture_broken(s->capture))
			return -1;
		if (!capture_busy(s->capture) && capture_request(s->capture, false) != 0)
			return -1;
	}
	return -1;
}

static void ensure_picture(struct session *s)
{
	int w, h, fps;
	bool letterbox;

	s->proto->geometry(s, &w, &h, &letterbox, &fps);
	if (s->pic.plane[0] && s->pic.width == w && s->pic.height == h)
		return;
	picture_free(&s->pic);
	s->have_pic = false;
	if (picture_alloc(&s->pic, w, h) != 0)
		session_fail(s, "capture");
}

/* Pipeline statistics, logged every 10 s while casting. */
static struct {
	int frames;
	int64_t capture_us, convert_us, encode_us, since_us;
} stats;

static void stats_log(void)
{
	int64_t now = now_us();

	if (!stats.since_us) {
		stats.since_us = now;
		return;
	}
	if (now - stats.since_us < 10000000 || !stats.frames)
		return;
	log_msg("pipeline: %.1f fps, capture %lld us, convert %lld us, encode+send %lld us per frame",
		stats.frames * 1e6 / (double)(now - stats.since_us), (long long)(stats.capture_us / stats.frames),
		(long long)(stats.convert_us / stats.frames), (long long)(stats.encode_us / stats.frames));
	memset(&stats, 0, sizeof(stats));
	stats.since_us = now;
}

static void handle_capture(struct session *s)
{
	struct capture_image img;
	int w, h, fps;
	bool letterbox;
	int64_t t0, t1;

	if (!capture_take(s->capture, &img))
		return;
	t0 = now_us();
	stats.capture_us += t0 - s->capture_started_us;
	/* Double buffering: the compositor copies the next frame while this
	 * one is converted and encoded. */
	s->proto->geometry(s, &w, &h, &letterbox, &fps);
	/* Half a frame early at most: the frame rate stays the protocol's. */
	if (s->proto->want_picture(s, t0) && t0 >= s->next_capture_us - 500000 / fps) {
		if (capture_request(s->capture, !no_damage) == 0) {
			s->capture_started_us = t0;
			s->next_capture_us = t0 + 1000000 / fps;
			capture_flush(s->capture);
		}
	}
	capture_logical_size(&img, &s->screen_w, &s->screen_h);
	ensure_picture(s);
	if (s->error[0] || converter_run(s->converter, &img, &s->pic, letterbox) != 0)
		return;
	t1 = now_us();
	s->have_pic = true;
	s->last_picture_us = t1;
	if (s->proto->picture(s, &s->pic) != 0)
		session_fail(s, "encoder");
	stats.convert_us += t1 - t0;
	stats.encode_us += now_us() - t1;
	stats.frames++;
	stats_log();
}

static void handle_audio(struct session *s)
{
	int16_t pcm[AUDIO_BLOCK * AUDIO_CHANNELS];

	audio_ack(s->audio);
	while (audio_available(s->audio) >= AUDIO_BLOCK) {
		int64_t t = audio_read(s->audio, pcm, AUDIO_BLOCK);

		if (t < 0)
			break;
		if (s->casting && s->proto->audio(s, pcm, AUDIO_BLOCK, t) != 0) {
			session_fail(s, "encoder");
			return;
		}
	}
}

static int run(struct session *s)
{
	struct pollfd pfd[MAX_PFD];

	while (!terminate && !s->error[0]) {
		int64_t now = now_us(), deadline = -1, d;
		int n = 0, cap_i = -1, aud_i = -1, in_i = -1, proto_first, proto_count, w, h, fps;
		bool letterbox;
		int timeout, repeat;

		if (stop_flag) {
			s->proto->stop(s);
			return 0;
		}
		if (key_flag) {
			key_flag = false;
			s->proto->force_key(s);
		}
		s->proto->geometry(s, &w, &h, &letterbox, &fps);
		if (s->proto->want_picture(s, now)) {
			if (!capture_busy(s->capture) && now >= s->next_capture_us) {
				capture_request(s->capture, s->have_pic && !no_damage);
				s->capture_started_us = now;
				s->next_capture_us = now + 1000000 / fps;
			}
			repeat = s->proto->repeat_ms(s);
			if (repeat > 0 && s->have_pic) {
				int64_t due = s->last_picture_us + (int64_t)repeat * 1000;

				if (now >= due) {
					s->pic.time_us = now;
					s->last_picture_us = now;
					if (s->proto->picture(s, &s->pic) != 0)
						session_fail(s, "encoder");
					continue;
				}
				deadline = due;
			}
			if (!capture_busy(s->capture) && (deadline < 0 || s->next_capture_us < deadline))
				deadline = s->next_capture_us;
		}
		d = s->proto->deadline(s);
		if (d >= 0 && (deadline < 0 || d < deadline))
			deadline = d;

		if (!stdin_closed) {
			pfd[n] = (struct pollfd){ STDIN_FILENO, POLLIN, 0 };
			in_i = n++;
		}
		if (capture_prepare(s->capture) != 0) {
			session_fail(s, "capture");
			break;
		}
		pfd[n] = (struct pollfd){ capture_fd(s->capture), POLLIN, 0 };
		cap_i = n++;
		if (s->audio) {
			pfd[n] = (struct pollfd){ audio_fd(s->audio), POLLIN, 0 };
			aud_i = n++;
		}
		proto_first = n;
		proto_count = s->proto->pollfds(s, pfd + n, MAX_PFD - n);
		n += proto_count;

		timeout = -1;
		if (deadline >= 0) {
			int64_t left = (deadline - now_us() + 999) / 1000;

			timeout = left < 0 ? 0 : left > 60000 ? 60000 : (int)left;
		}
		if (poll(pfd, (nfds_t)n, timeout) < 0 && errno != EINTR) {
			capture_dispatch(s->capture, false);
			break;
		}
		if (capture_dispatch(s->capture, (pfd[cap_i].revents & POLLIN) != 0) != 0 ||
		    capture_broken(s->capture)) {
			session_fail(s, "capture");
			break;
		}
		if (in_i >= 0 && pfd[in_i].revents)
			read_stdin();
		handle_capture(s);
		if (aud_i >= 0 && pfd[aud_i].revents)
			handle_audio(s);
		if (s->proto->dispatch(s, pfd + proto_first, proto_count) != 0)
			break;
		if (s->proto->timers(s, now_us()) != 0)
			break;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct session s;
	struct sigaction sa;
	int first;

	memset(&s, 0, sizeof(s));
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	set_nonblock(STDIN_FILENO);
	no_damage = getenv("NUUBOS_CAST_NO_DAMAGE") != NULL;
	s.latency_ms = 250;
	copy_text(s.console_name, sizeof(s.console_name), "nuubOS");

	if (argc >= 2 && !strcmp(argv[1], "discover"))
		return discover_run(argc >= 3 ? atoi(argv[2]) : 3);
	if (argc >= 5 && !strcmp(argv[1], "cast")) {
		s.proto = &proto_cast;
		copy_text(s.address, sizeof(s.address), argv[2]);
		s.port = atoi(argv[3]);
		copy_text(s.name, sizeof(s.name), argv[4]);
		first = 5;
	} else if (argc >= 4 && !strcmp(argv[1], "dlna")) {
		s.proto = &proto_dlna;
		copy_text(s.location, sizeof(s.location), argv[2]);
		copy_text(s.name, sizeof(s.name), argv[3]);
		first = 4;
	} else if (argc >= 4 && !strcmp(argv[1], "mice")) {
		s.proto = &proto_mice;
		copy_text(s.address, sizeof(s.address), argv[2]);
		copy_text(s.name, sizeof(s.name), argv[3]);
		first = 4;
	} else {
		usage();
		return 2;
	}
	if (parse_options(&s, argc, argv, first) != 0) {
		usage();
		return 2;
	}

	emit("@state connecting");
	s.capture = capture_open();
	s.converter = converter_new();
	if (!s.capture || !s.converter || first_capture(&s) != 0) {
		session_fail(&s, "capture");
	} else {
		if (s.with_audio) {
			s.audio = audio_open();
			if (!s.audio)
				log_msg("no sound for the receiver");
		}
		if (s.proto->start(&s) == 0 && !s.error[0])
			run(&s);
		else if (!s.error[0] && session_stop_requested())
			s.proto->stop(&s);
	}
	if (s.error[0])
		emit("@error %s", s.error);
	if (s.proto)
		s.proto->destroy(&s);
	audio_close(s.audio);
	picture_free(&s.pic);
	converter_free(s.converter);
	capture_close(s.capture);
	emit("@state ended");
	return 0;
}
