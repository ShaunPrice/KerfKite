"""Local workstation and paired-phone fixture: the C++ server in simulation, its phone monitor on the loopback.

    python validation/serve_workspace_demo.py [--server build/native/kerfkite-studio] [--port 50312]

Prints the editor's address and the phone monitor's pairing address, then serves until interrupted. Only the
simulator is available; no physical controller can be opened.
"""

import argparse
import time

from native_servers import studio


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--server', help='kerfkite-studio (default: KERFKITE_STUDIO or build/native)')
    parser.add_argument('--port', type=int, default=50312)
    args = parser.parse_args()
    with studio(args.server, args=('--monitor-host', '127.0.0.1', '--monitor-port', '0'), port=args.port) as server:
        server.post('api/connect', {'port': 'simulator'})
        server.enable_monitor()
        print(f'workspace={server.url} monitor={server.monitor}#token={server.token}', flush=True)
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
