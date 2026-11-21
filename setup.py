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
                # Offsets, pockets and fills, and the polygon kernel they are built on.
                'native/core/src/operations.cpp',
                'native/core/src/polygon/arrangement.cpp',
                'native/core/src/polygon/boolean.cpp',
                'native/core/src/polygon/clip.cpp',
                'native/core/src/polygon/measure.cpp',
                'native/core/src/polygon/offset.cpp',
            ],
            language='c++',
            include_dirs=['native/core/include'],
            optional=os.environ.get('KERFKITE_REQUIRE_NATIVE') != '1',
            extra_compile_args=['/O2', '/fp:precise', '/std:c++17', '/utf-8']
            if sys.platform == 'win32'
            else ['-O3', '-g0', '-ffp-contract=off', '-std=c++17'],
        )
    ],
)
