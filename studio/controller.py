"""Single-owner, acknowledgement-paced GRBL transport with an explicit simulator."""
import math
import threading
import time
from collections import deque
import serial
from serial.tools import list_ports


class Controller:
    def __init__(self, serial_factory=None, timeout=15, initialization_queries=()):
        self.lock = threading.RLock()
        self.factory = serial_factory or serial.Serial
        self.timeout = timeout
        self.initialization_queries = tuple(initialization_queries)
        self.init_remaining = deque()
        self.initialized = False
        self.identification = []
        self.serial = None
        self.thread = None
        self.stop = threading.Event()
        self.connected = False
        self.simulated = False
        self.state = 'Disconnected'
        self.error = None
        self.position = None
        self.settings = {}
        self.log = deque(maxlen=80)
        self.queue = deque()
        self.pending = None
        self.sent_at = 0
        self.total = self.acked = 0
        self.active = False
        self.paused = False
        self.draining = False
        self.ready = False
        self.last_status = 0
        self.drain_after = 0
        self.receive_sequence = self.drain_sequence = 0
        self.progress = 0

    def ports(self):
        return [{'device': p.device, 'description': p.description} for p in list_ports.comports()]

    def status(self):
        with self.lock:
            return dict(connected=self.connected, simulated=self.simulated, state=self.state,
                        error=self.error, position=self.position, settings=dict(self.settings), identification=list(self.identification),
                        progress=self.progress, log=list(self.log), active=self.active)

    def connect(self, port):
        if port != 'simulator' and port not in [p['device'] for p in self.ports()]:
            raise ValueError('Select an available USB serial port')
        self.disconnect()
        with self.lock:
            self.error = None
            self.settings = {}
            self.position = None
            self.progress = 0
            self.init_remaining = deque(self.initialization_queries)
            self.initialized = False
            self.identification = []
            self.simulated = port == 'simulator'
            self.ready = self.simulated
            self.state = 'Idle' if self.simulated else 'Connecting'
            if self.simulated:
                self.settings = {'30': 1000, '32': 0}
            else:
                try:
                    self.serial = self.factory(port, baudrate=115200, timeout=.05, write_timeout=1)
                except Exception as exc:
                    self.state = 'Disconnected'
                    raise RuntimeError(f'Cannot open serial port: {exc}') from exc
            self.connected = True
            self.last_status = time.monotonic() if self.simulated else 0
            self.stop.clear()
            self.thread = threading.Thread(target=self._loop, daemon=True)
            self.thread.start()

    def disconnect(self):
        with self.lock:
            self.stop.set()
            if self.serial:
                try:
                    if self.active:
                        self.serial.write(b'\x18')
                    self.serial.close()
                except Exception:
                    pass
                self.serial = None
            self.connected = False
            self.ready = False
            self.state = 'Disconnected'
            self._clear()
        if self.thread and self.thread is not threading.current_thread():
            self.thread.join(timeout=2)
        self.thread = None

    def _clear(self):
        self.queue.clear()
        self.pending = None
        self.active = self.paused = self.draining = False

    def _write(self, data):
        if self.serial:
            self.serial.write(data)

    def _fault(self, text):
        self.error = text
        self.log.append(text)
        self.ready = False
        self.state = 'Fault'
        self._clear()
        try:
            self._write(b'\x18')
        except Exception:
            pass

    def _idle(self):
        if not self.connected or not self.ready or self.error:
            raise RuntimeError('Controller is not ready; reconnect after resolving the fault')
        if self.active or self.pending or self.queue or self.state != 'Idle':
            raise RuntimeError('Controller must be idle')
        if not self.simulated and time.monotonic() - self.last_status > 2:
            raise RuntimeError('Controller status is stale')

    def start(self, lines):
        with self.lock:
            self._idle()
            if not isinstance(lines, list) or not lines:
                raise ValueError('Empty job')
            if any(not isinstance(s, str) or '\n' in s or '\r' in s or not s.isascii() or len(s) > 78 for s in lines):
                raise ValueError('Invalid G-code line')
            self.queue = deque(lines)
            self.total, self.acked, self.progress = len(lines), 0, 0
            self.active = True
            self.state = 'Run'

    def command(self, action, **kwargs):
        with self.lock:
            if not self.connected:
                raise RuntimeError('Connect first')
            if action == 'reset':
                self._write(b'\x18')
                self._clear()
                self.ready = self.simulated
                self.state = 'Idle' if self.simulated else 'Reset; reconnect required'
                self.error = None if self.simulated else 'Reset sent. Reconnect before further commands.'
                return
            if action == 'hold':
                self.paused = True
                self._write(b'!')
                if self.simulated:
                    self.state = 'Hold'
                return
            if action == 'resume':
                if self.error or not self.ready or not self.paused:
                    raise RuntimeError('No resumable held job')
                self._write(b'~')
                self.paused = False
                if self.simulated:
                    self.state = 'Run' if self.active else 'Idle'
                return
            self._idle()
            if action == 'jog':
                axis = kwargs.get('axis')
                if axis not in ('X', 'Y', 'Z'):
                    raise ValueError('Invalid jog axis')
                try:
                    distance, feed = float(kwargs['distance']), float(kwargs['feed'])
                except (KeyError, TypeError, ValueError):
                    raise ValueError('Invalid jog distance/feed') from None
                if not math.isfinite(distance) or not math.isfinite(feed) or not 0 < abs(distance) <= 10 or not 0 < feed <= 1000:
                    raise ValueError('Jog limited to 10 mm and 1000 mm/min')
                line = f'$J=G91 G21 {axis}{distance:.3f} F{feed:.1f}'
            else:
                commands = {'home': '$H', 'unlock': '$X', 'zero': 'G10 L20 P1 X0 Y0 Z0'}
                if action not in commands:
                    raise ValueError('Unsupported command')
                line = commands[action]
            self.start([line])

    def _receive(self, line):
        now = time.monotonic()
        self.receive_sequence += 1
        self.log.append(line)
        if line.startswith('<') and line.endswith('>'):
            parts = line[1:-1].split('|')
            self.last_status = now
            reported = parts[0].split(':')[0]
            if not self.error:
                self.state = reported
            for part in parts[1:]:
                if part.startswith(('MPos:', 'WPos:')):
                    try:
                        values = list(map(float, part.split(':')[1].split(',')))
                        self.position = dict(zip(('x', 'y', 'z'), values[:3]))
                        self.position['basis'] = part.split(':')[0]
                    except ValueError:
                        pass
            if reported == 'Alarm':
                self._fault('Controller alarm; reconnect after resolving it')
                return
            if self.draining and reported == 'Idle' and self.receive_sequence > self.drain_sequence and not self.error:
                self.active = self.draining = False
                self.progress = 100
            if '30' in self.settings and '32' in self.settings and not self.pending and not self.init_remaining and not self.error:
                self.ready = True
        elif line.startswith('$') and '=' in line:
            key, value = line[1:].split('=', 1)
            try:
                parsed = float(value.split()[0])
                if not math.isfinite(parsed):
                    self._fault('Nonfinite controller setting readback');return
                self.settings[key] = parsed
            except ValueError:
                pass
        elif line == 'ok':
            if self.pending is None:
                return
            self.pending = None
            if not self.active and self.init_remaining and not self.error:
                self.pending = self.init_remaining.popleft()
                self._write((self.pending+'\n').encode('ascii'))
                self.sent_at = now
            if self.active:
                self.acked += 1
                self.progress = min(99, 100 * self.acked / self.total)
                if not self.queue:
                    self.draining = True
                    self.drain_after = now
                    # Serial receive order is causal even when the platform
                    # monotonic clock returns the same tick for both messages.
                    self.drain_sequence = self.receive_sequence
                    self._write(b'?')
        elif line.startswith(('error:', 'ALARM:')):
            self._fault(line)
        elif line.startswith(('[VER:', '[OPT:', '[FIRMWARE:')):
            self.identification.append(line)
            self.identification = self.identification[-8:]
        elif line.startswith(('Grbl', 'FluidNC')):
            self.settings.clear()
            self.ready = False
            if self.active or self.initialized:
                self._fault('Unexpected controller reset; reconnect and verify settings')

    def _loop(self):
        started = time.monotonic()
        polled = started
        initialized = False
        try:
            while not self.stop.wait(.01):
                with self.lock:
                    now = time.monotonic()
                    if self.simulated:
                        if self.active and not self.paused:
                            if self.queue:
                                self.log.append(self.queue.popleft())
                                self.acked += 1
                                self.progress = min(99, self.acked / self.total * 100)
                            else:
                                self.active = False
                                self.state = 'Idle'
                                self.progress = 100
                        continue
                    # Allow USB-reset boot banners to settle before querying configuration.
                    if not initialized and now - started > 2:
                        self._write(b'$$\n')
                        self.pending = '$$'
                        self.sent_at = now
                        initialized = True
                        self.initialized = True
                    raw = self.serial.readline()
                    if raw:
                        self._receive(raw.decode('ascii', errors='replace').strip())
                    if now - polled > .25:
                        self._write(b'?')
                        polled = now
                    if self.pending and now - self.sent_at > self.timeout and not self.paused:
                        self._fault('Acknowledgement timeout; job aborted')
                    if self.active and self.last_status and now - self.last_status > 3:
                        self._fault('Status timeout; job aborted')
                    if self.draining and now - self.drain_after > 120:
                        self._fault('Completion timeout; job aborted')
                    if self.active and not self.paused and not self.pending and self.queue and not self.error:
                        self.pending = self.queue.popleft()
                        self._write((self.pending + '\n').encode('ascii'))
                        self.sent_at = now
        except Exception as exc:
            with self.lock:
                self._fault(f'Serial connection failed: {exc}')
                self.connected = False
