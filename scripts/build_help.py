"""Rebuild bundled offline help. Development dependency: markdown-it-py."""

from pathlib import Path
from markdown_it import MarkdownIt
import re, html

root = Path(__file__).resolve().parents[1]
files = [root / 'README.md'] + list((root / 'docs').glob('*.md'))
md = MarkdownIt('commonmark', {'html': False}).enable('table')
body = ''
nav = ''
for p in files:
    key = p.stem.lower()
    nav += f'<a href="#{key}">{html.escape(p.stem.replace("-", " ").title())}</a> '
    body += f'<section id="{key}">' + md.render(p.read_text()) + '</section>'
body = re.sub(r'href="([^"#]+\.md)(#[^"]*)?"', lambda m: 'href="#' + Path(m[1]).stem.lower() + '"', body)
body = (
    body.replace('src="../studio/static/', 'src="')
    .replace('src="studio/static/', 'src="')
    .replace('href="../studio/static/', 'href="')
    .replace('href="studio/static/', 'href="')
)
body = body.replace('href="../LICENSE"', 'href="vendor/Studio-MIT.txt"').replace(
    'href="LICENSE"', 'href="vendor/Studio-MIT.txt"'
)
(root / 'studio/static/help.html').write_text(
    '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>KerfKite Studio Help</title><style>body{font:16px/1.65 system-ui;max-width:1000px;margin:40px auto;padding:0 24px;color:#203744;background:#f5f8fa}nav{display:flex;gap:12px;flex-wrap:wrap;padding:18px;background:#e4edf1}a{color:#176a9a}section{margin:60px 0;border-top:1px solid #ccd7de}img{max-width:100%}pre{overflow:auto;padding:16px;background:#e4edf1}table{border-collapse:collapse}td,th{padding:8px;border:1px solid #ccd7de}</style><h1>KerfKite Studio Help</h1><nav>'
    + nav
    + '</nav>'
    + body
    + '</html>'
)
