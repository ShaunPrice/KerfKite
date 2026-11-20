"""Parametric solid history. Dimensions are millimetres and rotations are degrees."""

import ast
import operator
import re
import struct

from .sketch_profile import crossing_axis, profile_geometry

_MAX_VALUE = 100_000
_MAX_FACES = 75_000
_SEGMENTS = 96
_MATH = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv}
_NAME = re.compile(r'[A-Za-z_][A-Za-z0-9_]{0,39}\Z')


def _number(value):
    if type(value) not in (float, int) or not -_MAX_VALUE <= value <= _MAX_VALUE:
        raise ValueError('Dimensions must be finite and within ±100000')
    return float(value)


class Dimensions:
    def __init__(self, definitions):
        if not isinstance(definitions, dict) or len(definitions) > 64:
            raise ValueError('Define at most 64 named parameters')
        if any(not isinstance(name, str) or not _NAME.fullmatch(name) for name in definitions):
            raise ValueError(
                'Use a letter or underscore followed by letters, digits or underscores for parameter names'
            )
        self.definitions = definitions
        self.values = {}
        self.resolving = set()
        for name in definitions:
            self.resolve(name)

    def script_parameters(self, overrides):
        """Resolve part dimensions first, then shadow them for this one macro instance."""
        if not isinstance(overrides, dict) or len(overrides) > 64:
            raise ValueError('Define at most 64 macro parameters')
        if any(not isinstance(name, str) or not _NAME.fullmatch(name) for name in overrides):
            raise ValueError('Invalid macro parameter name')
        return {**self.values, **{name: _number(value) for name, value in overrides.items()}}

    def resolve(self, name):
        if name in self.values:
            return self.values[name]
        if name not in self.definitions:
            raise ValueError(f'Unknown parameter: {name}')
        if name in self.resolving:
            raise ValueError(f'Circular parameter reference: {name}')
        self.resolving.add(name)
        try:
            self.values[name] = self.evaluate(self.definitions[name])
        finally:
            self.resolving.remove(name)
        return self.values[name]

    def evaluate(self, value):
        if not isinstance(value, str):
            return _number(value)
        if not 1 <= len(value) <= 160:
            raise ValueError('Use 1–160 characters per dimension expression')
        try:
            expr = ast.parse(value.strip(), mode='eval')
        except (SyntaxError, ValueError, RecursionError) as exc:
            raise ValueError('Invalid dimension expression') from exc
        if sum(1 for _ in ast.walk(expr)) > 48:
            raise ValueError('Dimension expression is too complex')
        return self._visit(expr.body)

    def _visit(self, node):
        if isinstance(node, ast.Constant):
            return _number(node.value)
        if isinstance(node, ast.Name):
            return self.resolve(node.id)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in ('cross_x', 'cross_y'):
            if len(node.args) != 8 or node.keywords:
                raise ValueError('A sketch crossing requires the eight endpoint coordinates')
            return _number(crossing_axis([self._visit(arg) for arg in node.args], int(node.func.id == 'cross_y')))
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
            value = self._visit(node.operand)
            return -value if isinstance(node.op, ast.USub) else value
        if isinstance(node, ast.BinOp) and type(node.op) in _MATH:
            left, right = self._visit(node.left), self._visit(node.right)
            try:
                return _number(_MATH[type(node.op)](left, right))
            except ZeroDivisionError as exc:
                raise ValueError('Division by zero in dimension') from exc
        raise ValueError('Use numbers, named parameters, parentheses and + − * / in dimensions')

    def vector(self, value, size):
        if not isinstance(value, list) or len(value) != size:
            raise ValueError(f'Expected {size} dimensions')
        return [self.evaluate(component) for component in value]

    def positive(self, value):
        result = self.evaluate(value)
        if result <= 0:
            raise ValueError('Length or radius must be positive')
        return result


def _sketch(feature, dimensions, kernel):
    _, points = profile_geometry(feature, dimensions)
    return kernel.CrossSection([points], kernel.FillRule.EvenOdd)


def _primitive(feature, dimensions, kernel, script_budget=None):
    kind = feature.get('kind')
    if kind == 'step':
        from .cad_exchange import step_manifold

        solid = step_manifold(feature, kernel)
    elif kind == 'script':
        from .scad_geometry import build_script

        parameters = dimensions.script_parameters(feature.get('scriptParameters', {}))
        dimension, solid = build_script(feature.get('source'), parameters, budget=script_budget)
        if dimension != 3:
            raise ValueError('A solid feature needs a 3D script. Use linear_extrude() to give a drawing thickness.')
    elif kind == 'box':
        size = dimensions.vector(feature.get('size'), 3)
        if min(size) <= 0:
            raise ValueError('Box dimensions must be positive')
        solid = kernel.Manifold.cube(size)
    elif kind == 'cylinder':
        radius = dimensions.positive(feature.get('radius'))
        top = dimensions.evaluate(feature.get('topRadius', radius))
        if top < 0:
            raise ValueError('Top radius must be zero or greater')
        solid = kernel.Manifold.cylinder(dimensions.positive(feature.get('height')), radius, top, _SEGMENTS)
    elif kind == 'sphere':
        solid = kernel.Manifold.sphere(dimensions.positive(feature.get('radius')), _SEGMENTS)
    elif kind == 'loft':
        from .solid_loft import loft_mesh

        solid = loft_mesh(feature, dimensions, kernel, _MAX_FACES)
    elif kind == 'extrude':
        solid = _sketch(feature, dimensions, kernel).extrude(dimensions.positive(feature.get('height')))
    elif kind == 'revolve':
        angle = dimensions.evaluate(feature.get('angle', 360))
        if not 0 < angle <= 360:
            raise ValueError('Revolve angle must be above 0 and at most 360 degrees')
        solid = _sketch(feature, dimensions, kernel).revolve(_SEGMENTS, angle)
    else:
        raise ValueError('Unknown solid primitive')
    rotation = dimensions.vector(feature.get('rotation', [0, 0, 0]), 3)
    origin = dimensions.vector(feature.get('origin', [0, 0, 0]), 3)
    return solid.rotate(rotation).translate(origin)


def _instances(solid, pattern, dimensions, kernel):
    if pattern is None:
        return solid
    if not isinstance(pattern, dict):
        raise ValueError('Pattern must be an object')
    count = dimensions.evaluate(pattern.get('count'))
    if not count.is_integer() or not 1 <= count <= 64:
        raise ValueError('Pattern count must be a whole number from 1 to 64')
    if solid.num_tri() * count > _MAX_FACES:
        raise ValueError('Pattern exceeds triangle budget')
    indices = range(1, int(count))
    copies = [solid]
    if pattern.get('kind') == 'linear':
        step = dimensions.vector(pattern.get('step'), 3)
        if count > 1 and not any(step):
            raise ValueError('Pattern step must move each instance')
        copies.extend(solid.translate([i * distance for distance in step]) for i in indices)
    elif pattern.get('kind') == 'polar':
        cx, cy, cz = dimensions.vector(pattern.get('center', [0, 0, 0]), 3)
        angle = dimensions.evaluate(pattern.get('angle'))
        if count > 1 and angle % 360 == 0:
            raise ValueError('Polar pattern needs a nonzero angular step')
        centered = solid.translate([-cx, -cy, -cz])
        copies.extend(centered.rotate([0, 0, angle * i]).translate([cx, cy, cz]) for i in indices)
    else:
        raise ValueError('Pattern must be linear or polar')
    return kernel.Manifold.batch_boolean(copies, kernel.OpType.Add)


def _build(model):
    """Recompute enabled features in order without modifying the input document."""
    try:
        import manifold3d as kernel
    except ImportError as exc:
        raise ValueError('Install manifold3d>=3.5,<4 in the Studio Python environment to rebuild solids') from exc
    if not isinstance(model, dict) or type(model.get('version')) is not int or model['version'] != 1:
        raise ValueError('Expected a version 1 solid model')
    dimensions = Dimensions(model.get('parameters', {}))
    features = model.get('features')
    if not isinstance(features, list) or not 1 <= len(features) <= 64:
        raise ValueError('A solid model needs 1–64 features')
    solid = None
    identifiers = set()
    script_budget = None
    if any(isinstance(f, dict) and f.get('kind') == 'script' and f.get('enabled', True) for f in features):
        from .scad_geometry import ScriptGeometryBudget

        script_budget = ScriptGeometryBudget()
    for i, feature in enumerate(features, 1):
        if not isinstance(feature, dict):
            raise ValueError(f'Invalid feature {i}')
        identifier = feature.get('id')
        if (
            not isinstance(identifier, str)
            or not identifier.strip()
            or len(identifier) > 80
            or identifier in identifiers
        ):
            raise ValueError(f'Feature {i} needs a unique ID of 1–80 characters')
        identifiers.add(identifier)
        if type(feature.get('enabled', True)) is not bool:
            raise ValueError(f'{identifier}: enabled must be true or false')
        if not feature.get('enabled', True):
            continue
        try:
            op = feature.get('op', 'add')
            if op not in ('add', 'cut', 'intersect'):
                raise ValueError('Choose add, cut or intersect')
            if solid is None and op != 'add':
                raise ValueError('First enabled feature must add material')
            tool = _instances(
                _primitive(feature, dimensions, kernel, script_budget), feature.get('pattern'), dimensions, kernel
            )
            if solid is None:
                solid = tool
            elif op == 'add':
                solid = solid + tool
            elif op == 'cut':
                solid = solid - tool
            else:
                solid = solid ^ tool
            if solid.status() != kernel.Error.NoError or solid.is_empty():
                raise ValueError('The operation produced an empty or invalid solid')
            if solid.num_tri() > _MAX_FACES:
                raise ValueError('Solid exceeds 75000 triangles')
        except ValueError as exc:
            raise ValueError(f'{identifier}: {exc}') from exc
    if solid is None:
        raise ValueError('Enable at least one additive feature')
    return solid, dimensions, kernel


def _rings(section):
    paths = []
    for contour in section.to_polygons():
        ring = contour.tolist()
        paths.append(ring + [ring[0]])
    return paths


def horizontal_section(model, z):
    """Solid material at an XY plane. Lower faces are inclusive, upper faces exclusive."""
    solid, dimensions, _ = _build(model)
    height = dimensions.evaluate(z)
    bounds = solid.bounding_box()
    if not bounds[2] <= height < bounds[5]:
        raise ValueError('Section Z must be at or above the bottom and below the top of the model')
    return {'z': height, 'profiles': _rings(solid.slice(height))}


def rebuild(model):
    solid, dimensions, kernel = _build(model)
    mesh = solid.to_mesh64()
    silhouette = kernel.CrossSection(solid.project().to_polygons(), kernel.FillRule.Positive)
    return {
        'vertices': mesh.vert_properties[:, :3].tolist(),
        'triangles': mesh.tri_verts.tolist(),
        'bounds': list(solid.bounding_box()),
        'volume': solid.volume(),
        'profiles': _rings(silhouette),
        'parameters': dimensions.values,
    }


def binary_stl(result):
    """Encode the oriented mesh produced by rebuild() as a binary STL in mm."""
    import numpy as np

    return _encode_stl(np.asarray(result['vertices'], dtype=np.float64), np.asarray(result['triangles']))


def model_stl(model):
    """Export directly from the kernel's indexed mesh, without building the JSON preview."""
    solid, _, _ = _build(model)
    mesh = solid.to_mesh64()
    return _encode_stl(mesh.vert_properties[:, :3], mesh.tri_verts)


def _encode_stl(vertices, triangles):
    from io import BytesIO
    import numpy as np

    output = BytesIO()
    output.write(b'KerfKite Studio solid, mm'.ljust(80, b'\0'))
    output.write(struct.pack('<I', len(triangles)))
    layout = [('normal', '<f4', 3), ('vertices', '<f4', (3, 3)), ('attr', '<u2')]
    for start in range(0, len(triangles), 2048):
        points = vertices[triangles[start : start + 2048]]
        normals = np.cross(points[:, 1] - points[:, 0], points[:, 2] - points[:, 0])
        lengths = np.linalg.norm(normals, axis=1)
        normals /= np.where(lengths == 0, 1, lengths)[:, None]
        records = np.zeros(len(points), dtype=layout)
        records['normal'], records['vertices'] = normals, points
        output.write(records.tobytes())
    return output.getvalue()
