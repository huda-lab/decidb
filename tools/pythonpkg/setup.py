#!/usr/bin/env python
# -*- coding: utf-8 -*-
import ctypes
import os
import platform
import sys
import traceback
from functools import lru_cache
from glob import glob
from os.path import exists
from typing import TextIO

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext as _build_ext


class CompilerLauncherMixin:
    """Add "compiler launchers" to distutils.

    We use this to be able to run the build using "ccache".

    A compiler launcher is a program that is invoked instead of invoking the
    compiler directly. It is passed the full compiler invocation command line.

    A similar feature exists in CMake, see
    https://cmake.org/cmake/help/latest/prop_tgt/LANG_COMPILER_LAUNCHER.html.
    """

    __is_set_up = False

    def build_extensions(self):
        # Integrate into "build_ext"
        self.__setup()
        super().build_extensions()

    def build_libraries(self):
        # Integrate into "build_clib"
        self.__setup()
        super().build_libraries()

    def __setup(self):
        if self.__is_set_up:
            return
        self.__is_set_up = True

        # On Windows, force the 64-bit hosted toolchain (HostX64) to avoid
        # the 32-bit linker (HostX86) running out of address space on large builds
        if platform.system() == 'Windows':
            original_spawn_for_linker = self.compiler.spawn

            def spawn_with_x64_linker(cmd, **kwargs):
                # Redirect HostX86 to HostX64 for 64-bit linker
                if cmd and 'HostX86' in cmd[0]:
                    x64_path = cmd[0].replace('HostX86', 'HostX64')
                    if os.path.exists(x64_path):
                        cmd = [x64_path] + cmd[1:]

                # Use a response file for long link commands to stay under
                # the 32,767-character CreateProcess limit on Windows.
                # With ~572 object files the linker command easily exceeds this.
                if cmd and os.path.basename(cmd[0]).lower().startswith('link'):
                    cmd_length = sum(len(arg) + 3 for arg in cmd)
                    if cmd_length > 30000:
                        import tempfile
                        rsp_fd, rsp_path = tempfile.mkstemp(suffix='.rsp')
                        try:
                            with os.fdopen(rsp_fd, 'w') as rsp:
                                for arg in cmd[1:]:
                                    rsp.write(f'"{arg}"\n')
                            return original_spawn_for_linker(
                                [cmd[0], f'@{rsp_path}'], **kwargs
                            )
                        finally:
                            try:
                                os.unlink(rsp_path)
                            except OSError:
                                pass

                return original_spawn_for_linker(cmd, **kwargs)

            self.compiler.spawn = spawn_with_x64_linker

        compiler_launcher = os.getenv("DISTUTILS_C_COMPILER_LAUNCHER")
        if compiler_launcher:

            def spawn_with_compiler_launcher(cmd, **kwargs):
                if platform.system() == 'Windows' and len(' '.join(cmd)) > 32766:
                    raise Exception("command too long: " + ' '.join(cmd))

                exclude_programs = ("link.exe",)
                if not cmd[0].endswith(exclude_programs):
                    cmd = [compiler_launcher] + cmd
                try:
                    return original_spawn(cmd, **kwargs)
                except Exception:
                    traceback.print_exc()
                    raise

            original_spawn = self.compiler.spawn
            self.compiler.spawn = spawn_with_compiler_launcher

            # for some reason, the copy of distutils included with setuptools  (instead of the one that was previously
            # part of the stdlib) pushes the link call over the maximum command line length on windows.
            # We can fix this by using special windows short paths, of the form C:\Program ~1\...

            def link_with_short_paths(
                target_desc,
                objects,
                *args,
                **kwargs,
            ):
                return original_link(target_desc, [get_short_path(x) for x in objects], *args, **kwargs)

            original_link = self.compiler.link
            self.compiler.link = link_with_short_paths


def get_short_path(long_name: str) -> str:
    """
    Gets the short path name of a given long path.
    http://stackoverflow.com/a/23598461/200291
    """

    @lru_cache()
    def get_short_path_name_w():
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        _GetShortPathNameW = kernel32.GetShortPathNameW
        _GetShortPathNameW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
        _GetShortPathNameW.restype = wintypes.DWORD
        return _GetShortPathNameW

    if platform.system() != 'Windows':
        return long_name

    assert exists(long_name), long_name
    gspn = get_short_path_name_w()

    output_buf_size = 0
    while True:
        output_buf = ctypes.create_unicode_buffer(output_buf_size)
        needed = gspn(long_name, output_buf, output_buf_size)
        if output_buf_size >= needed:
            return output_buf.value or long_name
        else:
            output_buf_size = needed


class build_ext(CompilerLauncherMixin, _build_ext):
    def build_extension(self, ext):
        # Before building this extension, wrap the compiler's compile method
        # to handle C and C++ files differently
        original_compiler_compile = None
        if hasattr(self.compiler, 'compile'):
            original_compiler_compile = self.compiler.compile

            def custom_compile(sources, output_dir=None, macros=None, include_dirs=None,
                              debug=0, extra_preargs=None, extra_postargs=None, depends=None):
                # Separate C and C++ sources
                c_sources = [s for s in sources if s.endswith('.c')]
                cxx_sources = [s for s in sources if not s.endswith('.c')]

                objects = []

                # Compile C++ files with original flags
                if cxx_sources:
                    objects.extend(original_compiler_compile(
                        cxx_sources, output_dir, macros, include_dirs,
                        debug, extra_preargs, extra_postargs, depends
                    ))

                # Compile C files with filtered flags (on non-Windows)
                if c_sources:
                    if os.name != 'nt':
                        # Filter out C++-specific flags for C compilation
                        c_postargs = [arg for arg in (extra_postargs or [])
                                     if arg not in ['-std=c++17', '-stdlib=libc++']]
                        # Add minimal C flags
                        c_postargs.insert(0, '-g0')
                        if platform.system() == 'Darwin':
                            c_postargs.append('-mmacosx-version-min=10.13')

                        objects.extend(original_compiler_compile(
                            c_sources, output_dir, macros, include_dirs,
                            debug, extra_preargs, c_postargs, depends
                        ))
                    else:
                        # Windows: filter C++-specific flags for C files
                        c_postargs = [arg for arg in (extra_postargs or [])
                                     if arg not in ['/std:c++17']]
                        c_postargs.append('/std:c17')
                        objects.extend(original_compiler_compile(
                            c_sources, output_dir, macros, include_dirs,
                            debug, extra_preargs, c_postargs, depends
                        ))

                return objects

            # Temporarily replace the compile method
            self.compiler.compile = custom_compile

        try:
            # Build the extension with our custom compile method
            super().build_extension(ext)
        finally:
            # Restore the original compile method
            if original_compiler_compile:
                self.compiler.compile = original_compiler_compile


lib_name = 'decidb'

extensions = ['core_functions', 'parquet', 'tpch', 'icu', 'json']

is_android = hasattr(sys, 'getandroidapilevel')
is_pyodide = 'PYODIDE' in os.environ
no_source_wheel = is_pyodide
use_jemalloc = (
    not is_android and not is_pyodide and platform.system() == 'Linux' and platform.architecture()[0] == '64bit'
)

if use_jemalloc:
    extensions.append('jemalloc')

unity_build = 0
if 'DUCKDB_BUILD_UNITY' in os.environ:
    unity_build = 16

try:
    import pybind11
except ImportError:
    raise Exception(
        'pybind11 could not be imported. This usually means you\'re calling setup.py directly, or using a version of pip that doesn\'t support PEP517'
    ) from None

# speed up compilation with: -j = cpu_number() on non Windows machines
if os.name != 'nt' and os.environ.get('DUCKDB_DISABLE_PARALLEL_COMPILE', '') != '1':
    from pybind11.setup_helpers import ParallelCompile

    ParallelCompile().install()


def open_utf8(fpath, flags):
    import sys

    if sys.version_info[0] < 3:
        return open(fpath, flags)
    else:
        return open(fpath, flags, encoding="utf8")


# make sure we are in the right directory
os.chdir(os.path.dirname(os.path.realpath(__file__)))

if os.name == 'nt':
    # windows:
    toolchain_args = ['/std:c++17', '/wd4244', '/wd4267', '/wd4200', '/wd26451', '/wd26495', '/D_CRT_SECURE_NO_WARNINGS', '/DNOMINMAX', '/utf-8']
else:
    # macos/linux
    toolchain_args = ['-std=c++17', '-g0']
    if 'DUCKDEBUG' in os.environ:
        toolchain_args = ['-std=c++17', '-Wall', '-O0', '-g']
if 'DUCKDB_INSTALL_USER' in os.environ and 'install' in sys.argv:
    sys.argv.append('--user')

existing_duckdb_dir = ''
libraries = []
if 'DUCKDB_BINARY_DIR' in os.environ:
    existing_duckdb_dir = os.environ['DUCKDB_BINARY_DIR']
if 'DUCKDB_COMPILE_FLAGS' in os.environ:
    toolchain_args = ['-std=c++17'] + os.environ['DUCKDB_COMPILE_FLAGS'].split()
if 'DUCKDB_LIBS' in os.environ:
    libraries = os.environ['DUCKDB_LIBS'].split(' ')

define_macros = [('DUCKDB_PYTHON_LIB_NAME', '_decidb')]

custom_platform = os.environ.get('DUCKDB_CUSTOM_PLATFORM')
if custom_platform is not None:
    define_macros.append(('DUCKDB_CUSTOM_PLATFORM', custom_platform))

if platform.system() == 'Darwin':
    toolchain_args.extend(['-stdlib=libc++', '-mmacosx-version-min=10.13'])
    # Add OpenMP support on macOS (requires: brew install libomp)
    # Check Homebrew locations: direct prefix, keg-only libomp, ARM and Intel
    omp_search_dirs = [
        '/opt/homebrew/opt/libomp',   # ARM keg-only (most common)
        '/usr/local/opt/libomp',      # Intel keg-only
        '/opt/homebrew',              # ARM symlinked
        '/usr/local',                 # Intel symlinked
    ]
    for omp_dir in omp_search_dirs:
        omp_include = os.path.join(omp_dir, 'include')
        omp_lib = os.path.join(omp_dir, 'lib')
        if os.path.exists(os.path.join(omp_include, 'omp.h')):
            toolchain_args.extend(['-Xpreprocessor', '-fopenmp', f'-I{omp_include}'])
            # Store for later use in linker_args (libraries list is processed separately)
            os.environ['DUCKDB_MACOS_OMP_LIB'] = omp_lib
            break

if platform.system() == 'Windows':
    define_macros.extend([('DUCKDB_BUILD_LIBRARY', None), ('WIN32', None)])

if is_pyodide:
    # show more useful error messages in the browser
    define_macros.append(('PYBIND11_DETAILED_ERROR_MESSAGES', None))

if 'BUILD_HTTPFS' in os.environ:
    libraries += ['crypto', 'ssl']
    extensions += ['httpfs']

for ext in extensions:
    define_macros.append(('DUCKDB_EXTENSION_{}_LINKED'.format(ext.upper()), None))

if not is_pyodide:
    # currently pyodide environment is not compatible with dynamic extension loading
    define_macros.extend([('DUCKDB_EXTENSION_AUTOLOAD_DEFAULT', '1'), ('DUCKDB_EXTENSION_AUTOINSTALL_DEFAULT', '1')])

# Set up linker arguments - only include flags valid for linking
if platform.system() == 'Windows':
    # Windows: linker doesn't need compile flags, only libraries
    linker_args = ['rstrtmgr.lib', 'bcrypt.lib']
elif platform.system() == 'Darwin':
    # macOS: linker needs stdlib and version min, but not std or optimization flags
    linker_args = ['-stdlib=libc++', '-mmacosx-version-min=10.13']
    # Add OpenMP linking if we found it earlier
    if 'DUCKDB_MACOS_OMP_LIB' in os.environ:
        omp_lib = os.environ['DUCKDB_MACOS_OMP_LIB']
        linker_args.extend([f'-L{omp_lib}', '-lomp'])
else:
    # Linux: no special linker flags needed
    linker_args = []

short_paths = False
if platform.system() == 'Windows':
    short_paths = True

extra_files = []
header_files = []


def list_source_files(directory):
    sources = glob('src/**/*.cpp', recursive=True)
    return sources


script_path = os.path.dirname(os.path.abspath(__file__))
main_include_path = os.path.join(script_path, 'src', 'include')
main_source_path = os.path.join(script_path, 'src')
main_source_files = ['duckdb_python.cpp'] + list_source_files(main_source_path)

include_directories = [main_include_path, pybind11.get_include(False), pybind11.get_include(True)]
if 'BUILD_HTTPFS' in os.environ and 'OPENSSL_ROOT_DIR' in os.environ:
    include_directories += [os.path.join(os.environ['OPENSSL_ROOT_DIR'], 'include')]


def exclude_extensions(f: TextIO):
    files = [x for x in f.read().split('\n') if len(x) > 0]
    if use_jemalloc:
        return files
    else:
        return [x for x in files if 'jemalloc' not in x]


if len(existing_duckdb_dir) == 0:
    # no existing library supplied: compile everything from source
    source_files = main_source_files

    # check if amalgamation exists
    if os.path.isfile(os.path.join(script_path, '..', '..', 'scripts', 'amalgamation.py')):
        # amalgamation exists: compiling from source directory
        # copy all source files to the current directory
        sys.path.append(os.path.join(script_path, '..', '..', 'scripts'))
        import package_build

        (source_list, include_list, original_sources) = package_build.build_package(
            os.path.join(script_path, "duckdb_build"), extensions, False, unity_build, "duckdb_build", short_paths
        )

        duckdb_sources = [
            os.path.sep.join(package_build.get_relative_path(script_path, x).split('/')) for x in source_list
        ]
        duckdb_sources.sort()

        original_sources = [os.path.join("duckdb_build", x) for x in original_sources]

        duckdb_includes = [os.path.join("duckdb_build", x) for x in include_list]
        duckdb_includes += ["duckdb_build"]

        # gather the include files
        import amalgamation

        header_files = amalgamation.list_includes_files(duckdb_includes)

        # write the source list, include list and git hash to separate files
        with open_utf8('sources.list', 'w+') as f:
            for source_file in duckdb_sources:
                f.write(source_file + "\n")

        with open_utf8('includes.list', 'w+') as f:
            for include_file in duckdb_includes:
                f.write(include_file + '\n')

        extra_files = ['sources.list', 'includes.list'] + original_sources
    else:
        # if amalgamation does not exist, we are in a package distribution
        # read the include files, source list and include files from the supplied lists
        with open_utf8('sources.list', 'r') as f:
            duckdb_sources = exclude_extensions(f)

        with open_utf8('includes.list', 'r') as f:
            duckdb_includes = exclude_extensions(f)

    source_files += duckdb_sources
    include_directories = duckdb_includes + include_directories

    libduckdb = Extension(
        lib_name + '._decidb',
        include_dirs=include_directories,
        sources=source_files,
        extra_compile_args=toolchain_args,
        extra_link_args=linker_args,
        libraries=libraries,
        language='c++',
        define_macros=define_macros,
    )
else:
    sys.path.append(os.path.join(script_path, '..', '..', 'scripts'))
    import package_build

    include_directories += [os.path.join('..', '..', include) for include in package_build.third_party_includes()]
    toolchain_args += ['-I' + x for x in package_build.includes(extensions)]

    result_libraries = package_build.get_libraries(existing_duckdb_dir, libraries, extensions)
    library_dirs = [x[0] for x in result_libraries if x[0] is not None]
    libnames = [x[1] for x in result_libraries if x[1] is not None]

    libduckdb = Extension(
        lib_name + '._decidb',
        include_dirs=include_directories,
        sources=main_source_files,
        extra_compile_args=toolchain_args,
        extra_link_args=linker_args,
        libraries=libnames,
        library_dirs=library_dirs,
        language='c++',
        define_macros=define_macros,
    )


# data files need to be formatted as [(directory, [files...]), (directory2, [files...])]
# no clue why the setup script can't do this automatically, but hey
def setup_data_files(data_files):
    directory_map = {}
    for data_file in data_files:
        normalized_fpath = os.path.sep.join(data_file.split('/'))
        splits = normalized_fpath.rsplit(os.path.sep, 1)
        if len(splits) == 1:
            # no directory specified
            directory = ""
            fname = normalized_fpath
        else:
            directory = splits[0]
            fname = splits[1]
        if directory not in directory_map:
            directory_map[directory] = []
        directory_map[directory].append(normalized_fpath)
    new_data_files = []
    for kv in directory_map.keys():
        new_data_files.append((kv, directory_map[kv]))
    return new_data_files


data_files = setup_data_files(extra_files + header_files)
if no_source_wheel:
    data_files = []

packages = [
    lib_name,
    'decidb.typing',
    'decidb.query_graph',
    'decidb.functional',
    'decidb.value',
    'decidb-stubs',
    'decidb-stubs.functional',
    'decidb-stubs.typing',
    'decidb-stubs.value',
    'decidb-stubs.value.constant',
    'adbc_driver_decidb',
]

spark_packages = [
    'decidb.experimental',
    'decidb.experimental.spark',
    'decidb.experimental.spark.sql',
    'decidb.experimental.spark.errors',
    'decidb.experimental.spark.errors.exceptions',
]

packages.extend(spark_packages)

# Read README.md with proper file handling
try:
    with open('README.md', encoding='utf-8') as f:
        long_description_text = f.read()
except FileNotFoundError:
    long_description_text = 'DecidB - Optimization-enabled SQL queries with the DECIDE clause'

setup(
    name=lib_name,
    description='DecidB - Optimization-enabled SQL queries with the DECIDE clause',
    keywords='DecidB SQL Optimization ILP Integer-Linear-Programming DECIDE Package-Queries',
    url="https://huda-lab.github.io/decidb",
    long_description=long_description_text,
    long_description_content_type='text/markdown',
    license='MIT',
    data_files=data_files,
    # NOTE: might need to be find_packages() ?
    packages=packages,
    include_package_data=True,
    python_requires='>=3.11.0',
    tests_require=['google-cloud-storage', 'mypy', 'pytest'],
    classifiers=[
        'Topic :: Database :: Database Engines/Servers',
        'Topic :: Scientific/Engineering :: Mathematics',
        'Intended Audience :: Developers',
        'Intended Audience :: Science/Research',
        'License :: OSI Approved :: MIT License',
    ],
    ext_modules=[libduckdb],
    maintainer="HUDA Lab",
    maintainer_email="huda-lab@nyu.edu",
    cmdclass={"build_ext": build_ext},
    project_urls={
        "Documentation": "https://huda-lab.github.io/decidb/documentation.html",
        "Source": "https://github.com/huda-lab/decidb",
        "Issues": "https://github.com/huda-lab/decidb/issues",
    },
)
