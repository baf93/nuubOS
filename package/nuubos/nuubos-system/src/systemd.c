
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/if_link.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
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
#define LIFECYCLE_PRE_POWER "/usr/lib/nuubos/lifecycle-pre-power"
#define LIFECYCLE_POST_WAKE "/usr/lib/nuubos/lifecycle-post-wake"
#define INPUT_DIR "/dev/input"
#define HDMI_ENABLED "/sys/class/drm/card0-HDMI-A-1/enabled"
/* Back-to-back sleeps while the lid stays closed (a key pressed in a bag
 * wakes the device): bounded so a wake source that fires at once cannot
 * keep the service in a suspend loop. */
#define LID_MAX_RESLEEPS 8
#define LONG_BITS (8 * sizeof(unsigned long))
#define NBITS(x) (((x) + LONG_BITS - 1) / LONG_BITS)
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
    IDLE_SLEEPING
};

struct config_state {
    enum profile_mode requested_profile;
    int auto_battery_threshold; /* 0=off */
    int sleep_after_min;        /* 0=off */
    int charge_limit;           /* 80..100 %, 100=off */
    bool hypersleep;            /* sleep with the SoC off (default) */
    bool fast_startup;          /* cold starts reuse the DRAM training (experimental, off) */
    bool power_key_sleep;       /* power key: press = Sleep, hold = power menu */
    bool lid_wake;              /* clamshells: a lid-close sleep opens with the lid */
};

struct runtime_state {
    struct config_state cfg;
    enum profile_mode effective_profile;
    int battery_percent;
    char battery_state[32];
    enum idle_stage idle_stage;
    long long stage_enter_ms;
    char last_wake_reason[24];
    long long last_sleep_elapsed_ms;
    int lid_fd;                 /* clamshells (SP): gpio-keys SW_LID, else -1 */
    bool lid_closed;

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

/*
 * Diagnostic mode (user request 2026-10-22, docs/dev/system.md): off by
 * default. DIAG_BOOTS holds how many more boots collect logs;
 * S04nuubos-diagnostics spends one at each boot and marks that boot with
 * DIAG_ACTIVE (RAM), which the boot collectors, rcS timing, emud, inputd
 * and the pre-power bundle hook look for. Turning it on also marks the
 * running boot; turning it off ends the running one too.
 */
#define DIAG_BOOTS "/state/diagnostics/boots"
#define DIAG_ACTIVE "/run/nuubos/diagnostics/active"
#define DIAG_MAX_BOOTS 20

static int diag_boots(void)
{
    char buf[32];
    int n;
    if (read_text_file(DIAG_BOOTS, buf, sizeof(buf)) != 0) return 0;
    n = atoi(buf);
    return n < 0 ? 0 : n > DIAG_MAX_BOOTS ? DIAG_MAX_BOOTS : n;
}

static int run_argv(char *const argv[]);

/* The heartbeat kept on STATE through a freeze (nuubos-recovery). */
static void diag_persist(const char *action)
{
    char *argv[] = { "/usr/libexec/nuubos-diag-persist", (char *)action, NULL };

    if (run_argv(argv) != 0)
        fprintf(stderr, "systemd: diag-persist %s failed\n", action);
}

static int set_diagnostics(int boots)
{
    char tmp[128];
    FILE *f;

    if (boots == 0) {
        if (unlink(DIAG_BOOTS) != 0 && errno != ENOENT) return -1;
        if (unlink(DIAG_ACTIVE) != 0 && errno != ENOENT) return -1;
        diag_persist("stop");
        return 0;
    }
    mkdir("/state/diagnostics", 0700);
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", DIAG_BOOTS, (long)getpid());
    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", boots);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f); unlink(tmp); return -1;
    }
    fclose(f);
    if (rename(tmp, DIAG_BOOTS) != 0) {
        unlink(tmp);
        return -1;
    }
    mkdir("/run/nuubos/diagnostics", 0700);
    f = fopen(DIAG_ACTIVE, "w");
    if (!f) return -1;
    fclose(f);
    diag_persist("start");
    /* With the mode off the service logs are links to /dev/null
     * (S04nuubos-diagnostics): gone, a log a service opens from now on is
     * real; complete from the next boot. */
    {
        DIR *d = opendir("/run/nuubos");
        struct dirent *e;
        char path[300];
        struct stat lst;

        while (d && (e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            if (n < 5 || strcmp(e->d_name + n - 4, ".log") != 0) continue;
            snprintf(path, sizeof(path), "/run/nuubos/%s", e->d_name);
            if (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode)) unlink(path);
        }
        if (d) closedir(d);
    }
    return 0;
}

static int atomic_save_config(const struct config_state *cfg)
{
    char tmp[256];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", CONFIG_FILE, (long)getpid());
    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "SYSTEM_CONFIG_VERSION=3\n");
    fprintf(f, "PERFORMANCE_PROFILE=%s\n", profile_name(cfg->requested_profile));
    fprintf(f, "AUTO_BATTERY_SAVER_THRESHOLD=%d\n", cfg->auto_battery_threshold);
    fprintf(f, "SLEEP_AFTER_MIN=%d\n", cfg->sleep_after_min);
    fprintf(f, "CHARGE_LIMIT=%d\n", cfg->charge_limit);
    fprintf(f, "HYPERSLEEP=%d\n", cfg->hypersleep ? 1 : 0);
    fprintf(f, "EXPERIMENTAL_FAST_STARTUP=%d\n", cfg->fast_startup ? 1 : 0);
    fprintf(f, "POWER_KEY_SLEEP=%d\n", cfg->power_key_sleep ? 1 : 0);
    fprintf(f, "LID_WAKE=%d\n", cfg->lid_wake ? 1 : 0);
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
 * Sleep is the only idle stage (user decision 2026-10-09: no screensaver,
 * no automatic power off; version-2 keys are ignored on load). */
#define DEFAULT_PROFILE PROFILE_AUTO
#define DEFAULT_AUTO_BATTERY_THRESHOLD 20
#define DEFAULT_SLEEP_AFTER_MIN 10
#define DEFAULT_CHARGE_LIMIT 100

static void config_defaults(struct config_state *cfg)
{
    cfg->requested_profile = DEFAULT_PROFILE;
    cfg->auto_battery_threshold = DEFAULT_AUTO_BATTERY_THRESHOLD;
    cfg->sleep_after_min = DEFAULT_SLEEP_AFTER_MIN;
    cfg->charge_limit = DEFAULT_CHARGE_LIMIT;
    cfg->hypersleep = true;
    /* Off until it is qualified on every board (user decision 2026-10-25:
     * an RG SP froze and restarted with it on, ran clean with it off). */
    cfg->fast_startup = false;
    /* On (user decision 2026-10-27, EPIC-045): a press sleeps, a hold
     * opens the power menu. */
    cfg->power_key_sleep = true;
    /* On (user decision 2026-11-10): opening a clamshell wakes it, as
     * users expect; that sleep keeps the SoC powered (~3-4 %/h). */
    cfg->lid_wake = true;
}

/* Charge limit: 80..100 in steps of 5 (100 = charge to full). */
static bool valid_charge_limit(int x)
{
    return x >= 80 && x <= 100 && x % 5 == 0;
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
        } else if (!strcmp(key, "SLEEP_AFTER_MIN")) {
            int x = atoi(val);
            if (x >= 0 && x <= 240) cfg->sleep_after_min = x;
        } else if (!strcmp(key, "CHARGE_LIMIT")) {
            int x = atoi(val);
            if (valid_charge_limit(x)) cfg->charge_limit = x;
        } else if (!strcmp(key, "HYPERSLEEP")) {
            cfg->hypersleep = strcmp(val, "0") != 0;
        } else if (!strcmp(key, "EXPERIMENTAL_FAST_STARTUP")) {
            /* A new key: FAST_STARTUP=1 was written for everyone while
             * it was on by default, so it is not a choice; ignored. */
            cfg->fast_startup = strcmp(val, "0") != 0;
        } else if (!strcmp(key, "POWER_KEY_SLEEP")) {
            cfg->power_key_sleep = strcmp(val, "0") != 0;
        } else if (!strcmp(key, "LID_WAKE")) {
            cfg->lid_wake = strcmp(val, "0") != 0;
        }
    }
    fclose(f);
}

/*
 * Fast Startup (user decision 2026-10-25, on by default; community freezes
 * on an RG SP): a cold start reuses the last DRAM training when the SPL
 * finds its mark in RTC GP0 (U-Boot 0006/0007, platform.md "Cached DRAM
 * training"). Off, the mark is cleared at every start and after every
 * HyperSleep resume (whose SPL sets it again), so any next cold start, a
 * crash included, trains in full (~5 s). Entering HyperSleep writes its own
 * resume flag into GP0: the resume is unaffected.
 */
#define RTC_PAGE 0x07000000UL
#define RTC_GP0 0x100
#define DRAM_TRAINED_MARK 0x4e440001u

static void apply_fast_startup(const struct config_state *cfg)
{
    volatile uint32_t *rtc;
    int fd;

    if (cfg->fast_startup)
        return;
    fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "systemd: fast startup: /dev/mem: %s\n", strerror(errno));
        return;
    }
    rtc = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, RTC_PAGE);
    close(fd);
    if (rtc == MAP_FAILED) {
        fprintf(stderr, "systemd: fast startup: mmap: %s\n", strerror(errno));
        return;
    }
    if (rtc[RTC_GP0 / 4] == DRAM_TRAINED_MARK)
        rtc[RTC_GP0 / 4] = 0;
    munmap((void *)rtc, 4096);
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

/*
 * Hypersleep (user decision 2026-10-22, on by default): the SoC is powered
 * off during sleep, ~0.x %/h, only the power key wakes it. Off: the SoC
 * stays powered, START and a lid wake it too, ~3-4 %/h. nuubos-suspend
 * tells the firmware which one (platform.md). A sleep started by closing a
 * clamshell's lid with Lid Wake on is a quick one, so opening the lid
 * wakes it (HyperSleep powers the lid's GPIO bank off).
 */
static int run_suspend(const struct runtime_state *st, bool by_lid)
{
    bool hyper = st->cfg.hypersleep && !(by_lid && st->cfg.lid_wake);
    char *argv[]={"/usr/sbin/nuubos-suspend", hyper ? "hypersleep" : "quick",NULL};
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

static void mark_activity(struct runtime_state *st)
{
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

/* restart | poweroff: the LED shape of the splash animation nuubUI starts
 * right after (notify_ui_lifecycle). */
static void lighting_shutdown(const char *action)
{
    if (run_lifecycle_helper(LIFECYCLE_LIGHTING, action) != 0)
        fprintf(stderr, "systemd: lighting shutdown hook failed\n");
}

/* Restart/Power Off under way: audiod turns the speaker amplifier off when
 * init stops it (after the shutdown sound), so the codec powers down
 * without a pop. */
static void mark_shutting_down(void)
{
    int fd = open("/run/nuubos/shutting-down", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);

    if (fd >= 0)
        close(fd);
}

/* Restart / Power Off: the LEDs and the UI's splash first, then the
 * pre-power hooks (game saved and closed, update installed, diagnostic
 * report) under it. The hooks used to run first: the screen stayed black
 * while they did, before the splash and the power off (user report
 * 2026-10-22, long with a diagnostic report). */
static void run_poweroff(struct runtime_state *st)
{
    char *argv[]={"/sbin/poweroff",NULL};
    lighting_shutdown("poweroff");
    notify_ui_lifecycle("poweroff");
    run_pre_power_hook("poweroff");
    usleep(500000); /* Intentional presentation interval for the shutdown curtain. */
    mark_shutting_down();
    if (run_argv(argv) != 0)
        lighting_wake(); /* Power off failed: lights back on. */
    mark_activity(st);
}

static void run_restart(struct runtime_state *st)
{
    char *argv[]={"/sbin/reboot",NULL};
    lighting_shutdown("restart");
    notify_ui_lifecycle("reboot");
    run_pre_power_hook("restart");
    usleep(500000); /* Intentional presentation interval for the reboot curtain. */
    mark_shutting_down();
    (void)run_argv(argv);
    mark_activity(st);
}

static void enter_sleep_mode(struct runtime_state *st, bool by_lid)
{
    int suspend_rc;

    run_pre_power_hook("sleep");
    lighting_sleep();
    st->idle_stage = IDLE_SLEEPING;
    st->stage_enter_ms = boottime_ms();
    st->last_sleep_elapsed_ms = 0;
    snprintf(st->last_wake_reason,sizeof(st->last_wake_reason),"sleeping");
    notify_subscribers();

    suspend_rc = run_suspend(st, by_lid);
    st->last_sleep_elapsed_ms = boottime_ms() - st->stage_enter_ms;
    if (run_lifecycle_helper(LIFECYCLE_POST_WAKE, "wake") != 0)
        fprintf(stderr, "systemd: post-wake hook failed\n");
    apply_fast_startup(&st->cfg);

    snprintf(st->last_wake_reason,sizeof(st->last_wake_reason),
             suspend_rc != 0 ? "suspend-failed" : "user");
    lighting_wake();
    /* Slept with the screen off (Media Screen Off): wake with it on. */
    if (access("/run/nuubos/screen-off", F_OK) == 0) {
        char *argv[]={"/usr/bin/nuubos-displayctl","screen","on",NULL};
        (void)run_argv(argv);
    }
    mark_activity(st);
    notify_subscribers();
}

static void enter_sleep(struct runtime_state *st)
{
    enter_sleep_mode(st, false);
}

static bool test_bit(const unsigned long *bits, unsigned int bit)
{
    return (bits[bit / LONG_BITS] >> (bit % LONG_BITS)) & 1UL;
}

/* Lid switch of the clamshells (RG35XX-SP, RG34XX-SP, RG-SP: gpio-keys
 * SW_LID, a wake source in the device tree). Read-only and never grabbed:
 * inputd ignores it (no mapped keys). -1 on devices without a lid. */
static int open_lid_switch(bool *closed)
{
    DIR *dir = opendir(INPUT_DIR);
    struct dirent *e;
    int lid = -1;

    if (!dir)
        return -1;
    while (lid < 0 && (e = readdir(dir)) != NULL) {
        unsigned long ev[NBITS(EV_MAX + 1)] = { 0 };
        unsigned long sw[NBITS(SW_MAX + 1)] = { 0 };
        unsigned long now[NBITS(SW_MAX + 1)] = { 0 };
        char path[288];
        int fd;

        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), INPUT_DIR "/%s", e->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) >= 0 && test_bit(ev, EV_SW) &&
            ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw)), sw) >= 0 && test_bit(sw, SW_LID) &&
            ioctl(fd, EVIOCGSW(sizeof(now)), now) >= 0) {
            *closed = test_bit(now, SW_LID);
            lid = fd;
        } else {
            close(fd);
        }
    }
    closedir(dir);
    return lid;
}

/* Current lid state after draining queued events (those of a sleep too). */
static void refresh_lid(struct runtime_state *st)
{
    unsigned long now[NBITS(SW_MAX + 1)] = { 0 };
    struct input_event ev[16];

    if (st->lid_fd < 0)
        return;
    while (read(st->lid_fd, ev, sizeof(ev)) > 0)
        ;
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close(st->lid_fd);
        st->lid_fd = -1;
        st->lid_closed = false;
        return;
    }
    if (ioctl(st->lid_fd, EVIOCGSW(sizeof(now)), now) >= 0)
        st->lid_closed = test_bit(now, SW_LID);
}

/* HDMI is the active output (the output policy turns the panel off): a
 * closed lid then is a docked console, not a request to sleep. */
static bool hdmi_active(void)
{
    char v[32];

    return read_text_file(HDMI_ENABLED, v, sizeof(v)) == 0 && !strncmp(v, "enabled", 7);
}

/* Closing the lid sleeps through the same path as Sleep in the Power OSD;
 * opening it is the hardware wake (the switch wakes on release). Woken
 * with the lid still closed, the console goes back to sleep. */
static void lid_policy(struct runtime_state *st)
{
    bool was_closed = st->lid_closed;

    refresh_lid(st);
    if (!st->lid_closed || was_closed || st->idle_stage != IDLE_ACTIVE)
        return;
    for (int i = 0; i <= LID_MAX_RESLEEPS && st->lid_closed && !hdmi_active(); i++) {
        enter_sleep_mode(st, true);
        refresh_lid(st);
        if (!strcmp(st->last_wake_reason, "suspend-failed"))
            break;
        if (!st->lid_closed) {
            snprintf(st->last_wake_reason, sizeof(st->last_wake_reason), "lid");
            notify_subscribers();
        }
    }
}

static long long timeout_ms(int minutes)
{
    return (long long)minutes * 60000LL;
}

/* nuubUI's full-screen operation (system update, SD2 restore: ui-context
 * "operation") must not be put to sleep (EPIC-048: critical OTA/storage
 * phases inhibit suspend). Neither does playing media (nuubos-mediad keeps
 * /run/nuubos/media-playing while something plays, not while paused):
 * music with the screen off or a film has no input for a long time. */
static bool operation_running(void)
{
    char c[32] = "";
    FILE *f;

    if (access("/run/nuubos/media-playing", F_OK) == 0)
        return true;
    f = fopen("/run/nuubos/ui-context", "r");
    if (f) {
        if (!fgets(c, sizeof(c), f))
            c[0] = '\0';
        fclose(f);
    }
    return !strncmp(c, "operation", 9);
}

static void idle_policy_tick(struct runtime_state *st)
{
    long long elapsed = boottime_ms() - st->stage_enter_ms;

    if (st->idle_stage == IDLE_ACTIVE && st->cfg.sleep_after_min > 0 &&
        elapsed >= timeout_ms(st->cfg.sleep_after_min)) {
        /* The idle time starts over instead. */
        if (operation_running())
            mark_activity(st);
        else
            enter_sleep(st);
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

/* CPU busy percentage since the previous call (-1 on the first). */
static int read_cpu_load_pct(void)
{
    static unsigned long long prev_busy, prev_total;
    unsigned long long v[8] = {0}, busy, total;
    FILE *f = fopen("/proc/stat", "r");
    int pct = -1;

    if (!f)
        return -1;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 8) {
        fclose(f);
        return -1;
    }
    fclose(f);
    total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
    busy = total - v[3] - v[4];
    if (prev_total && total > prev_total)
        pct = (int)((busy - prev_busy) * 100 / (total - prev_total));
    prev_busy = busy;
    prev_total = total;
    return pct;
}

/* Battery power draw in mW (voltage x current), -1 when unknown. */
static long long read_battery_power_mw(void)
{
    static const char *const bases[] = {
        "/sys/class/power_supply/axp20x-battery",
        "/sys/class/power_supply/battery",
    };
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        char p[128];
        long long uv, ua, uw;

        snprintf(p, sizeof(p), "%s/power_now", bases[i]);
        if (read_int_file(p, &uw) == 0)
            return (uw < 0 ? -uw : uw) / 1000;
        snprintf(p, sizeof(p), "%s/voltage_now", bases[i]);
        if (read_int_file(p, &uv) != 0)
            continue;
        snprintf(p, sizeof(p), "%s/current_now", bases[i]);
        if (read_int_file(p, &ua) != 0)
            continue;
        if (ua < 0)
            ua = -ua;
        return uv / 1000 * (ua / 1000) / 1000;
    }
    return -1;
}

/* The console battery's charge limit (power_supply ABI; AXP717: kernel
 * patch 0102, which also stops and resumes charging). NULL when the
 * driver has no limit: Settings then hides it. */
static const char *charge_limit_path(void)
{
    static const char *const paths[] = {
        "/sys/class/power_supply/axp20x-battery/charge_control_end_threshold",
        "/sys/class/power_supply/battery/charge_control_end_threshold",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        if (access(paths[i], W_OK) == 0)
            return paths[i];
    return NULL;
}

static int apply_charge_limit(int limit)
{
    const char *path = charge_limit_path();
    char v[16];
    int fd, n, rc;

    if (!path)
        return -1;
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = snprintf(v, sizeof(v), "%d\n", limit);
    rc = write(fd, v, (size_t)n) == n ? 0 : -1;
    close(fd);
    if (rc != 0)
        fprintf(stderr, "systemd: charge limit %d: %s\n", limit, strerror(errno));
    return rc;
}

static void read_memory_kib(long long *total_kib, long long *available_kib);

/* PERF: the performance overlay's figures (sampled about once a second
 * by the Quick Menu while the overlay is shown over a game). */
static void reply_perf(int fd, const struct runtime_state *st)
{
    char buf[512];
    long long mem_total, mem_available;

    read_memory_kib(&mem_total, &mem_available);

    snprintf(buf, sizeof(buf),
        "cpu.load_pct=%d\n"
        "cpu.freq_khz=%lld\n"
        "gpu.freq_hz=%lld\n"
        "temp.cpu_millic=%lld\n"
        "temp.gpu_millic=%lld\n"
        "battery.percent=%d\n"
        "battery.state=%s\n"
        "battery.power_mw=%lld\n"
        "mem.used_mib=%lld\n"
        "end=1\n",
        read_cpu_load_pct(), read_cpu_freq_khz(), read_gpu_freq_hz(),
        read_temp_millic("cpu-thermal"), read_temp_millic("gpu-thermal"),
        st->battery_percent, st->battery_state, read_battery_power_mw(),
        mem_total >= 0 && mem_available >= 0 ? (mem_total - mem_available) / 1024 : -1LL);
    (void)write_all(fd, buf);
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

/* Storage (docs/dev/platform.md "SD2 = games card"): SD1 health from the
 * storage selector, the games card and Data on SD2 from nuubos-storagectl,
 * the console backup from nuubos-sd2-backup. They write these files and poke STORAGE
 * CHANGED; the free space is measured here at every snapshot. */
struct storage_view {
    char sd1_health[32], sd1_action[32];
    char sd2_slot[8], sd2_state[24], sd2_cid[96], sd2_fs[16], sd2_label[64], sd2_prompt[16];
    char backup_enabled[8], backup_items[96], backup_last[24], backup_result[16], backup_card[16];
    char backup_dest[8], backup_available[4], backup_found[4];
    char data_on[64], data_state[16], data_moving[16], data_conflicts[16];
    unsigned long long userdata_total, userdata_free, sd2_total, sd2_free;
};

static void space_of(const char *path, unsigned long long *total, unsigned long long *freeb)
{
    struct statvfs sv;

    *total = *freeb = 0;
    if (statvfs(path, &sv) == 0) {
        *total = (unsigned long long)sv.f_blocks * sv.f_frsize;
        *freeb = (unsigned long long)sv.f_bavail * sv.f_frsize;
    }
}

static void read_storage(struct storage_view *v)
{
    const char *sd1 = "/run/nuubos/storage/storage.status";
    const char *sd2 = "/run/nuubos/storage/sd2.status";
    const char *backup = "/run/nuubos/storage/backup.status";
    const char *data = "/run/nuubos/storage/data.status";

    memset(v, 0, sizeof(*v));
    snprintf(v->sd1_health, sizeof(v->sd1_health), "unknown");
    snprintf(v->sd1_action, sizeof(v->sd1_action), "NONE");
    snprintf(v->sd2_slot, sizeof(v->sd2_slot), "0");
    snprintf(v->sd2_state, sizeof(v->sd2_state), "unknown");
    snprintf(v->sd2_prompt, sizeof(v->sd2_prompt), "none");
    snprintf(v->backup_enabled, sizeof(v->backup_enabled), "1");
    snprintf(v->backup_card, sizeof(v->backup_card), "none");
    snprintf(v->backup_available, sizeof(v->backup_available), "0");
    snprintf(v->backup_found, sizeof(v->backup_found), "0");
    snprintf(v->data_state, sizeof(v->data_state), "none");
    snprintf(v->data_conflicts, sizeof(v->data_conflicts), "0");
    read_key_value_file(sd1, "STORAGE_HEALTH", v->sd1_health, sizeof(v->sd1_health));
    read_key_value_file(sd1, "ACTION_REQUIRED", v->sd1_action, sizeof(v->sd1_action));
    read_key_value_file(sd2, "SD2_SLOT", v->sd2_slot, sizeof(v->sd2_slot));
    read_key_value_file(sd2, "SD2_STATE", v->sd2_state, sizeof(v->sd2_state));
    read_key_value_file(sd2, "SD2_CID", v->sd2_cid, sizeof(v->sd2_cid));
    read_key_value_file(sd2, "SD2_FS", v->sd2_fs, sizeof(v->sd2_fs));
    read_key_value_file(sd2, "SD2_LABEL", v->sd2_label, sizeof(v->sd2_label));
    read_key_value_file(sd2, "SD2_PROMPT", v->sd2_prompt, sizeof(v->sd2_prompt));
    read_key_value_file(backup, "ENABLED", v->backup_enabled, sizeof(v->backup_enabled));
    read_key_value_file(backup, "ITEMS", v->backup_items, sizeof(v->backup_items));
    read_key_value_file(backup, "LAST", v->backup_last, sizeof(v->backup_last));
    read_key_value_file(backup, "RESULT", v->backup_result, sizeof(v->backup_result));
    read_key_value_file(backup, "CARD", v->backup_card, sizeof(v->backup_card));
    read_key_value_file(backup, "DEST", v->backup_dest, sizeof(v->backup_dest));
    read_key_value_file(backup, "AVAILABLE", v->backup_available, sizeof(v->backup_available));
    read_key_value_file(backup, "FOUND", v->backup_found, sizeof(v->backup_found));
    read_key_value_file(data, "DATA_ON", v->data_on, sizeof(v->data_on));
    read_key_value_file(data, "DATA_STATE", v->data_state, sizeof(v->data_state));
    read_key_value_file(data, "DATA_MOVING", v->data_moving, sizeof(v->data_moving));
    read_key_value_file(data, "DATA_CONFLICTS", v->data_conflicts, sizeof(v->data_conflicts));
    space_of("/userdata", &v->userdata_total, &v->userdata_free);
    if (!strcmp(v->sd2_state, "ready") || !strcmp(v->sd2_state, "readonly"))
        space_of("/media/sd2", &v->sd2_total, &v->sd2_free);
}

static void reply_status(int fd, const struct runtime_state *st)
{
    char ip[128],model[256],kernel[128];
    struct storage_view sv;
    struct utsname un;
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
    read_storage(&sv);
    char buf[6144];
    snprintf(buf,sizeof(buf),
        "protocol=1\n"
        "nuubos.version=%s\n"
        "nuubos.build=%s\n"
        "profile.requested=%s\n"
        "profile.effective=%s\n"
        "auto_battery.threshold=%d\n"
        "sleep_after_min=%d\n"
        "charge_limit=%d\n"
        "charge_limit.supported=%d\n"
        "hypersleep=%d\n"
        "fast_startup=%d\n"
        "power_key_sleep=%d\n"
        "lid_wake=%d\n"
        "battery.percent=%d\n"
        "battery.state=%s\n"
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
        "storage.health=%s\n"
        "storage.action_required=%s\n"
        "storage.userdata_total=%llu\n"
        "storage.userdata_free=%llu\n"
        "storage.sd2.slot=%s\n"
        "storage.sd2.state=%s\n"
        "storage.sd2.cid=%s\n"
        "storage.sd2.fs=%s\n"
        "storage.sd2.label=%s\n"
        "storage.sd2.prompt=%s\n"
        "storage.sd2.total=%llu\n"
        "storage.sd2.free=%llu\n"
        "storage.backup.enabled=%s\n"
        "storage.backup.items=%s\n"
        "storage.backup.last=%s\n"
        "storage.backup.result=%s\n"
        "storage.backup.card=%s\n"
        "storage.backup.dest=%s\n"
        "storage.backup.available=%s\n"
        "storage.backup.found=%s\n"
        "storage.data.on=%s\n"
        "storage.data.state=%s\n"
        "storage.data.moving=%s\n"
        "storage.data.conflicts=%s\n"
        "lifecycle.stage=%s\n"
        "lifecycle.last_wake=%s\n"
        "lifecycle.last_sleep_ms=%lld\n"
        "lifecycle.lid=%s\n"
        "diagnostics.boots=%d\n"
        "diagnostics.active=%d\n",
        NUUBOS_PRODUCT_VERSION,NUUBOS_BUILD_ID,
        profile_name(st->cfg.requested_profile),
        profile_name(st->effective_profile),
        st->cfg.auto_battery_threshold,
        st->cfg.sleep_after_min,
        st->cfg.charge_limit,
        charge_limit_path() ? 1 : 0,
        st->cfg.hypersleep ? 1 : 0,
        st->cfg.fast_startup ? 1 : 0,
        st->cfg.power_key_sleep ? 1 : 0,
        st->cfg.lid_wake ? 1 : 0,
        st->battery_percent,st->battery_state,
        cpu,gpu,tcpu,tgpu,mem_total_kib,mem_available_kib,uptime,model,kernel,ip,
        sv.sd1_health,sv.sd1_action,sv.userdata_total,sv.userdata_free,
        sv.sd2_slot,sv.sd2_state,sv.sd2_cid,sv.sd2_fs,sv.sd2_label,sv.sd2_prompt,
        sv.sd2_total,sv.sd2_free,
        sv.backup_enabled,sv.backup_items,sv.backup_last,sv.backup_result,sv.backup_card,
        sv.backup_dest,sv.backup_available,sv.backup_found,
        sv.data_on,sv.data_state,sv.data_moving,sv.data_conflicts,
        idle_stage_name(st->idle_stage),
        st->last_wake_reason,
        st->last_sleep_elapsed_ms,
        st->lid_fd < 0 ? "none" : st->lid_closed ? "closed" : "open",
        diag_boots(), access(DIAG_ACTIVE, F_OK) == 0 ? 1 : 0);
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
    static char *const backup_argv[]={"/usr/sbin/nuubos-sd2-backup","reset",NULL};
    int rc=0;

    config_defaults(&st->cfg);
    if(atomic_save_config(&st->cfg)!=0){fprintf(stderr,"systemd: reset: system config\n");rc=-1;}
    (void)apply_charge_limit(st->cfg.charge_limit);
    mark_activity(st);
    refresh_battery(st);
    update_effective_profile(st,true);

    if(run_argv(audio_argv)!=0){fprintf(stderr,"systemd: reset: audio\n");rc=-1;}
    if(run_argv(display_argv)!=0){fprintf(stderr,"systemd: reset: display\n");rc=-1;}
    if(run_argv(input_argv)!=0){fprintf(stderr,"systemd: reset: input bindings\n");rc=-1;}
    if(run_argv(controllers_argv)!=0){fprintf(stderr,"systemd: reset: controller mappings\n");rc=-1;}
    if(run_argv(backup_argv)!=0){fprintf(stderr,"systemd: reset: SD2 backup settings\n");rc=-1;}
    if(set_diagnostics(0)!=0){fprintf(stderr,"systemd: reset: diagnostic mode\n");rc=-1;}
    return rc;
}

static void handle_command(struct client *c, struct runtime_state *st, const char *line)
{
    if(!strcmp(line,"STATUS")){ reply_status(c->fd,st); return; }
    if(!strcmp(line,"PERF")){ reply_perf(c->fd,st); return; }
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
    if(sscanf(line,"SET CHARGE_LIMIT %d",&x)==1){
        if(!valid_charge_limit(x)){(void)write_all(c->fd,"ERR invalid charge limit\n");return;}
        if(!charge_limit_path()){(void)write_all(c->fd,"ERR unsupported\n");return;}
        st->cfg.charge_limit=x;
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        if(apply_charge_limit(x)!=0){(void)write_all(c->fd,"ERR apply failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET HYPERSLEEP %d",&x)==1){
        if(x!=0 && x!=1){(void)write_all(c->fd,"ERR invalid value\n");return;}
        st->cfg.hypersleep=x==1;
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET FAST_STARTUP %d",&x)==1){
        if(x!=0 && x!=1){(void)write_all(c->fd,"ERR invalid value\n");return;}
        st->cfg.fast_startup=x==1;
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        apply_fast_startup(&st->cfg);
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET LID_WAKE %d",&x)==1){
        if(x!=0 && x!=1){(void)write_all(c->fd,"ERR invalid value\n");return;}
        st->cfg.lid_wake=x==1;
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET POWER_KEY_SLEEP %d",&x)==1){
        if(x!=0 && x!=1){(void)write_all(c->fd,"ERR invalid value\n");return;}
        st->cfg.power_key_sleep=x==1;
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET DIAGNOSTICS %d",&x)==1){
        if(x<0||x>DIAG_MAX_BOOTS){(void)write_all(c->fd,"ERR invalid boots\n");return;}
        if(set_diagnostics(x)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(sscanf(line,"SET SLEEP %d",&x)==1){
        if(x<0||x>240){(void)write_all(c->fd,"ERR invalid timeout\n");return;}
        st->cfg.sleep_after_min=x; mark_activity(st);
        if(atomic_save_config(&st->cfg)!=0){(void)write_all(c->fd,"ERR persist failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    /* nuubos-storagectl / nuubos-sd2-backup changed their state files. */
    if(!strcmp(line,"STORAGE CHANGED")){
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    /* "SET STORAGE BACKUP " (with the space) never matches BACKUP_ITEM or
     * BACKUP_DEST. */
    if(!strncmp(line,"SET STORAGE BACKUP ",19) && sscanf(line,"SET STORAGE BACKUP %63s",val)==1){
        char *argv[]={"/usr/sbin/nuubos-sd2-backup","set","enabled",val,NULL};
        if(strcmp(val,"0") && strcmp(val,"1")){(void)write_all(c->fd,"ERR invalid value\n");return;}
        if(run_argv(argv)!=0){(void)write_all(c->fd,"ERR backup settings failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    {
        char item[32];
        if(sscanf(line,"SET STORAGE BACKUP_ITEM %31s %63s",item,val)==2){
            char *argv[]={"/usr/sbin/nuubos-sd2-backup","set","item",item,val,NULL};
            if(strcmp(val,"0") && strcmp(val,"1")){(void)write_all(c->fd,"ERR invalid value\n");return;}
            if(run_argv(argv)!=0){(void)write_all(c->fd,"ERR backup settings failed\n");return;}
            (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
        }
    }
    if(sscanf(line,"SET STORAGE BACKUP_DEST %63s",val)==1){
        char *argv[]={"/usr/sbin/nuubos-sd2-backup","set","dest",val,NULL};
        if(strcmp(val,"sd2") && strcmp(val,"sd1") && strcmp(val,"usb")){(void)write_all(c->fd,"ERR invalid value\n");return;}
        if(run_argv(argv)!=0){(void)write_all(c->fd,"ERR backup settings failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    /* Quick games-card actions (the long ones are jobs: sd2-format,
     * sd2-prepare, sd2-backup, sd2-restore). */
    if(sscanf(line,"STORAGE SD2 %63s",val)==1){
        /* USE (an ignored or ejected card) returns at once: the card is
         * identified, maybe repaired, in the background; the result arrives
         * as STORAGE CHANGED. */
        const char *verb=!strcmp(val,"EJECT")?"eject":!strcmp(val,"IGNORE")?"ignore":
                         !strcmp(val,"USE")?"use":NULL;
        char *argv[]={"/usr/sbin/nuubos-storagectl","sd2",(char *)verb,NULL};
        if(!verb){(void)write_all(c->fd,"ERR unknown command\n");return;}
        if(run_argv(argv)!=0){(void)write_all(c->fd,!strcmp(val,"EJECT")?"ERR busy\n":"ERR failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    /* Data on SD2 with the card gone for good: a category back on SD1
     * (the moves themselves are the storage-data-move job). */
    if(sscanf(line,"STORAGE DATA FORGET %15s",val)==1){
        char *argv[]={"/usr/sbin/nuubos-storagectl","data","forget",val,NULL};
        if(run_argv(argv)!=0){(void)write_all(c->fd,"ERR failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
    }
    if(!strcmp(line,"STORAGE BACKUP ARCHIVE")){
        char *argv[]={"/usr/sbin/nuubos-sd2-backup","archive",NULL};
        if(run_argv(argv)!=0){(void)write_all(c->fd,"ERR failed\n");return;}
        (void)write_all(c->fd,"OK\n"); notify_subscribers(); return;
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

    if (st->idle_stage == IDLE_ACTIVE && st->cfg.sleep_after_min > 0)
        target = timeout_ms(st->cfg.sleep_after_min);

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
    (void)apply_charge_limit(st.cfg.charge_limit);
    apply_fast_startup(&st.cfg);
    st.idle_stage=IDLE_ACTIVE;
    st.stage_enter_ms=boottime_ms();
    snprintf(st.last_wake_reason,sizeof(st.last_wake_reason),"none");
    st.last_sleep_elapsed_ms=0;
    st.lid_closed=false;
    st.lid_fd=open_lid_switch(&st.lid_closed);
    lighting_wake(); /* Lights back on if a previous instance died asleep (no-op when awake). */
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

        struct pollfd pf[4+MAX_CLIENTS];
        int map[4+MAX_CLIENTS],n=0;
        pf[n]=(struct pollfd){listen_fd,POLLIN,0};map[n++]=-1;
        if(input_fd>=0){pf[n]=(struct pollfd){input_fd,POLLIN|POLLHUP|POLLERR,0};map[n++]=-3;}
        if(status_fd>=0){pf[n]=(struct pollfd){status_fd,POLLIN|POLLHUP|POLLERR,0};map[n++]=-4;}
        if(st.lid_fd>=0){pf[n]=(struct pollfd){st.lid_fd,POLLIN,0};map[n++]=-5;}
        for(int i=0;i<MAX_CLIENTS;i++)if(clients[i].fd>=0){
            pf[n]=(struct pollfd){clients[i].fd,POLLIN|POLLHUP|POLLERR,0};map[n++]=i;
        }
        int rc=poll(pf,n,next_policy_timeout_ms(&st));
        if(rc<0){if(errno==EINTR)continue;break;}
        if(rc==0){
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
            if(map[j]==-5){lid_policy(&st);continue;}
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
            /* A command and its hang-up can come in one wake (storage-changed
             * writes and closes): read first, the read of the end closes. */
            if(pf[j].revents&POLLIN){
                service_client(&clients[i],&st);
            }else if(pf[j].revents&(POLLHUP|POLLERR)){
                close(clients[i].fd);clients[i].fd=-1;clients[i].subscribed=false;clients[i].used=0;
            }
        }
    }

    if (input_fd >= 0)
        close(input_fd);
    if (status_fd >= 0)
        close(status_fd);
    if (st.lid_fd >= 0)
        close(st.lid_fd);
    close(listen_fd);
    unlink(SOCKET_PATH);unlink(PIDFILE);
    return 0;
}
