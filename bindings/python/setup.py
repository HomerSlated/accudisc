"""Kept only for ``cffi_modules``, which has no pyproject.toml equivalent.

Everything else — name, version, dependencies — lives in pyproject.toml.
"""

from setuptools import setup

setup(cffi_modules=["build_accudisc.py:ffibuilder"])
