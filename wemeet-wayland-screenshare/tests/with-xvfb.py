import os
import select
import subprocess
import sys

# The regression sends damage on a private X server, never on the live desktop.
read_fd, write_fd = os.pipe()
server = subprocess.Popen([sys.argv[1], '-displayfd', str(write_fd), '-nolisten', 'tcp',
                           '-screen', '0', '64x48x24'], pass_fds=(write_fd,))
os.close(write_fd)
try:
    if not select.select([read_fd], [], [], 5)[0]:
        raise RuntimeError('Xvfb did not publish its display')
    display_number = os.read(read_fd, 32).decode().strip()
    if not display_number.isdigit():
        raise RuntimeError('Xvfb failed to start')
    environment = os.environ.copy()
    environment['DISPLAY'] = ':' + display_number
    sys.exit(subprocess.call(sys.argv[2:], env=environment))
finally:
    os.close(read_fd)
    server.terminate()
    server.wait(timeout=5)
