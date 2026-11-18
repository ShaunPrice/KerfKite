from importlib.util import find_spec
import os
from pathlib import Path
import secrets
from tempfile import TemporaryDirectory
import threading
import unittest
from unittest.mock import patch

from werkzeug.serving import make_server
from studio.mcp_files import HostedFiles, register_files
from studio.web_server import create_app, certificate
from studio.web_store import WebStore


class FileConfigurationTests(unittest.TestCase):
    def test_off_by_default_and_fixed_https_origin(self):
        with patch.dict(os.environ,{},clear=True):
            self.assertFalse(register_files(None,None))
        password = secrets.token_urlsafe(32)
        for url in ('http://127.0.0.1','https://editor.example/foreign','https://u@editor.example',
                    'https://editor.example?password='+password,'https://editor.example/#token'):
            with self.assertRaises(ValueError) as error:
                HostedFiles(url,'operator',password)
            self.assertNotIn(password,str(error.exception))
        with patch.dict(os.environ,{'KERFKITE_FILES_URL':'https://example.test'},clear=True):
            with self.assertRaises(ValueError):
                register_files(None,None)


@unittest.skipUnless(find_spec('fastmcp') and find_spec('cryptography'), 'Optional web/MCP dependencies required')
class HostedFileMCPTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        from fastmcp import FastMCP
        from studio.mcp_workshop import Workshop
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.cert,self.key = self.root/'cert.pem',self.root/'key.pem'
        certificate(self.cert,self.key,['127.0.0.1'])
        self.password = secrets.token_urlsafe(32)
        self.inbox,self.receiver = self.serve('inbox')
        self.inbox_token = self.inbox.issue_token('operator')
        self.web,self.origin = self.serve('web',{'Test receiver':{'url':self.receiver,'ca_file':str(self.cert),'token':self.inbox_token}})
        self.workshop = Workshop(self.root/'workshop')
        self.server = FastMCP('Hosted files test')
        secret = self.root/'password'
        secret.write_text(self.password+'\n')
        env = {'KERFKITE_FILES_URL':self.origin,'KERFKITE_FILES_USERNAME':'operator',
               'KERFKITE_FILES_PASSWORD_FILE':str(secret),'KERFKITE_FILES_CA_FILE':str(self.cert)}
        with patch.dict(os.environ,env,clear=True):
            self.assertTrue(register_files(self.server,self.workshop))

    def serve(self,name,destinations=None):
        store = WebStore(self.root/name)
        store.add_user('operator',self.password)
        http = make_server('127.0.0.1',0,lambda *_: [],threaded=True,ssl_context=(str(self.cert),str(self.key)))
        origin = f'https://127.0.0.1:{http.server_port}'
        http.app = create_app(store,origin,destinations)
        thread = threading.Thread(target=http.serve_forever,daemon=True)
        thread.start()
        self.addCleanup(http.server_close)
        self.addCleanup(thread.join,3)
        self.addCleanup(http.shutdown)
        return store,origin

    async def test_real_login_artifacts_named_receiver_and_confirmation(self):
        from fastmcp import Client
        from fastmcp.client.elicitation import ElicitResult
        approvals = []
        allow = False
        async def operator(message,*_):
            approvals.append(message)
            return ElicitResult(action='accept',content={}) if allow else ElicitResult(action='decline')
        self.web.add_user('another',secrets.token_urlsafe(32))
        other = self.web.put_file('another','private.nc',b'M5','web')
        data = '(asymmetric échantillon)\nM5\n'.encode()
        artifact = self.workshop.create_file('drawing.nc',data)['artifact']
        async with Client(self.server,elicitation_handler=operator) as client:
            async def call(tool,**args):
                return await client.call_tool(tool,args,raise_on_error=False)
            self.assertEqual((await call('hosted_list_files')).data,[])
            self.assertEqual((await call('hosted_destinations')).data,['Test receiver'])
            self.assertTrue((await call('hosted_download_file',file_id=other['id'])).is_error)
            self.assertTrue((await call('hosted_upload_file',artifact=artifact,name='../escape.nc')).is_error)
            self.assertTrue((await call('hosted_upload_file',artifact='../other.nc',name='ok.nc')).is_error)
            receipt = (await call('hosted_upload_file',artifact=artifact,name='drawing.nc')).data
            self.assertEqual(receipt['status'],'stored_for_review')
            file_id=receipt['id']
            listing = (await call('hosted_list_files')).data
            self.assertEqual([item['id'] for item in listing],[file_id])
            self.assertNotIn(self.password,str(listing))
            copy = (await call('hosted_download_file',file_id=file_id)).data
            self.assertEqual(self.workshop.file(copy['artifact']).read_bytes(),data)
            self.assertTrue((await call('hosted_send_file',destination=self.receiver,file_id=file_id)).is_error)
            self.assertFalse((await call('hosted_send_file',destination='Test receiver',file_id=file_id)).data['sent'])
            self.assertFalse((await call('hosted_delete_file',file_id=file_id)).data['deleted'])
            self.assertFalse(self.inbox.files('operator'))
            allow = True
            for source in ({'file_id':file_id},{'artifact':artifact,'name':'direct.nc'}):
                sent=(await call('hosted_send_file',destination='Test receiver',**source)).data
                self.assertTrue(sent['sent'])
                self.assertEqual(self.inbox.file('operator',sent['id'])['content'],data)
                self.assertNotIn(self.inbox_token,str(sent))
            self.assertTrue((await call('hosted_delete_file',file_id=file_id)).data['deleted'])
            self.assertEqual((await call('hosted_list_files')).data,[])
            self.assertTrue(any('inbox' in question for question in approvals))

    def test_wrong_login_untrusted_tls_and_expired_session(self):
        wrong = HostedFiles(self.origin,'operator',secrets.token_urlsafe(32),str(self.cert))
        with self.assertRaisesRegex(ValueError,'login failed') as error:
            wrong.api('/api/files')
        self.assertNotIn(self.password,str(error.exception))
        with self.assertRaises(ValueError):
            HostedFiles(self.origin,'operator',self.password).api('/api/files')
        client = HostedFiles(self.origin,'operator',self.password,str(self.cert))
        self.assertEqual(client.api('/api/files'),[])
        self.web.logout(client._session)
        self.assertEqual(client.api('/api/files'),[])
