"""Optional native relief and stock kernels; a compiler is not required to install Studio."""

import os
from pathlib import Path
import sys
from setuptools import Extension, setup, find_packages

GENERATED = Path('build', 'generated')


def protocols_header():
    """The controller table compiled into the core's CAM, made as native/CMakeLists.txt makes it."""
    template = Path('native/core/src/protocols_data.hpp.in').read_text(encoding='utf-8')
    table = Path('studio/static/protocols.json').read_text(encoding='utf-8')
    header = template.replace('@KERFKITE_PROTOCOLS_JSON@', table)
    target = GENERATED / 'protocols_data.hpp'
    if not target.is_file() or target.read_text(encoding='utf-8') != header:
        GENERATED.mkdir(parents=True, exist_ok=True)
        target.write_text(header, encoding='utf-8', newline='\n')


if os.environ.get('KERFKITE_PORTABLE_WHEEL') != '1':
    protocols_header()

setup(
    packages=find_packages(include=['studio', 'studio.*']),
    ext_modules=[]
    if os.environ.get("KERFKITE_PORTABLE_WHEEL") == "1"
    else [
        Extension(
            'studio._relief_native',
            [
                'studio/_relief_native.cpp',
                'studio/_gcode_native.cpp',
                # The native core's levelling module and its Python entry point.
                'studio/_core_native.cpp',
                'native/core/src/json.cpp',
                # Where the core answers by its own rules; Studio's Python keeps the reference's (see below).
                'native/core/src/own_rules.cpp',
                'native/core/src/leveling.cpp',
                # G-code and rotary output: the planar generator, drilling, the drag knife, laser paths, ramp
                # and helix entries, holding tabs, the self-crossing test they ask of contours, the
                # controller table and the rotary post.
                'native/core/src/cam.cpp',
                'native/core/src/drilling.cpp',
                'native/core/src/knife.cpp',
                'native/core/src/laser.cpp',
                'native/core/src/protocols.cpp',
                # Relief finishing and the stock-removal preview.
                'native/core/src/relief.cpp',
                'native/core/src/rotary.cpp',
                'native/core/src/router_entry.cpp',
                'native/core/src/simple_line.cpp',
                'native/core/src/stock.cpp',
                'native/core/src/tabs.cpp',
                # Plans of stages for manual tool changes, and the fixture clearance check.
                'native/core/src/tool_changes.cpp',
                'native/core/src/fixture_collision.cpp',
                # STL height fields for reliefs.
                'native/core/src/stl_heightmap.cpp',
                'native/core/src/artwork_relief.cpp',
                # Foreign drawings and toolpaths: G-code, HP-GL and LinuxCNC INI import.
                'native/core/src/program_import.cpp',
                'native/core/src/dxf.cpp',
                # Vector PDF pages, with the core's own inflate for FlateDecode.
                'native/core/src/pdf_import.cpp',
                'native/core/src/inflate.cpp',
                # LightBurn bundles: the ZIP archive and its text members.
                'native/core/src/bundle_import.cpp',
                'native/core/src/python_text.cpp',
                # Editable Bézier and B-spline surfaces.
                'native/core/src/surface.cpp',
                # Circuit board import: Gerber layers and Excellon drill files.
                'native/core/src/excellon.cpp',
                'native/core/src/gerber.cpp',
                'native/core/src/pcb.cpp',
                # Offsets, pockets and fills, where a helix enters a pocket, V-carving, and the polygon kernel they
                # are built on.
                'native/core/src/operations.cpp',
                'native/core/src/pocket_entry.cpp',
                'native/core/src/vcarve.cpp',
                # Isolation toolpaths for circuit boards, on the same kernel.
                'native/core/src/isolation.cpp',
                'native/core/src/polygon/arrangement.cpp',
                'native/core/src/polygon/boolean.cpp',
                'native/core/src/polygon/clip.cpp',
                'native/core/src/polygon/distance.cpp',
                'native/core/src/polygon/measure.cpp',
                'native/core/src/polygon/offset.cpp',
                # The Delaunay triangulation and Voronoi diagram of points, which V-carving follows.
                'native/core/src/polygon/voronoi.cpp',
                # The polygon kernel's own command: the 2D drawings of Studio's script engine.
                'native/core/src/polygon/command.cpp',
                # The solid kernel behind Studio's 3D modelling, and its command.
                'native/core/src/solid/arrangement.cpp',
                'native/core/src/solid/boolean.cpp',
                'native/core/src/solid/exact.cpp',
                'native/core/src/solid/hull.cpp',
                'native/core/src/solid/mesh.cpp',
                'native/core/src/solid/planar.cpp',
                'native/core/src/solid/primitives.cpp',
                'native/core/src/solid/profile.cpp',
                'native/core/src/solid/query.cpp',
                'native/core/src/solid/section.cpp',
                'native/core/src/solid/simplify.cpp',
                'native/core/src/solid/stl.cpp',
                'native/core/src/solid_command.cpp',
                'native/core/src/solid_cad.cpp',
                'native/core/src/scad.cpp',
                'native/core/src/assembly.cpp',
                'native/core/src/gpu/opencl.cpp',
            ],
            language='c++',
            include_dirs=['native/core/include', str(GENERATED)],
            optional=os.environ.get('KERFKITE_REQUIRE_NATIVE') != '1',
            extra_compile_args=['/O2', '/fp:precise', '/std:c++17', '/utf-8']
            if sys.platform == 'win32'
            else ['-O3', '-g0', '-ffp-contract=off', '-std=c++17', '-pthread'],
            extra_link_args=[] if sys.platform == 'win32' else ['-pthread'],
            libraries=['dl'] if sys.platform.startswith('linux') else [],
        )
    ],
)
