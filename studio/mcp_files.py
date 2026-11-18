"""Optional bridge from a workshop's artifact store to a fixed HTTPS editor account."""
import asyncio
import hashlib
import http.client
from http.cookies import SimpleCookie
import json
import os
from pathlib import Path
import re
import ssl
import threading
from urllib.parse import urlencode, urlsplit

from .web_server import COOKIE, LOGIN
from .web_store import username as validate_username
from .web_transfer import validate_upload

FILE_LIMIT = 8 * 1024 * 1024


class HostedFiles:
    def __init__(self, url, username, password, ca_file=None):
        try:
            target = urlsplit(url)
            self.host, self.port = target.hostname, target.port
            valid = (target.scheme == 'https' and self.host and self.port != 0 and target.username is None
                     and target.password is None and target.path in ('','/') and '?' not in url and '#' not in url
                     and not re.search(r'[\s\\\x00-\x1f\x7f]',url))
        except ValueError:
            valid = False
        if not valid:
            raise ValueError('Set KERFKITE_FILES_URL to the fixed HTTPS editor origin, without credentials, paths, query or fragment.')
        self.origin = url.rstrip('/')
        self._user = validate_username(username)
        if not password or len(password)>256:
            raise ValueError('Supply the hosted account password in the operator configuration.')
        self._password = password
        try:
            self._tls = ssl.create_default_context(cafile=ca_file)
        except (OSError, ValueError):
            raise ValueError('Could not load the configured hosted editor CA certificate.') from None
        self._lock = threading.Lock()
        self._session = self._csrf = None

    @classmethod
    def from_environment(cls):
        url, username, password, filename, ca = (os.environ.get('KERFKITE_FILES_' + k)
            for k in ('URL','USERNAME','PASSWORD','PASSWORD_FILE','CA_FILE'))
        if not any((url,username,password,filename,ca)):
            return None
        if not url or not username or bool(password) == bool(filename):
            raise ValueError('Set the hosted editor URL, username and exactly one password or password file.')
        if filename:
            try:
                with Path(filename).open('rb') as f:
                    data = f.read(1027)
                if len(data)>1026:
                    raise ValueError()
                password = data.decode('utf-8').removesuffix('\n').removesuffix('\r')
            except (OSError,ValueError):
                raise ValueError('Could not read the configured hosted editor password file.') from None
        return cls(url,username,password,ca)

    def _request(self, method, path, headers, body=None, limit=262144):
        # Connect only to the configured origin. Each call closes its TLS socket;
        # login and receiver redirects are never followed.
        conn = http.client.HTTPSConnection(self.host,self.port,context=self._tls,timeout=20)
        try:
            conn.request(method,path,body=body,headers=headers)
            response = conn.getresponse()
            data = response.read(limit+1)
            if len(data)>limit:
                raise ValueError('Hosted editor response exceeds the allowed size.')
            return response.status, response.headers, data
        except (OSError,http.client.HTTPException):
            raise ValueError('The HTTPS editor connection failed. Check stored files or receiver receipts before retrying an upload.') from None
        finally:
            conn.close()

    @staticmethod
    def _cookie(headers,name):
        cookies = SimpleCookie()
        for value in headers.get_all('Set-Cookie',[]):
            cookies.load(value)
        cookie = cookies.get(name)
        if not cookie or not cookie['secure'] or not re.fullmatch(r'[A-Za-z0-9_-]{32,128}',cookie.value):
            raise ValueError('The hosted editor did not issue the expected secure session cookie.')
        return cookie.value

    @staticmethod
    def _json(raw):
        try:
            return json.loads(raw)
        except (UnicodeError,ValueError):
            raise ValueError('Invalid response from the hosted editor.') from None

    def _login(self):
        status, headers, _ = self._request('GET','/login',{})
        if status != 200:
            raise ValueError('The configured HTTPS origin did not serve the KerfKite login form.')
        csrf = self._cookie(headers,LOGIN)
        form = urlencode({'username':self._user,'password':self._password,'csrf':csrf})
        status, headers, _ = self._request('POST','/login',{'Origin':self.origin,'Cookie':f'{LOGIN}={csrf}',
            'Content-Type':'application/x-www-form-urlencoded'}, form.encode('ascii'))
        if status != 302 or headers.get('Location') != '/':
            raise ValueError('Hosted editor login failed. Check the configured account or login rate limit.')
        session = self._cookie(headers,COOKIE)
        status, _, raw = self._request('GET','/api/session',{'Cookie':f'{COOKIE}={session}'})
        payload = self._json(raw) if status == 200 else {}
        if not isinstance(payload,dict) or payload.get('web_mode') is not True or not re.fullmatch(r'[A-Za-z0-9_-]{32,128}', str(payload.get('token',''))):
            raise ValueError('Hosted editor session could not be verified.')
        self._session, self._csrf = session, payload['token']

    def api(self,path,payload=None,*,download=False):
        with self._lock:
            if not self._session:
                self._login()
            for attempt in range(2):
                headers = {'Cookie':f'{COOKIE}={self._session}'}
                body = None
                if payload is not None:
                    body = json.dumps(payload,ensure_ascii=False).encode('utf-8')
                    headers.update({'Origin':self.origin,'X-Studio-Token':self._csrf,'Content-Type':'application/json'})
                status, _, raw = self._request('POST' if body is not None else 'GET',path,headers,body,
                                               limit=FILE_LIMIT if download else 262144)
                if status == 401 and attempt == 0:
                    # KerfKite rejects an expired session in before_request, before any write.
                    self._session = self._csrf = None
                    self._login()
                    continue
                if status != 200:
                    raise ValueError(f'Hosted editor rejected the file request (HTTP {status}). Check the account, file and configured receiver.')
                return raw if download else self._json(raw)

    def lookup(self,identifier):
        if not isinstance(identifier,str) or not re.fullmatch(r'[0-9a-f]{32}',identifier):
            raise ValueError('Supply the hosted file_id from hosted_list_files.')
        files = self.api('/api/files')
        for item in files:
            if item.get('id') == identifier:
                return item
        raise ValueError('The selected file is unavailable to the configured account.')

    def download(self,identifier):
        item = self.lookup(identifier)
        data = self.api('/api/files/'+identifier, download=True)
        if len(data)!=item['bytes'] or hashlib.sha256(data).hexdigest()!=item['sha256']:
            raise ValueError('Hosted file checksum mismatch.')
        name, data = validate_upload({'name':item['name'],'content':data.decode('utf-8')})
        return name,data

    def send(self,name,data,destination=None):
        digest = hashlib.sha256(data).hexdigest()
        payload = {'name':name,'content':data.decode('utf-8'),'sha256':digest}
        if destination is not None:
            payload['destination'] = destination
        result = self.api('/api/send' if destination is not None else '/api/files', payload)
        if (not isinstance(result,dict) or result.get('sha256')!=digest or result.get('status')!='stored_for_review'
                or result.get('bytes')!=len(data) or result.get('name')!=name or not re.fullmatch(r'[a-f0-9]{32}',str(result.get('id','')))):
            raise ValueError('Storage receipt could not be verified. Check the destination before retrying.')
        return {key:result[key] for key in ('id','name','bytes','sha256','status') if key in result}


def register_files(mcp,workshop):
    hosted = HostedFiles.from_environment()
    if hosted is None:
        return False
    from fastmcp import Context
    read = {'readOnlyHint':True,'destructiveHint':False,'openWorldHint':True,'idempotentHint':True}
    write = {'readOnlyHint':False,'destructiveHint':False,'openWorldHint':True,'idempotentHint':False}
    destroy = {**write,'destructiveHint':True}

    def artifact_data(artifact,name):
        with workshop.file(artifact).open('rb') as stream:
            data = stream.read(FILE_LIMIT+1)
        if len(data)>FILE_LIMIT:
            raise ValueError('Hosted uploads support up to 8 MiB.')
        try:
            value = data.decode('utf-8')
        except UnicodeError:
            raise ValueError('Choose a supported UTF-8 project, artwork or G-code artifact.') from None
        return validate_upload({'name':name,'content':value})

    async def approved(ctx,message):
        try:
            reply = await ctx.elicit(message,response_type=None)
        except Exception:
            raise ValueError('Use an elicitation-capable MCP client to confirm this operation, or use the hosted editor.') from None
        return reply.action == 'accept'

    @mcp.tool(annotations=read)
    async def hosted_list_files() -> list[dict]:
        """List stored files and checksums in the configured HTTPS editor account."""
        files = await asyncio.to_thread(hosted.api,'/api/files')
        return [{k:item[k] for k in ('id','name','bytes','sha256','created','source')} for item in files]

    @mcp.tool(annotations=read)
    async def hosted_destinations() -> list[str]:
        """List the receiver names already configured by the hosted editor operator."""
        return await asyncio.to_thread(hosted.api,'/api/destinations')

    @mcp.tool(annotations=write)
    async def hosted_upload_file(artifact: str,name: str) -> dict:
        """Save a local workshop artifact under a simple filename in the configured account."""
        name,data = await asyncio.to_thread(artifact_data,artifact,name)
        return await asyncio.to_thread(hosted.send,name,data)

    @mcp.tool(annotations=write)
    async def hosted_download_file(file_id: str) -> dict:
        """Copy a hosted file to a new workshop artifact and verify its SHA-256."""
        name,data = await asyncio.to_thread(hosted.download,file_id)
        return await asyncio.to_thread(workshop.create_file,name,data)

    @mcp.tool(annotations=destroy)
    async def hosted_delete_file(file_id: str, ctx: Context) -> dict:
        """Delete one file owned by the configured account after operator confirmation."""
        item = await asyncio.to_thread(hosted.lookup,file_id)
        if not await approved(ctx,f"Delete hosted file {item['name']} ({item['bytes']} bytes)?"):
            return {'deleted':False}
        result = await asyncio.to_thread(hosted.api,'/api/files/'+file_id+'/delete',{})
        return {'deleted':result.get('ok') is True, 'file_id':file_id}

    @mcp.tool(annotations=write)
    async def hosted_send_file(destination: str, ctx: Context, artifact: str='', name: str='', file_id: str='') -> dict:
        """Send a hosted file_id or workshop artifact and filename to a named receiver inbox for operator review."""
        if bool(file_id) == bool(artifact) or (file_id and name) or (artifact and not name):
            raise ValueError('Choose a hosted file_id, or supply both a workshop artifact and its filename.')
        targets = await asyncio.to_thread(hosted.api,'/api/destinations')
        if destination not in targets:
            raise ValueError('Choose a receiver name returned by hosted_destinations.')
        if file_id:
            name,data = await asyncio.to_thread(hosted.download,file_id)
        else:
            name,data = await asyncio.to_thread(artifact_data,artifact,name)
        if not await approved(ctx,f'Send {name} ({len(data)} bytes) to the {destination} receiver inbox for review?'):
            return {'sent':False}
        receipt = await asyncio.to_thread(hosted.send,name,data,destination)
        return {'sent':True,'destination':destination,**receipt}

    return True
