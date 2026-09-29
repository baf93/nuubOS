
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

#define RUNTIME_DIR "/run/nuubos"
#define STATE_FILE RUNTIME_DIR "/statusd.state"
#define SOCKET_FILE RUNTIME_DIR "/statusd.sock"
#define PID_FILE RUNTIME_DIR "/statusd.pid"
#define USER_DIR RUNTIME_DIR "/user"
#define ACTIVE_USER USER_DIR "/active"
#define MAX_SUBSCRIBERS 8
#define MAX_CONTROLLERS 4

struct topbar_state {
    char time_hhmm[8];
    char user_name[96];
    char wifi_state[24];
    int battery_percent;
    char battery_state[24];
    int controller_count;
    int controller_battery[MAX_CONTROLLERS];
};

static int subscribers[MAX_SUBSCRIBERS];
static bool running = true;

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
    time_t now = time(NULL);
    struct tm tm_now;

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

static void refresh_battery(struct topbar_state *state)
{
    DIR *dir;
    struct dirent *entry;
    char base[384];
    char path[448];
    char type[64];
    char status[64];
    int percent = -1;

    state->battery_percent = -1;
    snprintf(state->battery_state, sizeof(state->battery_state), "unknown");

    dir = opendir("/sys/class/power_supply");
    if (!dir)
        return;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        snprintf(base, sizeof(base), "/sys/class/power_supply/%s", entry->d_name);
        snprintf(path, sizeof(path), "%s/type", base);
        if (!read_first_line(path, type, sizeof(type)) || strcmp(type, "Battery") != 0)
            continue;

        snprintf(path, sizeof(path), "%s/capacity", base);
        if (!read_int_file(path, &percent))
            percent = -1;

        snprintf(path, sizeof(path), "%s/status", base);
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
        break;
    }

    closedir(dir);
}

static bool valid_bt_address(const char *addr)
{
    size_t i;

    if (strlen(addr) != 17)
        return false;
    for (i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (addr[i] != ':')
                return false;
        } else if (!isxdigit((unsigned char)addr[i])) {
            return false;
        }
    }
    return true;
}

static int parse_battery_percentage(const char *line)
{
    const char *p;
    int value = -1;

    p = strchr(line, '(');
    if (p && sscanf(p + 1, "%d", &value) == 1)
        return value;

    p = strchr(line, ':');
    if (p) {
        while (*p && !isdigit((unsigned char)*p))
            p++;
        if (*p && sscanf(p, "%d", &value) == 1)
            return value;
    }

    return -1;
}

static bool bluetooth_controller_info(const char *addr, int *battery)
{
    FILE *fp;
    char cmd[160];
    char line[512];
    bool connected = false;
    bool controller = false;
    int percent = -1;

    if (!valid_bt_address(addr))
        return false;

    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", addr);
    fp = popen(cmd, "r");
    if (!fp)
        return false;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Connected: yes"))
            connected = true;
        if (strstr(line, "Icon: input-gaming") ||
            strstr(line, "00001124-0000-1000-8000-00805f9b34fb") ||
            strstr(line, "00001812-0000-1000-8000-00805f9b34fb"))
            controller = true;
        if (strstr(line, "Battery Percentage:"))
            percent = parse_battery_percentage(line);
    }

    pclose(fp);
    if (!connected || !controller)
        return false;

    *battery = percent;
    return true;
}

static int address_compare(const void *a, const void *b)
{
    const char *aa = a;
    const char *bb = b;
    return strcmp(aa, bb);
}

static void refresh_controllers(struct topbar_state *state)
{
    FILE *fp;
    char line[512];
    char addresses[32][18];
    int address_count = 0;
    int i;

    state->controller_count = 0;
    for (i = 0; i < MAX_CONTROLLERS; i++)
        state->controller_battery[i] = -1;

    fp = popen("bluetoothctl devices 2>/dev/null", "r");
    if (!fp)
        return;

    while (address_count < 32 && fgets(line, sizeof(line), fp)) {
        char prefix[32];
        char addr[32];
        if (sscanf(line, "%31s %31s", prefix, addr) != 2)
            continue;
        if (strcmp(prefix, "Device") != 0 || !valid_bt_address(addr))
            continue;
        snprintf(addresses[address_count++], sizeof(addresses[0]), "%s", addr);
    }
    pclose(fp);

    qsort(addresses, (size_t)address_count, sizeof(addresses[0]), address_compare);

    for (i = 0; i < address_count && state->controller_count < MAX_CONTROLLERS; i++) {
        int battery = -1;
        if (!bluetooth_controller_info(addresses[i], &battery))
            continue;
        state->controller_battery[state->controller_count] = battery;
        state->controller_count++;
    }
}

static bool state_equal(const struct topbar_state *a,
                        const struct topbar_state *b)
{
    return strcmp(a->time_hhmm, b->time_hhmm) == 0 &&
           strcmp(a->user_name, b->user_name) == 0 &&
           strcmp(a->wifi_state, b->wifi_state) == 0 &&
           a->battery_percent == b->battery_percent &&
           strcmp(a->battery_state, b->battery_state) == 0 &&
           a->controller_count == b->controller_count &&
           memcmp(a->controller_battery, b->controller_battery,
                  sizeof(a->controller_battery)) == 0;
}

static bool publish_state(const struct topbar_state *state)
{
    char tmp[256];
    FILE *fp;
    int i;

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
    fprintf(fp, "CONTROLLER_COUNT=%d\n", state->controller_count);
    for (i = 0; i < MAX_CONTROLLERS; i++)
        fprintf(fp, "CONTROLLER_%d_BATTERY=%d\n", i + 1,
                state->controller_battery[i]);

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

static void commit_if_changed(struct topbar_state *current,
                              const struct topbar_state *next)
{
    if (state_equal(current, next))
        return;
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

static int make_inotify(void)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0)
        return -1;

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

static int make_timer(void)
{
    int fd;
    struct itimerspec spec;

    /* Minute tick for the top-bar clock. Battery updates are event-driven. */
    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = 60;
    spec.it_interval.tv_sec = 60;
    if (timerfd_settime(fd, 0, &spec, NULL) < 0) {
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
    refresh_controllers(next);
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
    for (i = 0; i < MAX_CONTROLLERS; i++)
        next.controller_battery[i] = -1;

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
                char buf[4096];
                while (read(inotify_fd, buf, sizeof(buf)) > 0) { }
                next = current;
                refresh_user(&next);
                refresh_controllers(&next);
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
                if (power) {
                    refresh_battery(&next);
                    refresh_controllers(&next);
                }
                if (rfkill)
                    refresh_wifi(&next);
                commit_if_changed(&current, &next);
            } else if (map[i] == -4) {
                uint64_t expirations;
                ssize_t timer_read;

                timer_read = read(timer_fd, &expirations, sizeof(expirations));
                if (timer_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
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
