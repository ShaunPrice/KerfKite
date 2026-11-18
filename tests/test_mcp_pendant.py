import asyncio
from importlib.util import find_spec
import os
from pathlib import Path
import secrets
from tempfile import TemporaryDirectory
import threading
import unittest
from unittest.mock import patch

from flask import Flask, redirect
from werkzeug.serving import make_server

from studio.controller import Controller
from studio.mcp_pendant import PendantClient, register_pendant
from studio.monitor import create_monitor_app
from studio.phone_jobs import PhoneJobs


class PendantConfigurationTests(unittest.TestCase):
    def test_requires_operator_configured_base_and_pairing(self):
        token = secrets.token_urlsafe(32)
        for url in ('http://192.0.2.1', 'file:///tmp/example', 'https://example.test/api/action',
                    'https://user@example.test', 'https://example.test/#token=' + token,
                    'https://example.test/?token=' + token, 'http://localhost:0'):
            with self.subTest(url=url.split('#')[0].split('?')[0]), self.assertRaises(ValueError) as error:
                PendantClient(url,token)
            self.assertNotIn(token, str(error.exception))
        with patch.dict(os.environ,{},clear=True):
            self.assertFalse(register_pendant(None))
            os.environ['KERFKITE_PENDANT_TOKEN'] = token
            with self.assertRaises(ValueError):
                register_pendant(None)
        with TemporaryDirectory() as directory:
            filename = Path(directory)/'monitor-token'
            filename.write_text(token+'\n')
            env = {'KERFKITE_PENDANT_URL':'http://localhost:8766','KERFKITE_PENDANT_TOKEN_FILE':str(filename)}
            with patch.dict(os.environ,env,clear=True):
                self.assertEqual(PendantClient.from_environment().host,'127.0.0.1')
                os.environ['KERFKITE_PENDANT_TOKEN'] = token
                with self.assertRaises(ValueError):
                    PendantClient.from_environment()

    def test_monitor_redirect_is_never_followed(self):
        calls = []
        app = Flask(__name__)
        @app.get('/api/status')
        def redirect_status():
            return redirect('/other')
        @app.get('/other')
        def other():
            calls.append(True)
            return {}
        http = make_server('127.0.0.1',0,app,threaded=True)
        worker = threading.Thread(target=http.serve_forever,daemon=True)
        worker.start()
        try:
            client = PendantClient(f'http://127.0.0.1:{http.server_port}',secrets.token_urlsafe(32))
            with self.assertRaisesRegex(ValueError,'redirect'):
                client.status()
            self.assertEqual(calls,[])
        finally:
            http.shutdown()
            worker.join(3)
            http.server_close()

    @unittest.skipUnless(find_spec('cryptography') and find_spec('cheroot'), 'HTTPS web extra required')
    def test_operator_trusted_https(self):
        from studio.web_server import certificate
        controller = Controller()
        controller.connect('simulator')
        self.addCleanup(controller.disconnect)
        with TemporaryDirectory() as directory:
            cert, key = (Path(directory)/name for name in ('tls.pem','key.pem'))
            certificate(cert,key,['127.0.0.1'])
            token = secrets.token_urlsafe(32)
            monitor = create_monitor_app(controller,token,PhoneJobs(controller))
            http = make_server('127.0.0.1',0,monitor,threaded=True,ssl_context=(str(cert),str(key)))
            worker = threading.Thread(target=http.serve_forever,daemon=True)
            worker.start()
            try:
                url = f'https://127.0.0.1:{http.server_port}'
                with self.assertRaises(ValueError):
                    PendantClient(url,token).status()
                self.assertTrue(PendantClient(url,token,str(cert)).status()['simulated'])
            finally:
                http.shutdown()
                worker.join(3)
                http.server_close()


@unittest.skipUnless(find_spec('fastmcp'), 'Optional MCP dependency required')
class PairedMCPTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        from fastmcp import FastMCP
        self.controller = Controller()
        self.controller.connect('simulator')
        self.addCleanup(self.controller.disconnect)
        self.jobs = PhoneJobs(self.controller)
        self.token = secrets.token_urlsafe(32)
        self.revoked = threading.Event()
        monitor = create_monitor_app(self.controller,self.token,self.jobs,self.revoked)
        self.http = make_server('127.0.0.1',0,monitor,threaded=True)
        self.worker = threading.Thread(target=self.http.serve_forever,daemon=True)
        self.worker.start()
        self.addCleanup(self.http.server_close)
        self.addCleanup(self.worker.join,3)
        self.addCleanup(self.http.shutdown)
        self.mcp = FastMCP('Pendant simulator')
        env = {'KERFKITE_PENDANT_URL':f'http://127.0.0.1:{self.http.server_port}','KERFKITE_PENDANT_TOKEN':self.token}
        with patch.dict(os.environ,env,clear=True):
            self.assertTrue(register_pendant(self.mcp))

    async def test_authorized_pendant_actions_and_replay(self):
        from fastmcp import Client
        from fastmcp.client.elicitation import ElicitResult
        prompts = []
        async def approve(message,*_):
            prompts.append(message)
            return ElicitResult(action='accept',content={})
        async with Client(self.mcp,elicitation_handler=approve) as client:
            self.assertEqual({tool.name for tool in await client.list_tools()},
                             {'pendant_status','pendant_job','pendant_reference','pendant_jog'})
            async def status():
                reply = (await client.call_tool('pendant_status',{})).data
                self.assertTrue(reply['simulated'])
                self.assertNotIn(self.token,str(reply))
                self.assertNotIn('log',reply)
                return {k:reply[k] for k in ('control_epoch','request_id')}
            async def action(name,payload):
                return await client.call_tool(name,payload,raise_on_error=False)
            async def wait_idle():
                for _ in range(80):
                    if self.controller.status()['state'] == 'Idle':
                        return
                    await asyncio.sleep(.025)
                self.fail('Simulator failed to finish the pendant step.')
            jog = {**await status(),'axis':'Y','distance':-.5,'feed':100}
            self.assertTrue((await action('pendant_jog',jog)).data['accepted'])
            self.assertIn('already',str((await action('pendant_jog',jog)).content))
            self.assertTrue((await action('pendant_jog',{**jog,'distance':25})).is_error)
            for ref in ('home','zero'):
                await wait_idle()
                self.assertTrue((await action('pendant_reference',{**await status(),'action':ref})).data['accepted'])
            await wait_idle()
            self.assertIn('arm',str((await action('pendant_job',{**await status(),'action':'start'})).content))

            self.jobs.arm(['M5'],require_workstation=True)
            stale = await status()
            arm_id = self.jobs.arm(['M5']*200,require_workstation=True)
            self.assertIn('changed',str((await action('pendant_job',{**stale,'action':'start'})).content))
            self.assertTrue(self.jobs.status()['armed'])
            def confirm_preview():
                for _ in range(160):
                    challenge = self.jobs.check_preview(arm_id).get('challenge')
                    if challenge:
                        self.jobs.confirm_preview(arm_id,challenge)
                        return True
                    threading.Event().wait(.01)
                return False
            confirmation = asyncio.create_task(asyncio.to_thread(confirm_preview))
            self.assertTrue((await action('pendant_job',{**await status(),'action':'start'})).data['accepted'])
            self.assertTrue(await confirmation)
            self.assertTrue((await action('pendant_job',{**await status(),'action':'pause'})).data['accepted'])
            self.assertEqual(self.controller.status()['state'],'Hold')
            self.assertTrue((await action('pendant_job',{**await status(),'action':'resume'})).data['accepted'])
            old = await status()
            self.assertTrue((await action('pendant_job',{'action':'stop','request_id':(await status())['request_id']})).data['accepted'])
            self.assertFalse(self.controller.status()['active'])
            self.assertIn('changed',str((await action('pendant_reference',{**old,'action':'home'})).content))
            self.revoked.set()
            self.assertTrue((await action('pendant_status',{})).is_error)
            self.assertTrue(any('-.5' in p or '-0.5' in p for p in prompts))
            self.assertTrue(any('Resume' in p for p in prompts))

    async def test_unavailable_or_declined_confirmation_disallows_motion(self):
        from fastmcp import Client
        from fastmcp.client.elicitation import ElicitResult
        async def decline(*_):
            return ElicitResult(action='decline')
        async with Client(self.mcp,elicitation_handler=decline) as client:
            state = (await client.call_tool('pendant_status',{})).data
            args = {'request_id':state['request_id'],'control_epoch':state['control_epoch'],'action':'home'}
            result = await client.call_tool('pendant_reference',args)
            self.assertFalse(result.data['accepted'])
        async with Client(self.mcp) as client:
            denied = await client.call_tool('pendant_reference',args,raise_on_error=False)
            self.assertTrue(denied.is_error)
            self.assertIn('elicitation-capable',str(denied.content))
            for action in ('pause','stop'):
                state = (await client.call_tool('pendant_status',{})).data
                result = await client.call_tool('pendant_job',{'action':action,'request_id':state['request_id'],'control_epoch':state['control_epoch']})
                self.assertTrue(result.data['accepted'])
