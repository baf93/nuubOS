#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/nuubos/localizationd.sock"
#define PIDFILE "/run/nuubos/localizationd.pid"
#define USER_DIR "/run/nuubos/user"
#define ACTIVE_USER USER_DIR "/active"
#define MAX_CLIENTS 8
#define LANGS "en,it,fr,de,es,pt,nl"

struct client { int fd; bool subscribed; char buf[256]; size_t used; };
static struct client clients[MAX_CLIENTS];
static volatile sig_atomic_t stop_requested = 0;
static char current_user[128] = "default";
static char current_language[16] = "en";

static void on_signal(int signo) { (void)signo; stop_requested = 1; }

static int write_all(int fd, const char *s)
{
    size_t left = strlen(s);
    while (left) {
        ssize_t n = write(fd, s, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        s += n;
        left -= (size_t)n;
    }
    return 0;
}

static bool language_supported(const char *code)
{
    static const char *langs[] = {"en","it","fr","de","es","pt","nl"};
    size_t i;
    for (i = 0; i < sizeof(langs)/sizeof(langs[0]); i++)
        if (strcmp(code, langs[i]) == 0) return true;
    return false;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
}

static void refresh_active_user(void)
{
    FILE *f = fopen(ACTIVE_USER, "r");
    char user[128] = "default";
    if (f) {
        if (fgets(user, sizeof(user), f)) {
            trim(user);
            if (!user[0]) snprintf(user, sizeof(user), "default");
        }
        fclose(f);
    }
    snprintf(current_user, sizeof(current_user), "%s", user);
}

static void localization_path(char *out, size_t size)
{
    snprintf(out, size, "/state/users/%s/localization.conf", current_user);
}

static bool read_language(const char *path)
{
    char line[128];
    bool found = false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    while (fgets(line, sizeof(line), f)) {
        char value[32];
        if (sscanf(line, "LANGUAGE=%31s", value) == 1 && language_supported(value)) {
            snprintf(current_language, sizeof(current_language), "%.15s", value);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

/* A user without an own choice gets the device language: the one set while
 * no user is active ("default": initial setup, user picker), else en. */
static void refresh_language(void)
{
    char path[384];
    snprintf(current_language, sizeof(current_language), "en");
    localization_path(path, sizeof(path));
    if (!read_language(path))
        (void)read_language("/state/users/default/localization.conf");
}

static int save_language(const char *code)
{
    char dir[384], path[384], tmp[420];
    FILE *f;
    if (!language_supported(code)) return -1;
    (void)mkdir("/state/users", 0755);
    snprintf(dir, sizeof(dir), "/state/users/%s", current_user);
    (void)mkdir(dir, 0755);
    localization_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "LOCALIZATION_CONFIG_VERSION=1\nLANGUAGE=%s\n", code);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f); unlink(tmp); return -1;
    }
    fclose(f);
    chmod(tmp, 0600);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    snprintf(current_language, sizeof(current_language), "%.15s", code);
    return 0;
}

static void send_status(int fd)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
             "language=%s\nuser=%s\navailable=%s\n",
             current_language, current_user, LANGS);
    (void)write_all(fd, buf);
}

static void notify_subscribers(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].subscribed)
            send_status(clients[i].fd);
}

static int make_listener(void)
{
    int fd;
    struct sockaddr_un addr;
    unlink(SOCKET_PATH);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd); return -1;
    }
    chmod(SOCKET_PATH, 0660);
    return fd;
}

static int make_inotify(void)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return -1;
    (void)mkdir(USER_DIR, 0755);
    (void)inotify_add_watch(fd, USER_DIR,
        IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_ATTRIB);
    return fd;
}

static void accept_clients(int listener)
{
    for (;;) {
        int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        int i;
        if (fd < 0) return;
        for (i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd < 0) break;
        if (i == MAX_CLIENTS) { close(fd); continue; }
        clients[i].fd = fd; clients[i].subscribed = false; clients[i].used = 0;
    }
}

static void handle_line(struct client *c, const char *line)
{
    char code[32];
    if (strcmp(line, "STATUS") == 0) { send_status(c->fd); return; }
    if (strcmp(line, "SUBSCRIBE") == 0) {
        c->subscribed = true;
        send_status(c->fd);
        return;
    }
    if (sscanf(line, "SET LANGUAGE %31s", code) == 1) {
        if (!language_supported(code)) { (void)write_all(c->fd, "ERR unsupported language\n"); return; }
        if (save_language(code) != 0) { (void)write_all(c->fd, "ERR persistence failed\n"); return; }
        (void)write_all(c->fd, "OK\n");
        notify_subscribers();
        return;
    }
    (void)write_all(c->fd, "ERR unknown command\n");
}

static void service_client(struct client *c)
{
    char tmp[256];
    ssize_t n = read(c->fd, tmp, sizeof(tmp) - 1);
    ssize_t i;
    if (n <= 0) { close(c->fd); c->fd = -1; c->subscribed = false; c->used = 0; return; }
    tmp[n] = '\0';
    for (i = 0; i < n; i++) {
        if (tmp[i] == '\n') {
            c->buf[c->used] = '\0';
            if (c->used && c->buf[c->used - 1] == '\r') c->buf[c->used - 1] = '\0';
            handle_line(c, c->buf);
            c->used = 0;
        } else if (c->used + 1 < sizeof(c->buf)) {
            c->buf[c->used++] = tmp[i];
        } else {
            c->used = 0;
        }
    }
}

static void write_pidfile(void)
{
    FILE *f = fopen(PIDFILE, "w");
    if (f) { fprintf(f, "%ld\n", (long)getpid()); fclose(f); }
}

int main(void)
{
    int listener, inotify_fd, i;
    char previous_user[128];
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, on_signal); signal(SIGINT, on_signal);
    for (i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
    (void)mkdir("/run/nuubos", 0755);
    refresh_active_user(); refresh_language();
    listener = make_listener(); inotify_fd = make_inotify();
    if (listener < 0) return 1;
    write_pidfile();

    while (!stop_requested) {
        struct pollfd fds[2 + MAX_CLIENTS];
        int map[2 + MAX_CLIENTS], n = 0, rc, j;
        fds[n] = (struct pollfd){listener, POLLIN, 0}; map[n++] = -1;
        if (inotify_fd >= 0) { fds[n] = (struct pollfd){inotify_fd, POLLIN, 0}; map[n++] = -2; }
        for (i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0) {
            fds[n] = (struct pollfd){clients[i].fd, POLLIN | POLLHUP | POLLERR, 0}; map[n++] = i;
        }
        rc = poll(fds, (nfds_t)n, -1);
        if (rc < 0) { if (errno == EINTR) continue; break; }
        for (j = 0; j < n; j++) {
            if (!fds[j].revents) continue;
            if (map[j] == -1) { accept_clients(listener); continue; }
            if (map[j] == -2) {
                char buf[1024];
                while (read(inotify_fd, buf, sizeof(buf)) > 0) { }
                snprintf(previous_user, sizeof(previous_user), "%s", current_user);
                refresh_active_user();
                if (strcmp(previous_user, current_user) != 0) {
                    refresh_language();
                    notify_subscribers();
                }
                continue;
            }
            i = map[j];
            if (fds[j].revents & (POLLHUP | POLLERR)) {
                close(clients[i].fd); clients[i].fd = -1; clients[i].subscribed = false; clients[i].used = 0;
            } else service_client(&clients[i]);
        }
    }

    for (i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0) close(clients[i].fd);
    if (inotify_fd >= 0) close(inotify_fd);
    close(listener); unlink(SOCKET_PATH); unlink(PIDFILE);
    return 0;
}
