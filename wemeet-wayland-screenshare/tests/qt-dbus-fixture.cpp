#include <dbus/dbus.h>

// Synthetic Qt-shaped PLT fixture; contains no client or Qt binaries.
extern "C" dbus_bool_t fixture_send(DBusConnection* connection, DBusMessage* message, dbus_uint32_t* serial) {
  return dbus_connection_send(connection, message, serial);
}
extern "C" dbus_bool_t fixture_send_reply(DBusConnection* connection, DBusMessage* message,
                                        DBusPendingCall** pending, int timeout) {
  return dbus_connection_send_with_reply(connection, message, pending, timeout);
}
