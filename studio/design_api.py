"""Reusable authenticated/local design endpoints; no machine access."""

from flask import Response, request, jsonify
from shapely.ops import unary_union
from .operations import region, outlines, value
from .relief import prepare_relief


def register_design_routes(app):
    from . import assembly, cad_exchange, fixture_collision, program_import, import_bundle

    for module in (assembly, cad_exchange, fixture_collision, program_import, import_bundle):
        module.register(app)

    def submitted_model():
        payload = request.get_json()
        if not isinstance(payload, dict) or not isinstance(payload.get('model'), dict):
            raise ValueError('Supply a CAD model in the model field')
        return payload['model']

    @app.post('/api/solid-cad')
    def rebuild_solid():
        from .solid_cad import rebuild

        return jsonify(rebuild(submitted_model()))

    @app.post('/api/cad-script')
    def preview_script():
        from .scad_geometry import build_script
        from .solid_cad import Dimensions, _rings

        payload = request.get_json()
        if not isinstance(payload, dict):
            raise ValueError('Supply a CAD script')
        parameters = Dimensions(payload.get('parameters', {})).script_parameters(payload.get('overrides', {}))
        dimension, shape = build_script(payload.get('source'), parameters)
        if dimension == 2:
            return jsonify(dimension=2, profiles=_rings(shape), bounds=shape.bounds(), area=shape.area())
        mesh = shape.to_mesh64()
        return jsonify(
            dimension=3,
            vertices=mesh.vert_properties[:, :3].tolist(),
            triangles=mesh.tri_verts.tolist(),
            bounds=shape.bounding_box(),
            volume=shape.volume(),
        )

    @app.post('/api/solid-cad/sketch-points')
    def resolve_sketch_points():
        from .solid_cad import Dimensions
        from .sketch_profile import profile_geometry
        from .sketch_constraints import solve_profile

        payload = request.get_json()
        if not isinstance(payload, dict) or not isinstance(payload.get('points'), list):
            raise ValueError('Supply sketch points and parameters')
        if len(payload['points']) > 256:
            raise ValueError('A sketch supports up to 256 vertices')
        dimensions = Dimensions(payload.get('parameters', {}))
        constraint_error = None
        try:
            points, diagnostics = solve_profile(payload, dimensions)
        except ValueError as exc:
            if payload.get('validate'):
                raise
            points = [dimensions.vector(point, 2) for point in payload['points']]
            diagnostics = None
            constraint_error = str(exc)
        outline = None
        issue = None
        if payload.get('validate') or 'fillets' in payload or 'curves' in payload:
            try:
                _, outline = profile_geometry(payload, dimensions, points)
            except ValueError as exc:
                if payload.get('validate'):
                    raise
                issue = str(exc)
        handles = None
        if 'curves' in payload:
            from .sketch_curves import resolve_curves

            try:
                handles = resolve_curves(payload['curves'], points, dimensions)
            except ValueError as exc:
                if payload.get('validate'):
                    raise
                issue = str(exc)
        return jsonify(
            points=points,
            parameters=dimensions.values,
            outline=outline,
            handles=handles,
            issue=issue,
            diagnostics=diagnostics,
            constraint_error=constraint_error,
        )

    @app.post('/api/solid-cad/section')
    def section_solid():
        from .solid_cad import horizontal_section

        payload = request.get_json()
        if not isinstance(payload, dict) or not isinstance(payload.get('model'), dict):
            raise ValueError('Supply a CAD model and section height')
        return jsonify(horizontal_section(payload['model'], payload.get('z')))

    @app.post('/api/solid-cad/stl')
    def export_solid():
        from .solid_cad import model_stl

        data = model_stl(submitted_model())
        return Response(data, mimetype='model/stl', headers={'Content-Disposition': 'attachment; filename="part.stl"'})

    @app.get('/api/protocols')
    def protocols():
        from dataclasses import asdict
        from .protocols import PROTOCOLS

        return jsonify([asdict(p) for p in PROTOCOLS.values()])

    @app.post('/api/tool-changes')
    def tool_change_plan():
        from .tool_changes import plan_tool_changes

        data = request.get_json()
        if not isinstance(data, dict):
            raise ValueError('Provide a project')
        return jsonify(plan_tool_changes(data.get('project')))

    @app.post('/api/rotary')
    def rotary_export():
        from .rotary import generate_rotary

        data = request.get_json()
        if not isinstance(data, dict) or not isinstance(data.get('project'), dict):
            raise ValueError('Provide a project and rotary setup')
        return jsonify(generate_rotary(data['project'], data.get('options', {})))

    @app.post('/api/geometry')
    def geometry():
        data = request.get_json()
        objects = data.get('objects', [])
        if not isinstance(objects, list) or not 1 <= len(objects) <= 200:
            raise ValueError('Select 1–200 objects')
        if sum(len(path) for obj in objects for path in obj.get('paths', [])) > 100000:
            raise ValueError('Geometry too complex')
        shapes = [region(o['paths']) for o in objects]
        op = data.get('action')
        if op == 'union':
            result = unary_union(shapes)
        elif op == 'difference':
            result = shapes[0].difference(unary_union(shapes[1:]))
        elif op == 'intersection':
            result = shapes[0]
            for s in shapes[1:]:
                result = result.intersection(s)
        elif op == 'offset':
            result = unary_union(shapes).buffer(value(data, 'distance', 0, -100, 100), quad_segs=32)
        else:
            raise ValueError('Choose union, difference, intersection or offset')
        if result.is_empty:
            raise ValueError('Operation produced empty geometry')
        paths = outlines(result)
        if sum(map(len, paths)) > 100000:
            raise ValueError('Result too complex')
        return jsonify(paths=paths)

    @app.post('/api/simulate')
    def stock_preview():
        from .simulation import simulate
        from . import stock_wire

        if request.mimetype == stock_wire.MIME:
            request.max_content_length = stock_wire.MAX_BINARY_BYTES + 1  # Allow one EOF probe on chunked bodies.
            data = stock_wire.from_stream(request.stream, request.content_length)
        else:
            # Older clients post seven decimal numbers per row. Apply this bound
            # only after normal request authentication, on this design route.
            request.max_content_length = 36 * 1024 * 1024
            data = request.get_json()
        return jsonify(simulate(data))

    @app.post('/api/extrude')
    def extrude_artwork():
        from .solid_text import extrude

        return jsonify(extrude(request.get_json()))

    @app.post('/api/artwork-relief')
    def artwork_relief():
        from shapely.geometry import box

        data = request.get_json()
        g = region(data.get('paths', []))
        width = value(data, 'width', 100, 0.1, 2000)
        height = value(data, 'height', 100, 0.1, 2000)
        x = value(data, 'x', 0, 0, 2000)
        y = value(data, 'y', 0, 0, 2000)
        n = int(value(data, 'resolution', 64, 16, 128))
        kind = data.get('kind', 'raised')
        if kind not in ('raised', 'recessed'):
            raise ValueError('Choose raised or recessed relief')
        # Protect raised material wherever its contour intersects a cell neighborhood.
        matrix = []
        for row in range(n):
            values = []
            for col in range(n):
                px = x + col * width / (n - 1)
                py = y + row * height / (n - 1)
                cell = box(px - width / (n - 1), py - height / (n - 1), px + width / (n - 1), py + height / (n - 1))
                values.append(1 if (g.intersects(cell) if kind == 'raised' else not g.covers(cell)) else 0)
            matrix.append(values)
        result = prepare_relief({**data, 'heightmap': matrix})
        result['warnings'].append(
            'Artwork relief is conservatively rasterized; increase map resolution for small glyph details.'
        )
        return jsonify(result)

    @app.post('/api/relief')
    def relief():
        data = request.get_json()
        if not isinstance(data, dict):
            raise ValueError('Expected relief settings')
        warnings = []
        if data.get('content_base64'):
            from .mesh_import import stl_heightmap

            mesh = stl_heightmap(data)
            data = {**data, 'heightmap': mesh['heightmap']}
            warnings = mesh['warnings']
        result = prepare_relief(data)
        result['warnings'] += warnings
        return jsonify(result)
