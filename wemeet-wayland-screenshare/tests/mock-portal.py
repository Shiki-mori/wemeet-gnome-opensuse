"""Run cancellation/retry checks on a private D-Bus, without a real picker."""
import os
import subprocess
import sys
from gi.repository import Gio, GLib

XML = """<node>
<interface name="org.freedesktop.portal.ScreenCast">
 <method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <property name="version" type="u" access="read"/>
 <property name="AvailableSourceTypes" type="u" access="read"/>
 <property name="AvailableCursorModes" type="u" access="read"/>
</interface>
<interface name="org.freedesktop.portal.Session"><method name="Close"/></interface>
<interface name="org.freedesktop.portal.Request"><method name="Close"/><signal name="Response"><arg type="u"/><arg type="a{sv}"/></signal></interface>
</node>"""

bus = Gio.bus_get_sync(Gio.BusType.SESSION)
bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
              "RequestName", GLib.Variant("(su)", ("org.freedesktop.portal.Desktop", 0)),
              None, Gio.DBusCallFlags.NONE, -1, None)
info = Gio.DBusNodeInfo.new_for_xml(XML)
attempts = 0
starts = 0
closed_requests = 0
registrations = []
request_recipients = {}

def respond(path, code, results):
    # Real portals send request responses to the requesting unique name.
    # libportal subscribes without a broadcast match rule.
    bus.emit_signal(request_recipients[path], path, "org.freedesktop.portal.Request", "Response",
                    GLib.Variant("(ua{sv})", (code, results)))
    return False

def method(connection, sender, path, interface, name, parameters, invocation):
    global attempts, starts, closed_requests
    if name == "Close":
        if interface.endswith("Request"):
            closed_requests += 1
        invocation.return_value(GLib.Variant("()", ()))
        return
    values = parameters.unpack()
    options = values[-1]
    token = options["handle_token"]
    client = sender[1:].replace(".", "_")
    request = f"/org/freedesktop/portal/desktop/request/{client}/{token}"
    request_recipients[request] = sender
    registrations.append(bus.register_object(request, info.interfaces[2], method, None, None))
    invocation.return_value(GLib.Variant("(o)", (request,)))
    if name == "CreateSession":
        attempts += 1
        if attempts == 1:
            GLib.timeout_add(20, respond, request, 1, {})
        else:
            session = f"/org/freedesktop/portal/desktop/session/{client}/{options['session_handle_token']}"
            registrations.append(bus.register_object(session, info.interfaces[1], method, None, None))
            GLib.timeout_add(20, respond, request, 0, {"session_handle": GLib.Variant("s", session)})
    elif name == "SelectSources":
        GLib.timeout_add(20, respond, request, 0, {})
    elif name == "Start":
        starts += 1
        if attempts == 2:
            GLib.timeout_add(20, respond, request, 1, {})
        # Attempt 3 deliberately leaves Start pending until client stop.

def get_property(connection, sender, path, interface, name):
    return GLib.Variant("u", {"version": 5, "AvailableSourceTypes": 3,
                              "AvailableCursorModes": 7}[name])

registrations.append(bus.register_object("/org/freedesktop/portal/desktop",
                                         info.interfaces[0], method, get_property, None))
env = dict(os.environ, DISPLAY="", XDG_SESSION_TYPE="x11")
process = subprocess.Popen([sys.argv[1]], env=env)
loop = GLib.MainLoop()
timed_out = False

def poll():
    if process.poll() is not None:
        loop.quit()
        return False
    return True

def timeout():
    global timed_out
    timed_out = True
    process.kill()
    loop.quit()
    return False

GLib.timeout_add(20, poll)
GLib.timeout_add_seconds(12, timeout)
loop.run()
result = process.wait()
print(f"mock portal: CreateSession={attempts}, Start={starts}, request Close={closed_requests}")
sys.exit(0 if result == 0 and not timed_out and attempts == 3 and starts == 2 else 1)
