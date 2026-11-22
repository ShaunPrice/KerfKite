"""Reusable authenticated/local design endpoints; no machine access."""

from flask import Response, request, jsonify
from shapely.ops import unary_union
from .operations import region, outlines, value
from .artwork_relief import artwork_relief, artwork_relief_json
from .relief import prepare_relief, relief_json


def register_design_routes(app):
    from . import assembly, cad_exchange, fixture_collision, program_import, import_bundle, pcb_api

    for module in (assembly, cad_exchange, fixture_collision, program_import, import_bundle, pcb_api):
        module.register(app)

    def body(message):
        # Design routes accept one JSON object. Anything else is a JSON 400, never an unhandled error.
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            raise ValueError(message)
        return payload

    def submitted_model():
        payload = body('Supply a CAD model in the model field')
        if not isinstance(payload.get('model'), dict):
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

        payload = body('Supply a CAD script')
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

        payload = body('Supply sketch points and parameters')
        if not isinstance(payload.get('points'), list):
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

        payload = body('Supply a CAD model and section height')
        if not isinstance(payload.get('model'), dict):
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
        from .tool_changes import plan_tool_changes, tool_changes_json

        data = body('Provide a project')
        # Every stage is a whole job: the core's own JSON text of the plan is the response.
        text = tool_changes_json(data.get('project'))
        if text is not None:
            return app.response_class(text, mimetype='application/json')
        return jsonify(plan_tool_changes(data.get('project')))

    @app.post('/api/rotary')
    def rotary_export():
        from .rotary import generate_rotary, rotary_json

        data = body('Provide a project and rotary setup')
        if not isinstance(data.get('project'), dict):
            raise ValueError('Provide a project and rotary setup')
        project, options = data['project'], data.get('options', {})
        text = rotary_json(project, options)
        if text is not None:
            return Response(text, mimetype='application/json')
        return jsonify(generate_rotary(project, options, backend='python'))

    @app.post('/api/geometry')
    def geometry():
        data = body('Select 1–200 objects')
        objects = data.get('objects', [])
        if not isinstance(objects, list) or not 1 <= len(objects) <= 200:
            raise ValueError('Select 1–200 objects')
        if not all(isinstance(o, dict) and isinstance(o.get('paths'), list) for o in objects):
            raise ValueError('Each object needs a list of paths')
        if not all(isinstance(path, list) for o in objects for path in o['paths']):
            raise ValueError('Each path is a list of points')
        if sum(len(path) for obj in objects for path in obj['paths']) > 100000:
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
            data = request.get_json(silent=True)
        return jsonify(simulate(data))

    @app.post('/api/extrude')
    def extrude_artwork():
        from .solid_text import extrude

        return jsonify(extrude(body('Supply closed artwork paths')))

    @app.post('/api/artwork-relief')
    def artwork_relief_route():
        data = body('Supply closed artwork paths')
        # The core's own JSON text is the response; the Shapely reference answers without the extension.
        text = artwork_relief_json(data)
        if text is not None:
            return app.response_class(text, mimetype='application/json')
        return jsonify(artwork_relief(data, backend='python'))

    @app.post('/api/relief')
    def relief():
        data = body('Expected relief settings')
        if not data.get('content_base64'):
            # A map of heights or an image's bytes: the core's own JSON text is the response.
            text = relief_json(data)
            if text is not None:
                return app.response_class(text, mimetype='application/json')
        warnings = []
        if data.get('content_base64'):
            from .mesh_import import stl_heightmap

            mesh = stl_heightmap(data)
            data = {**data, 'heightmap': mesh['heightmap']}
            warnings = mesh['warnings']
        result = prepare_relief(data)
        result['warnings'] += warnings
        return jsonify(result)
