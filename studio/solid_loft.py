"""Ruled loft between two parallel rectangular, elliptical or circular sections."""

from dataclasses import dataclass
import math

import numpy as np


CHORD_TOLERANCE = 0.025
WEIGHT = math.sqrt(0.5)


@dataclass(frozen=True)
class Section:
    kind: str
    x_radius: float
    y_radius: float
    center: tuple

    def coefficients(self, quadrant):
        """Map a unit-circle quadrant affinely onto a section arc or rectangle side."""
        x, y, z = self.center
        a, b = self.x_radius, self.y_radius
        if self.kind == 'rectangle':
            if quadrant in (0, 2):
                return x, y + (-b if quadrant == 0 else b), z, a / WEIGHT, 0.0
            return x + (a if quadrant == 1 else -a), y, z, 0.0, b / WEIGHT
        return x, y, z, a, b

    def point(self, quadrant, angle, weight=1.0):
        x, y, z, a, b = self.coefficients(quadrant)
        return (x + a * math.cos(angle) / weight, y + b * math.sin(angle) / weight, z)

    def poles(self, quadrant):
        angle = (quadrant - 1.5) * math.pi / 2
        return [
            self.point(quadrant, angle),
            self.point(quadrant, angle + math.pi / 4, WEIGHT),
            self.point(quadrant, angle + math.pi / 2),
        ]


def sections(feature, dimensions):
    result = []
    for level in ('base', 'top'):
        kind = feature.get(level + 'Shape')
        if kind not in ('rectangle', 'ellipse', 'circle'):
            raise ValueError('Choose a rectangle, ellipse or circle for each loft section')
        a = dimensions.positive(feature.get(level + 'Width')) / 2
        b = a if kind == 'circle' else dimensions.positive(feature.get(level + 'Depth')) / 2
        xyz = (0.0, 0.0, 0.0)
        if level == 'top':
            xyz = (
                *dimensions.vector(feature.get('offset', [0, 0]), 2),
                dimensions.positive(feature.get('height')),
            )
        result.append(Section(kind, a, b, xyz))
    return result


def loft_mesh(feature, dimensions, kernel, face_limit):
    base, top = sections(feature, dimensions)
    if base.kind == top.kind == 'rectangle':
        steps, tiers = 1, 1  # Each of the four sides is planar, including offset profiles.
    else:
        pairs = [(base.coefficients(i)[3:], top.coefficients(i)[3:]) for i in range(4)]
        radius = max(max(*a, *b) for a, b in pairs)
        # A circular/elliptical section has |P''(theta)| <= radius. This many
        # steps bounds linear interpolation error by half the requested tolerance.
        steps = max(2, 2 * math.ceil(math.pi / 4 * math.sqrt(radius / (4 * CHORD_TOLERANCE))))
        angle = math.pi / (2 * steps)
        mixed = 0.0
        for (a, b), (c, d) in pairs:
            # Parallel chords form planar strips. Otherwise refine Z to bound
            # the remaining error from triangulating each bilinear strip.
            if abs(a * d - b * c) > 1e-14 * max(abs(a * d), abs(b * c)):
                mixed = max(mixed, abs(a - c), abs(b - d))
        tiers = max(1, math.ceil(angle * mixed / (2 * CHORD_TOLERANCE)))
    count = 4 * steps
    if 2 * count * (tiers + 1) > face_limit:
        raise ValueError(
            'Loft exceeds the solid triangle budget at 0.025 mm tolerance. Reduce its section size or taper.'
        )
    rings = []
    for section in (base, top):
        rings.append(
            np.array(
                [
                    section.point(i, (i - 1.5 + j / steps) * math.pi / 2)
                    for i in range(4)
                    for j in range(steps)
                ]
            )
        )
    v = np.linspace(0.0, 1.0, tiers + 1)[:, None, None]
    vertices = ((1 - v) * rings[0] + v * rings[1]).reshape(-1, 3)
    base_center = len(vertices)
    vertices = np.vstack((vertices, base.center, top.center))
    edge = np.arange(count)
    after = np.roll(edge, -1)
    start = (np.arange(tiers)[:, None] * count + edge).ravel()
    end = (np.arange(tiers)[:, None] * count + after).ravel()
    faces = np.vstack(
        (
            np.column_stack((start, end, end + count)),
            np.column_stack((start, end + count, start + count)),
            np.column_stack((np.full(count, base_center), after, edge)),
            np.column_stack((np.full(count, base_center + 1), edge + tiers * count, after + tiers * count)),
        )
    )
    return kernel.Manifold(kernel.Mesh64(vertices, faces.astype(np.uint64)))


def loft_shape(feature, dimensions):
    """Exact quarter-section rational Bézier patches with a common rail correspondence."""
    from OCP.BRepBuilderAPI import (
        BRepBuilderAPI_MakeEdge,
        BRepBuilderAPI_MakeFace,
        BRepBuilderAPI_MakeSolid,
        BRepBuilderAPI_MakeWire,
        BRepBuilderAPI_Sewing,
    )
    from OCP.BRepCheck import BRepCheck_Analyzer
    from OCP.BRepLib import BRepLib
    from OCP.Geom import Geom_BezierCurve, Geom_BezierSurface
    from OCP.gp import gp_Pnt
    from OCP.TColgp import TColgp_Array1OfPnt, TColgp_Array2OfPnt
    from OCP.TColStd import TColStd_Array1OfReal, TColStd_Array2OfReal
    from OCP.TopAbs import TopAbs_SHELL
    from OCP.TopoDS import TopoDS

    profiles = sections(feature, dimensions)
    caps = [BRepBuilderAPI_MakeWire(), BRepBuilderAPI_MakeWire()]
    sewing = BRepBuilderAPI_Sewing(1e-7)
    for side in range(4):
        poles, weights = TColgp_Array2OfPnt(1, 3, 1, 2), TColStd_Array2OfReal(1, 3, 1, 2)
        for j, profile in enumerate(profiles, 1):
            row, values = TColgp_Array1OfPnt(1, 3), TColStd_Array1OfReal(1, 3)
            for i, (coords, w) in enumerate(zip(profile.poles(side), (1.0, WEIGHT, 1.0)), 1):
                pt = gp_Pnt(*coords)
                poles.SetValue(i, j, pt)
                weights.SetValue(i, j, w)
                row.SetValue(i, pt)
                values.SetValue(i, w)
            caps[j - 1].Add(BRepBuilderAPI_MakeEdge(Geom_BezierCurve(row, values)).Edge())
        sewing.Add(BRepBuilderAPI_MakeFace(Geom_BezierSurface(poles, weights), 1e-7).Face())
    for i, wire in enumerate(caps):
        cap = BRepBuilderAPI_MakeFace(wire.Wire(), True).Face()
        sewing.Add(cap.Reversed() if i == 0 else cap)
    sewing.Perform()
    shell = sewing.SewedShape()
    if shell.IsNull() or shell.ShapeType() != TopAbs_SHELL or sewing.NbFreeEdges():
        raise ValueError('The loft sections could not form a closed STEP shell')
    solid = BRepBuilderAPI_MakeSolid(TopoDS.Shell_s(shell)).Solid()
    if not BRepLib.OrientClosedSolid_s(solid) or not BRepCheck_Analyzer(solid).IsValid():
        raise ValueError('The exact loft could not form a valid STEP solid')
    return solid
