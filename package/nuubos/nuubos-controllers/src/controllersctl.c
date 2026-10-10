/*
 * nuubos-controllersctl: command-line client of org.nuubOS.Controllers1 for
 * clients without D-Bus (Quick Menu Player Assignment) and for development.
 *
 *   devices           device=<id>\t<name>\t<transport>\t<connected>\t<builtin>\t<preferred>\t<effective>\t<battery>
 *   players           player=<n>\t<controller id>\t<controller name>\t<available>
 *   assign N ID NAME  SetPlayerPreference (ID "" = Automatic) -> OK | ERR <reason>
 *
 * Lists end with "end=1". Tabs/newlines in names are replaced by spaces.
 */
#include <dbus/dbus.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVICE "org.nuubOS.Controllers"
#define PATH "/org/nuubOS/Controllers"
#define INTERFACE "org.nuubOS.Controllers1"
#define TIMEOUT_MS 3000

static void print_clean(const char *s)
{
	for (; *s; s++)
		putchar(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
}

static DBusMessage *call(DBusConnection *conn, DBusMessage *msg)
{
	DBusError err = DBUS_ERROR_INIT;
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, TIMEOUT_MS, &err);

	dbus_message_unref(msg);
	if (!reply) {
		printf("ERR %s\n", err.message ? err.message : "dbus");
		dbus_error_free(&err);
	}
	return reply;
}

/* Prints one struct field: s, b or i. */
static void print_field(DBusMessageIter *it)
{
	const char *s;
	dbus_bool_t b;
	dbus_int32_t i;

	switch (dbus_message_iter_get_arg_type(it)) {
	case DBUS_TYPE_STRING:
		dbus_message_iter_get_basic(it, &s);
		print_clean(s);
		break;
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_get_basic(it, &b);
		putchar(b ? '1' : '0');
		break;
	case DBUS_TYPE_INT32:
		dbus_message_iter_get_basic(it, &i);
		printf("%d", (int)i);
		break;
	default:
		break;
	}
}

static int list(DBusConnection *conn, const char *method, const char *prefix)
{
	DBusMessageIter root, array, item;
	DBusMessage *reply = call(conn, dbus_message_new_method_call(SERVICE, PATH, INTERFACE, method));

	if (!reply)
		return 1;
	if (!dbus_message_iter_init(reply, &root) ||
	    dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
		printf("ERR reply\n");
		dbus_message_unref(reply);
		return 1;
	}
	dbus_message_iter_recurse(&root, &array);
	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
		bool first = true;

		dbus_message_iter_recurse(&array, &item);
		printf("%s=", prefix);
		do {
			if (!first)
				putchar('\t');
			first = false;
			print_field(&item);
		} while (dbus_message_iter_next(&item));
		putchar('\n');
		dbus_message_iter_next(&array);
	}
	printf("end=1\n");
	dbus_message_unref(reply);
	return 0;
}

int main(int argc, char **argv)
{
	DBusError err = DBUS_ERROR_INIT;
	DBusConnection *conn;
	int rc = 2;

	if (argc < 2)
		goto usage;
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (!conn) {
		printf("ERR %s\n", err.message ? err.message : "dbus");
		dbus_error_free(&err);
		return 1;
	}
	if (!strcmp(argv[1], "devices") && argc == 2) {
		rc = list(conn, "GetDevices", "device");
	} else if (!strcmp(argv[1], "players") && argc == 2) {
		rc = list(conn, "GetAssignments", "player");
	} else if (!strcmp(argv[1], "assign") && (argc == 4 || argc == 5)) {
		dbus_int32_t player = atoi(argv[2]);
		const char *id = argv[3];
		const char *name = argc == 5 ? argv[4] : "";
		DBusMessage *msg = dbus_message_new_method_call(SERVICE, PATH, INTERFACE, "SetPlayerPreference");
		DBusMessage *reply;

		if (player < 1 || player > 8 || !msg) {
			printf("ERR player\n");
			return 1;
		}
		dbus_message_append_args(msg, DBUS_TYPE_INT32, &player, DBUS_TYPE_STRING, &id,
					 DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
		reply = call(conn, msg);
		if (!reply)
			return 1;
		dbus_message_unref(reply);
		printf("OK\n");
		rc = 0;
	} else {
		goto usage;
	}
	return rc;
usage:
	fprintf(stderr, "Usage: nuubos-controllersctl devices | players | assign N ID [NAME]\n");
	return 2;
}
