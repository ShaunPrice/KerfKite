"""Opt-in MCP access to a single paired phone monitor configured by the operator."""
import asyncio
import http.client
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import ssl
from typing import Literal
from urllib.parse import urlsplit
from uuid import UUID, uuid4


class PendantClient:
    def __init__(self, url, token, ca_file=None):
        try:
            target = urlsplit(url)
            host, port = target.hostname, target.port
            valid = (target.scheme in ('http', 'https') and host and target.username is None
                     and target.password is None and target.path in ('', '/') and '?' not in url
                     and '#' not in url and not re.search(r'[\s\\\x00-\x1f\x7f]', url) and port != 0)
        except ValueError:
            valid = False
        if not valid:
            raise ValueError('Set KERFKITE_PENDANT_URL to the monitor base address without credentials, paths, queries or fragments.')
        self.host = host
        self.port = port
        self.tls = None
        if target.scheme == 'http':
            if host == 'localhost':
                self.host = '127.0.0.1'
            try:
                loopback = ipaddress.ip_address(self.host).is_loopback
            except ValueError:
                loopback = False
            if not loopback:
                raise ValueError('A network pendant requires HTTPS. HTTP is supported on loopback only.')
        else:
            try:
                self.tls = ssl.create_default_context(cafile=ca_file)
            except (OSError, ValueError):
                raise ValueError('Could not load the operator-configured pendant CA certificate.') from None
        if not isinstance(token, str) or not re.fullmatch(r'[A-Za-z0-9_-]{32,256}', token):
            raise ValueError('Supply the generated monitor pairing token, separately from the base address.')
        self._token = token

    @classmethod
    def from_environment(cls):
        url, token, filename, ca = (os.environ.get('KERFKITE_PENDANT_' + k) for k in ('URL', 'TOKEN', 'TOKEN_FILE', 'CA_FILE'))
        if not any((url, token, filename, ca)):
            return None
        if not url or bool(token) == bool(filename):
            raise ValueError('Set the pendant URL and exactly one of KERFKITE_PENDANT_TOKEN or KERFKITE_PENDANT_TOKEN_FILE.')
        if filename:
            try:
                with Path(filename).open('rb') as source:
                    token = source.read(258)
                if len(token) > 257:
                    raise ValueError()
                token = token.decode('ascii').strip()
            except (OSError, ValueError):
                raise ValueError('Could not read the configured pendant token file.') from None
        return cls(url, token, ca)

    def request(self, path, payload=None):
        # A direct connection never follows redirects, uses environment proxies, or retries motion.
        if self.tls is None:
            connection = http.client.HTTPConnection(self.host, self.port, timeout=6)
        else:
            connection = http.client.HTTPSConnection(self.host, self.port, context=self.tls, timeout=6)
        try:
            body = json.dumps(payload, allow_nan=False).encode('utf-8') if payload is not None else None
            connection.request('POST' if body is not None else 'GET', path, body=body,
                               headers={'Authorization':'Bearer ' + self._token,'Content-Type':'application/json'})
            response = connection.getresponse()
            raw = response.read(131073)
            if response.status == 401:
                raise ValueError('Pairing expired. Renew the monitor pairing at the workstation and restart this MCP server.')
            if 300 <= response.status < 400:
                raise ValueError('The paired monitor returned a redirect. Check its configured base address.')
            if len(raw) > 131072:
                raise ValueError('The paired monitor response is too large.')
            try:
                # Prevent a misconfigured endpoint from echoing the bearer into an MCP result.
                result = json.loads(raw.decode('utf-8').replace(self._token, '[redacted]'))
            except (ValueError, UnicodeError):
                raise ValueError('The paired monitor returned an invalid response. Check its status before retrying.') from None
            if not isinstance(result, dict):
                raise ValueError('The paired monitor returned an unexpected response.')
            if response.status != 200:
                message = str(result.get('error','Action rejected'))[:300]
                raise ValueError('Paired monitor: ' + message)
            return result
        except (http.client.HTTPException, OSError):
            raise ValueError('The monitor connection failed. An issued action may have reached the machine. Read its status before retrying.') from None
        finally:
            connection.close()

    def status(self):
        state = self.request('/api/status')
        fields = ('connected','simulated','state','active','progress','position','error','protocol','pendant','controls','armed','control_epoch')
        return {**{key:state.get(key) for key in fields}, 'request_id':str(uuid4())}

    async def action(self, ctx, action, request_id, control_epoch, **values):
        try:
            identifier = str(UUID(request_id))
        except (ValueError, AttributeError, TypeError):
            raise ValueError('Use a request_id from pendant_status, or supply a UUID for this one action.') from None
        payload = {'action':action, 'request_id':identifier, **values}
        if action != 'stop':
            if not isinstance(control_epoch, str) or not 1 <= len(control_epoch) <= 128:
                raise ValueError('Read pendant_status and supply its current control_epoch.')
            payload['control_epoch'] = control_epoch
        if action == 'jog':
            axis, distance, feed = values['axis'], values['distance'], values['feed']
            if axis not in ('X','Y','Z') or not all(math.isfinite(v) for v in (distance, feed)) or not .001 <= abs(distance) <= 10 or not .1 <= feed <= 1000:
                raise ValueError('A jog uses one XYZ axis, a signed 0.001–10 mm step and 0.1–1000 mm/min feed.')
            prompt = f'Jog {axis} by {distance:g} mm at {feed:g} mm/min on the paired machine?'
        else:
            prompt = {'start':'Start the job currently armed at the workstation?', 'resume':'Resume the held job on the paired machine?',
                      'home':'Home the paired machine axes?', 'zero':'Set the paired machine work zero at its current XYZ position?'}.get(action)
        if prompt:
            try:
                answer = await ctx.elicit(prompt, response_type=None)
            except Exception:
                raise ValueError('This action requires an operator confirmation through an elicitation-capable MCP client. Use the paired pendant or workstation if unavailable.') from None
            if answer.action != 'accept':
                return {'accepted':False, 'action':action}
        endpoint = '/api/pendant' if action in ('jog','home','zero') else '/api/action'
        result = await asyncio.to_thread(self.request, endpoint, payload)
        if result.get('ok') is not True:
            raise ValueError('The paired monitor did not confirm this action. Read its status before retrying.')
        return {'accepted':True, 'action':action, 'request_id':identifier}


def register_pendant(mcp):
    client = PendantClient.from_environment()
    if client is None:
        return False
    from fastmcp import Context

    read = {'readOnlyHint':True, 'destructiveHint':False, 'idempotentHint':True, 'openWorldHint':True}
    motion = {'readOnlyHint':False, 'destructiveHint':True, 'idempotentHint':False, 'openWorldHint':True}

    @mcp.tool(annotations=read)
    async def pendant_status() -> dict:
        """Read the paired controller and workstation arm state. Use the returned request_id once and retain it on a retry."""
        return await asyncio.to_thread(client.status)

    @mcp.tool(annotations=motion)
    async def pendant_job(action: Literal['start','pause','resume','stop'], request_id: str, ctx: Context, control_epoch: str='') -> dict:
        """Control the paired job. Start needs an armed workstation preview. Stop aborts the job and needs only a request UUID."""
        return await client.action(ctx, action, request_id, control_epoch)

    @mcp.tool(annotations=motion)
    async def pendant_jog(axis: Literal['X','Y','Z'], distance: float, feed: float, request_id: str, control_epoch: str, ctx: Context) -> dict:
        """Move one axis by a signed distance (mm) at the specified feed (mm/min), after operator confirmation."""
        return await client.action(ctx,'jog', request_id, control_epoch, axis=axis, distance=distance, feed=feed)

    @mcp.tool(annotations=motion)
    async def pendant_reference(action: Literal['home','zero'], request_id: str, control_epoch: str, ctx: Context) -> dict:
        """Home the axes or set XYZ work zero, after operator confirmation. The monitor requires an idle machine."""
        return await client.action(ctx,action, request_id, control_epoch)

    return True
