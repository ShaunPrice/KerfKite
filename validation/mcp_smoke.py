"""Launch the actual stdio server and exercise tools through the MCP SDK."""
import asyncio
import json
import sys
import tempfile
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def payload(result):
    if result.isError:
        raise AssertionError(str(result.content))
    return result.structuredContent or json.loads(result.content[0].text)


async def main():
    with tempfile.TemporaryDirectory() as output:
        params=StdioServerParameters(command=sys.executable,args=['-m','studio.mcp_server','--output-dir',output],cwd=str(Path(__file__).resolve().parents[1]))
        async with stdio_client(params) as (read,write):
            async with ClientSession(read,write) as session:
                await session.initialize()
                names=[t.name for t in (await session.list_tools()).tools]
                assert len(names)==6 and not any(n in names for n in ['start','stop','command','connect'])
                listing=payload(await session.call_tool('list_templates',{'query':'flower'}))
                template=payload(await session.call_tool('get_template',{'template_id':'flower-round-6','x':10,'y':10,'width':50,'height':50}))
                created=payload(await session.call_tool('create_artwork',{'name':'MCP flower test','paths':template['paths']}))
                assert Path(created['project_path']).exists()
                saved=payload(await session.call_tool('save_template',{'name':'Flower test','category':'Flowers','paths':template['paths']}))
                assert Path(saved['library_path']).exists()
                project=json.loads(Path(created['project_path']).read_text())['settings']
                preview=payload(await session.call_tool('preview_job',{'project':project}))
                exported=payload(await session.call_tool('export_gcode',{'project':project}))
                assert Path(exported['gcode_path']).read_text().endswith('M5\n')
                bad=await session.call_tool('get_template',{'template_id':'unknown'})
                assert bad.isError
                print(json.dumps({'stdio_initialize':True,'tools':names,'artwork_files_created':True,'template_bundle_created':True,'cam_lines':preview['line_count'],'gcode_export':True,'invalid_id_rejected':True}))


if __name__=='__main__':asyncio.run(main())
