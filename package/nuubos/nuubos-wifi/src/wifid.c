#include <arpa/inet.h>
#include <dbus/dbus.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVICE_NAME "org.nuubOS.Wifi"
#define OBJECT_PATH "/org/nuubOS/Wifi"
#define INTERFACE_NAME "org.nuubOS.Wifi1"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"
#define WIFI_IFACE "wlan0"

struct wifi_state {
    bool enabled;
    char state[24];
    char ssid[128];
    char ipv4[INET_ADDRSTRLEN];
    int32_t signal_dbm;
};

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t refresh_requested = 0;

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
    "<signal name='StateChanged'>"
    "<arg name='enabled' type='b'/>"
    "<arg name='state' type='s'/>"
    "<arg name='ssid' type='s'/>"
    "<arg name='ipv4' type='s'/>"
    "<arg name='signal_dbm' type='i'/>"
    "</signal>"
    "</interface>"
    "<interface name='org.freedesktop.DBus.Introspectable'>"
    "<method name='Introspect'>"
    "<arg name='xml_data' type='s' direction='out'/>"
    "</method>"
    "</interface>"
    "</node>";

static void on_signal(int signo)
{
    if (signo == SIGTERM || signo == SIGINT)
        running = 0;
    else if (signo == SIGUSR1)
        refresh_requested = 1;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;

    if (src == NULL)
        src = "";

    snprintf(dst, dst_size, "%s", src);
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

static void trim_newline(char *value)
{
    size_t len = strlen(value);

    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r')) {
        value[len - 1] = '\0';
        len--;
    }
}

static void refresh_signal_strength(struct wifi_state *state)
{
    FILE *fp;
    char line[128];

    state->signal_dbm = 0;

    if (strcmp(state->state, "connected") != 0)
        return;

    fp = popen("wpa_cli -i wlan0 signal_poll 2>/dev/null", "r");
    if (fp == NULL)
        return;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *value;

        if (strncmp(line, "RSSI=", 5) != 0)
            continue;

        value = line + 5;
        trim_newline(value);
        state->signal_dbm = (int32_t)strtol(value, NULL, 10);
        break;
    }

    (void)pclose(fp);
}

static void refresh_state(struct wifi_state *state)
{
    FILE *fp;
    char line[256];
    char wpa_state[64] = "";

    memset(state, 0, sizeof(*state));
    state->signal_dbm = 0;

    if (access("/sys/class/net/" WIFI_IFACE, F_OK) != 0) {
        copy_string(state->state, sizeof(state->state), "unavailable");
        return;
    }

    state->enabled = interface_up(WIFI_IFACE);
    if (!state->enabled) {
        copy_string(state->state, sizeof(state->state), "off");
        return;
    }

    copy_string(state->state, sizeof(state->state), "disconnected");

    fp = popen("wpa_cli -i wlan0 status 2>/dev/null", "r");
    if (fp == NULL)
        return;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *separator = strchr(line, '=');
        char *key;
        char *value;

        if (separator == NULL)
            continue;

        *separator = '\0';
        key = line;
        value = separator + 1;
        trim_newline(value);

        if (strcmp(key, "wpa_state") == 0)
            copy_string(wpa_state, sizeof(wpa_state), value);
        else if (strcmp(key, "ssid") == 0)
            copy_string(state->ssid, sizeof(state->ssid), value);
        else if (strcmp(key, "ip_address") == 0)
            copy_string(state->ipv4, sizeof(state->ipv4), value);
    }

    (void)pclose(fp);

    if (strcmp(wpa_state, "COMPLETED") == 0) {
        copy_string(state->state, sizeof(state->state), "connected");
    } else if (strcmp(wpa_state, "SCANNING") == 0 ||
               strcmp(wpa_state, "AUTHENTICATING") == 0 ||
               strcmp(wpa_state, "ASSOCIATING") == 0 ||
               strcmp(wpa_state, "ASSOCIATED") == 0 ||
               strcmp(wpa_state, "4WAY_HANDSHAKE") == 0 ||
               strcmp(wpa_state, "GROUP_HANDSHAKE") == 0) {
        copy_string(state->state, sizeof(state->state), "connecting");
    } else if (strcmp(wpa_state, "INTERFACE_DISABLED") == 0) {
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

static bool append_snapshot(DBusMessage *message, const struct wifi_state *state)
{
    dbus_bool_t enabled = state->enabled ? TRUE : FALSE;
    const char *status = state->state;
    const char *ssid = state->ssid;
    const char *ipv4 = state->ipv4;
    dbus_int32_t signal_dbm = state->signal_dbm;

    return dbus_message_append_args(
        message,
        DBUS_TYPE_BOOLEAN, &enabled,
        DBUS_TYPE_STRING, &status,
        DBUS_TYPE_STRING, &ssid,
        DBUS_TYPE_STRING, &ipv4,
        DBUS_TYPE_INT32, &signal_dbm,
        DBUS_TYPE_INVALID);
}

static void emit_state_changed(DBusConnection *conn, const struct wifi_state *state)
{
    DBusMessage *signal;

    signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "StateChanged");
    if (signal == NULL)
        return;

    if (append_snapshot(signal, state)) {
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

static void handle_message(DBusConnection *conn, DBusMessage *message,
                           struct wifi_state *state)
{
    DBusMessage *reply;

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
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY,
                        "Unable to serialize Wi-Fi snapshot");
            return;
        }

        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (dbus_message_is_method_call(message, INTROSPECT_IFACE, "Introspect")) {
        const char *xml = introspection_xml;

        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;

        if (!dbus_message_append_args(reply,
                                      DBUS_TYPE_STRING, &xml,
                                      DBUS_TYPE_INVALID)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY,
                        "Unable to serialize introspection data");
            return;
        }

        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL)
        reply_error(conn, message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
}

int main(void)
{
    DBusConnection *conn;
    DBusError error = DBUS_ERROR_INIT;
    struct sigaction sa;
    struct wifi_state current;
    int request_result;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGUSR1, &sa, NULL);

    conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (conn == NULL) {
        fprintf(stderr, "nuubos-wifid: system bus unavailable: %s\n",
                dbus_error_is_set(&error) ? error.message : "unknown error");
        if (dbus_error_is_set(&error))
            dbus_error_free(&error);
        return 1;
    }

    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    request_result = dbus_bus_request_name(
        conn, SERVICE_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (dbus_error_is_set(&error) || request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "nuubos-wifid: cannot own %s: %s\n",
                SERVICE_NAME,
                dbus_error_is_set(&error) ? error.message : "name already owned");
        if (dbus_error_is_set(&error))
            dbus_error_free(&error);
        dbus_connection_unref(conn);
        return 1;
    }

    refresh_state(&current);

    while (running) {
        DBusMessage *message;

        (void)dbus_connection_read_write(conn, 1000);

        while ((message = dbus_connection_pop_message(conn)) != NULL) {
            handle_message(conn, message, &current);
            dbus_message_unref(message);
        }

        if (refresh_requested) {
            struct wifi_state next;

            refresh_requested = 0;
            refresh_state(&next);
            if (!state_equal(&current, &next)) {
                current = next;
                emit_state_changed(conn, &current);
            }
        }

        if (!dbus_connection_get_is_connected(conn))
            break;
    }

    dbus_connection_unref(conn);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    return 0;
}
