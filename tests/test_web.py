import tempfile
import unittest
from studio.web_store import WebStore
from studio.web_server import create_app
from studio.web_transfer import validate_upload,validate_destination

class WebTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
  self.store=WebStore(self.tmp.name);self.store.add_user('alice','A secure chosen password!')
  self.app=create_app(self.store,'https://localhost:8443');self.c=self.app.test_client();self.base='https://localhost:8443'
 def login(self,user='alice',password='A secure chosen password!'):
  self.c.get('/login',base_url=self.base)
  cookie=self.c.get_cookie('__Host-cnc-login',domain='localhost').value
  return self.c.post('/login',base_url=self.base,headers={'Origin':self.base},data={'username':user,'password':password,'csrf':cookie})
 def auth(self):
  self.assertEqual(self.login().status_code,302)
  token=self.c.get('/api/session',base_url=self.base).json['token']
  return {'Origin':self.base,'X-Studio-Token':token}
 def test_no_defaults_tls_host_and_csrf(self):
  with tempfile.TemporaryDirectory() as t:
   with self.assertRaises(ValueError):create_app(WebStore(t),self.base)
  self.assertEqual(self.c.get('/api/session',base_url=self.base).status_code,401)
  self.assertEqual(self.c.get('/',base_url='http://localhost:8443').status_code,400)
  self.assertEqual(self.c.get('/',base_url='https://evil.example').status_code,400)
  h=self.auth();cookie=self.c.get_cookie('__Host-cnc-session',domain='localhost');self.assertTrue(cookie.secure);self.assertTrue(cookie.http_only)
  self.assertEqual(self.c.post('/api/files',base_url=self.base,json={'name':'a.nc','content':'M5'}).status_code,403)
  h['Origin']='https://evil.example';self.assertEqual(self.c.post('/api/logout',base_url=self.base,headers=h).status_code,403)
 def test_private_files_and_revocation(self):
  h=self.auth();r=self.c.post('/api/files',base_url=self.base,headers=h,json={'name':'a.nc','content':'M5'});self.assertEqual(r.status_code,200)
  identifier=r.json['id'];self.assertEqual(self.c.get('/api/files/'+identifier,base_url=self.base).data,b'M5')
  self.store.add_user('bob','Another secure chosen password!');self.login('bob','Another secure chosen password!')
  self.assertEqual(self.c.get('/api/files/'+identifier,base_url=self.base).status_code,404)
  self.store.add_user('bob','Replacement secure password!',replace=True)
  self.assertEqual(self.c.get('/api/session',base_url=self.base).status_code,401)
 def test_upload_only_token_and_null_origin_optin(self):
  token=self.store.issue_token('alice');headers={'Authorization':'Bearer '+token,'Origin':'null'}
  r=self.c.post('/api/receive',base_url=self.base,headers=headers,json={'name':'job.nc','content':'G0 X1'});self.assertEqual(r.json['status'],'stored_for_review');self.assertNotIn('Access-Control-Allow-Origin',r.headers)
  self.assertEqual(self.c.get('/api/files',base_url=self.base,headers=headers).status_code,401)
  self.assertEqual(self.c.post('/api/run',base_url=self.base,headers=headers,json={}).status_code,401)
  app=create_app(self.store,self.base,allow_file_upload=True);c=app.test_client()
  r=c.options('/api/receive',base_url=self.base,headers={'Origin':'null'});self.assertEqual(r.headers['Access-Control-Allow-Origin'],'null')
  self.store.revoke_tokens('alice');self.assertEqual(c.post('/api/receive',base_url=self.base,headers=headers,json={}).status_code,401)
 def test_pdf_review_requires_authenticated_session(self):
  import base64
  from validation.pdf_import_workflow import example_pdf
  data={'content':base64.b64encode(example_pdf()).decode('ascii'),'page':2}
  self.assertEqual(self.c.post('/api/pdf-import',base_url=self.base,json=data).status_code,401)
  response=self.c.post('/api/pdf-import',base_url=self.base,headers=self.auth(),json=data)
  self.assertEqual(response.status_code,200)
  self.assertEqual(len(response.json['objects']),2)
 def test_limits_and_invalid_files(self):
  for i in range(10):self.assertEqual(self.login(password='wrong').status_code,401)
  self.assertEqual(self.login().status_code,429)
  for d in [{'name':'../job.nc','content':'M5'},{'name':'x.html','content':'x'},{'name':'x.nc','content':'M5','sha256':'bad'}]:
   with self.assertRaises(ValueError):validate_upload(d)
  with self.assertRaises(ValueError):validate_destination({'url':'http://pi:8443','token':'x'*43})

if __name__=='__main__':unittest.main()

class LoginRaceTests(unittest.TestCase):
 def test_reset_during_verification_cannot_restore_old_session(self):
  from unittest.mock import patch
  from werkzeug.security import check_password_hash as verify
  with tempfile.TemporaryDirectory() as tmp:
   store=WebStore(tmp);store.add_user('alice','Original strong password!')
   def during_reset(hashed,password):
    result=verify(hashed,password);store.add_user('alice','Replacement strong password!',replace=True);return result
   with patch('studio.web_store.check_password_hash',side_effect=during_reset):
    session,_=store.check_login('alice','Original strong password!','127.0.0.1')
   self.assertIsNone(session)
 def test_successful_logins_do_not_exhaust_failed_attempt_counter(self):
  with tempfile.TemporaryDirectory() as tmp:
   store=WebStore(tmp);store.add_user('alice','A valid strong password!')
   for _ in range(12):
    session,limited=store.check_login('alice','A valid strong password!','127.0.0.1');self.assertIsNotNone(session);self.assertFalse(limited);store.logout(session['sid'])
