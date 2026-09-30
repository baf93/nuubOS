#include <dbus/dbus.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVICE_NAME "org.nuubOS.Bluetooth"
#define OBJECT_PATH "/org/nuubOS/Bluetooth"
#define INTERFACE_NAME "org.nuubOS.Bluetooth1"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"
#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_ROOT "/"
#define OBJECT_MANAGER_IFACE "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE "org.bluez.Device1"

struct bluetooth_state {
    bool present;
    bool powered;
    char address[32];
    uint32_t connected_count;
    uint32_t paired_count;
};

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
    "<signal name='StateChanged'>"
    "<arg name='present' type='b'/>"
    "<arg name='powered' type='b'/>"
    "<arg name='address' type='s'/>"
    "<arg name='connected_count' type='u'/>"
    "<arg name='paired_count' type='u'/>"
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
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;

    if (src == NULL)
        src = "";

    snprintf(dst, dst_size, "%s", src);
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
                bool powered = false;
                if (variant_bool(&entry, &powered))
                    state->powered = powered;
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
                                    struct bluetooth_state *state)
{
    DBusMessageIter prop = *properties;
    bool connected = false;
    bool paired = false;

    while (dbus_message_iter_get_arg_type(&prop) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        const char *key = NULL;

        dbus_message_iter_recurse(&prop, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &key);

        if (key != NULL && dbus_message_iter_next(&entry)) {
            if (strcmp(key, "Connected") == 0)
                (void)variant_bool(&entry, &connected);
            else if (strcmp(key, "Paired") == 0)
                (void)variant_bool(&entry, &paired);
        }

        dbus_message_iter_next(&prop);
    }

    if (connected)
        state->connected_count++;
    if (paired)
        state->paired_count++;
}

static void parse_interfaces(DBusMessageIter *interfaces,
                             struct bluetooth_state *state)
{
    DBusMessageIter iface = *interfaces;

    while (dbus_message_iter_get_arg_type(&iface) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        DBusMessageIter properties;
        const char *name = NULL;

        dbus_message_iter_recurse(&iface, &entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING) {
            dbus_message_iter_next(&iface);
            continue;
        }

        dbus_message_iter_get_basic(&entry, &name);
        if (!dbus_message_iter_next(&entry) ||
            dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&iface);
            continue;
        }

        dbus_message_iter_recurse(&entry, &properties);

        if (strcmp(name, ADAPTER_IFACE) == 0) {
            state->present = true;
            parse_adapter_properties(&properties, state);
        } else if (strcmp(name, DEVICE_IFACE) == 0) {
            parse_device_properties(&properties, state);
        }

        dbus_message_iter_next(&iface);
    }
}

static void refresh_state(DBusConnection *conn, struct bluetooth_state *state)
{
    DBusMessage *request = NULL;
    DBusMessage *reply = NULL;
    DBusMessageIter root;
    DBusMessageIter objects;
    DBusError error = DBUS_ERROR_INIT;

    memset(state, 0, sizeof(*state));

    request = dbus_message_new_method_call(
        BLUEZ_SERVICE, BLUEZ_ROOT, OBJECT_MANAGER_IFACE, "GetManagedObjects");
    if (request == NULL)
        return;

    reply = dbus_connection_send_with_reply_and_block(conn, request, 3000, &error);
    dbus_message_unref(request);

    if (reply == NULL) {
        if (dbus_error_is_set(&error))
            dbus_error_free(&error);
        return;
    }

    if (!dbus_message_iter_init(reply, &root) ||
        dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return;
    }

    dbus_message_iter_recurse(&root, &objects);
    while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter object_entry;

        dbus_message_iter_recurse(&objects, &object_entry);
        if (!dbus_message_iter_next(&object_entry) ||
            dbus_message_iter_get_arg_type(&object_entry) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&objects);
            continue;
        }

        {
            DBusMessageIter interfaces;
            dbus_message_iter_recurse(&object_entry, &interfaces);
            parse_interfaces(&interfaces, state);
        }

        dbus_message_iter_next(&objects);
    }

    dbus_message_unref(reply);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
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

static void emit_state_changed(DBusConnection *conn,
                               const struct bluetooth_state *state)
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

static void handle_method(DBusConnection *conn, DBusMessage *message,
                          struct bluetooth_state *state)
{
    DBusMessage *reply;

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetSnapshot")) {
        struct bluetooth_state next;

        refresh_state(conn, &next);
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
                        "Unable to serialize Bluetooth snapshot");
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
    int request_result;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
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

    request_result = dbus_bus_request_name(
        conn, SERVICE_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (dbus_error_is_set(&error) || request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "nuubos-bluetoothd: cannot own %s: %s\n",
                SERVICE_NAME,
                dbus_error_is_set(&error) ? error.message : "name already owned");
        if (dbus_error_is_set(&error))
            dbus_error_free(&error);
        dbus_connection_unref(conn);
        return 1;
    }

    dbus_bus_add_match(
        conn,
        "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
        &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "nuubos-bluetoothd: PropertiesChanged match failed: %s\n",
                error.message);
        dbus_error_free(&error);
    }

    dbus_bus_add_match(
        conn,
        "type='signal',interface='org.freedesktop.DBus.ObjectManager'",
        &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "nuubos-bluetoothd: ObjectManager match failed: %s\n",
                error.message);
        dbus_error_free(&error);
    }

    dbus_bus_add_match(
        conn,
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
        &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "nuubos-bluetoothd: NameOwnerChanged match failed: %s\n",
                error.message);
        dbus_error_free(&error);
    }

    dbus_connection_flush(conn);
    refresh_state(conn, &current);

    while (running) {
        DBusMessage *message;

        (void)dbus_connection_read_write(conn, 1000);

        while ((message = dbus_connection_pop_message(conn)) != NULL) {
            if (bluez_state_signal(message)) {
                struct bluetooth_state next;

                refresh_state(conn, &next);
                if (!state_equal(&current, &next)) {
                    current = next;
                    emit_state_changed(conn, &current);
                }
            } else {
                handle_method(conn, message, &current);
            }

            dbus_message_unref(message);
        }

        if (!dbus_connection_get_is_connected(conn))
            break;
    }

    dbus_connection_unref(conn);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    return 0;
}
