"""Single-owner, acknowledgement-paced GRBL transport with an explicit simulator."""
import math
import re
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
        self.position_count = 0
        self.spindle_type = None
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
        self.hold_requested_at = None
        self.held_since = None
        self.resume_requested_at = None
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
                        error=self.error, position=dict(self.position) if self.position else None, settings=dict(self.settings), position_count=self.position_count, spindle_type=self.spindle_type, identification=list(self.identification),
                        progress=self.progress, log=list(self.log), active=self.active)

    def connect(self, port):
        if port != 'simulator' and port not in [p['device'] for p in self.ports()]:
            raise ValueError('Select an available USB serial port')
        self.disconnect()
        with self.lock:
            self.error = None
            self.settings = {}
            self.position = None
            self.position_count = 0
            self.spindle_type = None
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
        self.hold_requested_at = self.held_since = self.resume_requested_at = None

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

    def _end_confirmed_hold(self, now):
        if self.held_since is None:
            return
        # A final acknowledgement can start the completion interval during the hold.
        if self.pending:
            self.sent_at += now - max(self.held_since, self.sent_at)
        if self.draining:
            self.drain_after += now - max(self.held_since, self.drain_after)
        self.held_since = None

    def _check_timeouts(self, now):
        if self.active and now - self.last_status > 3:
            self._fault('Status timeout; job aborted')
        elif self.held_since is None:
            if self.pending and now - self.sent_at > self.timeout:
                self._fault('Acknowledgement timeout; job aborted')
            elif self.draining and now - self.drain_after > 120:
                self._fault('Completion timeout; job aborted')
            elif self.hold_requested_at is not None and now - self.hold_requested_at > self.timeout:
                self._fault('Feed hold was not confirmed; job aborted')
            elif self.resume_requested_at is not None and now - self.resume_requested_at > self.timeout:
                self._fault('Resume was not confirmed; job aborted')

    def _idle(self):
        if not self.connected or not self.ready or self.error:
            raise RuntimeError('Controller is not ready; reconnect after resolving the fault')
        if self.active or self.paused or self.pending or self.queue or self.state != 'Idle':
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
                self._write(b'!')
                if self.simulated:
                    if self.active:
                        self.paused, self.state = True, 'Hold'
                    return
                if not self.paused or self.resume_requested_at is not None:
                    self.hold_requested_at = time.monotonic()
                    self.resume_requested_at = None
                    self.paused, self.state = True, 'Hold requested'
                return
            if action == 'resume':
                now = time.monotonic()
                if not self.simulated:
                    self._check_timeouts(now)
                if self.error or not self.ready or not self.paused:
                    raise RuntimeError('No resumable held job')
                if not self.simulated and (self.held_since is None or self.state != 'Hold'
                                            or now - self.last_status > 2):
                    raise RuntimeError('Wait for a fresh, fully stopped feed hold before resuming')
                self._write(b'~')
                self._end_confirmed_hold(now)
                self.hold_requested_at = None
                if self.simulated:
                    self.paused = False
                    self.state = 'Run' if self.active else 'Idle'
                else:
                    # Keep subsequent host lines queued until firmware reports Run or Idle.
                    self.resume_requested_at, self.state = now, 'Resuming'
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

    def _motion_status(self, status, now):
        motion = status.split(':')[0]
        if self.resume_requested_at is not None:
            if motion in ('Run', 'Idle'):
                self.resume_requested_at = None
                self.paused = False
            elif motion == 'Hold':
                return 'Resuming'
        if not self.active and motion == 'Idle':
            self.paused = False
            self.hold_requested_at = self.held_since = None
        if self.active and motion == 'Hold' and not self.paused:
            # A controller-panel feed hold must also stop host transmission.
            self.paused = True
            self.hold_requested_at = now
        if self.paused:
            if status in ('Hold', 'Hold:0'):
                if self.held_since is None:
                    self.held_since = now
            elif self.held_since is not None:
                self._end_confirmed_hold(now)
                self.hold_requested_at = now
            if self.hold_requested_at is not None and motion in ('Idle', 'Run') and not self.draining:
                return 'Hold requested'
        return 'Decelerating' if status == 'Hold:1' else motion

    def _receive(self, line):
        now = time.monotonic()
        self.receive_sequence += 1
        self.log.append(line)
        if line.startswith('<') and line.endswith('>'):
            parts = line[1:-1].split('|')
            self.last_status = now
            reported = parts[0].split(':')[0]
            for part in parts[1:]:
                if part.startswith(('MPos:', 'WPos:')):
                    try:
                        values = list(map(float, part.split(':')[1].split(',')))
                        if not 3 <= len(values) <= 9 or not all(math.isfinite(v) for v in values):
                            raise ValueError('Invalid position')
                        self.position_count = len(values)
                        self.position = dict(zip('xyzabcuvw', values))
                        self.position['basis'] = part.split(':')[0]
                    except ValueError:
                        self.position_count = 0
            if reported == 'Alarm':
                self._fault('Controller alarm; reconnect after resolving it')
                return
            if not self.error:
                self.state = self._motion_status(parts[0], now)
            if self.draining and reported == 'Idle' and self.receive_sequence > self.drain_sequence and not self.error:
                self.active = self.draining = self.paused = False
                self.hold_requested_at = self.held_since = self.resume_requested_at = None
                self.progress = 100
            if '30' in self.settings and '32' in self.settings and not self.pending and not self.init_remaining and not self.error:
                self.ready = True
        elif line.startswith('$') and '=' in line:
            key, value = line[1:].split('=', 1)
            if key.lower() == 'spindle/type':
                self.spindle_type = value.strip().upper()
                return
            if key.lower() == 'laser/fullpower':
                key = 'Laser/FullPower'
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
            if self.pending == '$I' and '[FIRMWARE:grblHAL]' in self.identification and '376' not in self.settings:
                if any(re.fullmatch(r'\[AXS:[4-9]:XYZ[A-Z]+\]', line) for line in self.identification):
                    self.init_remaining.append('$376')
            if self.pending == '$I' and any(
                    re.fullmatch(r'\[MSG:Using machine:CNC_xPRO_V5_XYAZ_(?:NC|NO)\]', row) for row in self.identification):
                self.init_remaining.extend(('$Spindle/Type','$Laser/FullPower'))
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
        elif line.startswith(('[VER:', '[OPT:', '[FIRMWARE:', '[AXS:', '[MSG:Using machine:')):
            self.identification.append(line)
            self.identification = self.identification[-8:]
        elif line.startswith(('Grbl', 'FluidNC')):
            self.settings.clear()
            self.identification.clear()
            self.position_count = 0
            self.spindle_type = None
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
                                line = self.queue.popleft()
                                self.log.append(line)
                                if line.startswith(('G0 ', 'G1 ')):
                                    for word in line.split()[1:]:
                                        if re.fullmatch(r'[XYZABC][+-]?(?:\d+(?:\.\d*)?|\.\d+)', word):
                                            self.position = self.position or {'basis':'WPos','x':0.0,'y':0.0,'z':0.0}
                                            self.position[word[0].lower()] = float(word[1:])
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
                    now = time.monotonic()
                    # An overdue reply cannot revive a lost link or confirm a late hold.
                    self._check_timeouts(now)
                    if raw:
                        self._receive(raw.decode('ascii', errors='replace').strip())
                    if now - polled > .25:
                        self._write(b'?')
                        polled = now
                    if self.active and not self.paused and not self.pending and self.queue and not self.error:
                        self.pending = self.queue.popleft()
                        self._write((self.pending + '\n').encode('ascii'))
                        self.sent_at = now
        except Exception as exc:
            with self.lock:
                self._fault(f'Serial connection failed: {exc}')
                self.connected = False
