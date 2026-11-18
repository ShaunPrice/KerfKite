"""Build the downloadable single-file edition of the Studio materials guide."""

import argparse
import base64
from io import BytesIO
from pathlib import Path
import re

from PIL import Image

STATIC = Path(__file__).resolve().parents[1] / "studio/static"
REPOSITORY = "https://github.com/ShaunPrice/KerfKite"


def build(destination):
    page = (STATIC / "materials-guide.html").read_text(encoding="utf-8")
    page = re.sub(
        r'<link rel="stylesheet" href="\./([\w-]+\.css)">',
        lambda m: "<style>" + (STATIC / m[1]).read_text(encoding="utf-8") + "</style>",
        page,
    )
    page = re.sub(
        r'<script src="\./([\w-]+\.js)"></script>',
        lambda m: "<script>" + (STATIC / m[1]).read_text(encoding="utf-8") + "</script>",
        page,
    )
    with Image.open(STATIC / "app-icon.png") as image:
        image.thumbnail((48, 48), Image.Resampling.LANCZOS)
        small_icon = BytesIO()
        image.save(small_icon, format="PNG")
    icon = base64.b64encode(small_icon.getvalue()).decode("ascii")
    page = page.replace("./app-icon.png", "data:image/png;base64," + icon)
    page = page.replace('href="./index.html"', f'href="{REPOSITORY}"')
    page = page.replace('href="./help.html"', f'href="{REPOSITORY}/blob/main/docs/MATERIALS.md"')
    page = page.replace('class="help-link">Help centre', 'class="help-link">Online documentation')
    if 'src="./' in page or 'href="./' in page:
        raise ValueError("A guide asset still requires a sibling file")
    notice = (STATIC.parents[1] / "LICENSE").read_text(encoding="utf-8").strip()
    page = page.replace("<!doctype html>", "<!doctype html>\n<!--\n" + notice + "\n-->")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(page, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    build(parser.parse_args().destination)
