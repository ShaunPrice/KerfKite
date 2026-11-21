"""Portable artwork artifacts shared with the browser's project/library formats."""

import html
import json
import uuid
from pathlib import Path
from .cam import generate, number
from .template_catalog import TemplateCatalog


def label(value, field, limit):
    if not isinstance(value, str) or not value.strip() or len(value) > limit:
        raise ValueError(f'{field} must contain 1–{limit} characters')
    return value.strip()


def validate_paths(paths):
    if not isinstance(paths, list) or not 1 <= len(paths) <= 3000:
        raise ValueError('Provide 1–3000 paths')
    count = 0
    result = []
    for path in paths:
        if not isinstance(path, list) or len(path) < 2:
            raise ValueError('Each path needs at least two points')
        clean = []
        for point in path:
            if not isinstance(point, list) or len(point) != 2:
                raise ValueError('Each point must be [x, y]')
            clean.append([number(v, 'Coordinate', 0, 2000) for v in point])
        count += len(clean)
        if count > 100000:
            raise ValueError('Artwork exceeds 100,000 points')
        result.append(clean)
    return result


class ArtworkStore:
    def __init__(self, output_dir):
        self.root = Path(output_dir).expanduser().resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.builtins = TemplateCatalog()

    def write(self, suffix, content):
        # Callers supply content only. Names are generated, never caller-controlled paths.
        target = self.root / (uuid.uuid4().hex + suffix)
        with target.open('x', encoding='utf-8') as f:
            f.write(content)
        return str(target)

    def list_templates(self, query='', category='', limit=30):
        return self.builtins.search(query, category, limit)

    def template(self, template_id, x=0, y=0, width=100, height=100):
        t = self.builtins.get(template_id)
        if t is None:
            raise ValueError('Unknown template id; use list_templates')
        x, y, width, height = [
            number(v, n, 0 if n in ('X', 'Y') else 0.1, 2000)
            for v, n in zip((x, y, width, height), ('X', 'Y', 'Width', 'Height'))
        ]
        paths = t['paths']
        for path in paths:
            for point in path:
                point[0] = number(x + point[0] * width / 100, 'Coordinate', 0, 2000)
                point[1] = number(y + point[1] * height / 100, 'Coordinate', 0, 2000)
        return {'name': t['name'], 'category': t['category'], 'paths': paths}

    def create_artwork(self, name, paths, width=750, height=400):
        name = label(name, 'Name', 80)
        paths = validate_paths(paths)
        project = {
            'width': width,
            'height': height,
            'mode': 'laser',
            'feed': 600,
            'power': 1000,
            'safe': 5,
            'depth': 1,
            'step': 0.5,
            'plunge': 100,
            'passes': 1,
            'lead_in': 0,
            'lead_out': 0,
            'start_power': 100,
            'end_power': 100,
            'ramp_in': 0,
            'ramp_out': 0,
            'paths': paths,
        }
        generate(project)  # Same bounds and parameter validation as the UI.
        obj = {'name': name, 'paths': paths}
        document = {'format': 'pi-cnc-studio/1', 'settings': project, 'objects': [obj]}
        svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{float(width)}mm" height="{float(height)}mm" viewBox="0 0 {float(width)} {float(height)}"><title>{html.escape(name)}</title>'
        # Separate paths preserve discontinuities for the current UI importer.
        svg += (
            ''.join(
                '<path fill="none" stroke="black" d="M'
                + ' L'.join(f'{x:.6f},{float(height) - y:.6f}' for x, y in p)
                + '"/>'
                for p in paths
            )
            + '</svg>'
        )
        return {
            'project_path': self.write('.cncstudio.json', json.dumps(document)),
            'svg_path': self.write('.svg', svg),
            'object': obj,
            'note': 'Open the project in KerfKite Studio. Defaults are placeholders; review operation settings. No machine commands sent.',
        }

    def save_template(self, name, category, paths):
        name = label(name, 'Name', 80)
        category = label(category, 'Category', 40)
        paths = validate_paths(paths)
        points = [p for path in paths for p in path]
        minx, miny = min(p[0] for p in points), min(p[1] for p in points)
        w, h = max(p[0] for p in points) - minx, max(p[1] for p in points) - miny
        normalized = [[[100 * (x - minx) / (w or 1), 100 * (y - miny) / (h or 1)] for x, y in p] for p in paths]
        t = {'id': 'custom-' + uuid.uuid4().hex, 'name': name, 'category': category, 'paths': normalized}
        bundle = {'format': 'pi-cnc-templates/1', 'custom': [t], 'favorites': []}
        return {
            'library_path': self.write('.templates.json', json.dumps(bundle)),
            'template': t,
            'note': 'Import this file using Template library → Import library. Browser storage is separate from MCP artifacts.',
        }

    def export_job(self, project):
        result = generate(project)
        return {
            'gcode_path': self.write('.nc', '\n'.join(result['lines']) + '\n'),
            'bounds': result['bounds'],
            'cut_minutes': result['cut_minutes'],
            'warnings': result['warnings'],
            'line_count': len(result['lines']),
            'note': 'Review only; no physical machine commands were sent.',
        }
