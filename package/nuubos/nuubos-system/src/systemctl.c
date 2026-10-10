#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK "/run/nuubos/systemd.sock"

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;

    while (length > 0) {
        ssize_t written = write(fd, p, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        p += (size_t)written;
        length -= (size_t)written;
    }

    return 0;
}

int main(int argc, char **argv)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;

    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", SOCK);

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (argc < 2) {
        fprintf(stderr,
                "Usage: nuubos-systemctl status | set-profile <auto|battery-saver> | "
                "set-auto-battery <0-50> | set-sleep <min> | "
                "set-storage-backup-policy <off|daily|weekly|monthly> | "
                "set-storage-mode <single|auto|dual> | storage-backup-now | "
                "storage-move-back | storage-adopt <tf2-cid> | sleep | restart | poweroff | activity | reset-system-settings\n");
        close(fd);
        return 2;
    }

    char command[256];

    if (!strcmp(argv[1], "status"))
        snprintf(command, sizeof(command), "STATUS\n");
    else if (!strcmp(argv[1], "set-profile") && argc == 3)
        snprintf(command, sizeof(command), "SET PROFILE %s\n", argv[2]);
    else if (!strcmp(argv[1], "set-auto-battery") && argc == 3)
        snprintf(command, sizeof(command), "SET AUTO_BATTERY %s\n", argv[2]);
    else if (!strcmp(argv[1], "set-sleep") && argc == 3)
        snprintf(command, sizeof(command), "SET SLEEP %s\n", argv[2]);
    else if (!strcmp(argv[1], "set-storage-backup-policy") && argc == 3) {
        char value[32];
        snprintf(value, sizeof(value), "%s", argv[2]);
        for (char *p = value; *p; p++)
            if (*p >= 'a' && *p <= 'z')
                *p = (char)(*p - 'a' + 'A');
        snprintf(command, sizeof(command), "SET STORAGE BACKUP_POLICY %s\n", value);
    } else if (!strcmp(argv[1], "set-storage-mode") && argc == 3) {
        char value[32];
        snprintf(value, sizeof(value), "%s", argv[2]);
        for (char *p = value; *p; p++)
            if (*p >= 'a' && *p <= 'z')
                *p = (char)(*p - 'a' + 'A');
        snprintf(command, sizeof(command), "SET STORAGE MODE %s\n", value);
    } else if (!strcmp(argv[1], "storage-backup-now") && argc == 2)
        snprintf(command, sizeof(command), "START STORAGE BACKUP\n");
    else if (!strcmp(argv[1], "storage-move-back") && argc == 2)
        snprintf(command, sizeof(command), "START STORAGE MOVE_BACK\n");
    else if (!strcmp(argv[1], "storage-adopt") && argc == 3)
        snprintf(command, sizeof(command), "START STORAGE ADOPT %s\n", argv[2]);
    else if (!strcmp(argv[1], "sleep") && argc == 2)
        snprintf(command, sizeof(command), "ACTION SLEEP\n");
    else if (!strcmp(argv[1], "restart") && argc == 2)
        snprintf(command, sizeof(command), "ACTION RESTART\n");
    else if (!strcmp(argv[1], "poweroff") && argc == 2)
        snprintf(command, sizeof(command), "ACTION POWEROFF\n");
    else if (!strcmp(argv[1], "activity") && argc == 2)
        snprintf(command, sizeof(command), "ACTIVITY\n");
    else if (!strcmp(argv[1], "reset-system-settings"))
        snprintf(command, sizeof(command), "RESET SYSTEM SETTINGS\n");
    else {
        fprintf(stderr, "invalid command\n");
        close(fd);
        return 2;
    }

    if (write_all(fd, command, strlen(command)) < 0) {
        perror("write systemd command");
        close(fd);
        return 1;
    }

    if (shutdown(fd, SHUT_WR) < 0) {
        perror("shutdown");
        close(fd);
        return 1;
    }

    char buffer[1024];
    ssize_t count;

    while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
        if (write_all(STDOUT_FILENO, buffer, (size_t)count) < 0) {
            perror("write stdout");
            close(fd);
            return 1;
        }
    }

    if (count < 0) {
        perror("read systemd reply");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
