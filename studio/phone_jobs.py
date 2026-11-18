"""One-use jobs prepared at the workstation for a paired phone."""
import secrets
import threading
import time


class PhoneJobs:
    def __init__(self, controller, allow_physical=False, confirmation_timeout=1.8):
        self.controller = controller
        self.allow_physical = allow_physical
        self.lock = getattr(controller, 'lock', threading.RLock())
        self.changed = threading.Condition(self.lock)
        self.confirmation_timeout = confirmation_timeout
        self.arm_id = self.challenge = None
        self.control_epoch = secrets.token_urlsafe(18)
        self.challenge_deadline = self.owner_seen = 0
        self.confirmed = False
        self.lines = self.validator = None

    def arm(self, lines, validator=None, *, require_workstation=False):
        with self.changed:
            state = self.controller.status()
            if not state['connected'] or (not state['simulated'] and not self.allow_physical) or state['state'] != 'Idle' or state['active']:
                raise ValueError('Connect an idle controller before arming (physical control needs operator opt-in)')
            if not require_workstation and not state['simulated']:
                raise ValueError('A physical machine requires an armed workstation preview.')
            self.disarm()
            self.lines = list(lines)
            self.validator = validator
            if require_workstation:
                # This opaque handle is returned only to the editor that owns the preview.
                self.arm_id = secrets.token_urlsafe(32)
                self.owner_seen = time.monotonic()
            return self.arm_id

    def disarm(self, arm_id=None):
        with self.changed:
            if arm_id is not None and arm_id != self.arm_id:
                return False
            self.lines = self.validator = self.arm_id = self.challenge = None
            self.confirmed = False
            self.changed.notify_all()
            return True

    def invalidate_controls(self):
        with self.changed:
            self.control_epoch = secrets.token_urlsafe(18)
            self.disarm()

    def require_current_controls(self, epoch):
        with self.lock:
            if not isinstance(epoch, str) or not epoch:
                raise ValueError('Update the phone pendant or refresh the monitor before using job or pendant controls. Stop remains available.')
            if self.control_epoch != epoch:
                raise ValueError('Controller controls changed. Read the latest status before issuing another action.')

    def _expire_absent_editor(self):
        if self.arm_id and time.monotonic() - self.owner_seen > 8:
            self.disarm()

    def status(self):
        with self.lock:
            self._expire_absent_editor()
            return {'armed': self.lines is not None}

    def check_preview(self, arm_id):
        with self.lock:
            self._expire_absent_editor()
            if not self.arm_id or self.arm_id != arm_id:
                return {'armed': False}
            self.owner_seen = time.monotonic()
            return {'armed': True, 'challenge': self.challenge}

    def confirm_preview(self, arm_id, challenge):
        with self.changed:
            if (not self.challenge or self.challenge != challenge or self.arm_id != arm_id
                    or self.confirmed or time.monotonic() >= self.challenge_deadline):
                raise ValueError('Phone Start request expired or cancelled. Check controller status before rearming.')
            self.confirmed = True
            self.changed.notify_all()

    def start(self):
        with self.changed:
            self._expire_absent_editor()
            if self.lines is None:
                raise ValueError('Preview and arm a job on the workstation first.')
            if self.challenge is not None:
                raise ValueError('A phone Start is already waiting for the workstation.')
            arm_id = self.arm_id
            if arm_id:
                self.challenge = secrets.token_urlsafe(32)
                self.challenge_deadline = time.monotonic() + self.confirmation_timeout
                # Condition.wait_for releases the controller lock. Stop, disconnect and
                # revocation can invalidate this exact arm while its editor responds.
                self.changed.wait_for(lambda: self.arm_id != arm_id or self.confirmed,
                                      timeout=self.confirmation_timeout)
                if self.arm_id != arm_id:
                    raise ValueError('The armed preview changed or was disarmed. No job started.')
                if not self.confirmed or time.monotonic() >= self.challenge_deadline:
                    self.disarm()
                    raise ValueError('The workstation did not confirm this preview in time. No job started. Reopen and rearm the preview.')
            lines, validator = self.lines, self.validator
            self.disarm()  # A failed or ambiguous dispatch also consumes the arm.
            if not self.controller.status()['simulated'] and (not self.allow_physical or not arm_id):
                raise ValueError('Physical phone starts require commissioning.')
            if validator:
                validator()
            self.controller.start(lines)
