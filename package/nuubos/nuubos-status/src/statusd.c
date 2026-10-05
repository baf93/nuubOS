
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <nuubos/notify.h>

#define RUNTIME_DIR "/run/nuubos"
#define STATE_FILE RUNTIME_DIR "/statusd.state"
#define SOCKET_FILE RUNTIME_DIR "/statusd.sock"
#define PID_FILE RUNTIME_DIR "/statusd.pid"
#define USER_DIR RUNTIME_DIR "/user"
#define ACTIVE_USER USER_DIR "/active"
#define MAX_SUBSCRIBERS 8

struct topbar_state {
    char time_hhmm[8];
    char user_name[96];
    char wifi_state[24];
    int battery_percent;
    char battery_state[24];
};

/* Console battery alerts (EPIC-006, owner: Battery & Charging). Re-armed
 * only after charging or recovering above the re-arm level, so gauge jitter
 * around a threshold cannot repeat the warning. */
#define BATTERY_LOW_PERCENT 15
#define BATTERY_CRITICAL_PERCENT 5
#define BATTERY_REARM_PERCENT 20

static int subscribers[MAX_SUBSCRIBERS];
static bool running = true;
static int battery_alert_level;     /* 0 none, 1 low sent, 2 critical sent */
static bool charge_session;         /* charger seen since last discharge */
static bool full_notified;

static void trim(char *s)
{
    size_t len;
    char *start = s;

    while (*start && isspace((unsigned char)*start))
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);

    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}


static void copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (!dst || dst_size == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool read_first_line(const char *path, char *out, size_t out_size)
{
    FILE *fp;

    if (!out || out_size == 0)
        return false;
    out[0] = '\0';

    fp = fopen(path, "r");
    if (!fp)
        return false;

    if (!fgets(out, (int)out_size, fp)) {
        fclose(fp);
        out[0] = '\0';
        return false;
    }
    fclose(fp);
    trim(out);
    return true;
}

static bool read_key_value(const char *path, const char *key,
                           char *out, size_t out_size)
{
    FILE *fp;
    char line[256];
    size_t key_len = strlen(key);

    if (!out || out_size == 0)
        return false;
    out[0] = '\0';

    fp = fopen(path, "r");
    if (!fp)
        return false;

    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            copy_text(out, out_size, line + key_len + 1);
            fclose(fp);
            return true;
        }
    }

    fclose(fp);
    return false;
}

static void refresh_time(struct topbar_state *state)
{
    const time_t valid_after = (time_t)1577836800; /* 2020-01-01 UTC */
    struct timespec ts;
    time_t now;
    struct tm tm_now;

    /* Not time(): glibc serves it from the coarse clock, which still reads
     * the previous second when the minute timer fires. */
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        ts.tv_sec = 0;
    now = ts.tv_sec;

    /* glibc localtime_r() keeps the zone loaded at first use; tzset()
     * re-reads /etc/localtime only when it changed. */
    tzset();
    if (now < valid_after || localtime_r(&now, &tm_now) == NULL ||
        strftime(state->time_hhmm, sizeof(state->time_hhmm), "%H:%M", &tm_now) == 0) {
        snprintf(state->time_hhmm, sizeof(state->time_hhmm), "--:--");
    }
}

static void refresh_user(struct topbar_state *state)
{
    char id[128];
    char path[384];
    char name[96];

    snprintf(state->user_name, sizeof(state->user_name), "Player");

    if (!read_first_line(ACTIVE_USER, id, sizeof(id)) || id[0] == '\0')
        return;

    snprintf(path, sizeof(path), "/state/users/%s/profile.conf", id);
    if (read_key_value(path, "USER_NAME", name, sizeof(name)) && name[0] != '\0')
        copy_text(state->user_name, sizeof(state->user_name), name);
}

static void refresh_wifi(struct topbar_state *state)
{
    FILE *fp;
    char line[256];
    char wpa_state[64] = "";

    if (access("/sys/class/net/wlan0", F_OK) != 0) {
        snprintf(state->wifi_state, sizeof(state->wifi_state), "off");
        return;
    }

    fp = popen("wpa_cli -i wlan0 status 2>/dev/null", "r");
    if (!fp) {
        snprintf(state->wifi_state, sizeof(state->wifi_state), "disconnected");
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "wpa_state=", 10) == 0) {
            copy_text(wpa_state, sizeof(wpa_state), line + 10);
            trim(wpa_state);
            break;
        }
    }
    pclose(fp);

    if (strcmp(wpa_state, "COMPLETED") == 0) {
        snprintf(state->wifi_state, sizeof(state->wifi_state), "connected");
    } else if (strcmp(wpa_state, "SCANNING") == 0 ||
               strcmp(wpa_state, "AUTHENTICATING") == 0 ||
               strcmp(wpa_state, "ASSOCIATING") == 0 ||
               strcmp(wpa_state, "ASSOCIATED") == 0 ||
               strcmp(wpa_state, "4WAY_HANDSHAKE") == 0 ||
               strcmp(wpa_state, "GROUP_HANDSHAKE") == 0) {
        snprintf(state->wifi_state, sizeof(state->wifi_state), "connecting");
    } else if (strcmp(wpa_state, "INTERFACE_DISABLED") == 0) {
        snprintf(state->wifi_state, sizeof(state->wifi_state), "off");
    } else {
        snprintf(state->wifi_state, sizeof(state->wifi_state), "disconnected");
    }
}

static bool read_int_file(const char *path, int *value)
{
    char buf[64];
    char *end = NULL;
    long v;

    if (!read_first_line(path, buf, sizeof(buf)))
        return false;

    errno = 0;
    v = strtol(buf, &end, 10);
    if (errno || end == buf)
        return false;
    *value = (int)v;
    return true;
}

static int battery_candidate_score(const char *base)
{
    char path[448];
    char scope[64];
    int score = 0;

    snprintf(path, sizeof(path), "%s/scope", base);
    if (read_first_line(path, scope, sizeof(scope))) {
        if (strcasecmp(scope, "Device") == 0)
            return -1000;
        if (strcasecmp(scope, "System") == 0)
            score += 100;
    }

    snprintf(path, sizeof(path), "%s/status", base);
    if (access(path, R_OK) == 0)
        score += 20;
    snprintf(path, sizeof(path), "%s/voltage_now", base);
    if (access(path, R_OK) == 0)
        score += 10;
    snprintf(path, sizeof(path), "%s/current_now", base);
    if (access(path, R_OK) == 0)
        score += 10;
    snprintf(path, sizeof(path), "%s/health", base);
    if (access(path, R_OK) == 0)
        score += 5;
    snprintf(path, sizeof(path), "%s/present", base);
    if (access(path, R_OK) == 0)
        score += 5;

    return score;
}

static void refresh_battery(struct topbar_state *state)
{
    DIR *dir;
    struct dirent *entry;
    char base[384];
    char best_base[384] = "";
    char path[448];
    char type[64];
    char status[64];
    int best_score = -1001;
    int percent = -1;

    state->battery_percent = -1;
    snprintf(state->battery_state, sizeof(state->battery_state), "unknown");

    dir = opendir("/sys/class/power_supply");
    if (!dir)
        return;

    while ((entry = readdir(dir)) != NULL) {
        int score;

        if (entry->d_name[0] == '.')
            continue;

        snprintf(base, sizeof(base), "/sys/class/power_supply/%s", entry->d_name);
        snprintf(path, sizeof(path), "%s/type", base);
        if (!read_first_line(path, type, sizeof(type)) || strcmp(type, "Battery") != 0)
            continue;

        /*
         * Accessory/HID batteries are device-scoped and must never replace the
         * console battery in the top bar. Prefer System scope when available;
         * otherwise score battery telemetry richness rather than relying on a
         * hard-coded power_supply node name.
         */
        score = battery_candidate_score(base);
        if (score > best_score) {
            best_score = score;
            copy_text(best_base, sizeof(best_base), base);
        }
    }
    closedir(dir);

    if (best_base[0] == '\0' || best_score < 0)
        return;

    snprintf(path, sizeof(path), "%s/capacity", best_base);
    if (!read_int_file(path, &percent))
        percent = -1;

    snprintf(path, sizeof(path), "%s/status", best_base);
    if (!read_first_line(path, status, sizeof(status)))
        snprintf(status, sizeof(status), "Unknown");

    state->battery_percent = percent;
    if (strcasecmp(status, "Charging") == 0)
        snprintf(state->battery_state, sizeof(state->battery_state), "charging");
    else if (strcasecmp(status, "Full") == 0)
        snprintf(state->battery_state, sizeof(state->battery_state), "full");
    else if (strcasecmp(status, "Discharging") == 0)
        snprintf(state->battery_state, sizeof(state->battery_state), "discharging");
    else if (strcasecmp(status, "Not charging") == 0)
        snprintf(state->battery_state, sizeof(state->battery_state), "not-charging");
    else
        snprintf(state->battery_state, sizeof(state->battery_state), "unknown");
}

static bool state_equal(const struct topbar_state *a,
                        const struct topbar_state *b)
{
    return strcmp(a->time_hhmm, b->time_hhmm) == 0 &&
           strcmp(a->user_name, b->user_name) == 0 &&
           strcmp(a->wifi_state, b->wifi_state) == 0 &&
           a->battery_percent == b->battery_percent &&
           strcmp(a->battery_state, b->battery_state) == 0;
}

static bool publish_state(const struct topbar_state *state)
{
    char tmp[256];
    FILE *fp;

    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", STATE_FILE, (long)getpid());
    fp = fopen(tmp, "w");
    if (!fp)
        return false;

    fprintf(fp, "STATUS_VERSION=1\n");
    fprintf(fp, "TIME=%s\n", state->time_hhmm);
    fprintf(fp, "USER_NAME=%s\n", state->user_name);
    fprintf(fp, "WIFI_STATE=%s\n", state->wifi_state);
    fprintf(fp, "BATTERY_PERCENT=%d\n", state->battery_percent);
    fprintf(fp, "BATTERY_STATE=%s\n", state->battery_state);

    if (fflush(fp) != 0) {
        fclose(fp);
        unlink(tmp);
        return false;
    }
    if (fsync(fileno(fp)) != 0) {
        fclose(fp);
        unlink(tmp);
        return false;
    }
    fclose(fp);
    chmod(tmp, 0644);

    if (rename(tmp, STATE_FILE) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static void notify_subscribers(void)
{
    int i;
    const char msg[] = "changed\n";

    for (i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (subscribers[i] < 0)
            continue;
        if (write(subscribers[i], msg, sizeof(msg) - 1) < 0 &&
            errno != EAGAIN && errno != EWOULDBLOCK) {
            close(subscribers[i]);
            subscribers[i] = -1;
        }
    }
}

static void post_battery(const char *event, int percent)
{
    struct nuubos_notify n;

    /* One id for every console battery event: plugging the charger in
     * replaces a visible low-battery warning instead of stacking. */
    nuubos_notify_begin(&n, "POST", "battery", event);
    if (percent >= 0)
        nuubos_notify_int(&n, "percent", percent);
    (void)nuubos_notify_send(&n);
}

static void notify_battery(const struct topbar_state *prev,
                           const struct topbar_state *next)
{
    bool external = strcmp(next->battery_state, "charging") == 0 ||
                    strcmp(next->battery_state, "full") == 0;
    int percent = next->battery_percent;

    if (prev->battery_percent == -999) {
        /* First evaluation after start: adopt the state silently. */
        charge_session = external;
        full_notified = strcmp(next->battery_state, "full") == 0;
        return;
    }

    if (strcmp(next->battery_state, "discharging") == 0) {
        charge_session = false;
        full_notified = false;
    }

    if (external) {
        battery_alert_level = 0;
        if (!charge_session) {
            charge_session = true;
            post_battery("battery.charging", percent);
        }
        if (strcmp(next->battery_state, "full") == 0 && !full_notified) {
            full_notified = true;
            post_battery("battery.full", -1);
        }
        return;
    }

    if (percent < 0)
        return;
    if (percent > BATTERY_REARM_PERCENT) {
        battery_alert_level = 0;
    } else if (percent <= BATTERY_CRITICAL_PERCENT && battery_alert_level < 2) {
        battery_alert_level = 2;
        post_battery("battery.critical", percent);
    } else if (percent <= BATTERY_LOW_PERCENT && battery_alert_level < 1) {
        battery_alert_level = 1;
        post_battery("battery.low", percent);
    }
}

static void commit_if_changed(struct topbar_state *current,
                              const struct topbar_state *next)
{
    if (state_equal(current, next))
        return;
    if (current->battery_percent != next->battery_percent ||
        strcmp(current->battery_state, next->battery_state) != 0)
        notify_battery(current, next);
    *current = *next;
    if (publish_state(current))
        notify_subscribers();
}

static int make_listen_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_FILE);
    unlink(SOCKET_FILE);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    chmod(SOCKET_FILE, 0666);
    return fd;
}

static void accept_subscribers(int listen_fd)
{
    for (;;) {
        int client = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        int i;
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            return;
        }

        for (i = 0; i < MAX_SUBSCRIBERS; i++) {
            if (subscribers[i] < 0) {
                ssize_t written;

                subscribers[i] = client;
                written = write(client, "changed\n", 8);
                if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(client);
                    subscribers[i] = -1;
                }
                client = -1;
                break;
            }
        }
        if (client >= 0)
            close(client);
    }
}

static int localtime_watch = -1;

static int make_inotify(void)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0)
        return -1;

    /* regionald replaces /etc/localtime atomically (rename) on a zone change. */
    localtime_watch = inotify_add_watch(fd, "/etc",
        IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ONLYDIR);

    (void)inotify_add_watch(fd, USER_DIR,
        IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM |
        IN_CLOSE_WRITE | IN_ATTRIB);
    (void)inotify_add_watch(fd, "/dev/input",
        IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_ATTRIB);
    return fd;
}

static int make_uevent_socket(void)
{
    int fd;
    struct sockaddr_nl addr;
    int rcvbuf = 256 * 1024;

    fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                NETLINK_KOBJECT_UEVENT);
    if (fd < 0)
        return -1;

    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = (uint32_t)getpid();
    addr.nl_groups = 1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * Minute tick for the top-bar clock, aligned to the wall-clock minute so
 * HH:MM changes on time. A clock step (manual set, network sync, RTC) cancels
 * the timer, which is then refreshed and re-armed. Battery is event-driven.
 */
static int arm_minute_timer(int fd)
{
    struct itimerspec spec;
    time_t now = time(NULL);

    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = now - (now % 60) + 60;
    spec.it_interval.tv_sec = 60;
    return timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
                           &spec, NULL);
}

static int make_timer(void)
{
    int fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (arm_minute_timer(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int make_signal_fd(void)
{
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
        return -1;
    return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

static void refresh_all(struct topbar_state *next)
{
    refresh_time(next);
    refresh_user(next);
    refresh_wifi(next);
    refresh_battery(next);
}

int main(void)
{
    struct topbar_state current;
    struct topbar_state next;
    int listen_fd = -1;
    int inotify_fd = -1;
    int uevent_fd = -1;
    int timer_fd = -1;
    int signal_fd = -1;
    int i;

    memset(&current, 0, sizeof(current));
    memset(&next, 0, sizeof(next));
    current.battery_percent = -999;
    for (i = 0; i < MAX_SUBSCRIBERS; i++)
        subscribers[i] = -1;

    (void)mkdir(RUNTIME_DIR, 0755);

    listen_fd = make_listen_socket();
    inotify_fd = make_inotify();
    uevent_fd = make_uevent_socket();
    timer_fd = make_timer();
    signal_fd = make_signal_fd();

    if (listen_fd < 0 || signal_fd < 0) {
        fprintf(stderr, "nuubos-statusd: unable to initialize runtime contract\n");
        return 1;
    }

    refresh_all(&next);
    commit_if_changed(&current, &next);

    while (running) {
        struct pollfd fds[5 + MAX_SUBSCRIBERS];
        int map[5 + MAX_SUBSCRIBERS];
        int nfds = 0;
        int rc;

        fds[nfds] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
        map[nfds++] = -1;
        if (inotify_fd >= 0) {
            fds[nfds] = (struct pollfd){ .fd = inotify_fd, .events = POLLIN };
            map[nfds++] = -2;
        }
        if (uevent_fd >= 0) {
            fds[nfds] = (struct pollfd){ .fd = uevent_fd, .events = POLLIN };
            map[nfds++] = -3;
        }
        if (timer_fd >= 0) {
            fds[nfds] = (struct pollfd){ .fd = timer_fd, .events = POLLIN };
            map[nfds++] = -4;
        }
        fds[nfds] = (struct pollfd){ .fd = signal_fd, .events = POLLIN };
        map[nfds++] = -5;
        for (i = 0; i < MAX_SUBSCRIBERS; i++) {
            if (subscribers[i] < 0)
                continue;
            fds[nfds] = (struct pollfd){ .fd = subscribers[i], .events = POLLHUP | POLLERR };
            map[nfds++] = i;
        }

        rc = poll(fds, (nfds_t)nfds, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (i = 0; i < nfds; i++) {
            if (!fds[i].revents)
                continue;

            if (map[i] == -1) {
                accept_subscribers(listen_fd);
            } else if (map[i] == -2) {
                char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t n;
                bool zone = false;
                bool other = false;
                while ((n = read(inotify_fd, buf, sizeof(buf))) > 0) {
                    size_t off = 0;
                    while (off + sizeof(struct inotify_event) <= (size_t)n) {
                        const struct inotify_event *ev = (const struct inotify_event *)(buf + off);
                        if (ev->wd != localtime_watch)
                            other = true;
                        else if (ev->len > 0 && strcmp(ev->name, "localtime") == 0)
                            zone = true;
                        off += sizeof(*ev) + ev->len;
                    }
                }
                next = current;
                if (other)
                    refresh_user(&next);
                if (zone)
                    refresh_time(&next);
                commit_if_changed(&current, &next);
            } else if (map[i] == -3) {
                char buf[8192];
                ssize_t n;
                bool power = false;
                bool rfkill = false;
                while ((n = recv(uevent_fd, buf, sizeof(buf) - 1, 0)) > 0) {
                    buf[n] = '\0';
                    if (memmem(buf, (size_t)n, "SUBSYSTEM=power_supply", strlen("SUBSYSTEM=power_supply")))
                        power = true;
                    if (memmem(buf, (size_t)n, "SUBSYSTEM=rfkill", strlen("SUBSYSTEM=rfkill")))
                        rfkill = true;
                }
                next = current;
                if (power)
                    refresh_battery(&next);
                if (rfkill)
                    refresh_wifi(&next);
                commit_if_changed(&current, &next);
            } else if (map[i] == -4) {
                uint64_t expirations;
                ssize_t timer_read;

                timer_read = read(timer_fd, &expirations, sizeof(expirations));
                if (timer_read < 0 && errno == ECANCELED)
                    (void)arm_minute_timer(timer_fd);
                else if (timer_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    continue;
                next = current;
                refresh_time(&next);
                commit_if_changed(&current, &next);
            } else if (map[i] == -5) {
                struct signalfd_siginfo si;
                while (read(signal_fd, &si, sizeof(si)) == sizeof(si)) {
                    if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        running = false;
                    } else if (si.ssi_signo == SIGUSR1) {
                        next = current;
                        /*
                         * SIGUSR1 is the event-driven refresh hook shared by
                         * Wi-Fi state changes and successful network-time sync.
                         * Refresh both values so a clock step becomes visible
                         * immediately instead of leaving TIME=--:-- until the
                         * next timer tick.
                         */
                        refresh_wifi(&next);
                        refresh_time(&next);
                        commit_if_changed(&current, &next);
                    }
                }
            } else if (map[i] >= 0) {
                int slot = map[i];
                if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    close(subscribers[slot]);
                    subscribers[slot] = -1;
                }
            }
        }
    }

    for (i = 0; i < MAX_SUBSCRIBERS; i++)
        if (subscribers[i] >= 0)
            close(subscribers[i]);
    if (listen_fd >= 0) close(listen_fd);
    if (inotify_fd >= 0) close(inotify_fd);
    if (uevent_fd >= 0) close(uevent_fd);
    if (timer_fd >= 0) close(timer_fd);
    if (signal_fd >= 0) close(signal_fd);
    unlink(SOCKET_FILE);
    unlink(STATE_FILE);
    unlink(PID_FILE);
    return 0;
}
