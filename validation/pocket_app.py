"""Which installed Pocket build the device workflows drive.

A debug build installs as ``au.cncstudio.pocket.debug`` beside the release app, and is what these workflows drive
unless ``KERFKITE_POCKET_PACKAGE`` names another id (``au.cncstudio.pocket`` for the release build).
"""

import os

NAMESPACE = 'au.cncstudio.pocket'
PACKAGE = os.environ.get('KERFKITE_POCKET_PACKAGE') or NAMESPACE + '.debug'


def component(activity='WorkshopActivity'):
    """The activity's full component name. The class stays in the namespace whatever the application id."""
    return f'{PACKAGE}/{NAMESPACE}.{activity}'
