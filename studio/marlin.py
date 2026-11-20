"""Stop-and-wait Marlin transport; completion requires an acknowledged M400.

Pause only suspends host transmission. Already buffered movement may continue.
M112 requires firmware emergency-parser support for immediate processing; neither
software pause nor abort replaces a physical emergency stop.
"""

import math
import re
import threading
import time
from collections import deque

import serial
from serial.tools import list_ports


class Controller:
    MAX_LINES = 200000
    MAX_RESENDS = 3

    def __init__(self, serial_factory=None, timeout=15, boot_delay=2):
        self.factory = serial_factory or serial.Serial
        self.timeout, self.boot_delay = timeout, boot_delay
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.thread = self.serial = None
        self.connected = self.simulated = self.ready = False
        self.active = self.paused = False
        self.state = 'Disconnected'
        self.error = None
        self.position = None
        self.settings = {}
        self.log = deque(maxlen=80)
        self.queue = deque()
        self.pending = None
        self.number = self.total = self.acked = self.progress = 0
        self.sent_at = 0
        self.resends = 0
        self.resend_handshake = False
        self.receive_buffer = b''

    def ports(self):
        return [{'device': p.device, 'description': p.description} for p in list_ports.comports()]

    def status(self):
        with self.lock:
            return dict(
                connected=self.connected,
                simulated=self.simulated,
                state=self.state,
                error=self.error,
                position=None,
                settings={},
                progress=self.progress,
                active=self.active,
                log=list(self.log),
                protocol='marlin',
                notes='Host pause lets buffered motion drain. Position is not polled. '
                'Completion uses M400. M112 immediacy depends on firmware; '
                'use the hardware emergency stop when needed.',
            )

    @staticmethod
    def frame(number, line):
        payload = f'N{number} {line}'.encode('ascii')
        checksum = 0
        for byte in payload:
            checksum ^= byte
        return payload + f'*{checksum}\n'.encode('ascii')

    def connect(self, port):
        if port != 'simulator' and port not in [p['device'] for p in self.ports()]:
            raise ValueError('Select an available USB serial port')
        self.disconnect()
        with self.lock:
            self.simulated = port == 'simulator'
            self.error = None
            self.number = self.progress = self.acked = self.total = 0
            self.receive_buffer = b''
            self.ready = self.simulated
            self.state = 'Idle' if self.simulated else 'Connecting'
            if not self.simulated:
                try:
                    self.serial = self.factory(port, baudrate=115200, timeout=0.05, write_timeout=1)
                except Exception as exc:
                    self.state = 'Disconnected'
                    raise RuntimeError(f'Cannot open serial port: {exc}') from exc
            self.connected = True
            self.stop.clear()
            self.thread = threading.Thread(target=self._loop, daemon=True)
            self.thread.start()

    def _write(self, data):
        if self.serial:
            self.serial.write(data)

    def _clear(self):
        self.queue.clear()
        self.pending = None
        self.resend_handshake = False
        self.active = self.paused = False

    def disconnect(self):
        with self.lock:
            self.stop.set()
            if self.serial:
                try:
                    if self.active:
                        self._write(b'M112\n')
                    self.serial.close()
                except Exception:
                    pass
                self.serial = None
            self.connected = self.ready = False
            self.state = 'Disconnected'
            self._clear()
        if self.thread and self.thread is not threading.current_thread():
            self.thread.join(timeout=2)
        self.thread = None

    def _fault(self, message):
        self.error = message + '; reconnect required. Motion stop is not confirmed.'
        self.log.append(self.error)
        self.ready = False
        self.state = 'Fault'
        self._clear()
        try:
            self._write(b'M112\n')
        except Exception:
            pass

    def _idle(self):
        if not self.connected or not self.ready or self.error:
            raise RuntimeError('Controller not ready; reconnect after resolving faults')
        if self.active or self.pending or self.state != 'Idle':
            raise RuntimeError('Controller must be idle')

    def start(self, lines):
        with self.lock:
            self._idle()
            if not isinstance(lines, list) or not 1 <= len(lines) <= self.MAX_LINES:
                raise ValueError('Job must contain 1–200000 lines')
            if any(
                not isinstance(s, str)
                or len(s) > 78
                or any(ord(c) < 32 or ord(c) > 126 for c in s)
                or '*' in s
                or '(' in s
                or ')' in s
                or '\\' in s
                or '"' in s
                or re.match(r'\s*N\d', s, re.I)
                or re.match(r'\s*M0*110(?=\D|$)', s, re.I)
                for s in lines
            ):
                raise ValueError('Invalid or pre-numbered G-code line')
            lines = [s.split(';', 1)[0].strip() for s in lines]
            lines = [s for s in lines if s]
            if not lines:
                raise ValueError('Job contains no commands')
            self.queue = deque((s, 'job') for s in lines)
            self.queue.append(('M400', 'finish'))
            self.total, self.acked, self.progress = len(lines), 0, 0
            self.active, self.state = True, 'Run'

    def command(self, action, **kwargs):
        with self.lock:
            if not self.connected:
                raise RuntimeError('Connect first')
            if action == 'reset':
                self._fault('M112 emergency abort requested')
                return
            if action == 'hold':
                if not self.active or self.error:
                    raise RuntimeError('No active job to pause')
                self.paused, self.state = True, 'Hold'
                self.log.append('Host paused; buffered motion may continue')
                return
            if action == 'resume':
                if not self.active or not self.paused or not self.ready or self.error:
                    raise RuntimeError('No resumable held job')
                self.paused, self.state = False, 'Run'
                return
            self._idle()
            if action == 'jog':
                axis = kwargs.get('axis')
                try:
                    distance, feed = float(kwargs['distance']), float(kwargs['feed'])
                except (KeyError, TypeError, ValueError):
                    raise ValueError('Invalid jog distance/feed') from None
                if (
                    axis not in ('X', 'Y', 'Z')
                    or not math.isfinite(distance)
                    or not math.isfinite(feed)
                    or not 0 < abs(distance) <= 10
                    or not 0 < feed <= 1000
                ):
                    raise ValueError('Jog requires XYZ, at most 10 mm and 1000 mm/min')
                self.start(['G21', 'G91', f'G1 {axis}{distance:.3f} F{feed:.1f}', 'G90'])
            elif action in ('home', 'zero'):
                self.start([{'home': 'G28', 'zero': 'G92 X0 Y0 Z0'}[action]])
            else:
                raise ValueError('Unsupported Marlin command')

    def _send(self, line, kind):
        number = self.number
        self.number += 1
        self.pending = (number, self.frame(number, line), kind)
        self.resends = 0
        self.resend_handshake = False
        self.sent_at = time.monotonic()
        self._write(self.pending[1])
        self.log.append(f'> N{number} {line}')

    def _receive(self, line):
        self.log.append(line)
        resend = re.match(r'^(?:Resend:\s*|rs\s+N?)(\d+)\b', line, re.I)
        if resend:
            number = int(resend.group(1))
            if not self.pending or number != self.pending[0] or self.resends >= self.MAX_RESENDS:
                self._fault('Unsupported or excessive resend; refusing to replay acknowledged motion')
                return
            self.resends += 1
            self.sent_at = time.monotonic()
            # Marlin flush_and_request_resend emits Resend then an extra ok.
            # That ok is NOT acknowledgement of the retransmitted command.
            self.resend_handshake = True
            return
        # Marlin reports checksum/sequence errors before requesting a resend.
        if line.lower().startswith('error:'):
            if any(s in line.lower() for s in ('checksum', 'line number', 'no line number')):
                return  # A matching resend must arrive before the ordinary timeout.
            self._fault(line)
            return
        if line.lower().startswith(('!!', 'echo:busy: paused for user')):
            self._fault(line)
            return
        if line.lower() == 'start':
            if self.ready or self.pending:
                self._fault('Unexpected firmware restart')
            return
        if not re.match(r'^ok(?:\s|$)', line, re.I) or not self.pending:
            return
        if self.resend_handshake:
            self.resend_handshake = False
            self.sent_at = time.monotonic()
            self._write(self.pending[1])
            return
        ack_number = re.search(r'\bN(\d+)\b', line, re.I)
        if ack_number and int(ack_number.group(1)) != self.pending[0]:
            self._fault('Acknowledgement line number mismatch')
            return
        kind = self.pending[2]
        self.pending = None
        if kind == 'init':
            self.ready, self.state = True, 'Idle'
        elif kind == 'finish':
            self.active = self.paused = False
            self.progress, self.state = 100, 'Idle'
        else:
            self.acked += 1
            self.progress = min(99, 100 * self.acked / self.total)

    def _loop(self):
        started = time.monotonic()
        initialized = self.simulated
        try:
            while not self.stop.wait(0.01):
                with self.lock:
                    now = time.monotonic()
                    if not self.simulated:
                        raw = self.serial.readline(257)
                        self.receive_buffer += raw
                        if len(self.receive_buffer) > 256:
                            self._fault('Oversized firmware response')
                            self.receive_buffer = b''
                        elif self.receive_buffer.endswith(b'\n'):
                            self._receive(self.receive_buffer.decode('ascii', errors='replace').strip())
                            self.receive_buffer = b''
                        if not initialized and not self.error and now - started >= self.boot_delay:
                            self._send('M110 N0', 'init')
                            initialized = True
                    elif self.pending:
                        self._receive('ok')
                    if self.pending and now - self.sent_at > self.timeout:
                        self._fault('Acknowledgement/completion timeout')
                    if self.active and not self.paused and not self.pending and self.queue and not self.error:
                        self._send(*self.queue.popleft())
        except Exception as exc:
            with self.lock:
                self._fault(f'Serial connection failed: {exc}')
                self.connected = False
