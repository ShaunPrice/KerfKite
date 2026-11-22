"""Exercise image reliefs in the editor: a map larger than 256 points travels as the canvas's RGB bytes.

The image is made here, so the check needs no file: a 512 x 512 PNG of rings. The editor must send its pixels
exactly, top row first, and add the relief the server computes from them; maps beyond the server's limit, STL
maps beyond 128 and artwork maps beyond 128 must be refused with the limit named.
"""

import base64
import json
import struct
import zlib

from playwright.sync_api import expect

SIDE = 512


def ring(x, y):
    level = (x * x + y * y) // 97 % 256
    return level, (level * 3) % 256, 255 - level


def png(width, height, pixel):
    """An 8-bit RGB PNG, without a colour profile, of pixel(x, y) for each point, top row first."""
    rows = b''.join(b'\x00' + bytes(v for x in range(width) for v in pixel(x, y)) for y in range(height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


def run(page):
    loc = page.locator
    tab = lambda name: page.get_by_role("tab", name=name, exact=True).click()
    page.evaluate("() => window.studioReady")
    limit = page.evaluate("window.studioSession.relief_map")
    assert limit in (256, 1024), limit
    if limit < SIDE:
        print('SKIP image relief beyond 256 points: the server has no compiled extension')
        return
    before = page.evaluate("CNCProjects.document().objects.length")
    tab("Design")
    for field, value in (("#shapeX", "20"), ("#shapeY", "20"), ("#shapeW", "100"), ("#shapeH", "100")):
        loc(field).fill(value)
    tab("CAM")
    loc("#tool_diameter").fill("6")
    loc("#stepover").fill("1.2")
    tab("3D relief")
    loc("#reliefFile").evaluate("input => input.closest('details').open = true")
    loc("#reliefFile").set_input_files({"name": "rings.png", "mimeType": "image/png", "buffer": png(SIDE, SIDE, ring)})
    loc("#reliefDepth").fill("3")
    loc("#reliefTool").select_option("ball")

    loc("#reliefResolution").fill(str(limit + 1))
    loc("#makeRelief").click()
    expect(loc("#message")).to_contain_text(f"Image relief resolution must be 16–{limit}")

    loc("#reliefResolution").fill(str(SIDE))
    with page.expect_request(lambda request: request.url.endswith('/api/relief')) as sent:
        loc("#makeRelief").click()
    expect(loc("#message")).to_contain_text("C++ accelerated relief")
    page.wait_for_function(f"CNCProjects.document().objects.length === {before + 1}")
    payload = json.loads(sent.value.post_data)
    assert 'heightmap' not in payload and payload['heightmap_size'] == [SIDE, SIDE], sorted(payload)
    expected = bytes(v for y in range(SIDE) for x in range(SIDE) for v in ring(x, y))
    assert base64.b64decode(payload['heightmap_rgb']) == expected, 'the editor changed the pixels'
    relief = page.evaluate("CNCProjects.document().objects.at(-1)")
    assert relief['name'] == 'Relief · rings.png', relief['name']
    assert relief['surface']['rows'] == SIDE and relief['surface']['columns'] == SIDE, relief['surface']
    assert 'device' not in relief['surface']
    points = sum(len(path) for path in relief['paths'])
    assert points == 512 * 85 and len(relief['depths']) == 85, (points, len(relief['depths']))

    stl = b'solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid t\n'
    loc("#reliefFile").set_input_files({"name": "tile.stl", "mimeType": "model/stl", "buffer": stl})
    loc("#reliefResolution").fill("200")
    loc("#makeRelief").click()
    expect(loc("#message")).to_contain_text("STL relief resolution must be 16–128")
    tab("Text")
    loc("#raisedText").evaluate("button => button.closest('details').open = true")
    loc("#raisedText").click()
    expect(loc("#message")).to_contain_text("Artwork relief resolution must be 16–128")

    loc("#reliefResolution").fill("64")
    assert page.evaluate("objects[selected].name") == 'Relief · rings.png'
    tab("Objects")
    loc("#remove").click()
    page.wait_for_function(f"CNCProjects.document().objects.length === {before}")
    tab("Design")
    print(
        f"PASS a {SIDE}-point image relief travels as its exact RGB bytes and gives {points} points;"
        f" maps beyond {limit}, and STL and artwork maps beyond 128, are refused"
    )
