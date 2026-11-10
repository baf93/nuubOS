/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-emuctl — command line client of nuubos-emud (tests, lifecycle
 * hooks). Prints the reply; exits 0 on OK, 1 on ERR or no service.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/nuubos/emud.sock"
/* PRE_POWER waits for the automatic state and, before restart/poweroff,
 * for RetroArch to exit; never hold a lifecycle transition longer. */
#define REPLY_TIMEOUT_S 25

static void usage(void)
{
	fprintf(stderr,
		"Usage: nuubos-emuctl COMMAND\n"
		"  status | watch\n"
		"  launch GAME_ID [resume|new|slot:N [host|connect:ADDRESS:PORT:CORE]]\n"
		"                                 (slot:-1 = automatic state; netplay)\n"
		"  pause | resume | save | load | reset | fast-forward\n"
		"  slot N|+1|-1\n"
		"  disc N                         change the disc (0-based) of a multi-disc game\n"
		"  advanced                       open RetroArch's own menu\n"
		"  menu                           RetroArch's own menu without a game\n"
		"  reset-retroarch                the user's RetroArch configuration to the defaults\n"
		"  screenshot\n"
		"  hotkey ACTION [ARG]            run a game hotkey (screen_layout, stylus right...)\n"
		"  quit\n"
		"  game-settings GAME SYSTEM       per-game overrides of the active user\n"
		"  game-set GAME SYSTEM core|aspect|filter|accuracy|pak|resolution|remap_CONTROL VALUE (\"\" = inherit)\n"
		"  game-reset GAME\n"
		"  system-settings SYSTEM         System Settings of the active user\n"
		"  system-set SYSTEM KEY VALUE    as game-set, for every game of the system\n"
		"  states [GAME_ID]               save states (running game or GAME_ID)\n"
		"  settings                       emulation settings of the active user\n"
		"  set KEY VALUE\n"
		"  cheats                         cheat lists and cheats of the game\n"
		"  cheat K 0|1 | cheat-file PATH\n"
		"  netplay-host | netplay-stop    host the running game, end netplay\n"
		"  netplay-hosts                  netplay games on the local network\n"
		"  pre-power sleep|restart|poweroff\n"
		"  storage-lost MOUNT_POINT       the games card went away\n");
}

int main(int argc, char **argv)
{
	struct sockaddr_un addr;
	struct timeval tv = { REPLY_TIMEOUT_S, 0 };
	char command[PATH_MAX + 64];
	char buf[1024];
	const char *verb = argc > 1 ? argv[1] : "";
	int multi = 0;
	int watch = 0;
	int fd;
	int ok = 0;

	if (!strcmp(verb, "status") && argc == 2) {
		snprintf(command, sizeof(command), "STATUS\n");
		multi = 1;
	} else if (!strcmp(verb, "watch") && argc == 2) {
		snprintf(command, sizeof(command), "SUBSCRIBE\n");
		watch = 1;
	} else if (!strcmp(verb, "launch") && argc >= 3 && argc <= 5) {
		snprintf(command, sizeof(command), "LAUNCH\t%s%s%s%s%s\n", argv[2],
			 argc >= 4 ? "\t" : "", argc >= 4 ? argv[3] : "",
			 argc == 5 ? "\t" : "", argc == 5 ? argv[4] : "");
	} else if ((!strcmp(verb, "cheats") || !strcmp(verb, "netplay-hosts")) && argc == 2) {
		snprintf(command, sizeof(command), "%s\n", verb[0] == 'c' ? "CHEATS" : "NETPLAY_HOSTS");
		multi = 1;
	} else if (!strcmp(verb, "cheat") && argc == 4) {
		snprintf(command, sizeof(command), "CHEAT_SET\t%s\t%s\n", argv[2], argv[3]);
	} else if (!strcmp(verb, "cheat-file") && argc == 3) {
		snprintf(command, sizeof(command), "CHEAT_FILE\t%s\n", argv[2]);
	} else if (!strcmp(verb, "states") && (argc == 2 || argc == 3)) {
		snprintf(command, sizeof(command), "STATES%s%s\n",
			 argc == 3 ? "\t" : "", argc == 3 ? argv[2] : "");
		multi = 1;
	} else if (!strcmp(verb, "settings") && argc == 2) {
		snprintf(command, sizeof(command), "SETTINGS\n");
		multi = 1;
	} else if (!strcmp(verb, "set") && argc == 4) {
		snprintf(command, sizeof(command), "SET\t%s\t%s\n", argv[2], argv[3]);
	} else if (!strcmp(verb, "slot") && argc == 3) {
		snprintf(command, sizeof(command), "SLOT\t%s\n", argv[2]);
	} else if (!strcmp(verb, "game-settings") && argc == 4) {
		snprintf(command, sizeof(command), "GAME_SETTINGS\t%s\t%s\n", argv[2], argv[3]);
		multi = 1;
	} else if (!strcmp(verb, "game-set") && argc == 6) {
		snprintf(command, sizeof(command), "GAME_SET\t%s\t%s\t%s\t%s\n",
			 argv[2], argv[3], argv[4], argv[5]);
	} else if (!strcmp(verb, "system-settings") && argc == 3) {
		snprintf(command, sizeof(command), "SYSTEM_SETTINGS\t%s\n", argv[2]);
		multi = 1;
	} else if (!strcmp(verb, "system-set") && argc == 5) {
		snprintf(command, sizeof(command), "SYSTEM_SET\t%s\t%s\t%s\n", argv[2], argv[3], argv[4]);
	} else if (!strcmp(verb, "game-reset") && argc == 3) {
		snprintf(command, sizeof(command), "GAME_RESET\t%s\n", argv[2]);
	} else if (!strcmp(verb, "hotkey") && (argc == 3 || argc == 4)) {
		snprintf(command, sizeof(command), "HOTKEY\t%s%s%s\n", argv[2],
			 argc == 4 ? " " : "", argc == 4 ? argv[3] : "");
	} else if (!strcmp(verb, "storage-lost") && argc == 3) {
		snprintf(command, sizeof(command), "STORAGE_LOST\t%s\n", argv[2]);
	} else if (!strcmp(verb, "disc") && argc == 3) {
		snprintf(command, sizeof(command), "DISC\t%s\n", argv[2]);
	} else if (!strcmp(verb, "pre-power") && argc == 3) {
		snprintf(command, sizeof(command), "PRE_POWER\t%s\n", argv[2]);
	} else if (argc == 2) {
		static const char *const simple[][2] = {
			{ "pause", "PAUSE" }, { "resume", "RESUME" },
			{ "save", "SAVE_STATE" }, { "load", "LOAD_STATE" },
			{ "reset", "RESET" }, { "fast-forward", "FAST_FORWARD" },
			{ "advanced", "ADVANCED" }, { "quit", "QUIT" },
			{ "screenshot", "SCREENSHOT" },
			{ "menu", "MENU" }, { "reset-retroarch", "RESET_RETROARCH" },
			{ "netplay-host", "NETPLAY_HOST" }, { "netplay-stop", "NETPLAY_STOP" },
		};
		size_t i;

		for (i = 0; i < sizeof(simple) / sizeof(simple[0]); i++)
			if (!strcmp(verb, simple[i][0]))
				break;
		if (i == sizeof(simple) / sizeof(simple[0])) {
			usage();
			return 2;
		}
		snprintf(command, sizeof(command), "%s\n", simple[i][1]);
	} else {
		usage();
		return 2;
	}

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		/* No emulation service: nothing runs, nothing to protect. */
		if (!strcmp(verb, "pre-power"))
			return 0;
		fprintf(stderr, "nuubos-emuctl: %s\n", strerror(errno));
		return 1;
	}
	if (!watch)
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (write(fd, command, strlen(command)) != (ssize_t)strlen(command)) {
		close(fd);
		return 1;
	}

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);

		if (n <= 0)
			break;
		buf[n] = '\0';
		fputs(buf, stdout);
		fflush(stdout);
		if (watch)
			continue;
		if (!multi) {
			ok = strncmp(buf, "OK", 2) == 0;
			break;
		}
		if (!strncmp(buf, "ERR", 3))
			break;
		if (strstr(buf, "end=1\n")) {
			ok = 1;
			break;
		}
	}
	close(fd);
	return ok ? 0 : 1;
}
