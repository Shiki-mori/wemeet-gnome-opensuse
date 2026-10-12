#include "native-cursor.hpp"
#include "logging.hpp"
#include "plthook.h"
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <unistd.h>

namespace {
using Message = std::unique_ptr<DBusMessage, decltype(&dbus_message_unref)>;
using Send = dbus_bool_t(*)(DBusConnection*, DBusMessage*, dbus_uint32_t*);
using SendReply = dbus_bool_t(*)(DBusConnection*, DBusMessage*, DBusPendingCall**, int);
std::atomic<Send> real_send{nullptr};
std::atomic<SendReply> real_send_reply{nullptr};
std::atomic<bool> ready{false};

bool copy_value(DBusMessageIter& source, DBusMessageIter& destination, unsigned depth = 0) {
  if (depth > 64) return false;
  const int type = dbus_message_iter_get_arg_type(&source);
  if (type == DBUS_TYPE_UNIX_FD) {
    int descriptor = -1;
    dbus_message_iter_get_basic(&source, &descriptor);
    if (descriptor < 0) return false;
    const bool copied = dbus_message_iter_append_basic(&destination, type, &descriptor);
    close(descriptor); // get_basic duplicates a received descriptor.
    return copied;
  }
  if (dbus_type_is_basic(type)) {
    union { dbus_uint64_t integer; double floating; const char* string; } value{};
    dbus_message_iter_get_basic(&source, &value);
    return dbus_message_iter_append_basic(&destination, type, &value);
  }
  if (type != DBUS_TYPE_ARRAY && type != DBUS_TYPE_VARIANT &&
      type != DBUS_TYPE_STRUCT && type != DBUS_TYPE_DICT_ENTRY) return false;
  DBusMessageIter child, output;
  dbus_message_iter_recurse(&source, &child);
  char* signature = nullptr;
  const char* contained = nullptr;
  if (type == DBUS_TYPE_ARRAY) {
    signature = dbus_message_iter_get_signature(&source);
    if (!signature) return false;
    contained = signature + 1;
  } else if (type == DBUS_TYPE_VARIANT) {
    signature = dbus_message_iter_get_signature(&child);
    if (!signature) return false;
    contained = signature;
  }
  const bool opened = dbus_message_iter_open_container(&destination, type, contained, &output);
  dbus_free(signature);
  if (!opened) return false;
  while (dbus_message_iter_get_arg_type(&child) != DBUS_TYPE_INVALID) {
    if (!copy_value(child, output, depth + 1)) {
      dbus_message_iter_abandon_container(&destination, &output);
      return false;
    }
    dbus_message_iter_next(&child);
  }
  return dbus_message_iter_close_container(&destination, &output);
}

bool append_cursor(DBusMessageIter& array) {
  DBusMessageIter entry, variant;
  const char* key = "cursor_mode";
  dbus_uint32_t embedded = 2;
  if (!dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry)) return false;
  if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
      !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant)) {
    dbus_message_iter_abandon_container(&array, &entry);
    return false;
  }
  if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &embedded)) {
    dbus_message_iter_abandon_container(&entry, &variant);
    dbus_message_iter_abandon_container(&array, &entry);
    return false;
  }
  if (!dbus_message_iter_close_container(&entry, &variant)) {
    dbus_message_iter_abandon_container(&array, &entry);
    return false;
  }
  return dbus_message_iter_close_container(&array, &entry);
}

void copy_sent_serial(DBusMessage* original, DBusMessage* sent, dbus_bool_t success) {
  const auto serial = dbus_message_get_serial(sent);
  if (serial) dbus_message_set_serial(original, serial);
  if (success) dbus_message_lock(original);
}

dbus_bool_t send_cursor(DBusConnection* connection, DBusMessage* message, dbus_uint32_t* serial) {
  Message replacement(native_cursor_message(message), dbus_message_unref);
  const auto result = real_send.load()(connection, replacement ? replacement.get() : message, serial);
  if (replacement) copy_sent_serial(message, replacement.get(), result);
  return result;
}

dbus_bool_t send_cursor_reply(DBusConnection* connection, DBusMessage* message,
                            DBusPendingCall** pending, int timeout) {
  Message replacement(native_cursor_message(message), dbus_message_unref);
  const auto result = real_send_reply.load()(connection, replacement ? replacement.get() : message, pending, timeout);
  if (replacement) copy_sent_serial(message, replacement.get(), result);
  return result;
}
} // namespace

DBusMessage* native_cursor_message(DBusMessage* message) {
  if (!message || dbus_message_get_serial(message) != 0 ||
      !dbus_message_is_method_call(message, "org.freedesktop.portal.ScreenCast", "SelectSources") ||
      !dbus_message_has_destination(message, "org.freedesktop.portal.Desktop") ||
      !dbus_message_has_path(message, "/org/freedesktop/portal/desktop") ||
      !dbus_message_has_signature(message, "oa{sv}")) return nullptr;
  DBusMessageIter source, options, entry;
  dbus_message_iter_init(message, &source);
  dbus_message_iter_next(&source);
  dbus_message_iter_recurse(&source, &options);
  dbus_uint32_t old_mode = 1;
  bool found = false;
  while (dbus_message_iter_get_arg_type(&options) != DBUS_TYPE_INVALID) {
    dbus_message_iter_recurse(&options, &entry);
    const char* key = nullptr;
    dbus_message_iter_get_basic(&entry, &key);
    if (std::strcmp(key, "cursor_mode") == 0) {
      if (found) return nullptr;
      found = true;
      dbus_message_iter_next(&entry);
      DBusMessageIter value;
      dbus_message_iter_recurse(&entry, &value);
      if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_UINT32) return nullptr;
      dbus_message_iter_get_basic(&value, &old_mode);
    }
    dbus_message_iter_next(&options);
  }
  if (found && old_mode == 2) return nullptr;
  Message output(dbus_message_new_method_call(dbus_message_get_destination(message),
      dbus_message_get_path(message), dbus_message_get_interface(message), dbus_message_get_member(message)),
      dbus_message_unref);
  if (!output) return nullptr;
  dbus_message_set_auto_start(output.get(), dbus_message_get_auto_start(message));
  dbus_message_set_no_reply(output.get(), dbus_message_get_no_reply(message));
  dbus_message_set_allow_interactive_authorization(output.get(), dbus_message_get_allow_interactive_authorization(message));
  DBusMessageIter destination, array;
  dbus_message_iter_init(message, &source);
  dbus_message_iter_init_append(output.get(), &destination);
  if (!copy_value(source, destination)) return nullptr;
  dbus_message_iter_next(&source);
  dbus_message_iter_recurse(&source, &options);
  if (!dbus_message_iter_open_container(&destination, DBUS_TYPE_ARRAY, "{sv}", &array)) return nullptr;
  bool copied = true;
  while (dbus_message_iter_get_arg_type(&options) != DBUS_TYPE_INVALID && copied) {
    dbus_message_iter_recurse(&options, &entry);
    const char* key = nullptr;
    dbus_message_iter_get_basic(&entry, &key);
    copied = std::strcmp(key, "cursor_mode") == 0 ? append_cursor(array) : copy_value(options, array);
    dbus_message_iter_next(&options);
  }
  if (copied && !found) copied = append_cursor(array);
  if (!copied) {
    dbus_message_iter_abandon_container(&destination, &array);
    return nullptr;
  }
  if (!dbus_message_iter_close_container(&destination, &array)) return nullptr;
  hook_log("[cursor] native SelectSources cursor_mode=%u -> 2 (embedded)%s", old_mode,
           found ? "" : " (default hidden)");
  return output.release();
}

bool native_cursor_ready() { return ready.load(); }

bool patch_native_cursor(void* handle) {
  if (ready.load()) return true;
  // Resolve real exports from the loaded provider, never unbound PLT entries.
  auto send = reinterpret_cast<Send>(dlsym(handle, "dbus_connection_send"));
  auto reply = reinterpret_cast<SendReply>(dlsym(handle, "dbus_connection_send_with_reply"));
  if (!send || !reply) return false;
  plthook_t* plt = nullptr;
  if (plthook_open_by_handle(&plt, handle) != PLTHOOK_SUCCESS) return false;
  real_send.store(send);
  real_send_reply.store(reply);
  const int first = plthook_replace(plt, "dbus_connection_send", reinterpret_cast<void*>(send_cursor), nullptr);
  const int second = plthook_replace(plt, "dbus_connection_send_with_reply", reinterpret_cast<void*>(send_cursor_reply), nullptr);
  plthook_close(plt);
  ready.store(first == PLTHOOK_SUCCESS && second == PLTHOOK_SUCCESS);
  hook_log("[cursor] Qt D-Bus send hooks result=%d/%d ready=%d", first, second, ready.load());
  return ready.load();
}
