"""Local simulator for the installed desktop launcher."""

import argparse

from werkzeug.serving import make_server
from . import relief
from .windows_support import require_supported_windows
from .server import create_app


def main():
    require_supported_windows()
    parser = argparse.ArgumentParser()
    parser.add_argument("--enable-hardware", action="store_true")
    args = parser.parse_args()
    server = make_server("127.0.0.1", 18765, create_app(allow_hardware=args.enable_hardware), threaded=True)
    print("KERFKITE_READY", flush=True)
    relief.calibrate_in_background()
    server.serve_forever()


if __name__ == "__main__":
    main()
