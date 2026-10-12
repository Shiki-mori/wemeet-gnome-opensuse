"""Verify the scoped send hook on a private bus; no real portal or Qt client."""
import os
import subprocess
import sys
from gi.repository import Gio, GLib

XML = '''<node><interface name="org.freedesktop.portal.ScreenCast">
<method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/>
<arg type="u" direction="out"/></method></interface></node>'''
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
             'RequestName', GLib.Variant('(su)', ('org.freedesktop.portal.Desktop', 0)),
             None, Gio.DBusCallFlags.NONE, 2000, None)
info = Gio.DBusNodeInfo.new_for_xml(XML)
calls = 0
errors = []


def method(connection, sender, path, interface, member, parameters, invocation):
    global calls
    calls += 1
    try:
        session, options = parameters.unpack()
        assert session == '/org/freedesktop/portal/desktop/session/cursor_test'
        assert options['handle_token'] == 'cursor-check' and options['restore_token'] == 'keep-token'
        assert options['types'] == 1 and options['persist_mode'] == 1 and options['multiple'] is False
        assert options['scale'] == 3.25 and options['extra'] == [7, 8, 9]
        message = invocation.get_message()
        assert not (message.get_flags() & Gio.DBusMessageFlags.NO_AUTO_START) == 0
        assert message.get_flags() & Gio.DBusMessageFlags.ALLOW_INTERACTIVE_AUTHORIZATION
        descriptor = message.get_unix_fd_list().get(options['fd'])
        try:
            assert os.read(descriptor, 1) == b'x'
        finally:
            os.close(descriptor)
        mode = options.get('cursor_mode', 1)
        if mode == 'hidden':
            mode = 99
        invocation.return_value(GLib.Variant('(u)', (mode,)))
    except Exception as error:
        errors.append(str(error))
        invocation.return_dbus_error('org.freedesktop.DBus.Error.Failed', str(error))


registrations = [bus.register_object(path, info.interfaces[0], method, None, None)
                 for path in ['/org/freedesktop/portal/desktop', '/foreign']]
environment = dict(os.environ, LD_PRELOAD=sys.argv[1], WEMEET_CAPTURE_MODE=sys.argv[4])
process = subprocess.Popen([sys.argv[2], sys.argv[3],
                            'native' if sys.argv[4] == 'native-portal' else 'bridge'], env=environment)
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


GLib.timeout_add(10, poll)
GLib.timeout_add_seconds(8, timeout)
loop.run()
result = process.wait()
print(f'cursor mock: calls={calls} errors={errors} timeout={timed_out}')
sys.exit(0 if result == 0 and calls == 7 and not errors and not timed_out else 1)
