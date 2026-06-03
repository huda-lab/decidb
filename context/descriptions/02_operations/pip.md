# Python Package (pip install)

## 1. Overview
DeciDB is distributed as a Python package named `decidb`. The package builds the entire C++ codebase (DuckDB core + DeciDB extensions + HiGHS solver) from source into a single compiled `.so` extension module, then wraps it with Python bindings via pybind11.

Key files:
- `tools/pythonpkg/setup.py` — Main build logic
- `tools/pythonpkg/pyproject.toml` — PEP 517 build config, CI/cibuildwheel settings
- `scripts/package_build.py` — Source/include gathering for the amalgamation build path
- `scripts/amalgamation.py` — Lists all core DuckDB sources and includes

## 2. Build Paths
`setup.py` has two distinct build paths depending on whether `DUCKDB_BINARY_DIR` is set:

### 2.1 From-Source Build (default, used by `pip install`)
When `DUCKDB_BINARY_DIR` is **not** set (the normal `pip install .` path):
1. `setup.py` detects that `scripts/amalgamation.py` exists (meaning we're in the repo, not a sdist).
2. Calls `package_build.build_package()` which:
   - Uses `amalgamation.list_sources()` to gather all core DuckDB `.cpp` files.
   - Uses `package_build.third_party_sources()` to gather third-party source directories (including `third_party/highs`).
   - Uses `package_build.third_party_includes()` to gather all include paths.
   - Copies everything into a `duckdb_build/` staging directory inside `tools/pythonpkg/`.
   - Generates unity build files where CMakeLists.txt uses `add_library_unity`.
3. All gathered sources are compiled into a single `decidb._decidb` extension module (the `.so`).

### 2.2 Pre-Built Binary Path (CMake `BUILD_PYTHON=1`)
When `DUCKDB_BINARY_DIR` **is** set:
1. Only the Python binding sources (`tools/pythonpkg/src/`) are compiled.
2. Links against the pre-built `duckdb_static` library and extension libraries found in the binary dir.
3. Third-party includes (including SymbolicC++, HiGHS) are added via `package_build.third_party_includes()`.

## 3. Bundled Extensions
Extensions are compiled directly into the `_decidb.so` binary ("baked in"). They require no network access or downloads at runtime.

The extensions list is defined in `setup.py`:
```python
extensions = ['core_functions', 'parquet', 'tpch', 'icu', 'json']
```

- **core_functions** — Built-in SQL functions
- **parquet** — Parquet file read/write
- **tpch** — TPC-H benchmark data generation
- **icu** — Timezone and locale support (required for `SET timezone=...` and timestamp-with-timezone types)
- **json** — JSON parsing and querying (required for `::JSON` casts and `read_json()`)
- **jemalloc** — Memory allocator (auto-added on 64-bit Linux only)

All extension source code lives locally in `extension/` (e.g., `extension/icu/`, `extension/json/`). Additional extensions available in the repo but not currently bundled: `tpcds`, `autocomplete`, `delta`, `demo_capi`.

The `duckdb_extension_config.cmake` file separately configures extensions for CMake-based builds (used by `BUILD_PYTHON=1`). Keep both in sync when adding/removing extensions.

**Important**: DeciDB does not have its own extension hosting server. Extensions that are not baked into the wheel cannot be auto-downloaded at runtime (DuckDB's `extensions.duckdb.org` does not host DeciDB builds). Always bundle any required extensions in `setup.py`.

## 4. HiGHS Integration in pip
HiGHS is compiled directly into the wheel — there is no external dependency or system library required.

- **Sources**: `package_build.third_party_sources()` includes `third_party/highs`, which causes all HiGHS `.cpp`, `.cc`, and `.c` files to be gathered recursively.
- **Includes**: `package_build.third_party_includes()` lists all HiGHS subdirectories (lines 42–62 in `scripts/package_build.py`): root, `highs/`, `extern/`, `extern/filereaderlp`, `extern/pdqsort`, and every HiGHS module directory (`interfaces`, `io`, `ipm`, `ipm/ipx`, `ipm/basiclu`, `lp_data`, `mip`, `model`, `parallel`, `pdlp`, `pdlp/cupdlp`, `presolve`, `qpsolver`, `simplex`, `test_kkt`, `util`).
- **CMake build**: `third_party/highs/CMakeLists.txt` builds `duckdb_highs` as a static library. `src/CMakeLists.txt` links both `duckdb` and `duckdb_static` against `duckdb_highs`.

## 5. Package Structure
The installed `decidb` package contains:
```
decidb/
├── __init__.py              # Re-exports everything from ._decidb
├── _decidb.cpython-*.so     # The compiled C++ extension (DuckDB + DeciDB + HiGHS)
├── functional/              # UDF type helpers
├── typing/                  # Type system wrappers
├── value/                   # Value type wrappers
├── query_graph/             # Query graph utilities
├── experimental/spark/      # Spark SQL compatibility layer
└── ...
```

The compiled `.so` is named `_decidb` (as `lib_name + '._decidb'` in setup.py, where `lib_name = 'decidb'`). All Python modules import from `decidb._decidb` (the `.so`) using `from ._decidb import ...`.

## 6. DuckDB-to-DeciDB Rename (Python Module Name)

Since DeciDB is a fork of DuckDB, the Python module was renamed from `duckdb` to `decidb`. This rename touches three layers:

### 6.1 Python Layer
- Package name in `setup.py`: `lib_name = 'decidb'`
- All Python source files under `tools/pythonpkg/decidb/` use `import decidb`
- The `decidb/experimental/spark/` subpackage uses `decidb.xxx` (not `duckdb.xxx`) for type annotations, function calls, etc.

### 6.2 C++ Layer (runtime Python imports)
The compiled C++ extension internally imports the Python module for certain operations (e.g., `Value` conversion, filesystem access). These import strings are defined in:
- `tools/pythonpkg/duckdb_python.cpp` — `m.attr("__package__") = "decidb"`
- `tools/pythonpkg/src/include/duckdb_python/import_cache/modules/duckdb_module.hpp` — Import cache items use `"decidb"` and `"decidb.filesystem"`

Note: The C++ namespace remains `namespace duckdb { ... }` — this is a code-level namespace and is independent of the Python module name.

### 6.3 Test Suite
- All test files under `tools/pythonpkg/tests/` use `import decidb` (not `import duckdb`)

## 7. How to Build Locally

### Prerequisites
- Python 3.11+
- `pybind11`, `setuptools`, `setuptools_scm` (installed automatically by PEP 517)

### Using a virtual environment
```bash
cd tools/pythonpkg
python3 -m venv venv
source venv/bin/activate
pip install .
```

A venv already exists at `tools/pythonpkg/venv/` (gitignored). To use it:
```bash
cd tools/pythonpkg
./venv/bin/pip install .
```

### Editable install (recommended for development)
Use `pip install -e .` to install in editable/development mode. This builds the `_decidb.cpython-*.so` directly into the `decidb/` source directory, so the local directory *is* the installed package — no shadowing issues:
```bash
cd tools/pythonpkg
source venv/bin/activate
pip install -e .
```

### Important: shadowing with non-editable installs
If you use `pip install .` (non-editable), **do not** run Python from within `tools/pythonpkg/`. The local `decidb/` source directory will shadow the installed package, causing `ModuleNotFoundError: No module named 'decidb._decidb'` because the source directory lacks the compiled `.so`. Run from any other directory:
```bash
cd /tmp
/path/to/venv/bin/python -c "import decidb; print(decidb.connect().sql('SELECT 42').fetchall())"
```

### Using CMake (alternative)
```bash
BUILD_PYTHON=1 make
# or
mkdir build && cd build
cmake .. -DBUILD_PYTHON=1
```

## 8. Key Internals

### Source Gathering (`package_build.py`)
- `third_party_includes()` — Returns all third-party include paths needed for compilation. Any new third-party dependency must be added here for pip builds to work.
- `third_party_sources()` — Returns directories containing third-party `.cpp/.c/.cc` source files. These are recursively scanned for source files during the amalgamation.
- `build_package()` — Orchestrates the full amalgamation: copies sources, generates unity builds, patches version info.

### Unity Builds
The amalgamation path detects `add_library_unity` in CMakeLists.txt files and generates unity build `.cpp` files that `#include` multiple source files. This speeds up compilation significantly.

### Version Detection
Version is derived from `git describe --tags --long` via `setuptools_scm` (configured in `pyproject.toml` with `root = "../.."`). The version format is `0.1.devN` where N is the number of commits since the last tag.

## 9. CI / Wheel Building
The GitHub Actions workflow (`.github/workflows/python-wheels.yml`) uses `cibuildwheel` to build cross-platform wheels. It is triggered by `workflow_dispatch` (manual, requires a version input) or on GitHub release publish.

### Build Job (`build_wheels`)
- **Platforms**: ubuntu-22.04 (Linux x86_64), macos-14 (macOS arm64), windows-2022 (AMD64)
- **Python versions**: cp311, cp312, cp313
- **Smoke test**: After building, runs `python -c "import decidb; print(decidb.__version__)"`
- **Artifacts**: Wheels are uploaded as GitHub Actions artifacts (7-day retention). Each OS produces 3 `.whl` files (one per Python version). The sdist is built separately on Linux.

### Test Job (`test_wheels`)
After wheels are built, a separate `test_wheels` job downloads and tests them:
1. Downloads the wheel artifact for the target OS
2. Installs the wheel plus test dependencies (`pytest`, `numpy`, `pandas<3.0`, `pyarrow`, `pytz`, `typing_extensions`)
3. Runs smoke tests (import + basic SQL query)
4. Runs the fast test suite: `python -m pytest fast --verbose --continue-on-collection-errors`

**Pandas version constraint**: Tests require `pandas<3.0` because pandas 3.0 changed its default string dtype to `StringDtype` (pyarrow-backed), which the C++ binding's pandas scanner does not yet recognize (`Data type 'str' not recognized`).

### Publish Job (`publish_pypi`)
- Only runs on release events or when `publish_to_pypi` is set in manual dispatch
- Depends on both `build_wheels` and `test_wheels` passing
- Uses trusted publishing (`id-token: write`)

`pyproject.toml` also configures `cibuildwheel` defaults (used when building locally with `cibuildwheel`):
- Runs `pytest` test suite after building.
- musllinux, i686, and aarch64 run a reduced "fast" test suite.

### Prerequisites for CI
- **HiGHS must be committed**: `third_party/highs/` must be tracked in git (not just present locally). The workflow checks out the repo with `submodules: recursive`, but HiGHS is not a submodule — it must be committed directly.
- **Version tags**: `setuptools_scm` derives version from git tags. Without tags, version will be `0.1.devN`. The workflow sets `OVERRIDE_GIT_DESCRIBE` but this only affects CMake builds, not the pip/setuptools_scm path.

## 10. Testing the Python Package

### Test Location
All Python tests live in `tools/pythonpkg/tests/`:
```
tests/
├── conftest.py              # Fixtures, markers, extension skip logic
├── pytest.ini               # Warning filters
├── fast/                    # Fast test suite (~3100 tests)
│   ├── api/                 # DBAPI, connection, config tests
│   ├── arrow/               # PyArrow integration tests
│   ├── pandas/              # Pandas integration tests
│   ├── spark/               # Spark SQL compatibility tests
│   ├── types/               # Type conversion tests
│   ├── udf/                 # User-defined function tests
│   ├── relational_api/      # Relational API tests
│   └── ...                  # Various feature tests
├── slow/                    # Slow tests (large data)
├── extensions/              # Extension-specific tests
└── spark_namespace/         # Spark compatibility helpers
```

### Running Tests Locally
```bash
cd tools/pythonpkg
source venv/bin/activate
pip install -e .
pip install pytest numpy 'pandas<3.0' pyarrow pytz typing_extensions

# Run the fast suite
python -m pytest tests/fast --verbose

# Run a specific test file
python -m pytest tests/fast/api/test_config.py --verbose
```

### Testing Against a Built Wheel
To test a pre-built wheel artifact (e.g., from CI):
```bash
python3 -m venv /path/to/test-env
source /path/to/test-env/bin/activate
pip install /path/to/decidb-*.whl
pip install pytest numpy 'pandas<3.0' pyarrow pytz typing_extensions

cd /path/to/decidb-repo
python -m pytest tools/pythonpkg/tests/fast --verbose
```

### Skipped Tests
- **Optional dependency tests** (~315 tests) — Tests for `torch`, `tensorflow`, `polars`, `pyspark`, `adbc_driver_manager` are skipped if those packages aren't installed.

### Test Result Expectations
With all extensions bundled and `pandas<3.0`:
- **~2859 passed**
- **~315 skipped** (optional dependencies not installed)
- **0 failed**

### Key Test Fixtures (`conftest.py`)
- `duckdb_cursor` — Creates a fresh in-memory DeciDB connection per test
- `require` — Loads a DuckDB extension from build directory (for extension tests)
- `spark` — Creates a Spark-compatible session (requires spark_namespace helpers)
- `integers`, `timestamps` — Pre-populated table fixtures
- `NumpyPandas`, `ArrowPandas` — Pandas DataFrames with different backends for parametrized tests
