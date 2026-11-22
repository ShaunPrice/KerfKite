"""Optional native relief and stock kernels; a compiler is not required to install Studio."""

import os
import sys
from setuptools import Extension, setup, find_packages

setup(
    packages=find_packages(include=['studio', 'studio.*']),
    ext_modules=[]
    if os.environ.get("KERFKITE_PORTABLE_WHEEL") == "1"
    else [
        Extension(
            'studio._relief_native',
            [
                'studio/_relief_native.cpp',
                'studio/_stock_native.cpp',
                'studio/_gcode_native.cpp',
                # The native core's levelling module and its Python entry point.
                'studio/_core_native.cpp',
                'native/core/src/json.cpp',
                'native/core/src/leveling.cpp',
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
                'native/core/src/gpu/opencl.cpp',
            ],
            language='c++',
            include_dirs=['native/core/include'],
            optional=os.environ.get('KERFKITE_REQUIRE_NATIVE') != '1',
            extra_compile_args=['/O2', '/fp:precise', '/std:c++17', '/utf-8']
            if sys.platform == 'win32'
            else ['-O3', '-g0', '-ffp-contract=off', '-std=c++17', '-pthread'],
            extra_link_args=[] if sys.platform == 'win32' else ['-pthread'],
            libraries=['dl'] if sys.platform.startswith('linux') else [],
        )
    ],
)
