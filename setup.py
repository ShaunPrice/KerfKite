"""Optional native relief kernel; a compiler is not required to install Studio."""
import os
import sys
from setuptools import Extension, setup, find_packages

setup(packages=find_packages(include=['studio', 'studio.*']), ext_modules=[] if os.environ.get("KERFKITE_PORTABLE_WHEEL") == "1" else [Extension(
    'studio._relief_native', ['studio/_relief_native.cpp'], language='c++',
    optional=True, extra_compile_args=['/O2'] if sys.platform == 'win32' else ['-O3', '-std=c++11'],
)])
