"""Stateful assistant editor using the same UI, project format and simulated controller as Studio."""

import asyncio
import base64
import binascii
import json
import os
import re
import tempfile
import threading
import uuid
from pathlib import Path
from urllib.parse import urlsplit

LIMIT = 8 * 1024 * 1024
STATIC = Path(__file__).with_name('static')
# Only these application documents can become top-level browser pages.
PAGES = {'/', '/index.html', '/materials-guide.html', '/help.html'}
SUFFIX = re.compile(r'(?:\.[a-zA-Z0-9_-]{1,20}){1,3}$')
READ = dict(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
EDIT = dict(readOnlyHint=False, destructiveHint=True, openWorldHint=False)


class Workshop:
    def __init__(self, output_dir, show=False):
        self.root = Path(output_dir).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.show = show
        self.lock = asyncio.Lock()
        self.browser = self.runtime = self.server = self.context = None
        self.page = None
        self.confirm = None
        self.dialogs = []
        self.downloads = []
        self.transfers = set()
        self.storage_lock = None
        self.storage = self.root / '.workshop-storage.json'

    def file(self, artifact):
        if not isinstance(artifact, str) or not re.fullmatch(r'[a-f0-9]{32}(?:\.[a-zA-Z0-9_-]{1,20}){1,3}', artifact):
            raise ValueError('Supply the generated artifact filename from this workshop.')
        path = self.root / artifact
        if path.is_symlink() or not path.is_file():
            raise ValueError('Unknown workshop artifact.')
        return path

    def create_file(self, name, content):
        suffix = SUFFIX.search(name)
        if not suffix or len(content) > LIMIT:
            raise ValueError('Supply a filename with an extension and at most 8 MiB of content.')
        identifier = uuid.uuid4().hex + suffix.group().lower()
        with (self.root / identifier).open('xb') as stream:
            stream.write(content)
        return {'artifact': identifier, 'name': Path(name).name, 'size': len(content)}

    async def _download(self, download):
        path = await download.path()
        if not path or path.stat().st_size > LIMIT:
            raise ValueError('Download failed or exceeds 8 MiB.')
        self.downloads.append(self.create_file(download.suggested_filename, path.read_bytes()))

    def _watch(self, page):
        async def dialog(event):
            self.dialogs.append(
                {'type': event.type, 'message': event.message[:500], 'accepted': self.confirm is not None}
            )
            if self.confirm is None:
                await event.dismiss()
            else:
                answer, self.confirm = self.confirm, None
                await event.accept(answer if event.type == 'prompt' else None)

        def download(event):
            task = asyncio.create_task(self._download(event))
            self.transfers.add(task)

            def done(future):
                self.transfers.discard(future)
                if not future.cancelled() and future.exception():
                    self.downloads.append({'error': str(future.exception())})

            task.add_done_callback(done)

        page.on('dialog', dialog)
        page.on('download', download)
        page.set_default_timeout(4500)

    def claim_storage(self):
        stream = (self.root / '.workshop.lock').open('a+b')
        try:
            if os.name == 'nt':
                import msvcrt

                if stream.seek(0, 2) == 0:
                    stream.write(b'0')
                    stream.flush()
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            stream.close()
            raise ValueError(
                'This output directory already has an active editor. Connect to that MCP service or use another directory.'
            ) from None
        self.storage_lock = stream

    async def start(self):
        if self.page and not self.page.is_closed():
            return
        if self.runtime:
            await self.close()
        from flask import jsonify, request
        from werkzeug.serving import make_server
        from playwright.async_api import async_playwright
        from .server import create_app

        app = create_app(allow_hardware=False)

        @app.before_request
        def isolated():
            if request.path == '/api/phone-monitor':
                return (
                    jsonify(enabled=False)
                    if (request.get_json(silent=True) or {}).get('action') == 'status'
                    else (jsonify(error='Use the operator workstation for physical pairing.'), 403)
                )
            if request.path == '/api/ports':
                return jsonify([])

        self.server = make_server('127.0.0.1', 0, app, threaded=True)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.origin = 'http://127.0.0.1:' + str(self.server.server_port)
        try:
            self.claim_storage()
            self.runtime = await async_playwright().start()
            self.browser = await self.runtime.chromium.launch(
                headless=not self.show,
                executable_path=os.getenv('KERFKITE_MCP_CHROMIUM') or None,
                chromium_sandbox=True,
            )
            saved = None
            if self.storage.exists():
                if self.storage.is_symlink() or self.storage.stat().st_size > 32 * 1024 * 1024:
                    raise ValueError('Invalid saved workshop storage.')
                saved = json.loads(self.storage.read_text())
                for origin in saved['origins']:
                    origin['origin'] = self.origin
            self.context = await self.browser.new_context(
                viewport={'width': 1440, 'height': 960},
                storage_state=saved,
                service_workers='block',
                accept_downloads=True,
            )
            await self.context.add_init_script(path=STATIC / 'mcp-controls.js')

            async def local_only(route):
                url = urlsplit(route.request.url)
                allowed = (url.scheme + '://' + url.netloc) == self.origin
                if allowed and route.request.is_navigation_request():
                    allowed = url.path in PAGES
                await (route.continue_() if allowed else route.abort())

            await self.context.route('**/*', local_only)
            self.context.on('page', self._watch)
            self.page = await self.context.new_page()
            await self.page.goto(self.origin)
            await self.page.evaluate('() => window.studioReady')
        except BaseException:
            await self.close()
            raise

    async def persist(self):
        state = await self.context.storage_state(indexed_db=True)
        state = {'cookies': [], 'origins': [o for o in state['origins'] if o['origin'] == self.origin]}
        encoded = json.dumps(state, separators=(',', ':')).encode()
        if len(encoded) > 32 * 1024 * 1024:
            raise ValueError('Saved browser library exceeds 32 MiB. Export and remove unused library entries.')
        with tempfile.NamedTemporaryFile(dir=self.root, delete=False) as stream:
            stream.write(encoded)
        os.replace(stream.name, self.storage)

    async def close(self):
        try:
            if self.context:
                await self.context.close()
            if self.browser:
                await self.browser.close()
        finally:
            if self.runtime:
                await self.runtime.stop()
            if self.server:
                await asyncio.to_thread(self.server.shutdown)
                self.server.server_close()
            if self.storage_lock:
                self.storage_lock.close()
                self.storage_lock = None
            self.runtime = self.browser = self.context = self.page = self.server = None

    async def state(self):
        result = await self.page.evaluate("""() => ({
          page:document.title, workshop:typeof KerfKiteViews!=='undefined'?KerfKiteViews.current():null,
          project:window.projectMetadata?.name, objects:typeof objects==='undefined'?null:objects.length,
          featureNames:window.CNCSolidExchange?.model().features.map(f=>f.id),
          selection:typeof selected==='number'&&objects[selected]?objects[selected].name:null,
          dialog:document.querySelector('dialog[open]')?.id,
          feedback:['message','cad-command-result','cad-script-feedback','cad-macro-status','solid-status','summary']
            .map(id=>document.getElementById(id)).filter(e=>e&&e.getClientRects().length&&e.textContent)
            .map(e=>[e.id,e.textContent.slice(0,300)]),
          tabs:[...document.querySelectorAll('[role=tab][aria-selected=true]')].map(b=>b.id)
        })""")
        result.update(
            pages=[{'index': i, 'title': await p.title()} for i, p in enumerate(self.context.pages)],
            dialogs=self.dialogs[-4:],
            downloads=self.downloads[-8:],
        )
        return result

    async def element(self, ref):
        target = await self.page.evaluate_handle('(ref)=>KerfKiteAssistantControls.element(ref)', ref)
        element = target.as_element()
        if not element:
            await target.dispose()
            raise ValueError('Control changed. Search the controls again.')
        return element

    async def action(self, action, ref, value, points, confirm):
        if action == 'page':
            if not value.isdigit() or int(value) >= len(self.context.pages):
                raise ValueError('Use a page index from workshop_state.')
            self.page = self.context.pages[int(value)]
            return await self.state()
        element = await self.element(ref)
        self.confirm = confirm
        self.dialogs.clear()
        try:
            if action in ('click', 'double_click'):
                await (element.click() if action == 'click' else element.dblclick())
            elif action == 'fill':
                await element.fill(value)
            elif action == 'select':
                await element.select_option(value)
            elif action == 'check':
                if value not in ('true', 'false'):
                    raise ValueError('Set value to true or false.')
                await element.set_checked(value == 'true')
            elif action == 'key':
                if len(value) > 50 or not re.fullmatch(r'[A-Za-z0-9+\-/ ]+', value):
                    raise ValueError('Supply a keyboard key or chord.')
                await element.press(value)
            elif action == 'upload':
                path = self.file(value)
                if path.stat().st_size > LIMIT:
                    raise ValueError('File exceeds 8 MiB.')
                await element.set_input_files(path)
            elif action in ('point', 'drag', 'wheel'):
                length = 2 if action == 'point' else 4
                if len(points) != length or any(type(v) not in (int, float) or not abs(v) <= 10000 for v in points):
                    raise ValueError('Supply local pixel coordinates as indicated in the tool description.')
                await element.scroll_into_view_if_needed()
                b = await element.bounding_box()
                if not b:
                    raise ValueError('Open the control’s panel before pointing at it.')

                def position(x, y):
                    if not (0 <= x < b['width'] and 0 <= y < b['height']):
                        raise ValueError('Position lies outside this control.')
                    return b['x'] + x, b['y'] + y

                start = position(*points[:2])
                if action == 'point':
                    await self.page.mouse.click(*start)
                else:
                    end = position(*points[2:]) if action == 'drag' else None
                    await self.page.mouse.move(*start)
                    if end:
                        await self.page.mouse.down()
                        try:
                            await self.page.mouse.move(*end, steps=12)
                        finally:
                            await self.page.mouse.up()
                    else:
                        await self.page.mouse.wheel(*points[2:])
            elif action == 'inspect':
                return await element.evaluate("""el=>({text:(el.innerText||el.textContent||'').slice(0,4096),
                    value:el.type==='password'?null:el.value, rect:el.getBoundingClientRect().toJSON(),
                    options:el.tagName==='SELECT'?[...el.options].map(o=>[o.value,o.label]):undefined})""")
            else:
                raise ValueError('Unknown workshop action.')
            if self.transfers:
                await asyncio.gather(*list(self.transfers), return_exceptions=True)
            await self.persist()
            return await self.state()
        finally:
            self.confirm = None
            await element.dispose()


def register_workshop(mcp, workshop):
    from fastmcp.utilities.types import Image
    from typing import Literal
    from flask import Flask
    from .design_api import register_design_routes

    design = Flask('kerfkite-design', static_folder=None)
    register_design_routes(design)
    design.config.update(MAX_CONTENT_LENGTH=LIMIT, TESTING=True)
    routes = {
        r.rule.removeprefix('/api/'): ('GET' if 'GET' in r.methods else 'POST') for r in design.url_map.iter_rules()
    }
    fields = {
        'cad-script': 'source, parameters?, overrides?',
        'solid-cad': 'model',
        'solid-cad/sketch-points': 'points, parameters?, constraints?',
        'solid-cad/section': 'model, z',
        'solid-cad/stl': 'model',
        'solid-cad/surface': 'surface',
        'solid-cad/step/import': 'content',
        'solid-cad/step/export': 'model or surface',
        'solid-cad/exchange-stl': 'surface or model',
        'geometry': 'action, objects, distance?',
        'rotary': 'project, options',
        'tool-changes': 'project',
        'simulate': 'segments, width, height, stock_thickness, tool_diameter, resolution?',
        'assembly/solve': 'assembly, mesh?',
        'assembly/example': 'no inputs',
        'fixture-collision': 'lines, start (XYZ), fixtures, tool',
        'protocols': 'no inputs',
        'relief': 'heightmap, width, height, depth, tool_diameter, tool_shape, stepover',
        'extrude': 'paths, height',
        'artwork-relief': 'paths, width, height and relief settings',
        'pdf-import': 'content (base64 PDF), page?',
        'program-import': 'name, text, unit?',
        'import-bundle': 'content (base64 ZIP), index? to select an entry',
    }

    @mcp.tool(annotations=READ)
    def design_actions(query: str = '') -> list[dict]:
        """Discover Studio's stateless CAD, CAM, solid/surface, import, assembly and simulation calculations."""
        return [
            {'action': name, 'method': verb, 'inputs': fields.get(name, 'Studio JSON payload')}
            for name, verb in sorted(routes.items())
            if query.casefold() in (name + ' ' + fields.get(name, '')).casefold()
        ]

    @mcp.tool(annotations={**READ, 'readOnlyHint': False})
    def design_call(action: str, payload: dict | None = None) -> dict:
        """Run one catalogued calculation. Returns small JSON inline, larger geometry or binary exports as artifacts."""
        if action not in routes:
            raise ValueError('Unknown action. Use design_actions to list supported names.')
        if len(json.dumps(payload, allow_nan=False).encode()) > LIMIT:
            raise ValueError('Payload exceeds 8 MiB.')
        with design.test_client() as client:
            try:
                r = client.open('/api/' + action, method=routes[action], json=payload or {})
            except (ValueError, RuntimeError, TypeError, KeyError) as e:
                raise ValueError(str(e)) from None
            if r.status_code != 200:
                raise ValueError('This design operation returned HTTP ' + str(r.status_code))
            if r.is_json and len(r.data) <= 24576:
                return {'result': r.json}
            filename = r.headers.get('Content-Disposition', '').split('filename=')[-1].strip('"')
            return workshop.create_file(
                filename or ('result.json' if r.is_json else 'part.stl' if r.mimetype == 'model/stl' else 'result.bin'),
                r.data,
            )

    @mcp.tool(annotations=READ)
    async def workshop_state() -> dict:
        """Inspect the active assistant workshop's project, selected tabs, solid features, feedback, downloads and pages."""
        async with workshop.lock:
            await workshop.start()
            return await workshop.state()

    @mcp.tool(annotations=READ)
    async def workshop_controls(query: str = '', offset: int = 0, limit: int = 35, visible_only: bool = False) -> dict:
        """Search actual Studio UI controls by name, id or panel. Open a hidden control's panel first; rediscover refs after redraw."""
        if offset < 0 or not 1 <= limit <= 100:
            raise ValueError('Use offset ≥ 0 and limit 1–100.')
        async with workshop.lock:
            await workshop.start()
            return await workshop.page.evaluate(
                '(opts)=>KerfKiteAssistantControls.search(opts)',
                dict(query=query, offset=offset, limit=limit, visibleOnly=visible_only),
            )

    @mcp.tool(annotations=EDIT)
    async def workshop_action(
        action: Literal[
            'click',
            'double_click',
            'fill',
            'select',
            'check',
            'key',
            'upload',
            'point',
            'drag',
            'wheel',
            'inspect',
            'page',
        ],
        ref: str = '',
        value: str = '',
        points: list[float] | None = None,
        confirm: str | None = None,
    ) -> dict:
        """Operate a discovered control on the isolated simulator. fill/select/key use value. check uses true/false. upload uses artifact ID.
        point: [x,y], drag: [x1,y1,x2,y2], wheel: [x,y,dx,dy], in CSS pixels within control; inspect returns its size.
        page selects an open page index in value. Dialogs dismiss by default. Supply confirm only for a specifically approved prompt."""
        if len(value) > 4_000_000 or (confirm is not None and len(confirm) > 8192):
            raise ValueError('Action value exceeds the input limit.')
        async with workshop.lock:
            await workshop.start()
            return await workshop.action(action, ref, value, points or [], confirm)

    @mcp.tool(annotations=EDIT)
    async def workshop_command(line: str) -> dict:
        """Run a CAD command in the current editor, such as rect 60 30, view front, or cube([40,25,5]);. Returns actual UI state."""
        if not line.strip() or len(line.encode()) > 65536:
            raise ValueError('Enter up to 64 KiB of CAD source or commands.')
        async with workshop.lock:
            await workshop.start()
            message = await workshop.page.evaluate('(line)=>KerfKiteCAD.execute(line)', line)
            await workshop.persist()
            return {'message': message, 'state': await workshop.state()}

    @mcp.tool(annotations=READ)
    async def workshop_macros() -> list[dict]:
        """List the saved and starter macro definitions, commands/source and parameter defaults in the workshop library."""
        async with workshop.lock:
            await workshop.start()
            return await workshop.page.evaluate('()=>KerfKiteMacros.list()')

    @mcp.tool(annotations=EDIT)
    async def workshop_save_macro(
        name: str,
        kind: Literal['commands', 'scad'],
        source: str,
        parameters: dict[str, float] | None = None,
        replace: bool = False,
    ) -> dict:
        """Save or explicitly replace a named reusable macro. Command lines accept {width}; CAD source uses width directly."""
        async with workshop.lock:
            await workshop.start()
            entry = dict(name=name, kind=kind, source=source, parameters=parameters or {})
            await workshop.page.evaluate('([entry,replace])=>KerfKiteMacros.save(entry,replace)', [entry, replace])
            await workshop.persist()
            return {'saved': name}

    @mcp.tool(annotations=EDIT)
    async def workshop_macro(name: str, parameters: dict[str, float] | None = None) -> dict:
        """Run a named starter or saved macro with optional per-instance numeric overrides, in the current project."""
        async with workshop.lock:
            await workshop.start()
            msg = await workshop.page.evaluate('([n,p])=>KerfKiteMacros.run(n,p)', [name, parameters or {}])
            await workshop.persist()
            return {'message': msg, 'state': await workshop.state()}

    @mcp.tool(annotations=EDIT)
    async def workshop_export(kind: Literal['project', 'svg', 'gcode', 'stl']) -> dict:
        """Save the current project/SVG, generate and export simulated CAM G-code, or export the current solid as STL."""
        async with workshop.lock:
            await workshop.start()
            if kind == 'stl':
                model = await workshop.page.evaluate('()=>CNCSolidExchange.model()')
                return await asyncio.to_thread(design_call, 'solid-cad/stl', {'model': model})
            payload = await workshop.page.evaluate(
                """async kind=>{
              if(kind==='gcode')await generateDesignPreview();
              return artifact(kind);
            }""",
                kind,
            )
            return workshop.create_file(payload['name'], payload['content'].encode())

    @mcp.tool(annotations=EDIT)
    def workshop_upload(name: str, content_base64: str) -> dict:
        """Store supplied file bytes as a new workshop artifact. Use workshop_action(upload) on the relevant file input to import it."""
        if len(content_base64) > 4 * ((LIMIT + 2) // 3):
            raise ValueError('Upload exceeds 8 MiB.')
        try:
            content = base64.b64decode(content_base64, validate=True)
        except (binascii.Error, ValueError):
            raise ValueError('Supply standard base64 file content.') from None
        return workshop.create_file(name, content)

    @mcp.tool(annotations=READ)
    def workshop_read_file(artifact: str, offset: int = 0, length: int = 24576) -> dict:
        """Read an exported workshop artifact in base64 chunks. Offsets and lengths count decoded bytes."""
        path = workshop.file(artifact)
        if offset < 0 or offset > path.stat().st_size or not 1 <= length <= 131072:
            raise ValueError('Use a valid byte offset and a chunk length from 1 to 131072.')
        with path.open('rb') as f:
            f.seek(offset)
            data = f.read(length)
        return {
            'artifact': artifact,
            'offset': offset,
            'next': offset + len(data),
            'size': path.stat().st_size,
            'content_base64': base64.b64encode(data).decode(),
        }

    @mcp.tool(annotations=READ)
    async def workshop_text(offset: int = 0, length: int = 4096) -> dict:
        """Read visible editor labels, feedback, help, quotes or material guidance on the selected page in text chunks."""
        if offset < 0 or not 1 <= length <= 12000:
            raise ValueError('Enter a nonnegative offset and a length of 1–12000 characters.')
        async with workshop.lock:
            await workshop.start()
            return await workshop.page.evaluate(
                """([i,n])=>{
              const text=document.body.innerText;
              return {text:text.slice(i,i+n),offset:i,length:text.length};
            }""",
                [offset, length],
            )

    @mcp.tool(annotations=EDIT)
    async def workshop_print() -> dict:
        """Save a PDF of the selected workshop page. For a drawing or customer quote, open its normal Print view first."""
        async with workshop.lock:
            await workshop.start()
            data = await workshop.page.pdf(format='A4', print_background=True, prefer_css_page_size=True)
            return workshop.create_file('workshop.pdf', data)

    @mcp.tool(annotations=READ)
    async def workshop_snapshot() -> Image:
        """See a screenshot of the current editor or guide, including geometry, toolpaths and control placement."""
        async with workshop.lock:
            await workshop.start()
            return Image(data=await workshop.page.screenshot(type='png'), format='png')
