#include <arpa/inet.h>
#include <ctype.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <nuubos/notify.h>

#define SERVICE_NAME "org.nuubOS.Wifi"
#define OBJECT_PATH "/org/nuubOS/Wifi"
#define INTERFACE_NAME "org.nuubOS.Wifi1"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"
#define WIFI_IFACE "wlan0"
#define WPA_CONF "/state/network/wpa_supplicant.conf"
#define INTERFACES_CONF "/state/network/interfaces"
#define WIFI_STATE_DIR "/state/network/wifi"
#define WIFI_PROFILE_DIR WIFI_STATE_DIR "/profiles"
#define WIFI_ENABLED_FILE WIFI_STATE_DIR "/enabled"
#define WIFI_BACKEND_MARKER WIFI_STATE_DIR "/backend-v2"
#define WPA_CTRL_SOCKET "/var/run/wpa_supplicant/" WIFI_IFACE
#define WPA_MONITOR_DIR "/run/nuubos"
#define MAX_NETWORKS 64
#define MAX_CAPTURE 32768
#define SCAN_SESSION_INTERVAL_MS 5000ULL
/* A drop that reconnects to the same SSID within this window (roaming
 * between access points, reassociation after resume: rtw88 has no WoWLAN
 * and is deauthenticated in suspend) is not reported as a loss. */
#define LOSS_GRACE_MS 15000ULL

struct wifi_state {
    bool enabled;
    char state[24];
    char ssid[128];
    char ipv4[INET_ADDRSTRLEN];
    int32_t signal_dbm;
};

struct wifi_network {
    char ssid[128];
    char security[32];
    int32_t signal_dbm;
    bool saved;
    bool current;
};

struct ip_config {
    char mode[16];
    char address[INET_ADDRSTRLEN];
    uint32_t prefix;
    char netmask[INET_ADDRSTRLEN];
    char gateway[INET_ADDRSTRLEN];
    char dns[256];
    char runtime_address[INET_ADDRSTRLEN];
    uint32_t runtime_prefix;
    char runtime_netmask[INET_ADDRSTRLEN];
    char runtime_gateway[INET_ADDRSTRLEN];
    char runtime_dns[256];
};

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t refresh_requested = 0;
static bool scan_results_requested = false;
/* Own wpa_supplicant control-interface monitor (ATTACH): unlike the
 * `wpa_cli -a` action script it receives every event, including
 * CTRL-EVENT-SCAN-RESULTS, WRONG_KEY and SSID-TEMP-DISABLED. */
static int wpa_monitor_fd = -1;
static char wpa_monitor_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static bool scan_session_active = false;
static bool scan_in_flight = false;
static uint64_t next_scan_due_ms = 0;
/* Pending "wifi.lost": deadline (0 = none), SSID, and the suspended time
 * (CLOCK_BOOTTIME - CLOCK_MONOTONIC) when it was armed. */
static uint64_t loss_due_ms = 0;
static uint64_t loss_suspended_ms = 0;
static char loss_ssid[128];
/* A WPS push-button session is running in wpa_supplicant. It ends with
 * WPS-SUCCESS, WPS-FAIL, WPS-TIMEOUT (walk time), an overlap or a cancel. */
static bool wps_active = false;

static const char introspection_xml[] =
    "<node>"
    "<interface name='org.nuubOS.Wifi1'>"
    "<method name='GetSnapshot'>"
    "<arg name='enabled' type='b' direction='out'/>"
    "<arg name='state' type='s' direction='out'/>"
    "<arg name='ssid' type='s' direction='out'/>"
    "<arg name='ipv4' type='s' direction='out'/>"
    "<arg name='signal_dbm' type='i' direction='out'/>"
    "</method>"
    "<method name='SetEnabled'>"
    "<arg name='enabled' type='b' direction='in'/>"
    "</method>"
    "<method name='Scan'/>"
    "<method name='StartScanSession'/>"
    "<method name='StopScanSession'/>"
    "<method name='GetNetworks'>"
    "<arg name='networks' type='a(ssibb)' direction='out'/>"
    "</method>"
    "<method name='GetSavedNetworks'>"
    "<arg name='networks' type='a(ssibb)' direction='out'/>"
    "</method>"
    "<method name='Connect'>"
    "<arg name='ssid' type='s' direction='in'/>"
    "<arg name='password' type='s' direction='in'/>"
    "<arg name='hidden' type='b' direction='in'/>"
    "</method>"
    "<method name='Disconnect'/>"
    "<method name='StartWps'>"
    "<arg name='ssid' type='s' direction='in'/>"
    "</method>"
    "<method name='CancelWps'/>"
    "<method name='GetWpsActive'>"
    "<arg name='active' type='b' direction='out'/>"
    "</method>"
    "<method name='Forget'>"
    "<arg name='ssid' type='s' direction='in'/>"
    "</method>"
    "<method name='GetIpConfiguration'>"
    "<arg name='ssid' type='s' direction='in'/>"
    "<arg name='mode' type='s' direction='out'/>"
    "<arg name='address' type='s' direction='out'/>"
    "<arg name='prefix' type='u' direction='out'/>"
    "<arg name='netmask' type='s' direction='out'/>"
    "<arg name='gateway' type='s' direction='out'/>"
    "<arg name='dns' type='s' direction='out'/>"
    "<arg name='runtime_address' type='s' direction='out'/>"
    "<arg name='runtime_prefix' type='u' direction='out'/>"
    "<arg name='runtime_netmask' type='s' direction='out'/>"
    "<arg name='runtime_gateway' type='s' direction='out'/>"
    "<arg name='runtime_dns' type='s' direction='out'/>"
    "</method>"
    "<method name='SetIpConfiguration'>"
    "<arg name='ssid' type='s' direction='in'/>"
    "<arg name='mode' type='s' direction='in'/>"
    "<arg name='address' type='s' direction='in'/>"
    "<arg name='prefix' type='u' direction='in'/>"
    "<arg name='gateway' type='s' direction='in'/>"
    "<arg name='dns' type='s' direction='in'/>"
    "</method>"
    "<signal name='StateChanged'>"
    "<arg name='enabled' type='b'/>"
    "<arg name='state' type='s'/>"
    "<arg name='ssid' type='s'/>"
    "<arg name='ipv4' type='s'/>"
    "<arg name='signal_dbm' type='i'/>"
    "</signal>"
    "<signal name='ScanStateChanged'>"
    "<arg name='scanning' type='b'/>"
    "</signal>"
    "<signal name='NetworksChanged'/>"
    "<signal name='NetworksSnapshotChanged'>"
    "<arg name='networks' type='a(ssibb)'/>"
    "</signal>"
    "<signal name='WpsStateChanged'>"
    "<arg name='active' type='b'/>"
    "</signal>"
    "<signal name='IpConfigurationChanged'>"
    "<arg name='ssid' type='s'/>"
    "</signal>"
    "<signal name='OperationFailed'>"
    "<arg name='operation' type='s'/>"
    "<arg name='message' type='s'/>"
    "</signal>"
    "</interface>"
    "<interface name='org.freedesktop.DBus.Introspectable'>"
    "<method name='Introspect'>"
    "<arg name='xml_data' type='s' direction='out'/>"
    "</method>"
    "</interface>"
    "</node>";

/* Self-pipe: libdbus restarts its own poll on EINTR, so signals must wake
 * the main loop through a descriptor instead of a periodic timeout. */
static int wake_pipe[2] = {-1, -1};

static uint64_t monotonic_ms(void);

static void on_signal(int signo)
{
    int saved_errno = errno;

    if (signo == SIGTERM || signo == SIGINT)
        running = 0;
    else if (signo == SIGUSR1)
        refresh_requested = 1;
    if (wake_pipe[1] >= 0)
        if (write(wake_pipe[1], "w", 1) < 0) {
            /* Pipe full: a wakeup is already pending. */
        }
    errno = saved_errno;
}

static bool open_wake_pipe(void)
{
    if (pipe(wake_pipe) < 0)
        return false;
    for (int i = 0; i < 2; i++) {
        (void)fcntl(wake_pipe[i], F_SETFL, O_NONBLOCK);
        (void)fcntl(wake_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    return true;
}

/* Sleep until D-Bus traffic, a wpa_supplicant event, a signal (child exit,
 * DHCP lease, stop) or the next scan of an active scan session. */
static void wait_for_work(DBusConnection *conn, int dbus_fd)
{
    struct pollfd fds[3];
    nfds_t count = 0;
    int timeout = -1;
    char buf[64];

    if (dbus_connection_get_dispatch_status(conn) == DBUS_DISPATCH_DATA_REMAINS ||
        refresh_requested || scan_results_requested || !running)
        timeout = 0;
    else if (scan_session_active && !scan_in_flight && next_scan_due_ms != 0) {
        uint64_t now = monotonic_ms();
        timeout = next_scan_due_ms > now ? (int)(next_scan_due_ms - now) : 0;
    }
    if (timeout != 0 && loss_due_ms != 0) {
        uint64_t now = monotonic_ms();
        int loss_timeout = loss_due_ms > now ? (int)(loss_due_ms - now) : 0;
        if (timeout < 0 || loss_timeout < timeout)
            timeout = loss_timeout;
    }

    fds[count].fd = dbus_fd;
    fds[count++].events = POLLIN;
    if (wake_pipe[0] >= 0) {
        fds[count].fd = wake_pipe[0];
        fds[count++].events = POLLIN;
    }
    if (wpa_monitor_fd >= 0) {
        fds[count].fd = wpa_monitor_fd;
        fds[count++].events = POLLIN;
    }
    (void)poll(fds, count, timeout);
    if (wake_pipe[0] >= 0)
        while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
            ;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (dst_size == 0)
        return;
    if (src == NULL)
        src = "";

    len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void trim_newline(char *value)
{
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r')) {
        value[len - 1] = '\0';
        len--;
    }
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

/* Time spent in suspend since boot. */
static uint64_t suspended_ms(void)
{
    struct timespec boot;

    if (clock_gettime(CLOCK_BOOTTIME, &boot) != 0)
        return 0;
    return (uint64_t)boot.tv_sec * 1000ULL + (uint64_t)boot.tv_nsec / 1000000ULL -
           monotonic_ms();
}

static bool ensure_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0 || errno == EEXIST)
        return true;
    return false;
}

static bool ensure_state_dirs(void)
{
    if (!ensure_dir("/state/network", 0755))
        return false;
    if (!ensure_dir(WIFI_STATE_DIR, 0700))
        return false;
    if (!ensure_dir(WIFI_PROFILE_DIR, 0700))
        return false;
    (void)chmod(WIFI_STATE_DIR, 0700);
    (void)chmod(WIFI_PROFILE_DIR, 0700);
    return true;
}

static int run_capture(char *const argv[], char *output, size_t output_size)
{
    int pipefd[2];
    pid_t pid;
    size_t used = 0;
    int status = 0;

    if (output_size > 0)
        output[0] = '\0';

    if (pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        int devnull;
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);
    while (output_size > 0 && used + 1 < output_size) {
        ssize_t rc = read(pipefd[0], output + used, output_size - used - 1);
        if (rc > 0) {
            used += (size_t)rc;
            continue;
        }
        if (rc < 0 && errno == EINTR)
            continue;
        break;
    }
    if (output_size > 0)
        output[used] = '\0';
    close(pipefd[0]);

    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static int run_quiet(char *const argv[])
{
    char output[64];
    return run_capture(argv, output, sizeof(output));
}


static bool interface_up(const char *name)
{
    struct ifreq ifr;
    int fd;
    bool up = false;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;

    memset(&ifr, 0, sizeof(ifr));
    copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), name);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0)
        up = (ifr.ifr_flags & IFF_UP) != 0;
    close(fd);
    return up;
}

static bool read_enabled_preference(void)
{
    FILE *fp = fopen(WIFI_ENABLED_FILE, "r");
    char value[16];
    if (fp == NULL)
        return true;
    if (fgets(value, sizeof(value), fp) == NULL) {
        fclose(fp);
        return true;
    }
    fclose(fp);
    return value[0] != '0';
}

static bool write_enabled_preference(bool enabled)
{
    char tmp[256];
    FILE *fp;

    if (!ensure_state_dirs())
        return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", WIFI_ENABLED_FILE, (long)getpid());
    fp = fopen(tmp, "w");
    if (fp == NULL)
        return false;
    if (fprintf(fp, "%d\n", enabled ? 1 : 0) < 0 || fflush(fp) != 0) {
        fclose(fp);
        unlink(tmp);
        return false;
    }
    if (fchmod(fileno(fp), 0600) != 0) {
        fclose(fp);
        unlink(tmp);
        return false;
    }
    if (fclose(fp) != 0) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, WIFI_ENABLED_FILE) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static bool valid_ssid(const char *ssid)
{
    size_t i;
    size_t len;
    if (ssid == NULL)
        return false;
    len = strlen(ssid);
    if (len == 0 || len > 32)
        return false;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)ssid[i];
        if (c == '\n' || c == '\r' || c == '\0')
            return false;
    }
    return true;
}

static bool valid_password(const char *password)
{
    size_t len;
    if (password == NULL)
        return false;
    len = strlen(password);
    return len == 0 || (len >= 8 && len <= 63);
}

static bool valid_ipv4_or_empty(const char *value)
{
    struct in_addr addr;
    if (value == NULL || value[0] == '\0')
        return true;
    return inet_pton(AF_INET, value, &addr) == 1;
}

static bool valid_dns_list(const char *dns)
{
    char copy[256];
    char *saveptr = NULL;
    char *token;

    if (dns == NULL || dns[0] == '\0')
        return true;
    copy_string(copy, sizeof(copy), dns);
    token = strtok_r(copy, ", ", &saveptr);
    if (token == NULL)
        return false;
    while (token != NULL) {
        if (!valid_ipv4_or_empty(token) || token[0] == '\0')
            return false;
        token = strtok_r(NULL, ", ", &saveptr);
    }
    return true;
}

static void quote_wpa_value(const char *value, char *out, size_t out_size)
{
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)value;
    if (out_size == 0)
        return;
    out[used++] = '"';
    while (*p != '\0' && used + 3 < out_size) {
        if (*p == '\\' || *p == '"')
            out[used++] = '\\';
        out[used++] = (char)*p++;
    }
    if (used + 2 <= out_size)
        out[used++] = '"';
    out[used] = '\0';
}

static int wpa_network_id_for_ssid(const char *ssid)
{
    char output[MAX_CAPTURE];
    char *saveptr = NULL;
    char *line;
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "list_networks", NULL};

    if (run_capture(argv, output, sizeof(output)) != 0)
        return -1;

    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL) {
        char *id_text;
        char *ssid_text;
        char *tab;
        if (strncmp(line, "network id", 10) == 0) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        id_text = line;
        tab = strchr(id_text, '\t');
        if (tab == NULL) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        *tab = '\0';
        ssid_text = tab + 1;
        tab = strchr(ssid_text, '\t');
        if (tab != NULL)
            *tab = '\0';
        if (strcmp(ssid_text, ssid) == 0)
            return (int)strtol(id_text, NULL, 10);
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return -1;
}

static bool rewrite_wpa_config(const char *remove_ssid, const char *add_ssid,
                               const char *password, bool hidden);

static bool wpa_ping(void)
{
    char output[128];
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "ping", NULL};
    return run_capture(argv, output, sizeof(output)) == 0 && strstr(output, "PONG") != NULL;
}

static void close_wpa_monitor(void)
{
    if (wpa_monitor_fd >= 0)
        close(wpa_monitor_fd);
    wpa_monitor_fd = -1;
    if (wpa_monitor_path[0] != '\0')
        unlink(wpa_monitor_path);
    wpa_monitor_path[0] = '\0';
}

/* Same protocol as wpa_ctrl: a bound datagram socket connected to the
 * interface socket, registered with ATTACH. Events then arrive as
 * "<level>EVENT ..." datagrams and wake the main loop. */
static bool open_wpa_monitor(void)
{
    struct sockaddr_un local;
    struct sockaddr_un dest;
    struct pollfd pfd;
    char reply[256];
    ssize_t len;
    int fd;

    if (wpa_monitor_fd >= 0)
        return true;

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return false;
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), WPA_MONITOR_DIR "/wifid-wpa-%ld",
             (long)getpid());
    (void)mkdir(WPA_MONITOR_DIR, 0755);
    unlink(local.sun_path);
    memset(&dest, 0, sizeof(dest));
    dest.sun_family = AF_UNIX;
    copy_string(dest.sun_path, sizeof(dest.sun_path), WPA_CTRL_SOCKET);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(fd);
        return false;
    }
    copy_string(wpa_monitor_path, sizeof(wpa_monitor_path), local.sun_path);
    wpa_monitor_fd = fd;
    if (connect(fd, (struct sockaddr *)&dest, sizeof(dest)) < 0 ||
        send(fd, "ATTACH", 6, 0) != 6) {
        close_wpa_monitor();
        return false;
    }
    /* wpa_supplicant answers immediately; events only follow the OK. */
    pfd.fd = fd;
    pfd.events = POLLIN;
    while (poll(&pfd, 1, 2000) > 0) {
        len = recv(fd, reply, sizeof(reply) - 1, 0);
        if (len < 0)
            break;
        reply[len] = '\0';
        if (strncmp(reply, "OK", 2) == 0)
            return true;
        if (reply[0] != '<')
            break;
    }
    close_wpa_monitor();
    return false;
}

static bool ensure_wpa_running(void)
{
    pid_t pid;
    int status;
    if (wpa_ping()) {
        if (!open_wpa_monitor())
            fprintf(stderr, "nuubos-wifid: cannot attach to wpa_supplicant\n");
        return true;
    }
    /* Clean STATE has no config yet: write the base one (no networks). */
    if (access(WPA_CONF, R_OK) != 0 && !rewrite_wpa_config(NULL, NULL, NULL, false))
        return false;

    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        execlp("wpa_supplicant", "wpa_supplicant", "-B", "-D", "nl80211",
               "-i", WIFI_IFACE, "-c", WPA_CONF, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;
    usleep(150000);
    if (!wpa_ping())
        return false;
    /* A previous instance's monitor died with it. */
    close_wpa_monitor();
    if (!open_wpa_monitor())
        fprintf(stderr, "nuubos-wifid: cannot attach to wpa_supplicant\n");
    return true;
}

static void refresh_signal_strength(struct wifi_state *state)
{
    char output[1024];
    char *saveptr = NULL;
    char *line;
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "signal_poll", NULL};

    state->signal_dbm = 0;
    if (strcmp(state->state, "connected") != 0)
        return;
    if (run_capture(argv, output, sizeof(output)) != 0)
        return;
    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL) {
        if (strncmp(line, "RSSI=", 5) == 0) {
            state->signal_dbm = (int32_t)strtol(line + 5, NULL, 10);
            return;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
}

static void refresh_state(struct wifi_state *state)
{
    char output[4096];
    char *saveptr = NULL;
    char *line;
    char wpa_state[64] = "";
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "status", NULL};

    memset(state, 0, sizeof(*state));
    if (access("/sys/class/net/" WIFI_IFACE, F_OK) != 0) {
        copy_string(state->state, sizeof(state->state), "unavailable");
        return;
    }

    state->enabled = interface_up(WIFI_IFACE) && read_enabled_preference();
    if (!state->enabled) {
        copy_string(state->state, sizeof(state->state), "off");
        return;
    }

    copy_string(state->state, sizeof(state->state), "disconnected");
    if (run_capture(argv, output, sizeof(output)) != 0)
        return;

    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL) {
        char *separator = strchr(line, '=');
        if (separator != NULL) {
            *separator = '\0';
            if (strcmp(line, "wpa_state") == 0)
                copy_string(wpa_state, sizeof(wpa_state), separator + 1);
            else if (strcmp(line, "ssid") == 0)
                copy_string(state->ssid, sizeof(state->ssid), separator + 1);
            else if (strcmp(line, "ip_address") == 0)
                copy_string(state->ipv4, sizeof(state->ipv4), separator + 1);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (strcmp(wpa_state, "COMPLETED") == 0)
        copy_string(state->state, sizeof(state->state), "connected");
    else if (strcmp(wpa_state, "SCANNING") == 0 ||
             strcmp(wpa_state, "AUTHENTICATING") == 0 ||
             strcmp(wpa_state, "ASSOCIATING") == 0 ||
             strcmp(wpa_state, "ASSOCIATED") == 0 ||
             strcmp(wpa_state, "4WAY_HANDSHAKE") == 0 ||
             strcmp(wpa_state, "GROUP_HANDSHAKE") == 0)
        copy_string(state->state, sizeof(state->state), "connecting");
    else if (strcmp(wpa_state, "INTERFACE_DISABLED") == 0) {
        copy_string(state->state, sizeof(state->state), "off");
        state->enabled = false;
    }

    refresh_signal_strength(state);
}

static bool state_equal(const struct wifi_state *a, const struct wifi_state *b)
{
    return a->enabled == b->enabled &&
           a->signal_dbm == b->signal_dbm &&
           strcmp(a->state, b->state) == 0 &&
           strcmp(a->ssid, b->ssid) == 0 &&
           strcmp(a->ipv4, b->ipv4) == 0;
}

static const char *security_from_flags(const char *flags)
{
    if (flags == NULL || flags[0] == '\0' || strcmp(flags, "[ESS]") == 0)
        return "Open";
    if (strstr(flags, "EAP") != NULL)
        return "Enterprise";
    if (strstr(flags, "SAE") != NULL)
        return "WPA3";
    if (strstr(flags, "WPA2") != NULL || strstr(flags, "RSN") != NULL)
        return "WPA2";
    if (strstr(flags, "WPA") != NULL)
        return "WPA";
    if (strstr(flags, "WEP") != NULL)
        return "WEP";
    return "Secured";
}

static bool is_saved_ssid(const char *ssid)
{
    return wpa_network_id_for_ssid(ssid) >= 0;
}

static size_t load_scan_networks(struct wifi_network *networks, size_t max_count,
                                 const char *current_ssid)
{
    char output[MAX_CAPTURE];
    char *saveptr = NULL;
    char *line;
    size_t count = 0;
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "scan_results", NULL};

    if (run_capture(argv, output, sizeof(output)) != 0)
        return 0;

    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL) {
        char *fields[5] = {0};
        char *cursor = line;
        int field = 0;
        char *tab;
        size_t i;
        int32_t signal;
        bool duplicate = false;

        if (strncmp(line, "bssid /", 7) == 0) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        while (field < 4 && (tab = strchr(cursor, '\t')) != NULL) {
            *tab = '\0';
            fields[field++] = cursor;
            cursor = tab + 1;
        }
        fields[field++] = cursor;
        if (field < 5 || fields[4] == NULL || fields[4][0] == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        signal = (int32_t)strtol(fields[2], NULL, 10);
        for (i = 0; i < count; i++) {
            if (strcmp(networks[i].ssid, fields[4]) == 0) {
                duplicate = true;
                if (signal > networks[i].signal_dbm)
                    networks[i].signal_dbm = signal;
                break;
            }
        }
        if (!duplicate && count < max_count) {
            memset(&networks[count], 0, sizeof(networks[count]));
            copy_string(networks[count].ssid, sizeof(networks[count].ssid), fields[4]);
            copy_string(networks[count].security, sizeof(networks[count].security),
                        security_from_flags(fields[3]));
            networks[count].signal_dbm = signal;
            networks[count].saved = is_saved_ssid(fields[4]);
            networks[count].current = current_ssid != NULL &&
                                      strcmp(current_ssid, fields[4]) == 0;
            count++;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            bool swap = false;
            if (networks[j].current && !networks[i].current)
                swap = true;
            else if (networks[j].current == networks[i].current &&
                     networks[j].signal_dbm > networks[i].signal_dbm)
                swap = true;
            if (swap) {
                struct wifi_network tmp = networks[i];
                networks[i] = networks[j];
                networks[j] = tmp;
            }
        }
    }
    return count;
}

static size_t load_saved_networks(struct wifi_network *networks, size_t max_count,
                                  const char *current_ssid)
{
    char output[MAX_CAPTURE];
    char *saveptr = NULL;
    char *line;
    size_t count = 0;
    struct wifi_network scanned[MAX_NETWORKS];
    size_t scanned_count = load_scan_networks(scanned, MAX_NETWORKS, current_ssid);
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "list_networks", NULL};

    if (run_capture(argv, output, sizeof(output)) != 0)
        return 0;
    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL && count < max_count) {
        char *tab1;
        char *tab2;
        char *ssid;
        if (strncmp(line, "network id", 10) == 0) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        tab1 = strchr(line, '\t');
        if (tab1 == NULL) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        ssid = tab1 + 1;
        tab2 = strchr(ssid, '\t');
        if (tab2 != NULL)
            *tab2 = '\0';
        if (ssid[0] == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        memset(&networks[count], 0, sizeof(networks[count]));
        copy_string(networks[count].ssid, sizeof(networks[count].ssid), ssid);
        copy_string(networks[count].security, sizeof(networks[count].security), "Saved");
        networks[count].saved = true;
        networks[count].current = current_ssid != NULL && strcmp(current_ssid, ssid) == 0;
        for (size_t i = 0; i < scanned_count; i++) {
            if (strcmp(scanned[i].ssid, ssid) == 0) {
                copy_string(networks[count].security, sizeof(networks[count].security),
                            scanned[i].security);
                networks[count].signal_dbm = scanned[i].signal_dbm;
                break;
            }
        }
        count++;
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return count;
}

static bool wpa_set_network(int id, const char *field, const char *value)
{
    char id_text[24];
    char output[256];
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "set_network",
                          id_text, (char *)field, (char *)value, NULL};
    snprintf(id_text, sizeof(id_text), "%d", id);
    return run_capture(argv, output, sizeof(output)) == 0 && strstr(output, "OK") != NULL;
}

static int wpa_add_network(void)
{
    char output[256];
    char *end = NULL;
    long id;
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "add_network", NULL};
    if (run_capture(argv, output, sizeof(output)) != 0)
        return -1;
    trim_newline(output);
    id = strtol(output, &end, 10);
    if (end == output || id < 0 || id > 65535)
        return -1;
    return (int)id;
}

static bool wpa_remove_network(int id)
{
    char id_text[24];
    char output[256];
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "remove_network", id_text, NULL};
    snprintf(id_text, sizeof(id_text), "%d", id);
    return run_capture(argv, output, sizeof(output)) == 0 && strstr(output, "OK") != NULL;
}

static bool wpa_select_network(int id)
{
    char id_text[24];
    char output[256];
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "select_network", id_text, NULL};
    snprintf(id_text, sizeof(id_text), "%d", id);
    return run_capture(argv, output, sizeof(output)) == 0 && strstr(output, "OK") != NULL;
}

static bool line_ssid_matches(const char *line, const char *ssid)
{
    const char *p = line;
    char parsed[128];
    size_t used = 0;
    while (isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "ssid=\"", 6) != 0)
        return false;
    p += 6;
    while (*p != '\0' && *p != '"' && used + 1 < sizeof(parsed)) {
        if (*p == '\\' && p[1] != '\0')
            p++;
        parsed[used++] = *p++;
    }
    parsed[used] = '\0';
    return strcmp(parsed, ssid) == 0;
}

static bool rewrite_wpa_config(const char *remove_ssid, const char *add_ssid,
                               const char *password, bool hidden)
{
    FILE *in;
    FILE *out;
    char tmp[256];
    char line[512];
    char block[8192];
    size_t block_used = 0;
    bool in_block = false;
    bool block_matches = false;
    bool update_config_seen = false;
    bool add_open = password == NULL || password[0] == '\0';
    char quoted_ssid[256];
    char quoted_password[256];

    if (!ensure_state_dirs())
        return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", WPA_CONF, (long)getpid());
    in = fopen(WPA_CONF, "r");
    out = fopen(tmp, "w");
    if (out == NULL) {
        if (in != NULL)
            fclose(in);
        return false;
    }

    if (in != NULL) {
        while (fgets(line, sizeof(line), in) != NULL) {
            char *p = line;
            while (isspace((unsigned char)*p))
                p++;

            if (!in_block && strncmp(p, "network={", 9) == 0) {
                in_block = true;
                block_matches = false;
                block_used = 0;
            }

            if (in_block) {
                size_t len = strlen(line);
                if (block_used + len + 1 < sizeof(block)) {
                    memcpy(block + block_used, line, len);
                    block_used += len;
                    block[block_used] = '\0';
                }
                if (remove_ssid != NULL && line_ssid_matches(line, remove_ssid))
                    block_matches = true;
                if (*p == '}') {
                    if (!block_matches && block_used > 0)
                        (void)fwrite(block, 1, block_used, out);
                    in_block = false;
                    block_matches = false;
                    block_used = 0;
                }
                continue;
            }

            if (strncmp(p, "update_config=", 14) == 0) {
                fputs("update_config=1\n", out);
                update_config_seen = true;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    } else {
        fputs("ctrl_interface=/var/run/wpa_supplicant\n", out);
        fputs("country=IT\n", out);
    }

    if (!update_config_seen)
        fputs("update_config=1\n", out);

    if (add_ssid != NULL) {
        quote_wpa_value(add_ssid, quoted_ssid, sizeof(quoted_ssid));
        quote_wpa_value(password == NULL ? "" : password,
                        quoted_password, sizeof(quoted_password));
        fputs("\nnetwork={\n", out);
        fprintf(out, "    ssid=%s\n", quoted_ssid);
        if (add_open)
            fputs("    key_mgmt=NONE\n", out);
        else
            fprintf(out, "    psk=%s\n", quoted_password);
        if (hidden)
            fputs("    scan_ssid=1\n", out);
        fputs("}\n", out);
    }

    if (fflush(out) != 0 || fchmod(fileno(out), 0600) != 0 || fclose(out) != 0) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, WPA_CONF) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static bool connect_network(const char *ssid, const char *password, bool hidden,
                            char *error, size_t error_size)
{
    int id;
    char quoted_ssid[256];
    char quoted_password[256];

    if (!valid_ssid(ssid)) {
        copy_string(error, error_size, "SSID must contain 1-32 bytes");
        return false;
    }
    if (!valid_password(password)) {
        copy_string(error, error_size, "Password must be empty for open networks or 8-63 characters");
        return false;
    }
    if (!read_enabled_preference()) {
        copy_string(error, error_size, "Wi-Fi is disabled");
        return false;
    }
    if (!ensure_wpa_running()) {
        copy_string(error, error_size, "wpa_supplicant is unavailable");
        return false;
    }

    id = wpa_network_id_for_ssid(ssid);
    if (id >= 0 && password[0] == '\0') {
        if (!wpa_select_network(id)) {
            copy_string(error, error_size, "Unable to select saved network");
            return false;
        }
        return true;
    }

    if (id >= 0)
        (void)wpa_remove_network(id);

    id = wpa_add_network();
    if (id < 0) {
        copy_string(error, error_size, "Unable to create Wi-Fi network");
        return false;
    }

    quote_wpa_value(ssid, quoted_ssid, sizeof(quoted_ssid));
    if (!wpa_set_network(id, "ssid", quoted_ssid))
        goto setup_failed;

    if (password[0] == '\0') {
        if (!wpa_set_network(id, "key_mgmt", "NONE"))
            goto setup_failed;
    } else {
        quote_wpa_value(password, quoted_password, sizeof(quoted_password));
        if (!wpa_set_network(id, "psk", quoted_password))
            goto setup_failed;
    }

    if (hidden && !wpa_set_network(id, "scan_ssid", "1"))
        goto setup_failed;

    if (!rewrite_wpa_config(ssid, ssid, password, hidden)) {
        (void)wpa_remove_network(id);
        copy_string(error, error_size, "Unable to persist Wi-Fi credentials");
        return false;
    }

    if (!wpa_select_network(id)) {
        copy_string(error, error_size, "Unable to start connection");
        return false;
    }
    return true;

setup_failed:
    (void)wpa_remove_network(id);
    copy_string(error, error_size, "Unable to configure Wi-Fi network");
    return false;
}

static void profile_path(const char *ssid, char *path, size_t path_size);
static void clear_runtime_ip(void);

static bool forget_network(const char *ssid, char *error, size_t error_size)
{
    int id;
    bool removed = false;
    struct wifi_state state;
    bool was_current = false;

    if (!valid_ssid(ssid)) {
        copy_string(error, error_size, "Invalid SSID");
        return false;
    }

    refresh_state(&state);
    was_current = strcmp(state.state, "connected") == 0 && strcmp(state.ssid, ssid) == 0;

    while ((id = wpa_network_id_for_ssid(ssid)) >= 0) {
        if (!wpa_remove_network(id))
            break;
        removed = true;
    }
    if (!rewrite_wpa_config(ssid, NULL, NULL, false)) {
        copy_string(error, error_size, "Unable to update saved networks");
        return false;
    }
    if (!removed && access(WPA_CONF, R_OK) != 0) {
        copy_string(error, error_size, "Network is not saved");
        return false;
    }
    {
        char path[512];
        profile_path(ssid, path, sizeof(path));
        if (unlink(path) != 0 && errno != ENOENT) {
            copy_string(error, error_size, "Network forgotten, but IP profile cleanup failed");
            return false;
        }
    }
    if (was_current)
        clear_runtime_ip();
    return true;
}

static void ssid_to_hex(const char *ssid, char *hex, size_t hex_size)
{
    static const char digits[] = "0123456789abcdef";
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)ssid;
    while (*p != '\0' && used + 2 < hex_size) {
        hex[used++] = digits[*p >> 4];
        hex[used++] = digits[*p & 0x0f];
        p++;
    }
    hex[used] = '\0';
}

static void profile_path(const char *ssid, char *path, size_t path_size)
{
    char hex[257];
    ssid_to_hex(ssid, hex, sizeof(hex));
    snprintf(path, path_size, "%s/%s.conf", WIFI_PROFILE_DIR, hex);
}

static void prefix_to_netmask(uint32_t prefix, char *out, size_t out_size)
{
    uint32_t mask = prefix == 0 ? 0 : htonl(0xffffffffu << (32 - prefix));
    struct in_addr addr;
    addr.s_addr = mask;
    if (inet_ntop(AF_INET, &addr, out, out_size) == NULL)
        copy_string(out, out_size, "");
}


static void load_runtime_ip(struct ip_config *cfg)
{
    char output[4096];
    char *line;
    char *const addr_argv[] = {"ip", "-o", "-4", "addr", "show", "dev", WIFI_IFACE, NULL};
    char *const route_argv[] = {"ip", "-4", "route", "show", "default", "dev", WIFI_IFACE, NULL};
    FILE *fp;

    cfg->runtime_address[0] = '\0';
    cfg->runtime_prefix = 0;
    cfg->runtime_netmask[0] = '\0';
    cfg->runtime_gateway[0] = '\0';
    cfg->runtime_dns[0] = '\0';

    if (run_capture(addr_argv, output, sizeof(output)) == 0) {
        line = strstr(output, " inet ");
        if (line != NULL) {
            char value[64];
            char *slash;
            line += 6;
            if (sscanf(line, "%63s", value) == 1) {
                slash = strchr(value, '/');
                if (slash != NULL) {
                    *slash = '\0';
                    cfg->runtime_prefix = (uint32_t)strtoul(slash + 1, NULL, 10);
                }
                copy_string(cfg->runtime_address, sizeof(cfg->runtime_address), value);
                prefix_to_netmask(cfg->runtime_prefix, cfg->runtime_netmask,
                                  sizeof(cfg->runtime_netmask));
            }
        }
    }

    if (run_capture(route_argv, output, sizeof(output)) == 0) {
        line = strstr(output, " via ");
        if (line != NULL) {
            char gateway[64];
            line += 5;
            if (sscanf(line, "%63s", gateway) == 1)
                copy_string(cfg->runtime_gateway, sizeof(cfg->runtime_gateway), gateway);
        }
    }

    fp = fopen("/run/resolv.conf", "r");
    if (fp == NULL)
        fp = fopen("/etc/resolv.conf", "r");
    if (fp != NULL) {
        char dns_line[256];
        size_t used = 0;
        while (fgets(dns_line, sizeof(dns_line), fp) != NULL) {
            char server[64];
            if (sscanf(dns_line, "nameserver %63s", server) != 1)
                continue;
            if (used > 0 && used + 2 < sizeof(cfg->runtime_dns)) {
                cfg->runtime_dns[used++] = ',';
                cfg->runtime_dns[used++] = ' ';
            }
            if (used + strlen(server) + 1 >= sizeof(cfg->runtime_dns))
                break;
            strcpy(cfg->runtime_dns + used, server);
            used += strlen(server);
        }
        fclose(fp);
    }
}

static void load_ip_profile(const char *ssid, struct ip_config *cfg)
{
    char path[512];
    FILE *fp;
    char line[512];

    memset(cfg, 0, sizeof(*cfg));
    copy_string(cfg->mode, sizeof(cfg->mode), "automatic");
    profile_path(ssid, path, sizeof(path));
    fp = fopen(path, "r");
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp) != NULL) {
            char *eq = strchr(line, '=');
            char *key;
            char *value;
            if (eq == NULL)
                continue;
            *eq = '\0';
            key = line;
            value = eq + 1;
            trim_newline(value);
            if (strcmp(key, "MODE") == 0)
                copy_string(cfg->mode, sizeof(cfg->mode), value);
            else if (strcmp(key, "ADDRESS") == 0)
                copy_string(cfg->address, sizeof(cfg->address), value);
            else if (strcmp(key, "PREFIX") == 0)
                cfg->prefix = (uint32_t)strtoul(value, NULL, 10);
            else if (strcmp(key, "GATEWAY") == 0)
                copy_string(cfg->gateway, sizeof(cfg->gateway), value);
            else if (strcmp(key, "DNS") == 0)
                copy_string(cfg->dns, sizeof(cfg->dns), value);
        }
        fclose(fp);
    }
    prefix_to_netmask(cfg->prefix, cfg->netmask, sizeof(cfg->netmask));
    load_runtime_ip(cfg);
}

static bool save_ip_profile(const char *ssid, const struct ip_config *cfg)
{
    char path[512];
    char tmp[576];
    FILE *fp;
    if (!ensure_state_dirs())
        return false;
    profile_path(ssid, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    fp = fopen(tmp, "w");
    if (fp == NULL)
        return false;
    if (fprintf(fp, "MODE=%s\nADDRESS=%s\nPREFIX=%u\nGATEWAY=%s\nDNS=%s\n",
                cfg->mode, cfg->address, cfg->prefix, cfg->gateway, cfg->dns) < 0 ||
        fflush(fp) != 0 || fchmod(fileno(fp), 0600) != 0 || fclose(fp) != 0) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static bool write_resolv_conf(const char *dns)
{
    FILE *fp = fopen("/run/resolv.conf", "w");
    char copy[256];
    char *saveptr = NULL;
    char *token;
    if (fp == NULL)
        return false;
    copy_string(copy, sizeof(copy), dns == NULL ? "" : dns);
    token = strtok_r(copy, ", ", &saveptr);
    while (token != NULL) {
        if (valid_ipv4_or_empty(token) && token[0] != '\0')
            fprintf(fp, "nameserver %s\n", token);
        token = strtok_r(NULL, ", ", &saveptr);
    }
    if (fflush(fp) != 0) {
        fclose(fp);
        return false;
    }
    return fclose(fp) == 0;
}

static void clear_runtime_ip(void)
{
    char *flush[] = {"ip", "-4", "addr", "flush", "dev", WIFI_IFACE, NULL};
    char *del_default[] = {"ip", "route", "del", "default", "dev", WIFI_IFACE, NULL};
    (void)run_quiet(flush);
    (void)run_quiet(del_default);
    (void)write_resolv_conf("");
}

static bool runtime_matches_manual(const struct ip_config *cfg)
{
    return strcmp(cfg->address, cfg->runtime_address) == 0 &&
           cfg->prefix == cfg->runtime_prefix &&
           strcmp(cfg->gateway, cfg->runtime_gateway) == 0;
}

static bool apply_manual_ip(const struct ip_config *cfg)
{
    char cidr[64];
    char *flush_argv[] = {"ip", "-4", "addr", "flush", "dev", WIFI_IFACE, NULL};
    char *link_argv[] = {"ip", "link", "set", WIFI_IFACE, "up", NULL};
    char *addr_argv[] = {"ip", "addr", "add", cidr, "dev", WIFI_IFACE, NULL};
    char *del_default_argv[] = {"ip", "route", "del", "default", "dev", WIFI_IFACE, NULL};
    char *route_argv[] = {"ip", "route", "replace", "default", "via",
                          (char *)cfg->gateway, "dev", WIFI_IFACE, NULL};

    if (runtime_matches_manual(cfg)) {
        (void)write_resolv_conf(cfg->dns);
        return true;
    }

    snprintf(cidr, sizeof(cidr), "%s/%u", cfg->address, cfg->prefix);
    (void)run_quiet(flush_argv);
    if (run_quiet(link_argv) != 0)
        return false;
    if (run_quiet(addr_argv) != 0)
        return false;
    (void)run_quiet(del_default_argv);
    if (cfg->gateway[0] != '\0' && run_quiet(route_argv) != 0)
        return false;
    return write_resolv_conf(cfg->dns);
}

static void spawn_automatic_ip(void)
{
    pid_t pid = fork();
    if (pid != 0)
        return;
    setsid();
    execlp("udhcpc", "udhcpc", "-i", WIFI_IFACE, "-q", "-n",
           "-t", "5", "-T", "3", "-s", "/usr/libexec/nuubos-wifi-udhcpc",
           (char *)NULL);
    _exit(127);
}

static void apply_ip_profile_now(const char *ssid)
{
    struct ip_config cfg;
    if (!valid_ssid(ssid))
        return;
    load_ip_profile(ssid, &cfg);
    if (strcmp(cfg.mode, "manual") == 0) {
        if (cfg.address[0] != '\0' && cfg.prefix <= 32)
            (void)apply_manual_ip(&cfg);
        return;
    }
    spawn_automatic_ip();
}

static void spawn_apply_ip_profile(const char *ssid)
{
    pid_t pid;
    char ssid_copy[128];
    copy_string(ssid_copy, sizeof(ssid_copy), ssid);
    pid = fork();
    if (pid != 0)
        return;
    setsid();
    apply_ip_profile_now(ssid_copy);
    _exit(0);
}

static bool write_managed_interfaces(void)
{
    char tmp[256];
    FILE *fp;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", INTERFACES_CONF, (long)getpid());
    fp = fopen(tmp, "w");
    if (fp == NULL)
        return false;
    fputs("auto lo\niface lo inet loopback\n", fp);
    if (fflush(fp) != 0 || fchmod(fileno(fp), 0644) != 0 || fclose(fp) != 0) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, INTERFACES_CONF) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static bool file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    if (fp == NULL)
        return false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, needle) != NULL) {
            fclose(fp);
            return true;
        }
    }
    fclose(fp);
    return false;
}

static void migrate_legacy_network(void)
{
    struct wifi_state state;
    struct ip_config cfg;
    char profile[512];
    FILE *marker;

    if (access(WIFI_BACKEND_MARKER, F_OK) == 0)
        return;
    if (!ensure_state_dirs())
        return;

    refresh_state(&state);
    if (file_contains(INTERFACES_CONF, "iface wlan0 inet static")) {
        if (state.ssid[0] == '\0')
            return;
        profile_path(state.ssid, profile, sizeof(profile));
        if (access(profile, F_OK) != 0) {
            memset(&cfg, 0, sizeof(cfg));
            copy_string(cfg.mode, sizeof(cfg.mode), "manual");
            load_runtime_ip(&cfg);
            copy_string(cfg.address, sizeof(cfg.address), cfg.runtime_address);
            cfg.prefix = cfg.runtime_prefix;
            copy_string(cfg.gateway, sizeof(cfg.gateway), cfg.runtime_gateway);
            copy_string(cfg.dns, sizeof(cfg.dns), cfg.runtime_dns);
            prefix_to_netmask(cfg.prefix, cfg.netmask, sizeof(cfg.netmask));
            (void)save_ip_profile(state.ssid, &cfg);
        }
    }

    if (!write_managed_interfaces())
        return;
    marker = fopen(WIFI_BACKEND_MARKER, "w");
    if (marker != NULL) {
        fputs("2\n", marker);
        fclose(marker);
        (void)chmod(WIFI_BACKEND_MARKER, 0600);
    }
}

static bool set_enabled(bool enabled, char *error, size_t error_size)
{
    char *link_up[] = {"ip", "link", "set", WIFI_IFACE, "up", NULL};
    char *link_down[] = {"ip", "link", "set", WIFI_IFACE, "down", NULL};
    char *flush[] = {"ip", "-4", "addr", "flush", "dev", WIFI_IFACE, NULL};
    char *del_default[] = {"ip", "route", "del", "default", "dev", WIFI_IFACE, NULL};
    char output[256];

    if (!write_enabled_preference(enabled)) {
        copy_string(error, error_size, "Unable to persist Wi-Fi state");
        return false;
    }

    if (!enabled) {
        char *const disconnect[] = {"wpa_cli", "-i", WIFI_IFACE, "disconnect", NULL};
        (void)run_capture(disconnect, output, sizeof(output));
        (void)run_quiet(flush);
        (void)run_quiet(del_default);
        (void)write_resolv_conf("");
        if (run_quiet(link_down) != 0) {
            copy_string(error, error_size, "Unable to disable Wi-Fi interface");
            return false;
        }
        return true;
    }

    if (run_quiet(link_up) != 0) {
        copy_string(error, error_size, "Unable to enable Wi-Fi interface");
        return false;
    }
    if (!ensure_wpa_running()) {
        copy_string(error, error_size, "Unable to start wpa_supplicant");
        return false;
    }
    {
        char *const reconnect[] = {"wpa_cli", "-i", WIFI_IFACE, "reconnect", NULL};
        (void)run_capture(reconnect, output, sizeof(output));
    }
    return true;
}

static bool request_scan(char *error, size_t error_size)
{
    char output[256];
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "scan", NULL};
    if (!read_enabled_preference()) {
        copy_string(error, error_size, "Wi-Fi is disabled");
        return false;
    }
    /* Without the monitor the completion would never be seen. */
    if (!ensure_wpa_running() || wpa_monitor_fd < 0) {
        copy_string(error, error_size, "wpa_supplicant is unavailable");
        return false;
    }
    /* FAIL-BUSY: wpa_supplicant is already scanning on its own (reconnect,
     * autoscan); its CTRL-EVENT-SCAN-RESULTS completes this request too. */
    if (run_capture(argv, output, sizeof(output)) != 0 ||
        (strstr(output, "OK") == NULL && strstr(output, "FAIL-BUSY") == NULL)) {
        copy_string(error, error_size, "Unable to start Wi-Fi scan");
        return false;
    }
    return true;
}

static void abort_scan(void)
{
    char output[256];
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "abort_scan", NULL};

    (void)run_capture(argv, output, sizeof(output));
}

static bool disconnect_current(char *error, size_t error_size)
{
    char output[256];
    char *flush[] = {"ip", "-4", "addr", "flush", "dev", WIFI_IFACE, NULL};
    char *del_default[] = {"ip", "route", "del", "default", "dev", WIFI_IFACE, NULL};
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "disconnect", NULL};
    if (run_capture(argv, output, sizeof(output)) != 0 || strstr(output, "OK") == NULL) {
        copy_string(error, error_size, "Unable to disconnect Wi-Fi");
        return false;
    }
    (void)run_quiet(flush);
    (void)run_quiet(del_default);
    (void)write_resolv_conf("");
    return true;
}

/* Strongest BSSID of SSID that advertises WPS; empty when none does, so
 * wpa_supplicant then looks for any access point in push-button mode. */
static void wps_bssid_for_ssid(const char *ssid, char *bssid, size_t bssid_size)
{
    char output[MAX_CAPTURE];
    char *saveptr = NULL;
    char *line;
    int32_t best = INT32_MIN;
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "scan_results", NULL};

    copy_string(bssid, bssid_size, "");
    if (ssid[0] == '\0' || run_capture(argv, output, sizeof(output)) != 0)
        return;
    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL) {
        char *fields[5] = {0};
        char *cursor = line;
        int field = 0;
        char *tab;

        while (field < 4 && (tab = strchr(cursor, '\t')) != NULL) {
            *tab = '\0';
            fields[field++] = cursor;
            cursor = tab + 1;
        }
        fields[field++] = cursor;
        if (field == 5 && strcmp(fields[4], ssid) == 0 &&
            strstr(fields[3], "[WPS") != NULL) {
            int32_t signal = (int32_t)strtol(fields[2], NULL, 10);
            if (signal > best) {
                best = signal;
                copy_string(bssid, bssid_size, fields[0]);
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
}

static bool start_wps(const char *ssid, char *error, size_t error_size)
{
    char output[256];
    char bssid[32];
    char *const set_update[] = {"wpa_cli", "-i", WIFI_IFACE, "set",
                                "update_config", "1", NULL};
    char *const pbc_any[] = {"wpa_cli", "-i", WIFI_IFACE, "wps_pbc", NULL};
    char *const pbc_bssid[] = {"wpa_cli", "-i", WIFI_IFACE, "wps_pbc", bssid, NULL};

    if (ssid[0] != '\0' && !valid_ssid(ssid)) {
        copy_string(error, error_size, "Invalid SSID");
        return false;
    }
    if (!read_enabled_preference()) {
        copy_string(error, error_size, "Wi-Fi is disabled");
        return false;
    }
    if (!ensure_wpa_running()) {
        copy_string(error, error_size, "wpa_supplicant is unavailable");
        return false;
    }
    /* wpa_supplicant persists the received credentials itself, but only
     * with update_config set (older STATE files carry update_config=0). */
    if (run_capture(set_update, output, sizeof(output)) != 0 || strstr(output, "OK") == NULL) {
        copy_string(error, error_size, "Unable to prepare WPS");
        return false;
    }
    wps_bssid_for_ssid(ssid, bssid, sizeof(bssid));
    if (run_capture(bssid[0] != '\0' ? pbc_bssid : pbc_any, output, sizeof(output)) != 0 ||
        strstr(output, "OK") == NULL) {
        copy_string(error, error_size, "WPS is not supported");
        return false;
    }
    return true;
}

static void cancel_wps(void)
{
    char output[256];
    char *const argv[] = {"wpa_cli", "-i", WIFI_IFACE, "wps_cancel", NULL};

    (void)run_capture(argv, output, sizeof(output));
}

static bool set_ip_configuration(const char *ssid, const char *mode,
                                 const char *address, uint32_t prefix,
                                 const char *gateway, const char *dns,
                                 char *error, size_t error_size)
{
    struct ip_config cfg;
    struct wifi_state state;

    if (!valid_ssid(ssid)) {
        copy_string(error, error_size, "Invalid SSID");
        return false;
    }
    memset(&cfg, 0, sizeof(cfg));
    if (strcmp(mode, "automatic") == 0) {
        copy_string(cfg.mode, sizeof(cfg.mode), "automatic");
    } else if (strcmp(mode, "manual") == 0) {
        if (!valid_ipv4_or_empty(address) || address[0] == '\0' || prefix > 32 ||
            !valid_ipv4_or_empty(gateway) || !valid_dns_list(dns)) {
            copy_string(error, error_size, "Manual IPv4 configuration is invalid");
            return false;
        }
        copy_string(cfg.mode, sizeof(cfg.mode), "manual");
        copy_string(cfg.address, sizeof(cfg.address), address);
        cfg.prefix = prefix;
        copy_string(cfg.gateway, sizeof(cfg.gateway), gateway);
        copy_string(cfg.dns, sizeof(cfg.dns), dns);
        prefix_to_netmask(prefix, cfg.netmask, sizeof(cfg.netmask));
    } else {
        copy_string(error, error_size, "IP mode must be automatic or manual");
        return false;
    }

    if (!save_ip_profile(ssid, &cfg)) {
        copy_string(error, error_size, "Unable to persist IP configuration");
        return false;
    }
    refresh_state(&state);
    if (strcmp(state.state, "connected") == 0 && strcmp(state.ssid, ssid) == 0)
        spawn_apply_ip_profile(ssid);
    return true;
}

static bool append_snapshot(DBusMessage *message, const struct wifi_state *state)
{
    dbus_bool_t enabled = state->enabled ? TRUE : FALSE;
    const char *status = state->state;
    const char *ssid = state->ssid;
    const char *ipv4 = state->ipv4;
    dbus_int32_t signal_dbm = state->signal_dbm;
    return dbus_message_append_args(message,
        DBUS_TYPE_BOOLEAN, &enabled,
        DBUS_TYPE_STRING, &status,
        DBUS_TYPE_STRING, &ssid,
        DBUS_TYPE_STRING, &ipv4,
        DBUS_TYPE_INT32, &signal_dbm,
        DBUS_TYPE_INVALID);
}

static bool append_network_array(DBusMessage *message,
                                 const struct wifi_network *networks, size_t count)
{
    DBusMessageIter iter;
    DBusMessageIter array;
    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ssibb)", &array))
        return false;
    for (size_t i = 0; i < count; i++) {
        DBusMessageIter item;
        const char *ssid = networks[i].ssid;
        const char *security = networks[i].security;
        dbus_int32_t signal = networks[i].signal_dbm;
        dbus_bool_t saved = networks[i].saved ? TRUE : FALSE;
        dbus_bool_t current = networks[i].current ? TRUE : FALSE;
        if (!dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &item))
            return false;
        if (!dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &ssid) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &security) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_INT32, &signal) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_BOOLEAN, &saved) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_BOOLEAN, &current))
            return false;
        if (!dbus_message_iter_close_container(&array, &item))
            return false;
    }
    return dbus_message_iter_close_container(&iter, &array);
}

static void emit_state_changed(DBusConnection *conn, const struct wifi_state *state)
{
    DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "StateChanged");
    if (signal == NULL)
        return;
    if (append_snapshot(signal, state)) {
        (void)dbus_connection_send(conn, signal, NULL);
        dbus_connection_flush(conn);
    }
    dbus_message_unref(signal);
}

static void emit_bool_signal(DBusConnection *conn, const char *name, bool value)
{
    DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, name);
    dbus_bool_t dbus_value = value ? TRUE : FALSE;
    if (signal == NULL)
        return;
    if (dbus_message_append_args(signal, DBUS_TYPE_BOOLEAN, &dbus_value, DBUS_TYPE_INVALID)) {
        (void)dbus_connection_send(conn, signal, NULL);
        dbus_connection_flush(conn);
    }
    dbus_message_unref(signal);
}

static void emit_empty_signal(DBusConnection *conn, const char *name)
{
    DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, name);
    if (signal == NULL)
        return;
    (void)dbus_connection_send(conn, signal, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(signal);
}

static void emit_networks_snapshot(DBusConnection *conn, const char *current_ssid)
{
    struct wifi_network networks[MAX_NETWORKS];
    size_t count = load_scan_networks(networks, MAX_NETWORKS, current_ssid);
    DBusMessage *signal =
        dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "NetworksSnapshotChanged");

    if (signal == NULL)
        return;
    if (append_network_array(signal, networks, count)) {
        (void)dbus_connection_send(conn, signal, NULL);
        dbus_connection_flush(conn);
    }
    dbus_message_unref(signal);
}

static void emit_string_signal(DBusConnection *conn, const char *name, const char *value)
{
    DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, name);
    const char *text = value;
    if (signal == NULL)
        return;
    if (dbus_message_append_args(signal, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID)) {
        (void)dbus_connection_send(conn, signal, NULL);
        dbus_connection_flush(conn);
    }
    dbus_message_unref(signal);
}

static void emit_operation_failed(DBusConnection *conn, const char *operation,
                                  const char *message)
{
    DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "OperationFailed");
    const char *op = operation;
    const char *msg = message;
    if (signal == NULL)
        return;
    if (dbus_message_append_args(signal,
                                 DBUS_TYPE_STRING, &op,
                                 DBUS_TYPE_STRING, &msg,
                                 DBUS_TYPE_INVALID)) {
        (void)dbus_connection_send(conn, signal, NULL);
        dbus_connection_flush(conn);
    }
    dbus_message_unref(signal);
}

static void reply_error(DBusConnection *conn, DBusMessage *request,
                        const char *name, const char *text)
{
    DBusMessage *reply = dbus_message_new_error(request, name, text);
    if (reply == NULL)
        return;
    (void)dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void reply_empty(DBusConnection *conn, DBusMessage *request)
{
    DBusMessage *reply = dbus_message_new_method_return(request);
    if (reply == NULL)
        return;
    (void)dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}


static void post_wifi_notification(const char *event, const char *ssid)
{
    struct nuubos_notify n;

    nuubos_notify_begin(&n, "POST", "wifi", event);
    nuubos_notify_str(&n, "ssid", ssid);
    (void)nuubos_notify_send(&n);
}

static void handle_message(DBusConnection *conn, DBusMessage *message,
                           struct wifi_state *state)
{
    DBusMessage *reply;

    /* A user action supersedes a pending unrequested loss. */
    if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetEnabled") ||
        dbus_message_is_method_call(message, INTERFACE_NAME, "Connect") ||
        dbus_message_is_method_call(message, INTERFACE_NAME, "Disconnect") ||
        dbus_message_is_method_call(message, INTERFACE_NAME, "Forget") ||
        dbus_message_is_method_call(message, INTERFACE_NAME, "StartWps"))
        loss_due_ms = 0;

    /* Connecting, disconnecting or turning Wi-Fi off ends a WPS session. */
    if (wps_active &&
        (dbus_message_is_method_call(message, INTERFACE_NAME, "SetEnabled") ||
         dbus_message_is_method_call(message, INTERFACE_NAME, "Connect") ||
         dbus_message_is_method_call(message, INTERFACE_NAME, "Disconnect"))) {
        cancel_wps();
        wps_active = false;
        emit_bool_signal(conn, "WpsStateChanged", false);
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetSnapshot")) {
        struct wifi_state next;
        refresh_state(&next);
        if (!state_equal(state, &next)) {
            *state = next;
            emit_state_changed(conn, state);
        }
        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;
        if (!append_snapshot(reply, state)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY, "Unable to serialize Wi-Fi snapshot");
            return;
        }
        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetEnabled")) {
        DBusError error = DBUS_ERROR_INIT;
        dbus_bool_t enabled = FALSE;
        char text[160];
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_BOOLEAN, &enabled,
                                   DBUS_TYPE_INVALID)) {
            reply_error(conn, message, DBUS_ERROR_INVALID_ARGS,
                        dbus_error_is_set(&error) ? error.message : "Expected boolean enabled");
            if (dbus_error_is_set(&error)) dbus_error_free(&error);
            return;
        }
        if (!set_enabled(enabled != FALSE, text, sizeof(text))) {
            reply_error(conn, message, "org.nuubOS.Wifi.Error.OperationFailed", text);
            return;
        }
        if (enabled == FALSE) {
            if (scan_in_flight)
                abort_scan();
            scan_session_active = false;
            scan_in_flight = false;
            next_scan_due_ms = 0;
            emit_bool_signal(conn, "ScanStateChanged", false);
        }
        refresh_state(state);
        emit_state_changed(conn, state);
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "Scan")) {
        char text[160];
        if (!scan_in_flight && !request_scan(text, sizeof(text))) {
            reply_error(conn, message, "org.nuubOS.Wifi.Error.ScanFailed", text);
            return;
        }
        scan_in_flight = true;
        emit_bool_signal(conn, "ScanStateChanged", true);
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "StartScanSession")) {
        char text[160];

        scan_session_active = true;
        refresh_state(state);
        emit_networks_snapshot(conn, state->ssid);
        emit_empty_signal(conn, "NetworksChanged");

        if (!scan_in_flight && !request_scan(text, sizeof(text))) {
            scan_session_active = false;
            reply_error(conn, message, "org.nuubOS.Wifi.Error.ScanFailed", text);
            return;
        }
        scan_in_flight = true;
        next_scan_due_ms = 0;
        emit_bool_signal(conn, "ScanStateChanged", true);
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "StopScanSession")) {
        scan_session_active = false;
        next_scan_due_ms = 0;
        if (scan_in_flight)
            abort_scan();
        scan_in_flight = false;
        emit_bool_signal(conn, "ScanStateChanged", false);
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetNetworks") ||
        dbus_message_is_method_call(message, INTERFACE_NAME, "GetSavedNetworks")) {
        struct wifi_state current;
        struct wifi_network networks[MAX_NETWORKS];
        size_t count;
        refresh_state(&current);
        if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetNetworks"))
            count = load_scan_networks(networks, MAX_NETWORKS, current.ssid);
        else
            count = load_saved_networks(networks, MAX_NETWORKS, current.ssid);
        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;
        if (!append_network_array(reply, networks, count)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY, "Unable to serialize networks");
            return;
        }
        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "Connect")) {
        DBusError error = DBUS_ERROR_INIT;
        const char *ssid = NULL;
        const char *password = NULL;
        dbus_bool_t hidden = FALSE;
        char text[192];
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &ssid,
                                   DBUS_TYPE_STRING, &password,
                                   DBUS_TYPE_BOOLEAN, &hidden,
                                   DBUS_TYPE_INVALID)) {
            reply_error(conn, message, DBUS_ERROR_INVALID_ARGS,
                        dbus_error_is_set(&error) ? error.message : "Expected ssid,password,hidden");
            if (dbus_error_is_set(&error)) dbus_error_free(&error);
            return;
        }
        if (!connect_network(ssid, password, hidden != FALSE, text, sizeof(text))) {
            reply_error(conn, message, "org.nuubOS.Wifi.Error.ConnectFailed", text);
            return;
        }
        refresh_state(state);
        emit_state_changed(conn, state);
        emit_networks_snapshot(conn, state->ssid);
        emit_empty_signal(conn, "NetworksChanged");
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "Disconnect")) {
        char text[160];
        if (!disconnect_current(text, sizeof(text))) {
            reply_error(conn, message, "org.nuubOS.Wifi.Error.DisconnectFailed", text);
            return;
        }
        refresh_state(state);
        emit_state_changed(conn, state);
        emit_networks_snapshot(conn, state->ssid);
        emit_empty_signal(conn, "NetworksChanged");
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "StartWps")) {
        DBusError error = DBUS_ERROR_INIT;
        const char *ssid = NULL;
        char text[160];
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &ssid,
                                   DBUS_TYPE_INVALID)) {
            reply_error(conn, message, DBUS_ERROR_INVALID_ARGS,
                        dbus_error_is_set(&error) ? error.message : "Expected SSID");
            if (dbus_error_is_set(&error)) dbus_error_free(&error);
            return;
        }
        if (!start_wps(ssid, text, sizeof(text))) {
            reply_error(conn, message, "org.nuubOS.Wifi.Error.WpsFailed", text);
            return;
        }
        wps_active = true;
        emit_bool_signal(conn, "WpsStateChanged", true);
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "CancelWps")) {
        if (wps_active) {
            cancel_wps();
            wps_active = false;
            emit_bool_signal(conn, "WpsStateChanged", false);
        }
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetWpsActive")) {
        dbus_bool_t active = wps_active ? TRUE : FALSE;
        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;
        if (!dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY, "Unable to serialize WPS state");
            return;
        }
        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "Forget")) {
        DBusError error = DBUS_ERROR_INIT;
        const char *ssid = NULL;
        char text[160];
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &ssid,
                                   DBUS_TYPE_INVALID)) {
            reply_error(conn, message, DBUS_ERROR_INVALID_ARGS,
                        dbus_error_is_set(&error) ? error.message : "Expected SSID");
            if (dbus_error_is_set(&error)) dbus_error_free(&error);
            return;
        }
        if (!forget_network(ssid, text, sizeof(text))) {
            reply_error(conn, message, "org.nuubOS.Wifi.Error.ForgetFailed", text);
            return;
        }
        refresh_state(state);
        emit_networks_snapshot(conn, state->ssid);
        emit_empty_signal(conn, "NetworksChanged");
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetIpConfiguration")) {
        DBusError error = DBUS_ERROR_INIT;
        const char *ssid = NULL;
        struct ip_config cfg;
        const char *mode;
        const char *address;
        const char *netmask;
        const char *gateway;
        const char *dns;
        const char *runtime_address;
        const char *runtime_netmask;
        const char *runtime_gateway;
        const char *runtime_dns;
        dbus_uint32_t prefix;
        dbus_uint32_t runtime_prefix;
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &ssid,
                                   DBUS_TYPE_INVALID) || !valid_ssid(ssid)) {
            reply_error(conn, message, DBUS_ERROR_INVALID_ARGS,
                        dbus_error_is_set(&error) ? error.message : "Expected valid SSID");
            if (dbus_error_is_set(&error)) dbus_error_free(&error);
            return;
        }
        load_ip_profile(ssid, &cfg);
        mode = cfg.mode;
        address = cfg.address;
        prefix = cfg.prefix;
        netmask = cfg.netmask;
        gateway = cfg.gateway;
        dns = cfg.dns;
        runtime_address = cfg.runtime_address;
        runtime_prefix = cfg.runtime_prefix;
        runtime_netmask = cfg.runtime_netmask;
        runtime_gateway = cfg.runtime_gateway;
        runtime_dns = cfg.runtime_dns;
        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;
        if (!dbus_message_append_args(reply,
                                      DBUS_TYPE_STRING, &mode,
                                      DBUS_TYPE_STRING, &address,
                                      DBUS_TYPE_UINT32, &prefix,
                                      DBUS_TYPE_STRING, &netmask,
                                      DBUS_TYPE_STRING, &gateway,
                                      DBUS_TYPE_STRING, &dns,
                                      DBUS_TYPE_STRING, &runtime_address,
                                      DBUS_TYPE_UINT32, &runtime_prefix,
                                      DBUS_TYPE_STRING, &runtime_netmask,
                                      DBUS_TYPE_STRING, &runtime_gateway,
                                      DBUS_TYPE_STRING, &runtime_dns,
                                      DBUS_TYPE_INVALID)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY, "Unable to serialize IP configuration");
            return;
        }
        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetIpConfiguration")) {
        DBusError error = DBUS_ERROR_INIT;
        const char *ssid = NULL;
        const char *mode = NULL;
        const char *address = NULL;
        dbus_uint32_t prefix = 0;
        const char *gateway = NULL;
        const char *dns = NULL;
        char text[192];
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &ssid,
                                   DBUS_TYPE_STRING, &mode,
                                   DBUS_TYPE_STRING, &address,
                                   DBUS_TYPE_UINT32, &prefix,
                                   DBUS_TYPE_STRING, &gateway,
                                   DBUS_TYPE_STRING, &dns,
                                   DBUS_TYPE_INVALID)) {
            reply_error(conn, message, DBUS_ERROR_INVALID_ARGS,
                        dbus_error_is_set(&error) ? error.message : "Invalid IP configuration arguments");
            if (dbus_error_is_set(&error)) dbus_error_free(&error);
            return;
        }
        if (!set_ip_configuration(ssid, mode, address, prefix, gateway, dns,
                                  text, sizeof(text))) {
            reply_error(conn, message, "org.nuubOS.Wifi.Error.IpConfigurationFailed", text);
            return;
        }
        emit_string_signal(conn, "IpConfigurationChanged", ssid);
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTROSPECT_IFACE, "Introspect")) {
        const char *xml = introspection_xml;
        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;
        if (!dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY, "Unable to serialize introspection data");
            return;
        }
        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL)
        reply_error(conn, message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
}

static void finish_wps(DBusConnection *conn, const char *failure)
{
    struct wifi_state state;

    if (!wps_active)
        return;
    wps_active = false;
    emit_bool_signal(conn, "WpsStateChanged", false);
    if (failure != NULL) {
        /* Stable codes: the UI localizes them. */
        emit_operation_failed(conn, "wps", failure);
        return;
    }
    /* wpa_supplicant wrote the new network with group access. */
    (void)chmod(WPA_CONF, 0600);
    refresh_state(&state);
    emit_networks_snapshot(conn, state.ssid);
    emit_empty_signal(conn, "NetworksChanged");
}

/* Events are handled in arrival order, so a WPS-SUCCESS is processed
 * before the CTRL-EVENT-CONNECTED that follows it. */
static void handle_wpa_event(DBusConnection *conn, const char *event)
{
    if (event[0] == '<' && (event = strchr(event, '>')) != NULL)
        event++;
    if (event == NULL)
        return;

    if (strncmp(event, "CTRL-EVENT-SCAN-RESULTS", 23) == 0)
        scan_results_requested = true;
    else if (strncmp(event, "CTRL-EVENT-SCAN-FAILED", 22) == 0) {
        emit_operation_failed(conn, "scan", "Wi-Fi scan failed");
        scan_results_requested = true;
    } else if (strncmp(event, "CTRL-EVENT-CONNECTED", 20) == 0 ||
               strncmp(event, "CTRL-EVENT-DISCONNECTED", 23) == 0)
        refresh_requested = 1;
    else if (strncmp(event, "CTRL-EVENT-SSID-TEMP-DISABLED", 29) == 0) {
        if (strstr(event, "WRONG_KEY") != NULL)
            emit_operation_failed(conn, "connect", "Authentication failed: wrong password");
        else if (strstr(event, "AUTH_FAILED") != NULL)
            emit_operation_failed(conn, "connect", "Authentication failed");
        refresh_requested = 1;
    } else if (strncmp(event, "WPS-SUCCESS", 11) == 0)
        finish_wps(conn, NULL);
    else if (strncmp(event, "WPS-TIMEOUT", 11) == 0)
        finish_wps(conn, "timeout");
    else if (strncmp(event, "WPS-OVERLAP-DETECTED", 20) == 0) {
        /* wpa_supplicant keeps waiting for the walk time: stop now. */
        if (wps_active)
            cancel_wps();
        finish_wps(conn, "overlap");
    } else if (strncmp(event, "WPS-FAIL", 8) == 0)
        finish_wps(conn, "failed");
    else if (strncmp(event, "CTRL-EVENT-TERMINATING", 22) == 0) {
        /* The next ensure_wpa_running() attaches to the new instance. */
        close_wpa_monitor();
        refresh_requested = 1;
    }
}

static void drain_wpa_monitor(DBusConnection *conn)
{
    char event[4096];
    ssize_t len;

    while (wpa_monitor_fd >= 0) {
        len = recv(wpa_monitor_fd, event, sizeof(event) - 1, 0);
        if (len < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                close_wpa_monitor();
            return;
        }
        event[len] = '\0';
        trim_newline(event);
        handle_wpa_event(conn, event);
    }
}

int main(void)
{
    DBusConnection *conn;
    DBusError error = DBUS_ERROR_INIT;
    struct sigaction sa;
    struct wifi_state current;
    int request_result;
    int dbus_fd = -1;

    if (!open_wake_pipe())
        fprintf(stderr, "nuubos-wifid: wake pipe failed: %s\n", strerror(errno));
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGUSR1, &sa, NULL);
    /* Child exits (IP profile/DHCP helpers) wake the loop so they are reaped. */
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    (void)sigaction(SIGCHLD, &sa, NULL);

    if (!ensure_state_dirs()) {
        fprintf(stderr, "nuubos-wifid: unable to prepare Wi-Fi STATE\n");
        return 1;
    }

    migrate_legacy_network();

    if (read_enabled_preference()) {
        char *link_up[] = {"ip", "link", "set", WIFI_IFACE, "up", NULL};
        (void)run_quiet(link_up);
        (void)ensure_wpa_running();
    } else {
        char *link_down[] = {"ip", "link", "set", WIFI_IFACE, "down", NULL};
        (void)run_quiet(link_down);
    }

    conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (conn == NULL) {
        fprintf(stderr, "nuubos-wifid: system bus unavailable: %s\n",
                dbus_error_is_set(&error) ? error.message : "unknown error");
        if (dbus_error_is_set(&error)) dbus_error_free(&error);
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    /*
     * Queue the well-known name instead of failing while the previous owner exits.
     *
     * SysV restart sends SIGTERM to the previous daemon and may start the
     * replacement before the old process has returned from its blocking
     * D-Bus read.  Queueing gives us an atomic D-Bus handover: the new
     * connection becomes owner as soon as the previous owner disconnects,
     * without polling, sleeps, retries, or a service rebind workaround.
     */
    request_result = dbus_bus_request_name(conn, SERVICE_NAME, 0, &error);
    if (dbus_error_is_set(&error) ||
        (request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
         request_result != DBUS_REQUEST_NAME_REPLY_IN_QUEUE &&
         request_result != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER)) {
        fprintf(stderr, "nuubos-wifid: cannot acquire or queue %s: %s\n",
                SERVICE_NAME,
                dbus_error_is_set(&error) ? error.message : "unexpected D-Bus reply");
        if (dbus_error_is_set(&error)) dbus_error_free(&error);
        dbus_connection_unref(conn);
        return 1;
    }

    refresh_state(&current);
    if (strcmp(current.state, "connected") == 0)
        spawn_apply_ip_profile(current.ssid);

    if (!dbus_connection_get_unix_fd(conn, &dbus_fd))
        dbus_fd = -1;

    while (running) {
        DBusMessage *message;
        if (dbus_fd >= 0 && wake_pipe[0] >= 0) {
            wait_for_work(conn, dbus_fd);
            (void)dbus_connection_read_write(conn, 0);
        } else {
            (void)dbus_connection_read_write(conn, 1000);
        }
        while ((message = dbus_connection_pop_message(conn)) != NULL) {
            handle_message(conn, message, &current);
            dbus_message_unref(message);
        }
        drain_wpa_monitor(conn);

        if (refresh_requested) {
            struct wifi_state next;
            refresh_requested = 0;
            refresh_state(&next);
            if (!state_equal(&current, &next)) {
                bool newly_connected = strcmp(next.state, "connected") == 0 &&
                    (strcmp(current.state, "connected") != 0 || strcmp(current.ssid, next.ssid) != 0);
                /* User-initiated Disconnect/Forget/Off update the state in
                 * their handlers, so a drop observed here was not requested.
                 * A rekey/handshake keeps the association (same SSID) and is
                 * not a loss. */
                bool lost = strcmp(current.state, "connected") == 0 &&
                    strcmp(next.state, "connected") != 0 && next.enabled &&
                    strcmp(next.ssid, current.ssid) != 0 && !wps_active;
                char lost_ssid[sizeof(current.ssid)];
                copy_string(lost_ssid, sizeof(lost_ssid), current.ssid);
                current = next;
                emit_state_changed(conn, &current);
                if (newly_connected) {
                    bool resumed = loss_due_ms != 0 &&
                        strcmp(loss_ssid, current.ssid) == 0;
                    loss_due_ms = 0;
                    spawn_apply_ip_profile(current.ssid);
                    if (!resumed)
                        post_wifi_notification("wifi.connected", current.ssid);
                } else if (lost && loss_due_ms == 0) {
                    copy_string(loss_ssid, sizeof(loss_ssid), lost_ssid);
                    loss_due_ms = monotonic_ms() + LOSS_GRACE_MS;
                    loss_suspended_ms = suspended_ms();
                }
            }
        }

        if (loss_due_ms != 0 && monotonic_ms() >= loss_due_ms) {
            uint64_t suspended = suspended_ms();
            if (suspended > loss_suspended_ms + 1000ULL) {
                /* Slept while the loss was pending: the reconnect starts
                 * only now, so the grace window restarts from the resume. */
                loss_suspended_ms = suspended;
                loss_due_ms = monotonic_ms() + LOSS_GRACE_MS;
            } else {
                loss_due_ms = 0;
                if (current.enabled && strcmp(current.state, "connected") != 0)
                    post_wifi_notification("wifi.lost", loss_ssid);
            }
        }

        if (scan_results_requested) {
            scan_results_requested = false;
            refresh_state(&current);
            emit_networks_snapshot(conn, current.ssid);
            emit_empty_signal(conn, "NetworksChanged");
            /* wpa_supplicant also scans on its own: only a requested scan
             * ends the scanning state and schedules the next one. */
            if (scan_in_flight) {
                scan_in_flight = false;
                emit_bool_signal(conn, "ScanStateChanged", false);
                if (scan_session_active)
                    next_scan_due_ms = monotonic_ms() + SCAN_SESSION_INTERVAL_MS;
            }
        }

        if (scan_session_active && !scan_in_flight && next_scan_due_ms != 0 &&
            monotonic_ms() >= next_scan_due_ms) {
            char text[160];
            if (request_scan(text, sizeof(text))) {
                scan_in_flight = true;
                next_scan_due_ms = 0;
                emit_bool_signal(conn, "ScanStateChanged", true);
            } else {
                emit_operation_failed(conn, "scan", text);
                next_scan_due_ms = monotonic_ms() + SCAN_SESSION_INTERVAL_MS;
            }
        }

        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;

        dbus_connection_flush(conn);
        if (!dbus_connection_get_is_connected(conn))
            break;
    }

    close_wpa_monitor();
    dbus_connection_unref(conn);
    if (dbus_error_is_set(&error)) dbus_error_free(&error);
    return 0;
}
