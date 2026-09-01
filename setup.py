from setuptools import setup, Extension
import pybind11

# Adjust include_dirs if your madras/dv1 headers live somewhere other than
# "../include" relative to this setup.py (i.e. sibling to this repo).
INCLUDE_DIRS = [pybind11.get_include(), "src/madras-trie/include"]
EXTRA_COMPILE_ARGS = ["-std=c++11", "-O3"]

ext_reader = Extension(
    "madras._madras_native",
    sources=["src/madras_pybind.cpp"],
    include_dirs=INCLUDE_DIRS,
    extra_compile_args=EXTRA_COMPILE_ARGS,
    language="c++",
)

# Builder (write/COPY TO) extension. Comment this out if madras_builder.hpp
# isn't available yet / doesn't compile in your tree -- the reader extension
# above is independent and will still build and work without it.
ext_builder = Extension(
    "madras._madras_builder_native",
    sources=["src/madras_builder_pybind.cpp"],
    include_dirs=INCLUDE_DIRS,
    extra_compile_args=EXTRA_COMPILE_ARGS,
    language="c++",
)

setup(
    name="madras",
    version="0.1.0",
    packages=["madras"],
    ext_modules=[ext_reader, ext_builder],
    install_requires=["numpy"],
    extras_require={
        "pandas": ["pandas"],
        "arrow": ["pyarrow"],
    },
    python_requires=">=3.7",
)
