
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/if_link.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <nuubos/notify.h>

#ifndef CLOCK_BOOTTIME_ALARM
#define CLOCK_BOOTTIME_ALARM 9
#endif

#define SOCKET_PATH "/run/nuubos/systemd.sock"
#define PIDFILE "/run/nuubos/systemd.pid"
#define CONFIG_FILE "/state/config/nuubos-system.conf"
#define STATUS_STATE "/run/nuubos/statusd.state"
#define INPUT_SOCKET "/run/nuubos/inputd.sock"
#define STATUS_SOCKET "/run/nuubos/statusd.sock"
#define UI_CONTROL "/run/nuubos/ui-control"
#define LIFECYCLE_LIGHTING "/usr/lib/nuubos/lifecycle-lighting"
#define LIFECYCLE_DISPLAY "/usr/lib/nuubos/lifecycle-display"
#define LIFECYCLE_PRE_POWER "/usr/lib/nuubos/lifecycle-pre-power"
#define MAX_CLIENTS 16

#ifndef NUUBOS_PRODUCT_VERSION
#define NUUBOS_PRODUCT_VERSION "unknown"
#endif

#ifndef NUUBOS_BUILD_ID
#define NUUBOS_BUILD_ID "unknown"
#endif

enum profile_mode {
    PROFILE_AUTO = 0,
    PROFILE_BATTERY_SAVER
};

enum idle_stage {
    IDLE_ACTIVE = 0,
    IDLE_SCREENSAVER,
    IDLE_SLEEPING
};

struct config_state {
    enum profile_mode requested_profile;
    int auto_battery_threshold; /* 0=off */
    int screensaver_after_min;  /* 0=off */
    int sleep_after_min;        /* 0=off, duration after previous stage */
    int poweroff_after_min;     /* 0=off, duration after previous stage */
};

struct runtime_state {
    struct config_state cfg;
    enum profile_mode effective_profile;
    int battery_percent;
    char battery_state[32];
    enum idle_stage idle_stage;
    long long stage_enter_ms;
    bool rtc_wakeup_supported;
    bool rtc_alarm_armed;
    char last_alarm_backend[24];
    char last_wake_reason[24];
    long long last_sleep_elapsed_ms;

    /* Storage operations can copy many GiB. Keep them outside the Product
     * Service event loop and expose their state as ordinary Product state. */
    pid_t storage_job_pid;
    char storage_job_action[32];
    char storage_job_state[16];
    int storage_job_exit_code;

    bool notifications_ready;
};

struct client {
    int fd;
    bool subscribed;
    char buf[512];
    size_t used;
};

static volatile sig_atomic_t stop_requested;
static struct client clients[MAX_CLIENTS];

static long long boottime_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static const char *idle_stage_name(enum idle_stage stage)
{
    switch (stage) {
    case IDLE_ACTIVE: return "active";
    case IDLE_SCREENSAVER: return "screensaver";
    case IDLE_SLEEPING: return "sleep";
    default: return "active";
    }
}

static const char *profile_name(enum profile_mode p)
{
    switch (p) {
    case PROFILE_AUTO: return "auto";
    case PROFILE_BATTERY_SAVER: return "battery-saver";
    default: return "auto";
    }
}

static bool parse_profile(const char *s, enum profile_mode *out)
{
    if (!strcmp(s, "auto")) *out = PROFILE_AUTO;
    else if (!strcmp(s, "battery-saver")) *out = PROFILE_BATTERY_SAVER;
    else return false;
    return true;
}

static void signal_handler(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int write_all(int fd, const char *s)
{
    size_t left = strlen(s);
    const char *p = s;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static int read_int_file(const char *path, long long *out)
{
    FILE *f = fopen(path, "r");
    long long v;
    if (!f) return -1;
    if (fscanf(f, "%lld", &v) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = v;
    return 0;
}

static int read_text_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f) return -1;
    n = fread(buf, 1, size - 1, f);
    fclose(f);
    if (!n) return -1;
    buf[n] = '\0';
    while (n && (buf[n-1]=='\n' || buf[n-1]=='\r' || buf[n-1]=='\0'))
        buf[--n]='\0';
    return 0;
}

static int atomic_save_config(const struct config_state *cfg)
{
    char tmp[256];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", CONFIG_FILE, (long)getpid());
    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "SYSTEM_CONFIG_VERSION=2\n");
    fprintf(f, "PERFORMANCE_PROFILE=%s\n", profile_name(cfg->requested_profile));
    fprintf(f, "AUTO_BATTERY_SAVER_THRESHOLD=%d\n", cfg->auto_battery_threshold);
    fprintf(f, "SCREENSAVER_AFTER_MIN=%d\n", cfg->screensaver_after_min);
    fprintf(f, "SLEEP_AFTER_MIN=%d\n", cfg->sleep_after_min);
    fprintf(f, "POWER_OFF_AFTER_MIN=%d\n", cfg->poweroff_after_min);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f); unlink(tmp); return -1;
    }
    fclose(f);
    chmod(tmp, 0600);
    if (rename(tmp, CONFIG_FILE) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Product defaults for a fresh STATE and for Reset System Settings.
 * Sleep is the idle stage that actually saves battery; the screensaver keeps
 * the panel and renderer busy, and Power Off after Sleep depends on per-device
 * RTC wake support, so both start Off. */
#define DEFAULT_PROFILE PROFILE_AUTO
#define DEFAULT_AUTO_BATTERY_THRESHOLD 20
#define DEFAULT_SCREENSAVER_AFTER_MIN 0
#define DEFAULT_SLEEP_AFTER_MIN 10
#define DEFAULT_POWEROFF_AFTER_MIN 0
#define DEFAULT_BACKUP_POLICY "WEEKLY"

static void config_defaults(struct config_state *cfg)
{
    cfg->requested_profile = DEFAULT_PROFILE;
    cfg->auto_battery_threshold = DEFAULT_AUTO_BATTERY_THRESHOLD;
    cfg->screensaver_after_min = DEFAULT_SCREENSAVER_AFTER_MIN;
    cfg->sleep_after_min = DEFAULT_SLEEP_AFTER_MIN;
    cfg->poweroff_after_min = DEFAULT_POWEROFF_AFTER_MIN;
}

static void load_config(struct config_state *cfg)
{
    FILE *f;
    char line[256];
    config_defaults(cfg);
    f = fopen(CONFIG_FILE, "r");
    if (!f) {
        (void)atomic_save_config(cfg);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        char *key, *val;
        if (!eq) continue;
        *eq = '\0'; key = line; val = eq+1;
        val[strcspn(val, "\r\n")] = '\0';
        if (!strcmp(key, "PERFORMANCE_PROFILE")) {
            enum profile_mode p;
            if (parse_profile(val, &p)) cfg->requested_profile = p;
        } else if (!strcmp(key, "AUTO_BATTERY_SAVER_THRESHOLD")) {
            int x = atoi(val);
            if (x >= 0 && x <= 50) cfg->auto_battery_threshold = x;
        } else if (!strcmp(key, "SCREENSAVER_AFTER_MIN")) {
            int x = atoi(val);
            if (x >= 0 && x <= 240) cfg->screensaver_after_min = x;
        } else if (!strcmp(key, "SLEEP_AFTER_MIN")) {
            int x = atoi(val);
            if (x >= 0 && x <= 240) cfg->sleep_after_min = x;
        } else if (!strcmp(key, "POWER_OFF_AFTER_MIN")) {
            int x = atoi(val);
            if (x >= 0 && x <= 480) cfg->poweroff_after_min = x;
        }
    }
    fclose(f);
}

static int run_argv(char *const argv[])
{
    pid_t pid = fork();
    int status;
    if (pid < 0) return -1;
    if (pid == 0) {
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            if (nullfd > STDERR_FILENO) close(nullfd);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status)==0 ? 0 : -1;
}

static bool safe_storage_token(const char *value)
{
    size_t n;

    if (!value || !*value)
        return false;

    n = strlen(value);
    if (n > 64)
        return false;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              c == '-' || c == '_' || c == ':'))
            return false;
    }

    return true;
}

/* Storage jobs are the System service's long-running operations: one Live
 * Notification per job, completed in place by its final state. */
static void notify_storage_job(const struct runtime_state *st)
{
    struct nuubos_notify n;

    nuubos_notify_begin(&n, "POST", "storage.job", "storage.job");
    nuubos_notify_str(&n, "action", st->storage_job_action);
    nuubos_notify_str(&n, "state", st->storage_job_state);
    if (!strcmp(st->storage_job_state, "running"))
        nuubos_notify_int(&n, "progress", -1);
    (void)nuubos_notify_send(&n);
}

static int start_storage_job(struct runtime_state *st,
                             const char *action,
                             const char *argument)
{
    pid_t pid;

    if (st->storage_job_pid > 0)
        return -2;

    pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        int logfd = open("/run/nuubos/system-storage-job.log",
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            if (logfd > STDERR_FILENO)
                close(logfd);
        }

        if (!strcmp(action, "backup")) {
            execl("/usr/sbin/nuubos-storagectl",
                  "nuubos-storagectl", "sync-to-tf1", (char *)NULL);
        } else if (!strcmp(action, "move-back")) {
            execl("/usr/sbin/nuubos-storagectl",
                  "nuubos-storagectl", "move-back-to-tf1", (char *)NULL);
        } else if (!strcmp(action, "adopt")) {
            char *preflight[] = {
                "/usr/sbin/nuubos-storagectl",
                "preflight-adopt-tf2",
                (char *)argument,
                NULL
            };
            char *auto_mode[] = {
                "/usr/sbin/nuubos-storagectl",
                "storage-mode",
                "AUTO",
                NULL
            };

            char *adopt[] = {
                "/usr/sbin/nuubos-storagectl",
                "adopt-tf2",
                "--confirm-erase-cid",
                (char *)argument,
                NULL
            };

            if (run_argv(preflight) != 0)
                _exit(20);
            if (run_argv(adopt) != 0)
                _exit(21);
            if (run_argv(auto_mode) != 0)
                _exit(22);
            _exit(0);
        }

        _exit(127);
    }

    st->storage_job_pid = pid;
    snprintf(st->storage_job_action, sizeof(st->storage_job_action), "%s", action);
    snprintf(st->storage_job_state, sizeof(st->storage_job_state), "running");
    st->storage_job_exit_code = -1;
    notify_storage_job(st);
    return 0;
}

static bool storage_job_tick(struct runtime_state *st)
{
    int status = 0;
    pid_t rc;

    if (st->storage_job_pid <= 0)
        return false;

    rc = waitpid(st->storage_job_pid, &status, WNOHANG);
    if (rc == 0)
        return false;

    if (rc < 0) {
        snprintf(st->storage_job_state, sizeof(st->storage_job_state), "failed");
        st->storage_job_exit_code = errno ? errno : 1;
        st->storage_job_pid = -1;
        notify_storage_job(st);
        return true;
    }

    if (WIFEXITED(status)) {
        st->storage_job_exit_code = WEXITSTATUS(status);
        snprintf(st->storage_job_state, sizeof(st->storage_job_state), "%s",
                 st->storage_job_exit_code == 0 ? "succeeded" : "failed");
    } else if (WIFSIGNALED(status)) {
        st->storage_job_exit_code = 128 + WTERMSIG(status);
        snprintf(st->storage_job_state, sizeof(st->storage_job_state), "failed");
    } else {
        st->storage_job_exit_code = 1;
        snprintf(st->storage_job_state, sizeof(st->storage_job_state), "failed");
    }

    st->storage_job_pid = -1;
    notify_storage_job(st);
    return true;
}

static int apply_effective_profile(enum profile_mode p)
{
    char *argv[4] = {
        "/usr/sbin/nuubos-power-mode",
        "apply-effective",
        (char *)profile_name(p),
        NULL
    };
    return run_argv(argv);
}

static void refresh_battery(struct runtime_state *st)
{
    FILE *f = fopen(STATUS_STATE, "r");
    char line[256];
    st->battery_percent = -1;
    snprintf(st->battery_state, sizeof(st->battery_state), "unknown");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "BATTERY_PERCENT=", 16))
            st->battery_percent = atoi(line+16);
        else if (!strncmp(line, "BATTERY_STATE=", 14)) {
            char *v = line + 14;
            size_t n = strcspn(v, "\r\n");
            if (n >= sizeof(st->battery_state))
                n = sizeof(st->battery_state) - 1;
            memcpy(st->battery_state, v, n);
            st->battery_state[n] = '\0';
        }
    }
    fclose(f);
}

static enum profile_mode desired_effective_profile(const struct runtime_state *st)
{
    bool on_battery = !strcmp(st->battery_state, "discharging") ||
                      !strcmp(st->battery_state, "not-charging");
    if (st->cfg.auto_battery_threshold > 0 &&
        on_battery &&
        st->battery_percent >= 0 &&
        st->battery_percent <= st->cfg.auto_battery_threshold)
        return PROFILE_BATTERY_SAVER;
    return st->cfg.requested_profile;
}

static void update_effective_profile(struct runtime_state *st, bool force)
{
    enum profile_mode previous = st->effective_profile;
    enum profile_mode next = desired_effective_profile(st);
    if (!force && next == previous)
        return;
    if (apply_effective_profile(next) != 0)
        return;
    st->effective_profile = next;

    /* Announce only the automatic switch; a user who selects Battery Saver
     * does not need to be told. The startup evaluation stays silent. */
    if (st->notifications_ready &&
        previous != PROFILE_BATTERY_SAVER &&
        next == PROFILE_BATTERY_SAVER &&
        st->cfg.requested_profile != PROFILE_BATTERY_SAVER) {
        struct nuubos_notify n;

        nuubos_notify_begin(&n, "POST", "battery.saver", "battery.saver");
        nuubos_notify_int(&n, "percent", st->battery_percent);
        (void)nuubos_notify_send(&n);
    }
}

static void notify_subscribers(void)
{
    const char *msg = "changed\n";
    for (int i=0;i<MAX_CLIENTS;i++) {
        if (clients[i].fd < 0 || !clients[i].subscribed) continue;
        if (write_all(clients[i].fd, msg) != 0) {
            close(clients[i].fd);
            clients[i].fd=-1; clients[i].subscribed=false; clients[i].used=0;
        }
    }
}

static int make_unix_listener(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    if (fd<0) return -1;
    memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX;
    snprintf(a.sun_path,sizeof(a.sun_path),"%s",SOCKET_PATH);
    unlink(SOCKET_PATH);
    if (bind(fd,(struct sockaddr*)&a,sizeof(a))<0 || listen(fd,8)<0) {
        close(fd); return -1;
    }
    chmod(SOCKET_PATH,0666);
    return fd;
}

static int connect_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    if (fd<0) return -1;
    memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX;
    snprintf(a.sun_path,sizeof(a.sun_path),"%s",path);
    if (connect(fd,(struct sockaddr*)&a,sizeof(a))<0) {
        close(fd); return -1;
    }
    return fd;
}

static bool rtc_wakeup_supported(void)
{
    char buf[64];
    if (read_text_file("/sys/class/rtc/rtc0/device/power/wakeup",buf,sizeof(buf))!=0)
        return false;
    return !strcmp(buf,"enabled") && access("/sys/class/rtc/rtc0/wakealarm", W_OK)==0;
}

static void clear_rtc_alarm(struct runtime_state *st)
{
    int fd = open("/sys/class/rtc/rtc0/wakealarm", O_WRONLY|O_CLOEXEC);
    if (fd >= 0) {
        ssize_t written = write(fd, "0\n", 2);
        if (written != 2)
            fprintf(stderr, "systemd: failed to clear RTC wakealarm: %s\n",
                    written < 0 ? strerror(errno) : "short write");
        close(fd);
    }
    st->rtc_alarm_armed=false;
}

static bool arm_rtc_alarm_relative(struct runtime_state *st, long long seconds)
{
    char buf[64];
    int fd;

    clear_rtc_alarm(st);
    fd = open("/sys/class/rtc/rtc0/wakealarm", O_WRONLY|O_CLOEXEC);
    if (fd < 0)
        return false;

    snprintf(buf,sizeof(buf),"+%lld\n",seconds);
    if (write(fd,buf,strlen(buf)) != (ssize_t)strlen(buf)) {
        close(fd);
        return false;
    }

    close(fd);
    st->rtc_alarm_armed=true;
    return true;
}

struct poweroff_alarm {
    int timer_fd;
    bool rtc_fallback;
    long long deadline_ms;
};

static void poweroff_alarm_init(struct poweroff_alarm *alarm)
{
    alarm->timer_fd = -1;
    alarm->rtc_fallback = false;
    alarm->deadline_ms = 0;
}

static bool arm_poweroff_alarm(struct runtime_state *st,
                               struct poweroff_alarm *alarm,
                               long long seconds)
{
    struct itimerspec it;

    poweroff_alarm_init(alarm);
    alarm->deadline_ms = boottime_ms() + seconds * 1000LL;

    alarm->timer_fd = timerfd_create(CLOCK_BOOTTIME_ALARM,
                                     TFD_NONBLOCK | TFD_CLOEXEC);
    if (alarm->timer_fd >= 0) {
        memset(&it, 0, sizeof(it));
        it.it_value.tv_sec = (time_t)seconds;
        if (timerfd_settime(alarm->timer_fd, 0, &it, NULL) == 0) {
            snprintf(st->last_alarm_backend,sizeof(st->last_alarm_backend),
                     "boottime-alarm");
            return true;
        }

        fprintf(stderr,
                "systemd: CLOCK_BOOTTIME_ALARM set failed: %s\n",
                strerror(errno));
        close(alarm->timer_fd);
        alarm->timer_fd = -1;
    } else {
        fprintf(stderr,
                "systemd: CLOCK_BOOTTIME_ALARM unavailable: %s\n",
                strerror(errno));
    }

    if (st->rtc_wakeup_supported &&
        arm_rtc_alarm_relative(st, seconds)) {
        alarm->rtc_fallback = true;
        snprintf(st->last_alarm_backend,sizeof(st->last_alarm_backend),
                 "rtc-sysfs");
        return true;
    }

    snprintf(st->last_alarm_backend,sizeof(st->last_alarm_backend),
             "none");
    return false;
}

static bool poweroff_alarm_expired(struct runtime_state *st,
                                   struct poweroff_alarm *alarm)
{
    if (alarm->timer_fd >= 0) {
        uint64_t expirations = 0;
        ssize_t n = read(alarm->timer_fd, &expirations, sizeof(expirations));

        if (n == (ssize_t)sizeof(expirations) && expirations > 0)
            return true;

        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            fprintf(stderr,
                    "systemd: CLOCK_BOOTTIME_ALARM read failed: %s\n",
                    strerror(errno));
        return false;
    }

    if (alarm->rtc_fallback) {
        /*
         * Relative RTC sysfs alarms do not expose a consumable timer object.
         * CLOCK_BOOTTIME includes suspend time, so a wake at/after the
         * requested deadline is the timeout path; an earlier wake is user
         * initiated and cancels the pending poweroff.
         */
        return boottime_ms() >= alarm->deadline_ms - 2000LL;
    }

    (void)st;
    return false;
}

static void cancel_poweroff_alarm(struct runtime_state *st,
                                  struct poweroff_alarm *alarm)
{
    if (alarm->timer_fd >= 0) {
        close(alarm->timer_fd);
        alarm->timer_fd = -1;
    }

    if (alarm->rtc_fallback || st->rtc_alarm_armed)
        clear_rtc_alarm(st);

    alarm->rtc_fallback = false;
}

static int run_suspend(void)
{
    char *argv[]={"/usr/sbin/nuubos-suspend",NULL};
    return run_argv(argv);
}

static int run_lifecycle_helper(const char *path, const char *action)
{
    char *argv[]={(char *)path,(char *)action,NULL};
    if (access(path, X_OK) != 0)
        return 0;
    return run_argv(argv);
}

static void notify_ui_lifecycle(const char *mode)
{
    int fd = open(UI_CONTROL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    int rc;

    if (fd < 0)
        return;

    rc = write_all(fd, mode);
    if (rc == 0)
        rc = write_all(fd, "\n");

    if (rc != 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        fprintf(stderr, "systemd: UI lifecycle notification failed: %s\n",
                strerror(errno));

    close(fd);
}

static void display_screensaver(void)
{
    if (run_lifecycle_helper(LIFECYCLE_DISPLAY, "screensaver") != 0)
        fprintf(stderr, "systemd: screensaver display dim failed\n");
}

static void display_active(void)
{
    if (run_lifecycle_helper(LIFECYCLE_DISPLAY, "active") != 0)
        fprintf(stderr, "systemd: screensaver display restore failed\n");
}

static void mark_activity(struct runtime_state *st)
{
    /* Avoid spawning the Display helper for ordinary active-state input.
     * It is needed only when leaving a lifecycle state that may have
     * applied temporary screensaver dimming. */
    if (st->idle_stage != IDLE_ACTIVE)
        display_active();
    st->idle_stage = IDLE_ACTIVE;
    st->stage_enter_ms = boottime_ms();
}

static void run_pre_power_hook(const char *action)
{
    if (run_lifecycle_helper(LIFECYCLE_PRE_POWER, action) != 0)
        fprintf(stderr, "systemd: pre-power hook failed for %s\n", action);
}

static void lighting_sleep(void)
{
    if (run_lifecycle_helper(LIFECYCLE_LIGHTING, "sleep") != 0)
        fprintf(stderr, "systemd: lighting sleep hook failed\n");
}

static void lighting_wake(void)
{
    if (run_lifecycle_helper(LIFECYCLE_LIGHTING, "wake") != 0)
        fprintf(stderr, "systemd: lighting wake hook failed\n");
}

static void run_poweroff(struct runtime_state *st)
{
    char *argv[]={"/sbin/poweroff",NULL};
    run_pre_power_hook("poweroff");
    notify_ui_lifecycle("poweroff");
    usleep(500000); /* Intentional presentation interval for the shutdown curtain. */
    if (run_argv(argv) != 0)
        lighting_wake(); /* Recover temporary suppression only if shutdown failed. */
    mark_activity(st);
}

static void run_restart(struct runtime_state *st)
{
    char *argv[]={"/sbin/reboot",NULL};
    run_pre_power_hook("restart");
    lighting_wake();
    notify_ui_lifecycle("reboot");
    usleep(500000); /* Intentional presentation interval for the reboot curtain. */
    (void)run_argv(argv);
    mark_activity(st);
}

static void enter_sleep(struct runtime_state *st)
{
    struct poweroff_alarm alarm;
    bool alarm_armed = false;
    bool timeout_wake = false;
    int suspend_rc;

    poweroff_alarm_init(&alarm);
    run_pre_power_hook("sleep");
    lighting_sleep();
    st->idle_stage = IDLE_SLEEPING;
    st->stage_enter_ms = boottime_ms();
    st->last_sleep_elapsed_ms = 0;
    snprintf(st->last_wake_reason,sizeof(st->last_wake_reason),"sleeping");
    snprintf(st->last_alarm_backend,sizeof(st->last_alarm_backend),"none");
    notify_subscribers();

    /*
     * Power Off After is a relative duration beginning at Sleep entry.
     * CLOCK_BOOTTIME_ALARM is the primary backend: the kernel's alarmtimer
     * infrastructure owns the RTC wake and the timerfd itself tells us
     * whether the wake was caused by timeout.  This avoids coupling the
     * product policy to CLOCK_REALTIME vs hardware-RTC synchronization.
     *
     * If alarmtimer is unavailable, fall back to the already-qualified
     * relative sysfs form ("+N"), never an absolute wall-clock epoch.
     */
    if (st->cfg.poweroff_after_min > 0) {
        long long off_sec = (long long)st->cfg.poweroff_after_min * 60LL;
        alarm_armed = arm_poweroff_alarm(st, &alarm, off_sec);
        if (!alarm_armed)
            fprintf(stderr,
                    "systemd: cannot arm wake for Power Off After; "
                    "sleep will require user wake\n");
    }

    suspend_rc = run_suspend();
    st->last_sleep_elapsed_ms = boottime_ms() - st->stage_enter_ms;

    if (suspend_rc == 0 && alarm_armed)
        timeout_wake = poweroff_alarm_expired(st, &alarm);

    cancel_poweroff_alarm(st, &alarm);

    if (suspend_rc != 0) {
        snprintf(st->last_wake_reason,sizeof(st->last_wake_reason),
                 "suspend-failed");
        lighting_wake();
        mark_activity(st);
        notify_subscribers();
        return;
    }

    if (timeout_wake) {
        snprintf(st->last_wake_reason,sizeof(st->last_wake_reason),
                 "poweroff-timeout");
        /*
         * Keep temporary lighting suppression active: this wake exists only
         * to complete Power Off and must not flash the RGB back on.
         */
        run_poweroff(st);
        return;
    }

    snprintf(st->last_wake_reason,sizeof(st->last_wake_reason),"user");
    lighting_wake();
    mark_activity(st);
    notify_subscribers();
}

static long long timeout_ms(int minutes)
{
    return (long long)minutes * 60000LL;
}

static void idle_policy_tick(struct runtime_state *st)
{
    long long now = boottime_ms();
    long long elapsed = now - st->stage_enter_ms;

    if (st->idle_stage == IDLE_SLEEPING)
        return;

    if (st->idle_stage == IDLE_ACTIVE) {
        if (st->cfg.screensaver_after_min > 0) {
            if (elapsed >= timeout_ms(st->cfg.screensaver_after_min)) {
                display_screensaver();
                st->idle_stage = IDLE_SCREENSAVER;
                st->stage_enter_ms = now;
                notify_subscribers();
            }
            return;
        }

        if (st->cfg.sleep_after_min > 0) {
            if (elapsed >= timeout_ms(st->cfg.sleep_after_min))
                enter_sleep(st);
            return;
        }

        if (st->cfg.poweroff_after_min > 0 &&
            elapsed >= timeout_ms(st->cfg.poweroff_after_min))
            run_poweroff(st);
        return;
    }

    if (st->idle_stage == IDLE_SCREENSAVER) {
        if (st->cfg.sleep_after_min > 0) {
            if (elapsed >= timeout_ms(st->cfg.sleep_after_min))
                enter_sleep(st);
            return;
        }

        if (st->cfg.poweroff_after_min > 0 &&
            elapsed >= timeout_ms(st->cfg.poweroff_after_min))
            run_poweroff(st);
    }
}

static long long read_cpu_freq_khz(void)
{
    long long v=-1;
    if (read_int_file("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq",&v)!=0)
        (void)read_int_file("/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_cur_freq",&v);
    return v;
}

static long long read_gpu_freq_hz(void)
{
    long long v=-1;
    (void)read_int_file("/sys/class/devfreq/1800000.gpu/cur_freq",&v);
    return v;
}

static long long read_temp_millic(const char *type)
{
    for (int i=0;i<16;i++) {
        char p[256],t[128];
        long long v;
        snprintf(p,sizeof(p),"/sys/class/thermal/thermal_zone%d/type",i);
        if (read_text_file(p,t,sizeof(t))!=0) continue;
        if (strcmp(t,type)) continue;
        snprintf(p,sizeof(p),"/sys/class/thermal/thermal_zone%d/temp",i);
        if (read_int_file(p,&v)==0) return v;
    }
    return -1;
}

static void read_memory_kib(long long *total_kib, long long *available_kib)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];

    *total_kib = -1;
    *available_kib = -1;
    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        long long value;
        if (sscanf(line, "MemTotal: %lld kB", &value) == 1)
            *total_kib = value;
        else if (sscanf(line, "MemAvailable: %lld kB", &value) == 1)
            *available_kib = value;
        if (*total_kib >= 0 && *available_kib >= 0)
            break;
    }
    fclose(f);
}

static void read_ipv4(char *out,size_t sz)
{
    struct ifaddrs *ifs=NULL,*it;
    snprintf(out,sz,"Unavailable");
    if (getifaddrs(&ifs)!=0) return;
    for (it=ifs;it;it=it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family!=AF_INET) continue;
        if (!strcmp(it->ifa_name,"lo")) continue;
        char b[INET_ADDRSTRLEN];
        struct sockaddr_in *sa=(struct sockaddr_in*)it->ifa_addr;
        if (inet_ntop(AF_INET,&sa->sin_addr,b,sizeof(b))) {
            snprintf(out,sz,"%s • %s",it->ifa_name,b);
            break;
        }
    }
    freeifaddrs(ifs);
}

static void read_key_value_file(const char *path,
                                const char *key,
                                char *out,
                                size_t out_size)
{
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (strcmp(line, key))
            continue;
        char *value = eq + 1;
        value[strcspn(value, "\r\n")] = '\0';
        snprintf(out, out_size, "%s", value);
        break;
    }
    fclose(f);
}

static void read_storage_fields(char *mode,size_t msz,char *active,size_t asz,
                                char *health,size_t hsz,char *tf2,size_t t2sz,
                                char *cid,size_t csz,char *action,size_t acsz,
                                char *backup,size_t bsz,char *due,size_t dsz,
                                char *reason,size_t rsz)
{
    const char *status="/run/nuubos/storage/tf2.status";

    snprintf(mode,msz,"Unknown");
    snprintf(active,asz,"Unknown");
    snprintf(health,hsz,"Unknown");
    snprintf(tf2,t2sz,"Unknown");
    if (csz > 0) cid[0] = '\0';
    snprintf(action,acsz,"NONE");
    snprintf(backup,bsz,"WEEKLY");
    snprintf(due,dsz,"Unknown");
    snprintf(reason,rsz,"Unknown");

    read_key_value_file(status,"STORAGE_MODE",mode,msz);
    read_key_value_file(status,"ACTIVE_STORAGE",active,asz);
    read_key_value_file(status,"STORAGE_HEALTH",health,hsz);
    read_key_value_file(status,"TF2_STATE",tf2,t2sz);
    read_key_value_file(status,"TF2_CID",cid,csz);
    read_key_value_file(status,"ACTION_REQUIRED",action,acsz);
    read_key_value_file(status,"TF1_BACKUP_POLICY",backup,bsz);
    read_key_value_file(status,"TF1_BACKUP_DUE",due,dsz);
    read_key_value_file(status,"TF1_BACKUP_REASON",reason,rsz);

    /* Configuration is authoritative for the selected backup policy. The
     * selector snapshot may legitimately lag a policy-only change until its
     * next restart, so always overlay the configured value here. */
    read_key_value_file("/state/config/nuubos.conf",
                        "TF1_BACKUP_POLICY",backup,bsz);
}

static void reply_status(int fd, const struct runtime_state *st)
{
    char ip[128],model[256],kernel[128];
    char mode[32],active[32],health[32],tf2[32],cid[96],action[64];
    char backup[32],due[32],reason[64];
    struct utsname un;
    struct statvfs sv;
    long long cpu=read_cpu_freq_khz(),gpu=read_gpu_freq_hz();
    long long tcpu=read_temp_millic("cpu-thermal"),tgpu=read_temp_millic("gpu-thermal");
    long long mem_total_kib=-1,mem_available_kib=-1;
    long long uptime=0;
    FILE *f=fopen("/proc/uptime","r");
    double up=0; if(f){if(fscanf(f,"%lf",&up)==1)uptime=(long long)up;fclose(f);}
    read_memory_kib(&mem_total_kib,&mem_available_kib);
    read_ipv4(ip,sizeof(ip));
    if(read_text_file("/proc/device-tree/model",model,sizeof(model))!=0)snprintf(model,sizeof(model),"Unknown H700 device");
    if(uname(&un)==0)snprintf(kernel,sizeof(kernel),"%s",un.release);else snprintf(kernel,sizeof(kernel),"Unknown");
    read_storage_fields(mode,sizeof(mode),active,sizeof(active),health,sizeof(health),
                        tf2,sizeof(tf2),cid,sizeof(cid),action,sizeof(action),
                        backup,sizeof(backup),due,sizeof(due),reason,sizeof(reason));

    unsigned long long total=0,freeb=0;
    if(statvfs("/userdata",&sv)==0){
        total=(unsigned long long)sv.f_blocks*sv.f_frsize;
        freeb=(unsigned long long)sv.f_bavail*sv.f_frsize;
    }
    char buf[6144];
    snprintf(buf,sizeof(buf),
        "protocol=1\n"
        "nuubos.version=%s\n"
        "nuubos.build=%s\n"
        "profile.requested=%s\n"
        "profile.effective=%s\n"
        "auto_battery.threshold=%d\n"
        "screensaver_after_min=%d\n"
        "sleep_after_min=%d\n"
        "poweroff_after_min=%d\n"
        "battery.percent=%d\n"
        "battery.state=%s\n"
        "rtc_wakeup=%d\n"
        "cpu.freq_khz=%lld\n"
        "gpu.freq_hz=%lld\n"
        "temp.cpu_millic=%lld\n"
        "temp.gpu_millic=%lld\n"
        "mem.total_kib=%lld\n"
        "mem.available_kib=%lld\n"
        "uptime_sec=%lld\n"
        "device.model=%s\n"
        "kernel=%s\n"
        "network=%s\n"
        "storage.mode=%s\n"
        "storage.active=%s\n"
        "storage.health=%s\n"
        "storage.tf2_state=%s\n"
        "storage.tf2_cid=%s\n"
        "storage.action_required=%s\n"
        "storage.backup_policy=%s\n"
        "storage.backup_due=%s\n"
        "storage.backup_reason=%s\n"
        "storage.userdata_total=%llu\n"
        "storage.userdata_free=%llu\n"
        "storage.job_state=%s\n"
        "storage.job_action=%s\n"
        "storage.job_exit=%d\n"
        "lifecycle.stage=%s\n"
        "lifecycle.last_wake=%s\n"
        "lifecycle.alarm_backend=%s\n"
        "lifecycle.last_sleep_ms=%lld\n",
        NUUBOS_PRODUCT_VERSION,NUUBOS_BUILD_ID,
        profile_name(st->cfg.requested_profile),
        profile_name(st->effective_profile),
        st->cfg.auto_battery_threshold,
        st->cfg.screensaver_after_min,
        st->cfg.sleep_after_min,
        st->cfg.poweroff_after_min,
        st->battery_percent,st->battery_state,
        st->rtc_wakeup_supported?1:0,
        cpu,gpu,tcpu,tgpu,mem_total_kib,mem_available_kib,uptime,model,kernel,ip,
        mode,active,health,tf2,cid,action,backup,due,reason,total,freeb,
        st->storage_job_state,st->storage_job_action,st->storage_job_exit_code,
        idle_stage_name(st->idle_stage),
        st->last_wake_reason,st->last_alarm_backend,
        st->last_sleep_elapsed_ms);
    (void)write_all(fd,buf);
}

/*
 * Reset System Settings restores every device-global setting to its product
 * default. Each owning service resets its own state through its own contract;
 * this service only orchestrates. Users, Wi-Fi/Bluetooth pairings, date/time,
 * storage layout and all USERDATA (ROMs, BIOS, saves, media) are kept.
 * Controller mappings are reset for every user.
 */
static int reset_system_settings(struct runtime_state *st)
{
    static char *const audio_argv[]={"/usr/bin/nuubos-audioctl","reset-defaults",NULL};
    static char *const display_argv[]={"/usr/bin/nuubos-displayctl","reset",NULL};
    static char *const input_argv[]={"/usr/bin/nuubos-inputctl","bind","reset",NULL};
    static char *const controllers_argv[]={
        "/usr/bin/dbus-send","--system","--print-reply",
        "--dest=org.nuubOS.Controllers","/org/nuubOS/Controllers",
        "org.nuubOS.Controllers1.ResetMappings",NULL};
    static char *const backup_argv[]={"/usr/sbin/nuubos-storagectl","backup-policy",
                                      DEFAULT_BACKUP_POLICY,NULL};
    int rc=0;

    config_defaults(&st->cfg);
    if(atomic_save_config(&st->cfg)!=0){fprintf(stderr,"systemd: reset: system config\n");rc=-1;}
    clear_rtc_alarm(st);
    mark_activity(st);
    refresh_battery(st);
    update_effective_profile(st,true);

    if(run_argv(audio_argv)!=0){fprintf(stderr,"systemd: reset: audio\n");rc=-1;}
    if(run_argv(display_argv)!=0){fprintf(stderr,"systemd: reset: display\n");rc=-1;}
    if(run_argv(input_argv)!=0){fprintf(stderr,"systemd: reset: input bindings\n");rc=-1;}
    if(run_argv(controllers_argv)!=0){fprintf(stderr,"systemd: reset: controller mappings\n");rc=-1;}
    if(st->storage_job_pid>0){fprintf(stderr,"systemd: reset: storage busy, backup policy kept\n");rc=-1;}
    else if(run_argv(backup_argv)!=0){fprintf(stderr,"systemd: reset: backup policy\n");rc=-1;}
    return rc;
}

static void handle_command(struct client *c, struct runtime_state *st, const char *line)
{
    if(!strcmp(line,"STATUS")){ reply_status(c->fd,st); return; }
    if(!strcmp(line,"SUBSCRIBE")){
        c->subscribed=true; (void)write_all(c->fd,"OK protocol=1\n"); return;
    }
    if(!strcmp(line,"ACTIVITY")){
        mark_activity(st); (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(!strcmp(line,"ACTION SLEEP")){
        (void)write_all(c->fd,"OK sleeping\n");
        enter_sleep(st); return;
    }
    if(!strcmp(line,"ACTION RESTART")){
        (void)write_all(c->fd,"OK restarting\n");
        run_restart(st); return;
    }
    if(!strcmp(line,"ACTION POWEROFF")){
        (void)write_all(c->fd,"OK powering-off\n");
        run_poweroff(st); return;
    }
    if(!strcmp(line,"RESET SYSTEM SETTINGS")){
        int rc=reset_system_settings(st);
        (void)write_all(c->fd,rc==0?"OK\n":"ERR reset incomplete\n"); notify_subscribers(); return;
    }
    char val[64];
    if(sscanf(line,"SET PROFILE %63s",val)==1){
        enum profile_mode p;
        if(!parse_profile(val,&p)){(void)write_all(c->fd,"ERR invalid profile\n");return;}
        st->cfg.requested_profile=p;
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        refresh_battery(st); update_effective_profile(st,true);
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    int x;
    if(sscanf(line,"SET AUTO_BATTERY %d",&x)==1){
        if(x<0||x>50){(void)write_all(c->fd,"ERR invalid threshold\n");return;}
        st->cfg.auto_battery_threshold=x;
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        refresh_battery(st); update_effective_profile(st,true);
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET SCREENSAVER %d",&x)==1){
        if(x<0||x>240){(void)write_all(c->fd,"ERR invalid timeout\n");return;}
        st->cfg.screensaver_after_min=x; mark_activity(st);
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET SLEEP %d",&x)==1){
        if(x<0||x>240){(void)write_all(c->fd,"ERR invalid timeout\n");return;}
        st->cfg.sleep_after_min=x; mark_activity(st);
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET POWEROFF %d",&x)==1){
        if(x<0||x>480){(void)write_all(c->fd,"ERR invalid timeout\n");return;}
        st->cfg.poweroff_after_min=x; mark_activity(st);
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET STORAGE BACKUP_POLICY %63s",val)==1){
        char *argv[]={"/usr/sbin/nuubos-storagectl","backup-policy",val,NULL};
        if(strcmp(val,"OFF") && strcmp(val,"DAILY") &&
           strcmp(val,"WEEKLY") && strcmp(val,"MONTHLY")){
            (void)write_all(c->fd,"ERR invalid backup policy\n");return;
        }
        if(run_argv(argv)!=0){(void)write_all(c->fd,"ERR storage policy failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET STORAGE MODE %63s",val)==1){
        char *argv[]={"/usr/sbin/nuubos-storagectl","storage-mode",val,NULL};
        if(strcmp(val,"SINGLE") && strcmp(val,"AUTO") && strcmp(val,"DUAL")){
            (void)write_all(c->fd,"ERR invalid storage mode\n");return;
        }
        if(st->storage_job_pid>0){(void)write_all(c->fd,"ERR storage busy\n");return;}
        if(run_argv(argv)!=0){(void)write_all(c->fd,"ERR storage mode failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(!strcmp(line,"START STORAGE BACKUP")){
        int rc=start_storage_job(st,"backup",NULL);
        if(rc==-2){(void)write_all(c->fd,"ERR storage busy\n");return;}
        if(rc!=0){(void)write_all(c->fd,"ERR storage job failed\n");return;}
        (void)write_all(c->fd,"OK started\n"); notify_subscribers(); return;
    }
    if(!strcmp(line,"START STORAGE MOVE_BACK")){
        int rc=start_storage_job(st,"move-back",NULL);
        if(rc==-2){(void)write_all(c->fd,"ERR storage busy\n");return;}
        if(rc!=0){(void)write_all(c->fd,"ERR storage job failed\n");return;}
        (void)write_all(c->fd,"OK started\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"START STORAGE ADOPT %63s",val)==1){
        int rc;
        if(!safe_storage_token(val)){(void)write_all(c->fd,"ERR invalid TF2 CID\n");return;}
        rc=start_storage_job(st,"adopt",val);
        if(rc==-2){(void)write_all(c->fd,"ERR storage busy\n");return;}
        if(rc!=0){(void)write_all(c->fd,"ERR storage job failed\n");return;}
        (void)write_all(c->fd,"OK started\n"); notify_subscribers(); return;
    }
    (void)write_all(c->fd,"ERR unknown command\n");
}

static void accept_clients(int lfd)
{
    for(;;){
        int fd=accept4(lfd,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
        if(fd<0){ if(errno==EAGAIN||errno==EWOULDBLOCK)return; return; }
        int slot=-1;
        for(int i=0;i<MAX_CLIENTS;i++) if(clients[i].fd<0){slot=i;break;}
        if(slot<0){close(fd);continue;}
        clients[slot].fd=fd;clients[slot].subscribed=false;clients[slot].used=0;
    }
}

static void service_client(struct client *c, struct runtime_state *st)
{
    char tmp[256];
    ssize_t n=read(c->fd,tmp,sizeof(tmp)-1);
    if(n<=0){
        close(c->fd);c->fd=-1;c->subscribed=false;c->used=0;return;
    }
    tmp[n]='\0';
    for(ssize_t i=0;i<n;i++){
        if(c->used+1>=sizeof(c->buf)){c->used=0;continue;}
        if(tmp[i]=='\n'){
            c->buf[c->used]='\0';
            if(c->used && c->buf[c->used-1]=='\r')c->buf[c->used-1]='\0';
            handle_command(c,st,c->buf);
            c->used=0;
        }else c->buf[c->used++]=tmp[i];
    }
}

static int next_policy_timeout_ms(const struct runtime_state *st)
{
    long long timeout = -1;
    long long elapsed = boottime_ms() - st->stage_enter_ms;
    long long target = -1;

    /* Storage jobs are exceptional active work. Poll at 1 Hz only while one
     * exists so child completion is noticed without a permanent heartbeat. */
    if (st->storage_job_pid > 0)
        timeout = 1000;

    if (st->idle_stage == IDLE_ACTIVE) {
        if (st->cfg.screensaver_after_min > 0)
            target = timeout_ms(st->cfg.screensaver_after_min);
        else if (st->cfg.sleep_after_min > 0)
            target = timeout_ms(st->cfg.sleep_after_min);
        else if (st->cfg.poweroff_after_min > 0)
            target = timeout_ms(st->cfg.poweroff_after_min);
    } else if (st->idle_stage == IDLE_SCREENSAVER) {
        if (st->cfg.sleep_after_min > 0)
            target = timeout_ms(st->cfg.sleep_after_min);
        else if (st->cfg.poweroff_after_min > 0)
            target = timeout_ms(st->cfg.poweroff_after_min);
    }

    if (target >= 0) {
        long long remaining = target - elapsed;
        if (remaining < 0) remaining = 0;
        if (remaining > INT_MAX) remaining = INT_MAX;
        if (timeout < 0 || remaining < timeout)
            timeout = remaining;
    }

    return timeout < 0 ? -1 : (int)timeout;
}

static void write_pidfile(void)
{
    FILE *f=fopen(PIDFILE,"w");
    if(f){fprintf(f,"%ld\n",(long)getpid());fclose(f);}
}

int main(void)
{
    struct runtime_state st;
    int listen_fd,input_fd=-1,status_fd=-1;
    signal(SIGPIPE,SIG_IGN); signal(SIGTERM,signal_handler); signal(SIGINT,signal_handler);
    for(int i=0;i<MAX_CLIENTS;i++)clients[i].fd=-1;
    mkdir("/run/nuubos",0755);
    load_config(&st.cfg);
    st.idle_stage=IDLE_ACTIVE;
    st.stage_enter_ms=boottime_ms();
    st.rtc_wakeup_supported=rtc_wakeup_supported();
    st.rtc_alarm_armed=false;
    snprintf(st.last_alarm_backend,sizeof(st.last_alarm_backend),"none");
    snprintf(st.last_wake_reason,sizeof(st.last_wake_reason),"none");
    st.last_sleep_elapsed_ms=0;
    st.storage_job_pid=-1;
    snprintf(st.storage_job_action,sizeof(st.storage_job_action),"none");
    snprintf(st.storage_job_state,sizeof(st.storage_job_state),"idle");
    st.storage_job_exit_code=0;
    lighting_wake(); /* Recover stale temporary RGB suppression after interrupted sleep. */
    display_active(); /* Recover a stale runtime-only screensaver dim after service restart. */
    refresh_battery(&st);
    st.effective_profile=PROFILE_AUTO;
    st.notifications_ready=false;
    update_effective_profile(&st,true);
    st.notifications_ready=true;

    listen_fd=make_unix_listener();
    if(listen_fd<0)return 1;
    write_pidfile();

    while(!stop_requested){
        if(input_fd<0){
            input_fd=connect_unix(INPUT_SOCKET);
            if(input_fd>=0)(void)write_all(input_fd,"SUBSCRIBE CONTROLLERS\n");
        }
        if(status_fd<0) status_fd=connect_unix(STATUS_SOCKET);

        struct pollfd pf[3+MAX_CLIENTS];
        int map[3+MAX_CLIENTS],n=0;
        pf[n]=(struct pollfd){listen_fd,POLLIN,0};map[n++]=-1;
        if(input_fd>=0){pf[n]=(struct pollfd){input_fd,POLLIN|POLLHUP|POLLERR,0};map[n++]=-3;}
        if(status_fd>=0){pf[n]=(struct pollfd){status_fd,POLLIN|POLLHUP|POLLERR,0};map[n++]=-4;}
        for(int i=0;i<MAX_CLIENTS;i++)if(clients[i].fd>=0){
            pf[n]=(struct pollfd){clients[i].fd,POLLIN|POLLHUP|POLLERR,0};map[n++]=i;
        }
        int rc=poll(pf,n,next_policy_timeout_ms(&st));
        if(rc<0){if(errno==EINTR)continue;break;}
        if(rc==0){
            if(storage_job_tick(&st)) notify_subscribers();
            idle_policy_tick(&st);
            continue;
        }
        for(int j=0;j<n;j++){
            if(!pf[j].revents)continue;
            if(map[j]==-1){accept_clients(listen_fd);continue;}
            if(map[j]==-3){
                if(pf[j].revents&(POLLHUP|POLLERR)){close(input_fd);input_fd=-1;continue;}
                char b[512];ssize_t r=read(input_fd,b,sizeof(b)-1);
                if(r<=0){close(input_fd);input_fd=-1;continue;}
                b[r]='\0';
                if(strstr(b,"RAW ")) mark_activity(&st);
                continue;
            }
            if(map[j]==-4){
                if(pf[j].revents&(POLLHUP|POLLERR)){close(status_fd);status_fd=-1;continue;}
                char b[128];ssize_t r=read(status_fd,b,sizeof(b));
                if(r<=0){close(status_fd);status_fd=-1;continue;}
                refresh_battery(&st);
                update_effective_profile(&st,false);
                notify_subscribers();
                continue;
            }
            int i=map[j];
            if(pf[j].revents&(POLLHUP|POLLERR)){
                close(clients[i].fd);clients[i].fd=-1;clients[i].subscribed=false;clients[i].used=0;
            }else service_client(&clients[i],&st);
        }
    }

    clear_rtc_alarm(&st);
    if (input_fd >= 0)
        close(input_fd);
    if (status_fd >= 0)
        close(status_fd);
    close(listen_fd);
    unlink(SOCKET_PATH);unlink(PIDFILE);
    return 0;
}
