"""One-use job prepared at the local workstation for a paired phone to start."""
import threading


class PhoneJobs:
    def __init__(self, controller, allow_physical=False):
        self.controller = controller
        self.allow_physical = allow_physical
        self.validator = None
        self.lock = getattr(controller,'lock',threading.RLock())
        self.lines = None

    def arm(self, lines, validator=None):
        with self.lock:
            # Physical commissioning is still incomplete. Do not allow an arm
            # operation to bypass the UI's physical connection restriction.
            state = self.controller.status()
            if not state['connected'] or (not state['simulated'] and not self.allow_physical) or state['state'] != 'Idle' or state['active']:
                raise ValueError('Connect an idle controller before arming (physical control needs operator opt-in)')
            self.validator = validator
            self.lines = list(lines)

    def disarm(self):
        with self.lock:
            self.lines = None
            self.validator = None

    def status(self):
        with self.lock:
            return {'armed': self.lines is not None}

    def start(self):
        with self.lock:
            if self.lines is None:
                raise ValueError('Preview and arm a job on the Pi first')
            state = self.controller.status()
            if (not state['simulated'] and not self.allow_physical):
                self.lines = None
                raise ValueError('Physical phone starts require commissioning')
            lines = self.lines
            validator=self.validator
            self.lines = None
            self.validator=None
            if validator:validator()
            self.controller.start(lines)  # One attempt only, including failed/ambiguous starts.
