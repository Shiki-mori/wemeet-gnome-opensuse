#pragma once
#include <dbus/dbus.h>

// A new owned message only when a fresh native SelectSources needs changing.
// The original body is retained for the caller; its send serial is propagated.
DBusMessage* native_cursor_message(DBusMessage* message);
bool patch_native_cursor(void* qt_dbus_handle);
bool native_cursor_ready();
