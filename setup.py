"""Optional native relief kernel; a compiler is not required to install Studio."""
import sys
from setuptools import Extension, setup, find_packages

setup(packages=find_packages(include=['studio', 'studio.*']), ext_modules=[Extension(
    'studio._relief_native', ['studio/_relief_native.cpp'], language='c++',
    optional=True, extra_compile_args=['/O2'] if sys.platform == 'win32' else ['-O3', '-std=c++11'],
)])
