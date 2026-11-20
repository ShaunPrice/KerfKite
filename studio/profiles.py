"""Generic design envelopes. A profile never writes settings to a controller."""

PROFILES = [
    {
        "id": "generic-grbl",
        "name": "Generic GRBL · 300 × 180 mm",
        "controller": "GRBL 1.1 compatible",
        "firmware": "Verify on controller",
        "width": 300,
        "height": 180,
        "z_travel": None,
        "heads": [],
        "commissioned": False,
        "head_mounting": "interchangeable",
        "unverified": ["Machine travel, origin, installed head and clearance"],
    },
    {
        "id": "generic-xpro-v5",
        "name": "xPRO V5 · example workspace",
        "controller": "xPRO V5",
        "firmware": "GRBL-ESP32 profile",
        "width": 600,
        "height": 400,
        "z_travel": None,
        "heads": [],
        "commissioned": False,
        "head_mounting": "interchangeable",
        "unverified": ["Configure actual machine envelope before preparing a job"],
    },
]
