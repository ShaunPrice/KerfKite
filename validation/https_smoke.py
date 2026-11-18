"""Real certificate-verified HTTPS transfer and browser fixture when --serve is used."""
import json,sys,tempfile,threading
from pathlib import Path
from cheroot.wsgi import Server
from cheroot.ssl.builtin import BuiltinSSLAdapter
from studio.web_server import create_app,certificate
from studio.web_store import WebStore
from studio.web_transfer import send_to_receiver
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp);cert=root/'cert.pem';key=root/'key.pem';certificate(cert,key,['localhost','127.0.0.1'])
 store=WebStore(root/'data');import secrets
 password=secrets.token_urlsafe(32);store.add_user('test-user',password);token=store.issue_token('test-user')
 port=60443
 app=create_app(store,f'https://localhost:{port}',allow_file_upload=True)
 server=Server(('127.0.0.1',port),app);server.ssl_adapter=BuiltinSSLAdapter(str(cert),str(key));server.prepare()
 thread=threading.Thread(target=server.serve,daemon=True);thread.start()
 try:
  entry={'url':f'https://localhost:{port}','token':token,'ca_file':str(cert)}
  receipt=send_to_receiver(entry,'test.nc',b'G21\nM5\n');assert len(store.files('test-user'))==1
  print(json.dumps({'verified_tls_upload':True,'receipt':receipt}),flush=True)
  if '--serve' in sys.argv:
   print(json.dumps({'test_user':'test-user','test_password':password,'cert':str(cert)}),flush=True)
   threading.Event().wait()
 finally:server.stop();thread.join(timeout=5)
