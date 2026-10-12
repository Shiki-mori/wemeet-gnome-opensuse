#include <dbus/dbus.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using Send = dbus_bool_t(*)(DBusConnection*, DBusMessage*, dbus_uint32_t*);
using SendReply = dbus_bool_t(*)(DBusConnection*, DBusMessage*, DBusPendingCall**, int);

bool option(DBusMessageIter& options, const char* key, int type, const char* signature, const void* value) {
  DBusMessageIter entry, variant;
  return dbus_message_iter_open_container(&options, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) &&
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) &&
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant) &&
    dbus_message_iter_append_basic(&variant, type, value) &&
    dbus_message_iter_close_container(&entry, &variant) &&
    dbus_message_iter_close_container(&options, &entry);
}

DBusMessage* make_message(int mode, bool foreign_path = false) {
  auto* message = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
    foreign_path ? "/foreign" : "/org/freedesktop/portal/desktop",
    "org.freedesktop.portal.ScreenCast", "SelectSources");
  const char* session = "/org/freedesktop/portal/desktop/session/cursor_test";
  const char* token = "cursor-check";
  const char* restore = "keep-token";
  dbus_uint32_t types = 1, persist = 1;
  dbus_bool_t multiple = FALSE;
  double scale = 3.25;
  DBusMessageIter body, options, entry, variant, array;
  dbus_message_iter_init_append(message, &body);
  if (!dbus_message_iter_append_basic(&body, DBUS_TYPE_OBJECT_PATH, &session) ||
      !dbus_message_iter_open_container(&body, DBUS_TYPE_ARRAY, "{sv}", &options)) return nullptr;
  bool ok = option(options, "handle_token", DBUS_TYPE_STRING, "s", &token) &&
    option(options, "restore_token", DBUS_TYPE_STRING, "s", &restore) &&
    option(options, "types", DBUS_TYPE_UINT32, "u", &types) &&
    option(options, "persist_mode", DBUS_TYPE_UINT32, "u", &persist) &&
    option(options, "multiple", DBUS_TYPE_BOOLEAN, "b", &multiple) &&
    option(options, "scale", DBUS_TYPE_DOUBLE, "d", &scale);
  if (mode == 99) {
    const char* invalid = "hidden";
    ok = ok && option(options, "cursor_mode", DBUS_TYPE_STRING, "s", &invalid);
  } else if (mode >= 0) {
    dbus_uint32_t cursor = mode;
    ok = ok && option(options, "cursor_mode", DBUS_TYPE_UINT32, "u", &cursor);
  }
  const char* key = "extra";
  ok = ok && dbus_message_iter_open_container(&options, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) &&
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) &&
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ai", &variant) &&
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "i", &array);
  for (dbus_int32_t value : {7, 8, 9})
    ok = ok && dbus_message_iter_append_basic(&array, DBUS_TYPE_INT32, &value);
  ok = ok && dbus_message_iter_close_container(&variant, &array) &&
    dbus_message_iter_close_container(&entry, &variant) &&
    dbus_message_iter_close_container(&options, &entry);
  int descriptors[2];
  if (pipe(descriptors) != 0) return nullptr;
  write(descriptors[1], "x", 1);
  close(descriptors[1]);
  ok = ok && option(options, "fd", DBUS_TYPE_UNIX_FD, "h", &descriptors[0]);
  close(descriptors[0]);
  ok = ok && dbus_message_iter_close_container(&body, &options);
  if (!ok) { dbus_message_unref(message); return nullptr; }
  dbus_message_set_auto_start(message, FALSE);
  dbus_message_set_allow_interactive_authorization(message, TRUE);
  return message;
}

bool roundtrip(DBusConnection* connection, DBusMessage* message, SendReply send_reply,
               Send send, unsigned expected) {
  if (!message) return false;
  DBusMessage* reply = nullptr;
  if (send_reply) {
    DBusPendingCall* pending = nullptr;
    if (!send_reply(connection, message, &pending, 2000) || !pending) return false;
    dbus_pending_call_block(pending);
    reply = dbus_pending_call_steal_reply(pending);
    dbus_pending_call_unref(pending);
  } else {
    dbus_uint32_t serial = 0;
    if (!send(connection, message, &serial)) return false;
    dbus_connection_flush(connection);
    for (int attempt = 0; attempt < 100 && !reply; ++attempt) {
      dbus_connection_read_write(connection, 20);
      while (auto* incoming = dbus_connection_pop_message(connection)) {
        if (dbus_message_get_reply_serial(incoming) == serial) { reply = incoming; break; }
        dbus_message_unref(incoming);
      }
    }
  }
  dbus_uint32_t value = 0;
  const bool ok = reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN &&
    dbus_message_get_reply_serial(reply) == dbus_message_get_serial(message) &&
    dbus_message_get_args(reply, nullptr, DBUS_TYPE_UINT32, &value, DBUS_TYPE_INVALID) && value == expected;
  if (!ok) std::fprintf(stderr, "roundtrip expected=%u actual=%u\n", expected, value);
  if (reply) dbus_message_unref(reply);
  dbus_message_unref(message);
  return ok;
}

int main(int argc, char** argv) {
  if (argc != 3) return 1;
  const bool native = std::strcmp(argv[2], "native") == 0;
  void* fixture = dlopen(argv[1], RTLD_NOW | RTLD_DEEPBIND);
  if (!fixture) return 1;
  auto send = reinterpret_cast<Send>(dlsym(fixture, "fixture_send"));
  auto send_reply = reinterpret_cast<SendReply>(dlsym(fixture, "fixture_send_reply"));
  if (!send || !send_reply) return 1;
  DBusError error;
  dbus_error_init(&error);
  auto* connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
  if (!connection) return 1;
  const bool ok = roundtrip(connection, make_message(1), send_reply, nullptr, native ? 2 : 1) &&
    roundtrip(connection, make_message(4), nullptr, send, native ? 2 : 4) &&
    roundtrip(connection, make_message(-1), send_reply, nullptr, native ? 2 : 1) &&
    roundtrip(connection, make_message(2), send_reply, nullptr, 2) &&
    roundtrip(connection, make_message(1), dbus_connection_send_with_reply, nullptr, 1) &&
    roundtrip(connection, make_message(1, true), send_reply, nullptr, 1) &&
    roundtrip(connection, make_message(99), send_reply, nullptr, 99);
  dbus_connection_close(connection);
  dbus_connection_unref(connection);
  dlclose(fixture);
  std::puts("send/send_with_reply, serials, options and FDs preserved; cursor replacement scope verified");
  return ok ? 0 : 1;
}
