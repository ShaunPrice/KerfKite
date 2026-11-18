"""Export the shared template catalogue as stand-alone SVGs and an offline index."""
import argparse
import html
import json
import math
import re
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]


def svg(template):
    aspect = template["aspect"]
    width, height = 100 * min(1, aspect), 100 * min(1, 1 / aspect)
    drawing = []
    for path in template["paths"]:
        closed = len(path) > 2 and math.dist(path[0], path[-1]) < 1e-8
        vertices = path[:-1] if closed else path
        points = " ".join(f"{x*width/100:.5f},{(100-y)*height/100:.5f}" for x, y in vertices)
        drawing.append(f'<path d="M{points}{" Z" if closed else ""}"/>')
    title = html.escape(template["name"])
    data = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.5f}mm" height="{height:.5f}mm" '
            f'viewBox="0 0 {width:.5f} {height:.5f}"><title>{title}</title>'
            '<g fill="none" stroke="#173c43" stroke-width="0.42" stroke-linejoin="round" stroke-linecap="round">'
            + "".join(drawing) + '</g></svg>')
    return data, f"{width:.1f} × {height:.1f} mm"


def export(destination, templates):
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
        archive.writestr("index.html", index.replace("ARTWORK_DATA",
            json.dumps(rows, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")))
        archive.write(ROOT / "LICENSE", "LICENSE.txt")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    export(args.output, json.loads((ROOT / "studio/builtin_templates.json").read_bytes()))
