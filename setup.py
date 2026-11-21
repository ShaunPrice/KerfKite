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
            ['studio/_relief_native.cpp', 'studio/_stock_native.cpp', 'studio/_gcode_native.cpp'],
            language='c++',
            include_dirs=['native/core/include'],
            optional=os.environ.get('KERFKITE_REQUIRE_NATIVE') != '1',
            extra_compile_args=['/O2', '/fp:precise']
            if sys.platform == 'win32'
            else ['-O3', '-g0', '-ffp-contract=off', '-std=c++11'],
        )
    ],
)
