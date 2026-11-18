import logging
from werkzeug.serving import make_server
from studio.server import create_app
logging.getLogger('werkzeug').setLevel(logging.ERROR)
server=make_server('127.0.0.1',60555,create_app(),threaded=True)
print('Integration editor http://127.0.0.1:60555/',flush=True)
server.serve_forever()
