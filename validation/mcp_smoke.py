"""Launch the MCP assistant server over stdio and exercise its tools through the MCP SDK.

    python validation/mcp_smoke.py [PROGRAM]

PROGRAM is kerfkite-mcp; without it, KERFKITE_MCP or the build in build/native. Requires the MCP Python SDK
(pip install "mcp>=1.30,<2")."""

import asyncio
import os
import base64
import json
import sys
import tempfile
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_servers import program as native_program  # noqa: E402 - beside this file


def payload(result):
    if result.isError:
        raise AssertionError(str(result.content))
    return result.structuredContent or json.loads(result.content[0].text)


async def main():
    program = native_program('kerfkite-mcp', sys.argv[1] if len(sys.argv) > 1 else None, 'KERFKITE_MCP')
    with tempfile.TemporaryDirectory() as output:
        params = StdioServerParameters(
            command=program,
            env={
                k: v
                for k, v in os.environ.items()
                if k
                in (
                    'PATH',
                    'HOME',
                    'USERPROFILE',
                    'SYSTEMROOT',
                    'TEMP',
                    'TMP',
                    'TMPDIR',
                    'KERFKITE_MCP_CHROMIUM',
                    'PLAYWRIGHT_BROWSERS_PATH',
                )
            },
            args=['--output-dir', output],
            cwd=str(Path(__file__).resolve().parents[1]),
        )
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                names = [t.name for t in (await session.list_tools()).tools]
                assert {
                    'list_templates',
                    'get_template',
                    'create_artwork',
                    'save_template',
                    'preview_job',
                    'export_gcode',
                    'design_actions',
                    'design_call',
                    'workshop_command',
                    'workshop_macro',
                    'workshop_controls',
                } <= set(names)
                assert not any(n.startswith('pendant_') for n in names)
                listing = payload(await session.call_tool('list_templates', {'query': 'flower'}))
                found = listing.get('result', listing) if isinstance(listing, dict) else listing
                assert found and all('flower' in (t['name'] + ' ' + t['category']).casefold() for t in found), found
                template = payload(
                    await session.call_tool(
                        'get_template', {'template_id': 'flower-round-6', 'x': 10, 'y': 10, 'width': 50, 'height': 50}
                    )
                )
                created = payload(
                    await session.call_tool('create_artwork', {'name': 'MCP flower test', 'paths': template['paths']})
                )
                assert Path(created['project_path']).exists()
                saved = payload(
                    await session.call_tool(
                        'save_template', {'name': 'Flower test', 'category': 'Flowers', 'paths': template['paths']}
                    )
                )
                assert Path(saved['library_path']).exists()
                project = json.loads(Path(created['project_path']).read_text())['settings']
                preview = payload(await session.call_tool('preview_job', {'project': project}))
                exported = payload(await session.call_tool('export_gcode', {'project': project}))
                assert Path(exported['gcode_path']).read_text().endswith('M5\n')
                bad = await session.call_tool('get_template', {'template_id': 'unknown'})
                assert bad.isError

                async def tool(tool_name, **arguments):
                    return payload(await session.call_tool(tool_name, arguments))

                solid = await tool(
                    'design_call', action='cad-script', payload={'source': 'cube([w,4,5]);', 'overrides': {'w': 12}}
                )
                assert solid['result']['volume'] == 240
                assert (await session.call_tool('design_call', {'action': 'connect'})).isError
                controls = await tool('workshop_controls', query='design-tab')
                assert controls['total'] >= 9
                assert (await tool('workshop_command', line='rect 20 12'))['state']['objects'] == 1
                gear = await tool('workshop_controls', query='gear')
                assert gear['total'] > 0
                await tool(
                    'workshop_save_macro',
                    name='Test spacer',
                    kind='scad',
                    source='cube([width,5,3]);',
                    parameters={'width': 10},
                )
                assert (await tool('workshop_macro', name='Test spacer', parameters={'width': 22}))['state'][
                    'featureNames'
                ] == ['Test spacer1']
                assert (await tool('workshop_command', line='view front'))['state']['workshop'] == '3d'
                doc = await tool('workshop_export', kind='project')
                data = await tool('workshop_read_file', artifact=doc['artifact'])
                model = json.loads(base64.b64decode(data['content_base64']))
                assert any(f['scriptParameters']['width'] == 22 for f in model['extras']['solidCAD']['features'])
                stl = await tool('workshop_export', kind='stl')
                assert Path(output, stl['artifact']).stat().st_size == 84 + 12 * 50
                pdf = await tool('workshop_print')
                assert Path(output, pdf['artifact']).read_bytes().startswith(b'%PDF-')
                assert len((await tool('workshop_text', length=12000))['text']) > 150
                image = await session.call_tool('workshop_snapshot', {})
                assert not image.isError and any(c.type == 'image' for c in image.content)
                await tool('workshop_action', action='click', ref='design-tab-layers')
                state = await tool('workshop_state')
                assert 'design-tab-layers' in state['tabs']
                assert (await session.call_tool('workshop_read_file', {'artifact': '../private'})).isError
                svg = await tool(
                    'workshop_upload', name='drawing.svg', content_base64=base64.b64encode(b'<svg/>').decode()
                )
                assert (await tool('workshop_read_file', artifact=svg['artifact']))[
                    'content_base64'
                ] == base64.b64encode(b'<svg/>').decode()
                print(
                    json.dumps(
                        {
                            'stdio_initialize': True,
                            'tools': names,
                            'artwork_files_created': True,
                            'template_bundle_created': True,
                            'cam_lines': preview['line_count'],
                            'gcode_export': True,
                            'invalid_id_rejected': True,
                            'editor_discovery': True,
                            'macro_part_round_trip': True,
                            'pdf_and_stl_export': True,
                            'artifact_content_round_trip': True,
                            'snapshot': True,
                        }
                    )
                )


if __name__ == '__main__':
    asyncio.run(main())
