"""Read-only portal diagnostics for one client PID; no capture or state changes."""
import argparse
import datetime
import json
import os
import signal
import time
from gi.repository import Gio, GLib


def unpack(value):
    if isinstance(value, GLib.Variant):
        return unpack(value.unpack())
    if isinstance(value, dict):
        return {key: unpack(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [unpack(item) for item in value]
    return value


class PortalMonitor:
    def __init__(self, pid, log_path):
        self.pid = pid
        self.log_file = os.fdopen(os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_APPEND |
                                         getattr(os, 'O_NOFOLLOW', 0), 0o600), 'w', buffering=1)
        self.control = Gio.bus_get_sync(Gio.BusType.SESSION)
        self.loop = GLib.MainLoop()
        self.clients = {}
        self.pending = {}
        self.requests = set()
        self.sessions = set()
        self.monitor = None
        self.monitoring = False

    def log(self, event, **fields):
        stamp = datetime.datetime.now().astimezone().isoformat(timespec='milliseconds')
        self.log_file.write(f'{stamp} pid={self.pid} [native] {event} ' +
                            json.dumps(fields, ensure_ascii=False, sort_keys=True) + '\n')

    def bus_call(self, member, parameters):
        return self.control.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                                      'org.freedesktop.DBus', member, parameters, None,
                                      Gio.DBusCallFlags.NONE, 1000, None).unpack()

    def is_client(self, name):
        if not name or not name.startswith(':'):
            return False
        if name not in self.clients:
            try:
                self.clients[name] = self.bus_call('GetConnectionUnixProcessID',
                                                   GLib.Variant('(s)', (name,)))[0] == self.pid
            except GLib.Error:
                return False
        return self.clients[name]

    def handle_message(self, message):
        # Runs on the main loop, not the GDBus I/O filter thread: credential
        # lookup must not block the thread dispatching its own synchronous reply.
        kind = message.get_message_type()
        sender = message.get_sender()
        destination = message.get_destination()
        interface = message.get_interface() or ''
        member = message.get_member() or ''
        path = message.get_path() or ''
        body = unpack(message.get_body()) if message.get_body() else []
        if kind == Gio.DBusMessageType.METHOD_CALL:
            if not self.is_client(sender) or not interface.startswith('org.freedesktop.portal.'):
                return False
            serial = message.get_serial()
            self.pending[(sender, serial)] = (interface, member)
            if len(self.pending) > 256:
                self.pending.pop(next(iter(self.pending)))
            fields = dict(sender=sender, serial=serial, interface=interface, member=member, path=path)
            if body and isinstance(body[0], str) and body[0].startswith('/org/freedesktop/portal/'):
                fields['session'] = body[0]
                self.sessions.add(body[0])
            if body and isinstance(body[-1], dict):
                options = body[-1]
                fields['options'] = {key: options[key] for key in
                                    ('handle_token', 'session_handle_token', 'types', 'multiple',
                                     'cursor_mode', 'persist_mode') if key in options}
                fields['has_restore_token'] = bool(options.get('restore_token'))
                token = options.get('handle_token')
                if token:
                    self.requests.add('/org/freedesktop/portal/desktop/request/' +
                                      sender[1:].replace('.', '_') + '/' + str(token))
            self.log('CALL', **fields)
        elif kind in (Gio.DBusMessageType.METHOD_RETURN, Gio.DBusMessageType.ERROR):
            call = self.pending.pop((destination, message.get_reply_serial()), None)
            if not call:
                return False
            fields = dict(member=call[1], reply_serial=message.get_reply_serial())
            if kind == Gio.DBusMessageType.ERROR:
                fields['error'] = message.get_error_name()
                self.log('ERROR', **fields)
            else:
                if body and isinstance(body[0], str) and body[0].startswith('/org/freedesktop/portal/'):
                    fields['request'] = body[0]
                    self.requests.add(body[0])
                self.log('RETURN', **fields)
        elif kind == Gio.DBusMessageType.SIGNAL:
            if not (self.is_client(destination) or path in self.requests or path in self.sessions):
                return False
            if interface == 'org.freedesktop.portal.Request' and member == 'Response':
                results = body[1] if len(body) > 1 and isinstance(body[1], dict) else {}
                fields = dict(path=path, code=body[0] if body else None, result_fields=sorted(results))
                if 'session_handle' in results:
                    fields['session'] = results['session_handle']
                    self.sessions.add(results['session_handle'])
                if 'streams' in results:
                    fields['streams'] = [dict(node=stream[0], **{
                        key: stream[1][key] for key in ('source_type', 'size') if key in stream[1]
                    }) for stream in results['streams']]
                self.log('RESPONSE', **fields)
                self.requests.discard(path)
            elif interface == 'org.freedesktop.portal.Session' and member == 'Closed':
                self.log('CLOSED', session=path)
                self.sessions.discard(path)
        return False

    def message_filter(self, connection, message, incoming, userdata):
        if incoming:
            GLib.idle_add(self.handle_message, message)
            # Observed method calls are addressed to the real portal. Letting
            # GDBus dispatch them would auto-send UnknownMethod from the
            # monitor connection, which the bus correctly forbids.
            if self.monitoring or message.get_message_type() == Gio.DBusMessageType.METHOD_CALL:
                return None
        return message

    def check_target(self):
        try:
            os.kill(self.pid, 0)
        except ProcessLookupError:
            self.log('TARGET_EXITED')
            self.loop.quit()
            return False
        return True

    def run(self):
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    portal_owner = self.bus_call('GetNameOwner',
                        GLib.Variant('(s)', ('org.freedesktop.portal.Desktop',)))[0]
                    break
                except GLib.Error:
                    if time.monotonic() >= deadline or not self.check_target():
                        raise
                    time.sleep(0.1)
            address = Gio.dbus_address_get_for_bus_sync(Gio.BusType.SESSION, None)
            self.monitor = Gio.DBusConnection.new_for_address_sync(address,
                Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
                Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
            self.monitor.add_filter(self.message_filter, None)
            rules = ["type='method_call',interface='org.freedesktop.portal.ScreenCast'",
                     "type='method_call',interface='org.freedesktop.portal.Session',member='Close'",
                     "type='method_call',interface='org.freedesktop.portal.Request',member='Close'",
                     "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
                     "type='signal',interface='org.freedesktop.portal.Session',member='Closed'",
                     f"type='method_return',sender='{portal_owner}'",
                     f"type='error',sender='{portal_owner}'"]
            self.monitor.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus.Monitoring', 'BecomeMonitor', GLib.Variant('(asu)', (rules, 0)),
                None, Gio.DBusCallFlags.NONE, 2000, None)
            self.monitoring = True
            self.log('READY', portal_owner=portal_owner)
            GLib.timeout_add(1000, self.check_target)
            for signum in (signal.SIGINT, signal.SIGTERM):
                GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signum, self.loop.quit)
            self.loop.run()
        except (GLib.Error, OSError) as error:
            self.log('MONITOR_UNAVAILABLE', reason=str(error))
            return 1
        finally:
            if self.monitor and not self.monitor.is_closed():
                self.monitor.close_sync(None)
            self.log_file.close()
        return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--log', required=True)
    arguments = parser.parse_args()
    if arguments.pid <= 0:
        parser.error('--pid must be positive')
    raise SystemExit(PortalMonitor(arguments.pid, arguments.log).run())
