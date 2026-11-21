"""Protocol selection shared by the workstation and paired monitor."""

import threading
from .controller import Controller as GrblController
from .marlin import Controller as MarlinController
from .protocols import get_protocol
from .rotary import controller_rotary_axes


class DeviceController:
    def __init__(self):
        self.lock = threading.RLock()
        self.protocol = 'grbl'
        # Every Grbl-family controller is asked to identify itself ($I), so that the machine can be described.
        self.driver = GrblController(initialization_queries=('$I',))

    def select_protocol(self, protocol):
        with self.lock:
            return self._select_protocol(protocol)

    def _select_protocol(self, protocol):
        profile = get_protocol(protocol)
        if not profile.live_transport:
            raise ValueError(
                profile.name + ' supports file export only; open the exported file in its controller software'
            )
        if protocol == self.protocol:
            return
        if self.driver.status()['connected']:
            raise ValueError('Disconnect before changing protocol')
        self.driver.disconnect()
        self.protocol = protocol
        self.driver = (
            GrblController(initialization_queries=('$30', '$32', '$I') if protocol == 'fluidnc' else ('$I',))
            if profile.family == 'grbl'
            else MarlinController()
        )

    def status(self):
        with self.lock:
            state = self.driver.status()
            return {**state, 'protocol': self.protocol, 'rotary_axes': controller_rotary_axes(self.protocol, state)}

    def __getattr__(self, name):
        with self.lock:
            attr = getattr(self.driver, name)
        if not callable(attr):
            return attr

        def call(*args, **kwargs):
            with self.lock:
                return getattr(self.driver, name)(*args, **kwargs)

        return call
