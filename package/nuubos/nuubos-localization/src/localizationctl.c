#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/nuubos/localizationd.sock"

int main(int argc, char **argv)
{
    int fd;
    struct sockaddr_un addr;
    char cmd[128], buf[1024];
    ssize_t n;

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        snprintf(cmd, sizeof(cmd), "STATUS\n");
    } else if (argc == 3 && strcmp(argv[1], "language") == 0) {
        snprintf(cmd, sizeof(cmd), "SET LANGUAGE %s\n", argv[2]);
    } else {
        fprintf(stderr, "usage: nuubos-localizationctl [status|language <code>]\n");
        return 2;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    memset(&addr, 0, sizeof(addr)); addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return 1; }
    if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) { close(fd); return 1; }
    shutdown(fd, SHUT_WR);
    while ((n = read(fd, buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
    return 0;
}
