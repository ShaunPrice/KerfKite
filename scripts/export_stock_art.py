"""Export the shared template catalogue as stand-alone SVGs and an offline index."""

import argparse
import html
import json
import math
import re
import struct
import zlib
from pathlib import Path
from tempfile import TemporaryDirectory
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]
CATALOGUE = ROOT / "studio" / "builtin_templates.kkart"
MAGIC = b"KKART\x01\r\n"


def catalogue(path=CATALOGUE):
    """Each template of the packed catalogue (scripts/build_templates.cjs), in order, with its paths: the magic,
    each template's zlib-compressed paths (a count, then each path's point count and little-endian double pairs),
    then a JSON index of [offset, length, metadata] records whose byte length ends the file."""
    data = Path(path).read_bytes()
    if not data.startswith(MAGIC):
        raise ValueError("Unsupported artwork catalogue format")
    (size,) = struct.unpack_from("<I", data, len(data) - 4)
    start = len(data) - 4 - size
    if start < len(MAGIC):
        raise ValueError("Invalid artwork catalogue index")
    for offset, length, metadata in json.loads(data[start:-4]):
        packed = zlib.decompress(data[offset : offset + length])
        (count,) = struct.unpack_from("<I", packed)
        position, paths = 4, []
        for _ in range(count):
            (points,) = struct.unpack_from("<I", packed, position)
            end = position + 4 + 16 * points
            if end > len(packed):
                raise ValueError("Truncated template path")
            paths.append([list(point) for point in struct.iter_unpack("<dd", packed[position + 4 : end])])
            position = end
        if position != len(packed):
            raise ValueError("Unexpected data after template paths")
        yield {**metadata, "paths": paths}


def svg(template):
    aspect = template["aspect"]
    width, height = 100 * min(1, aspect), 100 * min(1, 1 / aspect)
    drawing = []
    for path in template["paths"]:
        closed = len(path) > 2 and math.dist(path[0], path[-1]) < 1e-8
        vertices = path[:-1] if closed else path
        points = " ".join(f"{x * width / 100:.5f},{(100 - y) * height / 100:.5f}" for x, y in vertices)
        drawing.append(f'<path d="M{points}{" Z" if closed else ""}"/>')
    title = html.escape(template["name"])
    data = (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.5f}mm" height="{height:.5f}mm" '
        f'viewBox="0 0 {width:.5f} {height:.5f}"><title>{title}</title>'
        '<g fill="none" stroke="#173c43" stroke-width="0.42" stroke-linejoin="round" stroke-linecap="round">'
        + "".join(drawing)
        + '</g></svg>'
    )
    return data, f"{width:.1f} × {height:.1f} mm"


def export(destination, templates):
    destination = Path(destination)
    with TemporaryDirectory(dir=destination.parent) as scratch:
        pending = Path(scratch) / "stock.zip"
        _write_archive(pending, templates)
        pending.replace(destination)


def _write_archive(destination, templates):
    rows = []
    names = set()
    with ZipFile(destination, "w", ZIP_DEFLATED) as archive:
        for item in templates:
            directory = re.sub(r"[^\w-]+", "-", item["category"]).strip("-")
            if not re.fullmatch(r"[\w-]+", item["id"]):
                raise ValueError(f"Unexpected stock identifier: {item['id']}")
            filename = f"artwork/{directory}/{item['id']}.svg"
            if filename.casefold() in names:
                raise ValueError(f"Duplicate stock filename: {filename}")
            names.add(filename.casefold())
            data, dimensions = svg(item)
            archive.writestr(filename, data)
            rows.append([item["name"], item["category"], dimensions, filename])
        index = (ROOT / "scripts/stock_gallery.html").read_text()
        archive.writestr(
            "index.html",
            index.replace(
                "ARTWORK_DATA", json.dumps(rows, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
            ),
        )
        archive.write(ROOT / "LICENSE", "LICENSE.txt")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    export(args.output, catalogue())
