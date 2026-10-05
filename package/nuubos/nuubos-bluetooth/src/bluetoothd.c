#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuubos/notify.h>

#define SERVICE_NAME "org.nuubOS.Bluetooth"
#define OBJECT_PATH "/org/nuubOS/Bluetooth"
#define INTERFACE_NAME "org.nuubOS.Bluetooth1"
#define AGENT_PATH "/org/nuubOS/BluetoothAgent"
#define AGENT_IFACE "org.bluez.Agent1"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_ROOT "/"
#define BLUEZ_MANAGER_PATH "/org/bluez"
#define OBJECT_MANAGER_IFACE "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define AGENT_MANAGER_IFACE "org.bluez.AgentManager1"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE "org.bluez.Device1"
#define BATTERY_IFACE "org.bluez.Battery1"

/* Accessory battery alerts for Bluetooth devices reporting org.bluez.Battery1.
 * Controllers are announced by the Controllers service instead. */
#define BATTERY_LOW_PERCENT 15
#define BATTERY_REARM_PERCENT 20
/* Battery1 often appears a few seconds after Connected; inside this window
 * it completes the "connected" notification in place. */
#define BATTERY_LATE_UPDATE_S 15

#define MAX_DEVICES 64

struct bluetooth_state {
    bool present;
    bool powered;
    bool discovering;
    char address[32];
    char adapter_path[256];
    uint32_t connected_count;
    uint32_t paired_count;
};

struct bluetooth_device {
    char address[32];
    char name[128];
    char kind[32];
    char path[256];
    bool paired;
    bool connected;
    bool trusted;
    int32_t rssi;
    char icon[64];
    int32_t battery;
    bool battery_low_sent;
    long long connected_since;
};

enum pair_stage {
    PAIR_STAGE_IDLE = 0,
    PAIR_STAGE_PAIRING,
    PAIR_STAGE_CONNECTING,
};

struct pair_operation {
    enum pair_stage stage;
    dbus_uint32_t call_serial;
    DBusMessage *product_request;
    char address[32];
    char device_path[256];
    char secret[32];
    bool has_secret;
    time_t deadline;
};

static struct pair_operation pair_op;
static volatile sig_atomic_t running = 1;

static const char introspection_xml[] =
    "<node>"
    "<interface name='org.nuubOS.Bluetooth1'>"
    "<method name='GetSnapshot'>"
    "<arg name='present' type='b' direction='out'/>"
    "<arg name='powered' type='b' direction='out'/>"
    "<arg name='address' type='s' direction='out'/>"
    "<arg name='connected_count' type='u' direction='out'/>"
    "<arg name='paired_count' type='u' direction='out'/>"
    "</method>"
    "<method name='GetDevices'>"
    "<arg name='devices' type='a(sssbbbi)' direction='out'/>"
    "</method>"
    "<method name='SetEnabled'>"
    "<arg name='enabled' type='b' direction='in'/>"
    "</method>"
    "<method name='StartDiscoverySession'/>"
    "<method name='StopDiscoverySession'/>"
    "<method name='PairAndConnect'>"
    "<arg name='address' type='s' direction='in'/>"
    "</method>"
    "<method name='PairWithPin'>"
    "<arg name='address' type='s' direction='in'/>"
    "<arg name='pin' type='s' direction='in'/>"
    "</method>"
    "<method name='Connect'>"
    "<arg name='address' type='s' direction='in'/>"
    "</method>"
    "<method name='Disconnect'>"
    "<arg name='address' type='s' direction='in'/>"
    "</method>"
    "<method name='Forget'>"
    "<arg name='address' type='s' direction='in'/>"
    "</method>"
    "<signal name='StateChanged'>"
    "<arg name='present' type='b'/>"
    "<arg name='powered' type='b'/>"
    "<arg name='address' type='s'/>"
    "<arg name='connected_count' type='u'/>"
    "<arg name='paired_count' type='u'/>"
    "</signal>"
    "<signal name='DevicesSnapshotChanged'>"
    "<arg name='devices' type='a(sssbbbi)'/>"
    "</signal>"
    "<signal name='DiscoveryStateChanged'>"
    "<arg name='discovering' type='b'/>"
    "</signal>"
    "<signal name='OperationFailed'>"
    "<arg name='operation' type='s'/>"
    "<arg name='message' type='s'/>"
    "</signal>"
    "</interface>"
    "<interface name='org.bluez.Agent1'>"
    "<method name='Release'/>"
    "<method name='RequestPinCode'>"
    "<arg name='device' type='o' direction='in'/>"
    "<arg name='pincode' type='s' direction='out'/>"
    "</method>"
    "<method name='DisplayPinCode'>"
    "<arg name='device' type='o' direction='in'/>"
    "<arg name='pincode' type='s' direction='in'/>"
    "</method>"
    "<method name='RequestPasskey'>"
    "<arg name='device' type='o' direction='in'/>"
    "<arg name='passkey' type='u' direction='out'/>"
    "</method>"
    "<method name='DisplayPasskey'>"
    "<arg name='device' type='o' direction='in'/>"
    "<arg name='passkey' type='u' direction='in'/>"
    "<arg name='entered' type='q' direction='in'/>"
    "</method>"
    "<method name='RequestConfirmation'>"
    "<arg name='device' type='o' direction='in'/>"
    "<arg name='passkey' type='u' direction='in'/>"
    "</method>"
    "<method name='RequestAuthorization'>"
    "<arg name='device' type='o' direction='in'/>"
    "</method>"
    "<method name='AuthorizeService'>"
    "<arg name='device' type='o' direction='in'/>"
    "<arg name='uuid' type='s' direction='in'/>"
    "</method>"
    "<method name='Cancel'/>"
    "</interface>"
    "<interface name='org.freedesktop.DBus.Introspectable'>"
    "<method name='Introspect'>"
    "<arg name='xml_data' type='s' direction='out'/>"
    "</method>"
    "</interface>"
    "</node>";

/* Self-pipe: libdbus restarts its own poll on EINTR, so a stop request must
 * wake the main loop through a descriptor. */
static int wake_pipe[2] = {-1, -1};

static void on_signal(int signo)
{
    int saved_errno = errno;

    if (signo == SIGTERM || signo == SIGINT)
        running = 0;
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

static bool variant_bool(DBusMessageIter *variant, bool *value)
{
    DBusMessageIter inner;
    dbus_bool_t raw;

    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
        return false;
    dbus_message_iter_recurse(variant, &inner);
    if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_BOOLEAN)
        return false;
    dbus_message_iter_get_basic(&inner, &raw);
    *value = raw != FALSE;
    return true;
}

static bool variant_string(DBusMessageIter *variant, const char **value)
{
    DBusMessageIter inner;

    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
        return false;
    dbus_message_iter_recurse(variant, &inner);
    if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_STRING)
        return false;
    dbus_message_iter_get_basic(&inner, value);
    return true;
}

static bool variant_int16(DBusMessageIter *variant, int32_t *value)
{
    DBusMessageIter inner;
    dbus_int16_t raw;

    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
        return false;
    dbus_message_iter_recurse(variant, &inner);
    if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_INT16)
        return false;
    dbus_message_iter_get_basic(&inner, &raw);
    *value = (int32_t)raw;
    return true;
}

static const char *device_kind_from_icon(const char *icon)
{
    if (icon == NULL)
        return "Device";
    if (strstr(icon, "gaming") != NULL ||
        strstr(icon, "joystick") != NULL ||
        strstr(icon, "input") != NULL)
        return "Controller";
    if (strstr(icon, "audio") != NULL ||
        strstr(icon, "headset") != NULL ||
        strstr(icon, "headphones") != NULL)
        return "Audio";
    if (strstr(icon, "keyboard") != NULL)
        return "Keyboard";
    if (strstr(icon, "mouse") != NULL)
        return "Mouse";
    return "Device";
}

static bool state_equal(const struct bluetooth_state *a,
                        const struct bluetooth_state *b)
{
    return a->present == b->present &&
           a->powered == b->powered &&
           a->connected_count == b->connected_count &&
           a->paired_count == b->paired_count &&
           strcmp(a->address, b->address) == 0;
}

static void parse_adapter_properties(DBusMessageIter *properties,
                                     struct bluetooth_state *state)
{
    DBusMessageIter prop = *properties;

    while (dbus_message_iter_get_arg_type(&prop) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        const char *key = NULL;

        dbus_message_iter_recurse(&prop, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &key);

        if (key != NULL && dbus_message_iter_next(&entry)) {
            if (strcmp(key, "Powered") == 0) {
                (void)variant_bool(&entry, &state->powered);
            } else if (strcmp(key, "Discovering") == 0) {
                (void)variant_bool(&entry, &state->discovering);
            } else if (strcmp(key, "Address") == 0) {
                const char *address = NULL;
                if (variant_string(&entry, &address))
                    copy_string(state->address, sizeof(state->address), address);
            }
        }

        dbus_message_iter_next(&prop);
    }
}

static void parse_device_properties(DBusMessageIter *properties,
                                    struct bluetooth_device *device)
{
    DBusMessageIter prop = *properties;
    char icon[64] = "";
    char alias[128] = "";
    char name[128] = "";

    while (dbus_message_iter_get_arg_type(&prop) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        const char *key = NULL;

        dbus_message_iter_recurse(&prop, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &key);

        if (key != NULL && dbus_message_iter_next(&entry)) {
            if (strcmp(key, "Address") == 0) {
                const char *value = NULL;
                if (variant_string(&entry, &value))
                    copy_string(device->address, sizeof(device->address), value);
            } else if (strcmp(key, "Name") == 0) {
                const char *value = NULL;
                if (variant_string(&entry, &value))
                    copy_string(name, sizeof(name), value);
            } else if (strcmp(key, "Alias") == 0) {
                const char *value = NULL;
                if (variant_string(&entry, &value))
                    copy_string(alias, sizeof(alias), value);
            } else if (strcmp(key, "Icon") == 0) {
                const char *value = NULL;
                if (variant_string(&entry, &value))
                    copy_string(icon, sizeof(icon), value);
            } else if (strcmp(key, "Paired") == 0) {
                (void)variant_bool(&entry, &device->paired);
            } else if (strcmp(key, "Connected") == 0) {
                (void)variant_bool(&entry, &device->connected);
            } else if (strcmp(key, "Trusted") == 0) {
                (void)variant_bool(&entry, &device->trusted);
            } else if (strcmp(key, "RSSI") == 0) {
                (void)variant_int16(&entry, &device->rssi);
            }
        }

        dbus_message_iter_next(&prop);
    }

    if (alias[0] != '\0')
        copy_string(device->name, sizeof(device->name), alias);
    else if (name[0] != '\0')
        copy_string(device->name, sizeof(device->name), name);
    else
        copy_string(device->name, sizeof(device->name), device->address);

    copy_string(device->kind, sizeof(device->kind), device_kind_from_icon(icon));
    copy_string(device->icon, sizeof(device->icon), icon);
}

static void parse_battery_properties(DBusMessageIter *properties, int32_t *battery)
{
    DBusMessageIter prop = *properties;

    while (dbus_message_iter_get_arg_type(&prop) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        DBusMessageIter variant;
        const char *key = NULL;

        dbus_message_iter_recurse(&prop, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &key);
        if (key != NULL && strcmp(key, "Percentage") == 0 &&
            dbus_message_iter_next(&entry) &&
            dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&entry, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BYTE) {
                unsigned char value = 0;
                dbus_message_iter_get_basic(&variant, &value);
                if (value <= 100)
                    *battery = value;
            }
        }
        dbus_message_iter_next(&prop);
    }
}

static size_t refresh_all(DBusConnection *conn,
                          struct bluetooth_state *state,
                          struct bluetooth_device *devices,
                          size_t max_devices)
{
    DBusMessage *request = NULL;
    DBusMessage *reply = NULL;
    DBusMessageIter root;
    DBusMessageIter objects;
    DBusError error = DBUS_ERROR_INIT;
    size_t count = 0;

    memset(state, 0, sizeof(*state));
    if (devices != NULL && max_devices > 0)
        memset(devices, 0, sizeof(*devices) * max_devices);

    request = dbus_message_new_method_call(
        BLUEZ_SERVICE, BLUEZ_ROOT, OBJECT_MANAGER_IFACE, "GetManagedObjects");
    if (request == NULL)
        return 0;

    reply = dbus_connection_send_with_reply_and_block(conn, request, 3000, &error);
    dbus_message_unref(request);

    if (reply == NULL) {
        if (dbus_error_is_set(&error))
            dbus_error_free(&error);
        return 0;
    }

    if (!dbus_message_iter_init(reply, &root) ||
        dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return 0;
    }

    dbus_message_iter_recurse(&root, &objects);
    while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter object_entry;
        DBusMessageIter interfaces;
        const char *object_path = NULL;

        dbus_message_iter_recurse(&objects, &object_entry);
        if (dbus_message_iter_get_arg_type(&object_entry) == DBUS_TYPE_OBJECT_PATH)
            dbus_message_iter_get_basic(&object_entry, &object_path);

        if (!dbus_message_iter_next(&object_entry) ||
            dbus_message_iter_get_arg_type(&object_entry) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&objects);
            continue;
        }

        struct bluetooth_device *object_device = NULL;
        int32_t object_battery = -1;

        dbus_message_iter_recurse(&object_entry, &interfaces);
        while (dbus_message_iter_get_arg_type(&interfaces) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter iface_entry;
            DBusMessageIter properties;
            const char *iface_name = NULL;

            dbus_message_iter_recurse(&interfaces, &iface_entry);
            if (dbus_message_iter_get_arg_type(&iface_entry) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&iface_entry, &iface_name);

            if (iface_name != NULL &&
                dbus_message_iter_next(&iface_entry) &&
                dbus_message_iter_get_arg_type(&iface_entry) == DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&iface_entry, &properties);

                if (strcmp(iface_name, ADAPTER_IFACE) == 0) {
                    state->present = true;
                    if (object_path != NULL)
                        copy_string(state->adapter_path, sizeof(state->adapter_path), object_path);
                    parse_adapter_properties(&properties, state);
                } else if (strcmp(iface_name, DEVICE_IFACE) == 0 &&
                           devices != NULL && count < max_devices) {
                    struct bluetooth_device *device = &devices[count];
                    memset(device, 0, sizeof(*device));
                    if (object_path != NULL)
                        copy_string(device->path, sizeof(device->path), object_path);
                    parse_device_properties(&properties, device);
                    if (device->address[0] != '\0') {
                        if (device->connected)
                            state->connected_count++;
                        if (device->paired)
                            state->paired_count++;
                        object_device = device;
                        count++;
                    }
                } else if (strcmp(iface_name, BATTERY_IFACE) == 0) {
                    parse_battery_properties(&properties, &object_battery);
                }
            }

            dbus_message_iter_next(&interfaces);
        }

        /* Battery1 lives on the same object as Device1, in either order. */
        if (object_device != NULL)
            object_device->battery = object_battery;

        dbus_message_iter_next(&objects);
    }

    dbus_message_unref(reply);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    return count;
}

static bool append_snapshot(DBusMessage *message,
                            const struct bluetooth_state *state)
{
    dbus_bool_t present = state->present ? TRUE : FALSE;
    dbus_bool_t powered = state->powered ? TRUE : FALSE;
    const char *address = state->address;
    dbus_uint32_t connected_count = state->connected_count;
    dbus_uint32_t paired_count = state->paired_count;

    return dbus_message_append_args(
        message,
        DBUS_TYPE_BOOLEAN, &present,
        DBUS_TYPE_BOOLEAN, &powered,
        DBUS_TYPE_STRING, &address,
        DBUS_TYPE_UINT32, &connected_count,
        DBUS_TYPE_UINT32, &paired_count,
        DBUS_TYPE_INVALID);
}

static bool append_devices(DBusMessage *message,
                           const struct bluetooth_device *devices,
                           size_t count)
{
    DBusMessageIter root;
    DBusMessageIter array;
    size_t i;

    dbus_message_iter_init_append(message, &root);
    if (!dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY,
                                          "(sssbbbi)", &array))
        return false;

    for (i = 0; i < count; i++) {
        DBusMessageIter item;
        const char *address = devices[i].address;
        const char *name = devices[i].name;
        const char *kind = devices[i].kind;
        dbus_bool_t paired = devices[i].paired ? TRUE : FALSE;
        dbus_bool_t connected = devices[i].connected ? TRUE : FALSE;
        dbus_bool_t trusted = devices[i].trusted ? TRUE : FALSE;
        dbus_int32_t rssi = devices[i].rssi;

        if (!dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &item))
            return false;
        if (!dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &address) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &name) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &kind) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_BOOLEAN, &paired) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_BOOLEAN, &connected) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_BOOLEAN, &trusted) ||
            !dbus_message_iter_append_basic(&item, DBUS_TYPE_INT32, &rssi) ||
            !dbus_message_iter_close_container(&array, &item))
            return false;
    }

    return dbus_message_iter_close_container(&root, &array);
}

static void emit_state_changed(DBusConnection *conn,
                               const struct bluetooth_state *state)
{
    DBusMessage *signal =
        dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "StateChanged");

    if (signal == NULL)
        return;
    if (append_snapshot(signal, state)) {
        (void)dbus_connection_send(conn, signal, NULL);
        dbus_connection_flush(conn);
    }
    dbus_message_unref(signal);
}

static void emit_devices_snapshot(DBusConnection *conn,
                                  const struct bluetooth_device *devices,
                                  size_t count)
{
    DBusMessage *signal =
        dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "DevicesSnapshotChanged");

    if (signal == NULL)
        return;
    if (append_devices(signal, devices, count)) {
        (void)dbus_connection_send(conn, signal, NULL);
        dbus_connection_flush(conn);
    }
    dbus_message_unref(signal);
}

static void emit_discovery_state(DBusConnection *conn, bool discovering)
{
    DBusMessage *signal =
        dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "DiscoveryStateChanged");
    dbus_bool_t value = discovering ? TRUE : FALSE;

    if (signal == NULL)
        return;
    if (dbus_message_append_args(signal, DBUS_TYPE_BOOLEAN, &value, DBUS_TYPE_INVALID)) {
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

static bool reply_ok(DBusMessage *reply, char *error_text, size_t error_size)
{
    if (reply == NULL) {
        copy_string(error_text, error_size, "No reply from BlueZ");
        return false;
    }

    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *name = dbus_message_get_error_name(reply);
        const char *text = NULL;
        DBusMessageIter iter;

        if (dbus_message_iter_init(reply, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&iter, &text);

        if (text != NULL)
            copy_string(error_text, error_size, text);
        else if (name != NULL)
            copy_string(error_text, error_size, name);
        else
            copy_string(error_text, error_size, "BlueZ operation failed");
        return false;
    }

    return true;
}

static DBusMessage *bluez_call(DBusConnection *conn, const char *path,
                               const char *interface, const char *method,
                               int timeout_ms, DBusError *error)
{
    DBusMessage *request;
    DBusMessage *reply;

    request = dbus_message_new_method_call(BLUEZ_SERVICE, path, interface, method);
    if (request == NULL)
        return NULL;

    reply = dbus_connection_send_with_reply_and_block(conn, request, timeout_ms, error);
    dbus_message_unref(request);
    return reply;
}

static bool bluez_simple_call(DBusConnection *conn, const char *path,
                              const char *interface, const char *method,
                              int timeout_ms, char *error_text, size_t error_size)
{
    DBusError error = DBUS_ERROR_INIT;
    DBusMessage *reply = bluez_call(conn, path, interface, method, timeout_ms, &error);
    bool ok;

    if (reply == NULL && dbus_error_is_set(&error)) {
        copy_string(error_text, error_size, error.message);
        dbus_error_free(&error);
        return false;
    }

    ok = reply_ok(reply, error_text, error_size);
    if (reply != NULL)
        dbus_message_unref(reply);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    return ok;
}

static bool set_bool_property(DBusConnection *conn, const char *path,
                              const char *interface, const char *property,
                              bool value, char *error_text, size_t error_size)
{
    DBusMessage *request;
    DBusMessage *reply;
    DBusMessageIter root;
    DBusMessageIter variant;
    DBusError error = DBUS_ERROR_INIT;
    const char *iface = interface;
    const char *prop = property;
    dbus_bool_t raw = value ? TRUE : FALSE;
    bool ok;

    request = dbus_message_new_method_call(
        BLUEZ_SERVICE, path, PROPERTIES_IFACE, "Set");
    if (request == NULL) {
        copy_string(error_text, error_size, "Out of memory");
        return false;
    }

    dbus_message_iter_init_append(request, &root);
    if (!dbus_message_iter_append_basic(&root, DBUS_TYPE_STRING, &iface) ||
        !dbus_message_iter_append_basic(&root, DBUS_TYPE_STRING, &prop) ||
        !dbus_message_iter_open_container(&root, DBUS_TYPE_VARIANT, "b", &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &raw) ||
        !dbus_message_iter_close_container(&root, &variant)) {
        dbus_message_unref(request);
        copy_string(error_text, error_size, "Unable to serialize property");
        return false;
    }

    reply = dbus_connection_send_with_reply_and_block(conn, request, 10000, &error);
    dbus_message_unref(request);

    if (reply == NULL && dbus_error_is_set(&error)) {
        copy_string(error_text, error_size, error.message);
        dbus_error_free(&error);
        return false;
    }

    ok = reply_ok(reply, error_text, error_size);
    if (reply != NULL)
        dbus_message_unref(reply);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    return ok;
}

static struct bluetooth_device *find_device(struct bluetooth_device *devices,
                                            size_t count,
                                            const char *address)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(devices[i].address, address) == 0)
            return &devices[i];
    }
    return NULL;
}

static bool remove_device(DBusConnection *conn, const char *adapter_path,
                          const char *device_path, char *error_text,
                          size_t error_size)
{
    DBusMessage *request;
    DBusMessage *reply;
    DBusError error = DBUS_ERROR_INIT;
    const char *path = device_path;
    bool ok;

    request = dbus_message_new_method_call(
        BLUEZ_SERVICE, adapter_path, ADAPTER_IFACE, "RemoveDevice");
    if (request == NULL) {
        copy_string(error_text, error_size, "Out of memory");
        return false;
    }
    if (!dbus_message_append_args(request,
                                  DBUS_TYPE_OBJECT_PATH, &path,
                                  DBUS_TYPE_INVALID)) {
        dbus_message_unref(request);
        copy_string(error_text, error_size, "Unable to serialize device path");
        return false;
    }

    reply = dbus_connection_send_with_reply_and_block(conn, request, 10000, &error);
    dbus_message_unref(request);

    if (reply == NULL && dbus_error_is_set(&error)) {
        copy_string(error_text, error_size, error.message);
        dbus_error_free(&error);
        return false;
    }

    ok = reply_ok(reply, error_text, error_size);
    if (reply != NULL)
        dbus_message_unref(reply);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    return ok;
}

static void clear_pair_secret(void)
{
    volatile unsigned char *p = (volatile unsigned char *)pair_op.secret;
    size_t i;

    for (i = 0; i < sizeof(pair_op.secret); i++)
        p[i] = 0;
    pair_op.has_secret = false;
}

static void reset_pair_operation(void)
{
    if (pair_op.product_request != NULL)
        dbus_message_unref(pair_op.product_request);

    clear_pair_secret();
    memset(pair_op.address, 0, sizeof(pair_op.address));
    memset(pair_op.device_path, 0, sizeof(pair_op.device_path));
    pair_op.call_serial = 0;
    pair_op.product_request = NULL;
    pair_op.stage = PAIR_STAGE_IDLE;
    pair_op.deadline = 0;
}

static bool start_async_device_call(DBusConnection *conn,
                                    const char *path,
                                    const char *method,
                                    dbus_uint32_t *serial_out,
                                    char *error_text,
                                    size_t error_size)
{
    DBusMessage *request;
    dbus_uint32_t serial = 0;

    request = dbus_message_new_method_call(BLUEZ_SERVICE, path, DEVICE_IFACE, method);
    if (request == NULL) {
        copy_string(error_text, error_size, "Out of memory");
        return false;
    }

    if (!dbus_connection_send(conn, request, &serial) || serial == 0) {
        dbus_message_unref(request);
        copy_string(error_text, error_size, "Unable to start BlueZ operation");
        return false;
    }

    dbus_message_unref(request);
    dbus_connection_flush(conn);
    *serial_out = serial;
    return true;
}

static bool start_pair_operation(DBusConnection *conn,
                                 DBusMessage *product_request,
                                 const struct bluetooth_device *device,
                                 const char *secret,
                                 char *error_text,
                                 size_t error_size)
{
    if (pair_op.stage != PAIR_STAGE_IDLE) {
        copy_string(error_text, error_size, "Another Bluetooth pairing is already in progress");
        return false;
    }

    memset(&pair_op, 0, sizeof(pair_op));
    copy_string(pair_op.address, sizeof(pair_op.address), device->address);
    copy_string(pair_op.device_path, sizeof(pair_op.device_path), device->path);
    if (secret != NULL && secret[0] != '\0') {
        copy_string(pair_op.secret, sizeof(pair_op.secret), secret);
        pair_op.has_secret = true;
    }

    pair_op.product_request = dbus_message_ref(product_request);
    if (!start_async_device_call(conn, device->path, "Pair", &pair_op.call_serial,
                                 error_text, error_size)) {
        reset_pair_operation();
        return false;
    }
    pair_op.stage = PAIR_STAGE_PAIRING;
    pair_op.deadline = time(NULL) + 60;
    return true;
}

static bool pair_request_matches(DBusMessage *message)
{
    DBusMessageIter iter;
    const char *device_path = NULL;

    if (pair_op.stage == PAIR_STAGE_IDLE)
        return false;
    if (!dbus_message_iter_init(message, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
        return false;

    dbus_message_iter_get_basic(&iter, &device_path);
    return device_path != NULL && strcmp(device_path, pair_op.device_path) == 0;
}

static bool register_agent(DBusConnection *conn)
{
    DBusMessage *request;
    DBusMessage *reply;
    DBusError error = DBUS_ERROR_INIT;
    const char *agent_path = AGENT_PATH;
    const char *capability = "KeyboardDisplay";
    bool registered = false;

    request = dbus_message_new_method_call(
        BLUEZ_SERVICE, BLUEZ_MANAGER_PATH, AGENT_MANAGER_IFACE, "RegisterAgent");
    if (request != NULL) {
        if (dbus_message_append_args(request,
                                     DBUS_TYPE_OBJECT_PATH, &agent_path,
                                     DBUS_TYPE_STRING, &capability,
                                     DBUS_TYPE_INVALID)) {
            reply = dbus_connection_send_with_reply_and_block(conn, request, 3000, &error);
            if (reply != NULL) {
                if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR)
                    registered = true;
                else if (dbus_message_get_error_name(reply) != NULL &&
                         strcmp(dbus_message_get_error_name(reply),
                                "org.bluez.Error.AlreadyExists") == 0)
                    registered = true;
                dbus_message_unref(reply);
            }
        }
        dbus_message_unref(request);
    }
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);

    if (!registered)
        return false;

    request = dbus_message_new_method_call(
        BLUEZ_SERVICE, BLUEZ_MANAGER_PATH, AGENT_MANAGER_IFACE, "RequestDefaultAgent");
    if (request == NULL)
        return false;
    if (!dbus_message_append_args(request,
                                  DBUS_TYPE_OBJECT_PATH, &agent_path,
                                  DBUS_TYPE_INVALID)) {
        dbus_message_unref(request);
        return false;
    }

    reply = dbus_connection_send_with_reply_and_block(conn, request, 3000, &error);
    dbus_message_unref(request);
    if (reply != NULL)
        dbus_message_unref(reply);
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        return false;
    }
    return true;
}

static bool parse_address_arg(DBusMessage *message, const char **address)
{
    DBusError error = DBUS_ERROR_INIT;

    if (!dbus_message_get_args(message, &error,
                               DBUS_TYPE_STRING, address,
                               DBUS_TYPE_INVALID)) {
        if (dbus_error_is_set(&error))
            dbus_error_free(&error);
        return false;
    }
    return true;
}

static long long monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec;
}

static bool is_audio_device(const struct bluetooth_device *device)
{
    return strcmp(device->kind, "Audio") == 0;
}

static void post_device_notification(const char *verb, const char *event,
                                     const struct bluetooth_device *device)
{
    struct nuubos_notify n;
    char id[48];

    snprintf(id, sizeof(id), "bt:%.31s", device->address);
    nuubos_notify_begin(&n, verb, id, event);
    nuubos_notify_str(&n, "name", device->name);
    /* Headsets/headphones vs. other audio sinks (speakers, receivers). */
    nuubos_notify_str(&n, "kind",
                      strstr(device->icon, "headset") || strstr(device->icon, "headphone")
                          ? "headphones" : "speaker");
    if (device->battery >= 0 && strcmp(event, "headphones.connected") == 0)
        nuubos_notify_int(&n, "battery", device->battery);
    (void)nuubos_notify_send(&n);
}

static void post_battery_low(const struct bluetooth_device *device)
{
    struct nuubos_notify n;
    char id[48];

    snprintf(id, sizeof(id), "bt-battery:%.31s", device->address);
    nuubos_notify_begin(&n, "POST", id, "accessory.battery.low");
    nuubos_notify_str(&n, "name", device->name);
    nuubos_notify_int(&n, "percent", device->battery);
    nuubos_notify_str(&n, "kind", is_audio_device(device) ? "headphones" : "device");
    (void)nuubos_notify_send(&n);
}

/*
 * Bluetooth owns audio-accessory connection and accessory battery
 * notifications. Only transitions observed between two snapshots are
 * announced, so the startup inventory stays silent.
 */
static void notify_device_changes(const struct bluetooth_device *old_devices,
                                  size_t old_count,
                                  struct bluetooth_device *next_devices,
                                  size_t next_count)
{
    long long now = monotonic_seconds();
    size_t i;
    size_t j;

    for (i = 0; i < next_count; i++) {
        struct bluetooth_device *device = &next_devices[i];
        const struct bluetooth_device *old = NULL;

        for (j = 0; j < old_count; j++) {
            if (strcmp(old_devices[j].address, device->address) == 0) {
                old = &old_devices[j];
                break;
            }
        }
        if (old != NULL) {
            device->battery_low_sent = old->battery_low_sent;
            device->connected_since = old->connected_since;
        }

        if (device->connected && (old == NULL || !old->connected)) {
            device->connected_since = now;
            if (is_audio_device(device))
                post_device_notification("POST", "headphones.connected", device);
        } else if (!device->connected && old != NULL && old->connected) {
            device->battery_low_sent = false;
            if (is_audio_device(device))
                post_device_notification("POST", "headphones.disconnected", device);
        } else if (device->connected && is_audio_device(device) &&
                   old != NULL && old->battery < 0 && device->battery >= 0 &&
                   now - device->connected_since < BATTERY_LATE_UPDATE_S) {
            post_device_notification("UPDATE", "headphones.connected", device);
        }

        if (!device->connected || device->battery < 0 ||
            strcmp(device->kind, "Controller") == 0)
            continue;
        if (device->battery > BATTERY_REARM_PERCENT) {
            device->battery_low_sent = false;
        } else if (device->battery <= BATTERY_LOW_PERCENT && !device->battery_low_sent) {
            device->battery_low_sent = true;
            post_battery_low(device);
        }
    }

    /* A connected device removed outright (Forget) also disconnects. */
    for (j = 0; j < old_count; j++) {
        bool present = false;

        if (!old_devices[j].connected || !is_audio_device(&old_devices[j]))
            continue;
        for (i = 0; i < next_count && !present; i++)
            present = strcmp(next_devices[i].address, old_devices[j].address) == 0;
        if (!present)
            post_device_notification("POST", "headphones.disconnected", &old_devices[j]);
    }
}

static void refresh_and_emit(DBusConnection *conn,
                             struct bluetooth_state *state,
                             struct bluetooth_device *devices,
                             size_t *device_count)
{
    struct bluetooth_state next;
    struct bluetooth_device next_devices[MAX_DEVICES];
    size_t next_count = refresh_all(conn, &next, next_devices, MAX_DEVICES);
    bool old_discovering = state->discovering;

    /* A failed BlueZ query returns an empty inventory: do not read it as
     * every device disconnecting. */
    if (next.present || next_count > 0)
        notify_device_changes(devices, *device_count, next_devices, next_count);

    if (!state_equal(state, &next))
        emit_state_changed(conn, &next);
    if (old_discovering != next.discovering)
        emit_discovery_state(conn, next.discovering);

    *state = next;
    memcpy(devices, next_devices, sizeof(next_devices));
    *device_count = next_count;
    emit_devices_snapshot(conn, devices, *device_count);
}

static void handle_agent_method(DBusConnection *conn, DBusMessage *message)
{
    const char *member = dbus_message_get_member(message);

    if (member == NULL)
        return;

    if (strcmp(member, "RequestPinCode") == 0) {
        DBusMessage *reply;
        const char *pin;

        if (!pair_request_matches(message) || !pair_op.has_secret) {
            reply_error(conn, message, "org.bluez.Error.Rejected",
                        "PIN required; choose Pair with PIN in nuubOS");
            return;
        }

        pin = pair_op.secret;
        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;
        if (!dbus_message_append_args(reply,
                                      DBUS_TYPE_STRING, &pin,
                                      DBUS_TYPE_INVALID)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY,
                        "Unable to serialize PIN");
            return;
        }
        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (strcmp(member, "RequestPasskey") == 0) {
        DBusMessage *reply;
        char *end = NULL;
        unsigned long value;
        dbus_uint32_t passkey;

        if (!pair_request_matches(message) || !pair_op.has_secret) {
            reply_error(conn, message, "org.bluez.Error.Rejected",
                        "Passkey required; choose Pair with PIN in nuubOS");
            return;
        }

        value = strtoul(pair_op.secret, &end, 10);
        if (end == pair_op.secret || *end != '\0' || value > 999999UL) {
            reply_error(conn, message, "org.bluez.Error.Rejected",
                        "Passkey must contain only digits and be at most 6 digits");
            return;
        }

        passkey = (dbus_uint32_t)value;
        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;
        if (!dbus_message_append_args(reply,
                                      DBUS_TYPE_UINT32, &passkey,
                                      DBUS_TYPE_INVALID)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY,
                        "Unable to serialize passkey");
            return;
        }
        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (strcmp(member, "RequestConfirmation") == 0 ||
        strcmp(member, "RequestAuthorization") == 0) {
        if (!pair_request_matches(message)) {
            reply_error(conn, message, "org.bluez.Error.Rejected",
                        "No user-initiated pairing is active");
            return;
        }
        reply_empty(conn, message);
        return;
    }

    if (strcmp(member, "Cancel") == 0) {
        reply_empty(conn, message);
        return;
    }

    reply_empty(conn, message);
}

static bool handle_pair_reply(DBusConnection *conn,
                              DBusMessage *message,
                              struct bluetooth_state *state,
                              struct bluetooth_device *devices,
                              size_t *device_count)
{
    char text[192];
    dbus_uint32_t reply_serial;

    if (pair_op.stage == PAIR_STAGE_IDLE || pair_op.call_serial == 0)
        return false;

    reply_serial = dbus_message_get_reply_serial(message);
    if (reply_serial != pair_op.call_serial)
        return false;
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_RETURN &&
        dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_ERROR)
        return false;

    if (!reply_ok(message, text, sizeof(text))) {
        reply_error(conn, pair_op.product_request,
                    pair_op.stage == PAIR_STAGE_PAIRING
                        ? "org.nuubOS.Bluetooth.Error.PairFailed"
                        : "org.nuubOS.Bluetooth.Error.ConnectFailed",
                    text);
        reset_pair_operation();
        refresh_and_emit(conn, state, devices, device_count);
        return true;
    }

    if (pair_op.stage == PAIR_STAGE_PAIRING) {
        if (!set_bool_property(conn, pair_op.device_path, DEVICE_IFACE,
                               "Trusted", true, text, sizeof(text))) {
            reply_error(conn, pair_op.product_request,
                        "org.nuubOS.Bluetooth.Error.TrustFailed", text);
            reset_pair_operation();
            refresh_and_emit(conn, state, devices, device_count);
            return true;
        }

        if (!start_async_device_call(conn, pair_op.device_path, "Connect",
                                     &pair_op.call_serial, text, sizeof(text))) {
            reply_error(conn, pair_op.product_request,
                        "org.nuubOS.Bluetooth.Error.ConnectFailed", text);
            reset_pair_operation();
            refresh_and_emit(conn, state, devices, device_count);
            return true;
        }
        pair_op.stage = PAIR_STAGE_CONNECTING;
        pair_op.deadline = time(NULL) + 30;
        return true;
    }

    reply_empty(conn, pair_op.product_request);
    reset_pair_operation();
    refresh_and_emit(conn, state, devices, device_count);
    return true;
}

static void expire_pair_operation(DBusConnection *conn,
                                  struct bluetooth_state *state,
                                  struct bluetooth_device *devices,
                                  size_t *device_count)
{
    if (pair_op.stage == PAIR_STAGE_IDLE || pair_op.deadline == 0 ||
        time(NULL) < pair_op.deadline)
        return;

    reply_error(conn, pair_op.product_request,
                "org.nuubOS.Bluetooth.Error.Timeout",
                pair_op.stage == PAIR_STAGE_PAIRING
                    ? "Bluetooth pairing timed out"
                    : "Bluetooth connection timed out");
    reset_pair_operation();
    refresh_and_emit(conn, state, devices, device_count);
}

/* Sleep until D-Bus traffic, a stop signal or the pending pair deadline. */
static void wait_for_work(DBusConnection *conn, int dbus_fd)
{
    struct pollfd fds[2];
    int timeout = -1;
    char buf[64];

    if (dbus_connection_get_dispatch_status(conn) == DBUS_DISPATCH_DATA_REMAINS ||
        !running)
        timeout = 0;
    else if (pair_op.stage != PAIR_STAGE_IDLE && pair_op.deadline != 0) {
        time_t now = time(NULL);
        timeout = pair_op.deadline > now ? (int)(pair_op.deadline - now) * 1000 : 0;
    }

    fds[0].fd = dbus_fd;
    fds[0].events = POLLIN;
    fds[1].fd = wake_pipe[0];
    fds[1].events = POLLIN;
    (void)poll(fds, 2, timeout);
    while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
        ;
}

static void handle_method(DBusConnection *conn, DBusMessage *message,
                          struct bluetooth_state *state,
                          struct bluetooth_device *devices,
                          size_t *device_count)
{
    DBusMessage *reply;
    const char *path = dbus_message_get_path(message);

    if (path != NULL && strcmp(path, AGENT_PATH) == 0 &&
        dbus_message_has_interface(message, AGENT_IFACE)) {
        handle_agent_method(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetSnapshot")) {
        struct bluetooth_state next;
        struct bluetooth_device next_devices[MAX_DEVICES];
        size_t next_count = refresh_all(conn, &next, next_devices, MAX_DEVICES);

        *state = next;
        memcpy(devices, next_devices, sizeof(next_devices));
        *device_count = next_count;

        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;
        if (!append_snapshot(reply, state)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY,
                        "Unable to serialize Bluetooth snapshot");
            return;
        }
        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetDevices")) {
        struct bluetooth_state next;
        struct bluetooth_device next_devices[MAX_DEVICES];
        size_t next_count = refresh_all(conn, &next, next_devices, MAX_DEVICES);

        *state = next;
        memcpy(devices, next_devices, sizeof(next_devices));
        *device_count = next_count;

        reply = dbus_message_new_method_return(message);
        if (reply == NULL)
            return;
        if (!append_devices(reply, devices, *device_count)) {
            dbus_message_unref(reply);
            reply_error(conn, message, DBUS_ERROR_NO_MEMORY,
                        "Unable to serialize Bluetooth devices");
            return;
        }
        (void)dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetEnabled")) {
        DBusError error = DBUS_ERROR_INIT;
        dbus_bool_t enabled = FALSE;
        char text[192];

        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_BOOLEAN, &enabled,
                                   DBUS_TYPE_INVALID)) {
            if (dbus_error_is_set(&error))
                dbus_error_free(&error);
            reply_error(conn, message, DBUS_ERROR_INVALID_ARGS, "Expected enabled boolean");
            return;
        }
        if (!state->present || state->adapter_path[0] == '\0') {
            reply_error(conn, message, "org.nuubOS.Bluetooth.Error.Unavailable",
                        "Bluetooth adapter is not available");
            return;
        }
        if (!set_bool_property(conn, state->adapter_path, ADAPTER_IFACE,
                               "Powered", enabled != FALSE, text, sizeof(text))) {
            reply_error(conn, message, "org.nuubOS.Bluetooth.Error.ToggleFailed", text);
            return;
        }
        refresh_and_emit(conn, state, devices, device_count);
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "StartDiscoverySession")) {
        char text[192];

        if (!state->present || !state->powered || state->adapter_path[0] == '\0') {
            reply_error(conn, message, "org.nuubOS.Bluetooth.Error.Unavailable",
                        "Bluetooth must be enabled before discovery");
            return;
        }
        if (!state->discovering &&
            !bluez_simple_call(conn, state->adapter_path, ADAPTER_IFACE,
                               "StartDiscovery", 10000, text, sizeof(text))) {
            reply_error(conn, message, "org.nuubOS.Bluetooth.Error.DiscoveryFailed", text);
            return;
        }
        refresh_and_emit(conn, state, devices, device_count);
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "StopDiscoverySession")) {
        char text[192];

        if (state->present && state->discovering && state->adapter_path[0] != '\0') {
            if (!bluez_simple_call(conn, state->adapter_path, ADAPTER_IFACE,
                                   "StopDiscovery", 10000, text, sizeof(text))) {
                reply_error(conn, message, "org.nuubOS.Bluetooth.Error.DiscoveryFailed", text);
                return;
            }
        }
        refresh_and_emit(conn, state, devices, device_count);
        reply_empty(conn, message);
        return;
    }

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "PairAndConnect") ||
        dbus_message_is_method_call(message, INTERFACE_NAME, "PairWithPin") ||
        dbus_message_is_method_call(message, INTERFACE_NAME, "Connect") ||
        dbus_message_is_method_call(message, INTERFACE_NAME, "Disconnect") ||
        dbus_message_is_method_call(message, INTERFACE_NAME, "Forget")) {
        const char *address = NULL;
        const char *pin = NULL;
        struct bluetooth_device *device;
        char text[192];

        if (dbus_message_is_method_call(message, INTERFACE_NAME, "PairWithPin")) {
            DBusError args_error = DBUS_ERROR_INIT;
            if (!dbus_message_get_args(message, &args_error,
                                       DBUS_TYPE_STRING, &address,
                                       DBUS_TYPE_STRING, &pin,
                                       DBUS_TYPE_INVALID)) {
                if (dbus_error_is_set(&args_error))
                    dbus_error_free(&args_error);
                reply_error(conn, message, DBUS_ERROR_INVALID_ARGS,
                            "Expected device address and PIN/passkey");
                return;
            }
            if (pin == NULL || pin[0] == '\0' || strlen(pin) > 16) {
                reply_error(conn, message, DBUS_ERROR_INVALID_ARGS,
                            "PIN/passkey must contain 1 to 16 characters");
                return;
            }
        } else if (!parse_address_arg(message, &address)) {
            reply_error(conn, message, DBUS_ERROR_INVALID_ARGS, "Expected device address");
            return;
        }

        {
            struct bluetooth_state next;
            *device_count = refresh_all(conn, &next, devices, MAX_DEVICES);
            *state = next;
        }

        device = find_device(devices, *device_count, address);
        if (device == NULL) {
            reply_error(conn, message, "org.nuubOS.Bluetooth.Error.NotFound",
                        "Bluetooth device is no longer available");
            return;
        }

        if (dbus_message_is_method_call(message, INTERFACE_NAME, "PairAndConnect") ||
            dbus_message_is_method_call(message, INTERFACE_NAME, "PairWithPin")) {
            if (device->paired) {
                if (!set_bool_property(conn, device->path, DEVICE_IFACE,
                                       "Trusted", true, text, sizeof(text))) {
                    reply_error(conn, message, "org.nuubOS.Bluetooth.Error.TrustFailed", text);
                    return;
                }
                if (!bluez_simple_call(conn, device->path, DEVICE_IFACE,
                                       "Connect", 30000, text, sizeof(text)) &&
                    strstr(text, "Already Connected") == NULL &&
                    strstr(text, "Already connected") == NULL) {
                    reply_error(conn, message, "org.nuubOS.Bluetooth.Error.ConnectFailed", text);
                    return;
                }
                refresh_and_emit(conn, state, devices, device_count);
                reply_empty(conn, message);
                return;
            }

            if (!start_pair_operation(
                    conn, message, device,
                    dbus_message_is_method_call(message, INTERFACE_NAME, "PairWithPin")
                        ? pin : NULL,
                    text, sizeof(text))) {
                reply_error(conn, message, "org.nuubOS.Bluetooth.Error.PairFailed", text);
            }
            return;
        } else if (dbus_message_is_method_call(message, INTERFACE_NAME, "Connect")) {
            if (!bluez_simple_call(conn, device->path, DEVICE_IFACE,
                                   "Connect", 30000, text, sizeof(text))) {
                if (strstr(text, "Already Connected") == NULL &&
                    strstr(text, "Already connected") == NULL) {
                    reply_error(conn, message, "org.nuubOS.Bluetooth.Error.ConnectFailed", text);
                    return;
                }
            }
        } else if (dbus_message_is_method_call(message, INTERFACE_NAME, "Disconnect")) {
            if (device->connected &&
                !bluez_simple_call(conn, device->path, DEVICE_IFACE,
                                   "Disconnect", 15000, text, sizeof(text))) {
                reply_error(conn, message, "org.nuubOS.Bluetooth.Error.DisconnectFailed", text);
                return;
            }
        } else {
            if (state->adapter_path[0] == '\0' ||
                !remove_device(conn, state->adapter_path, device->path,
                               text, sizeof(text))) {
                reply_error(conn, message, "org.nuubOS.Bluetooth.Error.ForgetFailed", text);
                return;
            }
        }

        refresh_and_emit(conn, state, devices, device_count);
        reply_empty(conn, message);
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

static bool bluez_state_signal(DBusMessage *message)
{
    return dbus_message_is_signal(message, PROPERTIES_IFACE, "PropertiesChanged") ||
           dbus_message_is_signal(message, OBJECT_MANAGER_IFACE, "InterfacesAdded") ||
           dbus_message_is_signal(message, OBJECT_MANAGER_IFACE, "InterfacesRemoved") ||
           dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged");
}

int main(void)
{
    DBusConnection *conn;
    DBusError error = DBUS_ERROR_INIT;
    struct sigaction sa;
    struct bluetooth_state current;
    struct bluetooth_device devices[MAX_DEVICES];
    size_t device_count;
    int request_result;
    int dbus_fd = -1;

    if (!open_wake_pipe())
        fprintf(stderr, "nuubos-bluetoothd: wake pipe failed\n");
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);

    conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (conn == NULL) {
        fprintf(stderr, "nuubos-bluetoothd: system bus unavailable: %s\n",
                dbus_error_is_set(&error) ? error.message : "unknown error");
        if (dbus_error_is_set(&error))
            dbus_error_free(&error);
        return 1;
    }

    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    request_result = dbus_bus_request_name(conn, SERVICE_NAME, 0, &error);
    if (dbus_error_is_set(&error) ||
        (request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
         request_result != DBUS_REQUEST_NAME_REPLY_IN_QUEUE &&
         request_result != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER)) {
        fprintf(stderr, "nuubos-bluetoothd: cannot own %s: %s\n",
                SERVICE_NAME,
                dbus_error_is_set(&error) ? error.message : "request failed");
        if (dbus_error_is_set(&error))
            dbus_error_free(&error);
        dbus_connection_unref(conn);
        return 1;
    }

    dbus_bus_add_match(
        conn,
        "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',sender='org.bluez'",
        &error);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);

    dbus_bus_add_match(
        conn,
        "type='signal',interface='org.freedesktop.DBus.ObjectManager',sender='org.bluez'",
        &error);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);

    dbus_bus_add_match(
        conn,
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
        &error);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);

    dbus_connection_flush(conn);
    if (request_result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
        request_result == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER)
        (void)register_agent(conn);
    device_count = refresh_all(conn, &current, devices, MAX_DEVICES);

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
            if (handle_pair_reply(conn, message, &current, devices, &device_count)) {
                dbus_message_unref(message);
                continue;
            }

            if (dbus_message_is_signal(message, "org.freedesktop.DBus", "NameAcquired")) {
                const char *name = NULL;
                DBusError name_error = DBUS_ERROR_INIT;
                if (dbus_message_get_args(message, &name_error,
                                          DBUS_TYPE_STRING, &name,
                                          DBUS_TYPE_INVALID) &&
                    name != NULL && strcmp(name, SERVICE_NAME) == 0)
                    (void)register_agent(conn);
                if (dbus_error_is_set(&name_error))
                    dbus_error_free(&name_error);
            } else if (bluez_state_signal(message)) {
                if (dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged"))
                    (void)register_agent(conn);
                refresh_and_emit(conn, &current, devices, &device_count);
            } else {
                handle_method(conn, message, &current, devices, &device_count);
            }
            dbus_message_unref(message);
        }

        expire_pair_operation(conn, &current, devices, &device_count);

        dbus_connection_flush(conn);
        if (!dbus_connection_get_is_connected(conn))
            break;
    }

    reset_pair_operation();
    dbus_connection_unref(conn);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    return 0;
}
