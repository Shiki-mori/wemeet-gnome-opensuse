"""Exercise PID filtering and two portal lifecycles on a private D-Bus."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from gi.repository import Gio, GLib

SERVICE = 'org.freedesktop.portal.Desktop'
PATH = '/org/freedesktop/portal/desktop'
INTERFACE = 'org.freedesktop.portal.ScreenCast'

if len(sys.argv) > 1 and sys.argv[1] == '--foreign':
    foreign = Gio.bus_get_sync(Gio.BusType.SESSION)
    foreign.call_sync(SERVICE, PATH, INTERFACE, 'CreateSession',
        GLib.Variant('(a{sv})', ({'handle_token': GLib.Variant('s', 'FOREIGN_TOKEN'),
                                'session_handle_token': GLib.Variant('s', 'FOREIGN_SESSION')},)),
        None, Gio.DBusCallFlags.NONE, 2000, None)
    raise SystemExit(0)

XML = '''<node>
<interface name="org.freedesktop.portal.ScreenCast">
 <method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
</interface>
<interface name="org.freedesktop.portal.Session"><method name="Close"/></interface>
</node>'''
server = Gio.bus_get_sync(Gio.BusType.SESSION)
server.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                'RequestName', GLib.Variant('(su)', (SERVICE, 0)), None,
                Gio.DBusCallFlags.NONE, 2000, None)
info = Gio.DBusNodeInfo.new_for_xml(XML)
sessions = []


def method(connection, sender, path, interface, member, parameters, invocation):
    if member == 'Close':
        invocation.return_value(GLib.Variant('()', ()))
        server.emit_signal(sender, path, 'org.freedesktop.portal.Session', 'Closed',
                           GLib.Variant('(a{sv})', ({},)))
        return
    values = parameters.unpack()
    options = values[-1]
    token = options['handle_token']
    client = sender[1:].replace('.', '_')
    request = f'{PATH}/request/{client}/{token}'
    results = {}
    code = 0
    if member == 'CreateSession':
        session = f"{PATH}/session/{client}/{options['session_handle_token']}"
        sessions.append(server.register_object(session, info.interfaces[1], method, None, None))
        results['session_handle'] = GLib.Variant('s', session)
    elif member == 'Start':
        code = 1 if token == 'start0' else 0
        if not code:
            results['streams'] = GLib.Variant('a(ua{sv})', [(42, {
                'source_type': GLib.Variant('u', 1), 'size': GLib.Variant('(ii)', (1920, 1080))})])
    invocation.return_value(GLib.Variant('(o)', (request,)))
    server.emit_signal(sender, request, 'org.freedesktop.portal.Request', 'Response',
                       GLib.Variant('(ua{sv})', (code, results)))


registration = server.register_object(PATH, info.interfaces[0], method, None, None)
address = Gio.dbus_address_get_for_bus_sync(Gio.BusType.SESSION, None)
client = Gio.DBusConnection.new_for_address_sync(address,
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
    None, None)
responses = {}
condition = threading.Condition()


def response(connection, sender, path, interface, member, parameters, userdata):
    with condition:
        responses[path] = parameters.unpack()
        condition.notify_all()


client.signal_subscribe(SERVICE, 'org.freedesktop.portal.Request', 'Response', None, None,
                        Gio.DBusSignalFlags.NONE, response, None)
errors = []
done = threading.Event()


def call(member, arguments):
    reply = client.call_sync(SERVICE, PATH, INTERFACE, member, arguments, None,
                             Gio.DBusCallFlags.NONE, 2000, None).unpack()[0]
    with condition:
        if not condition.wait_for(lambda: reply in responses, timeout=2):
            raise RuntimeError('missing mock portal response')
        return responses.pop(reply)


def drive():
    try:
        for attempt in range(2):
            _, results = call('CreateSession', GLib.Variant('(a{sv})', ({
                'handle_token': GLib.Variant('s', f'create{attempt}'),
                'session_handle_token': GLib.Variant('s', f'session{attempt}')},)))
            session = results['session_handle']
            call('SelectSources', GLib.Variant('(oa{sv})', (session, {
                'handle_token': GLib.Variant('s', f'select{attempt}'), 'cursor_mode': GLib.Variant('u', 4)})))
            call('Start', GLib.Variant('(osa{sv})', (session, '', {
                'handle_token': GLib.Variant('s', f'start{attempt}')})))
            client.call_sync(SERVICE, session, 'org.freedesktop.portal.Session', 'Close',
                             None, None, Gio.DBusCallFlags.NONE, 2000, None)
        subprocess.run([sys.executable, __file__, '--foreign'], check=True, timeout=3)
    except Exception as error:
        errors.append(str(error))
    finally:
        done.set()


with tempfile.TemporaryDirectory(prefix='wemeet-native-monitor-') as directory:
    log_path = Path(directory) / 'trace.log'
    monitor_path = Path(__file__).resolve().parents[1] / 'native-portal-monitor.py'
    monitor = subprocess.Popen([sys.executable, str(monitor_path), '--pid', str(os.getpid()),
                                '--log', str(log_path)])
    loop = GLib.MainLoop()
    worker = None
    deadline = time.monotonic() + 10
    def poll():
        global worker
        trace = log_path.read_text() if log_path.exists() else ''
        if monitor.poll() is not None or time.monotonic() > deadline:
            errors.append('monitor failed or test timed out')
            loop.quit()
            return False
        if worker is None and 'READY' in trace:
            worker = threading.Thread(target=drive)
            worker.start()
        if done.is_set() and trace.count('CLOSED') == 2:
            loop.quit()
            return False
        return True
    GLib.timeout_add(20, poll)
    try:
        loop.run()
    finally:
        monitor.terminate()
        monitor.wait(timeout=3)
        if worker:
            worker.join(timeout=3)
    trace = log_path.read_text()
    assert not errors, errors
    assert trace.count('[native] CALL') == 8, trace
    assert trace.count('[native] RESPONSE') == 6, trace
    assert '"code": 1' in trace and '"node": 42' in trace, trace
    assert trace.count('CLOSED') == 2 and 'FOREIGN_TOKEN' not in trace, trace
    assert 'session0' in trace and 'session1' in trace, trace
    print('two portal lifecycles, cancellation, streams and close recorded; other PID excluded')
