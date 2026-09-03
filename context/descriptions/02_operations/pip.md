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
3. Third-party includes (HiGHS among them) are added via `package_build.third_party_includes()`.

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
Local builds derive their version from Git through `setuptools_scm` (configured in
`pyproject.toml` with `root = "../.."`). Tagged builds use the tag; development builds
use the next patch version plus a `.devN` suffix. The release workflow sets
`SETUPTOOLS_SCM_PRETEND_VERSION` from its version input or release tag for both wheels
and the source distribution, so every artifact in one release carries the same version.

## 9. CI / Wheel Building
The GitHub Actions workflow (`.github/workflows/python-wheels.yml`) uses `cibuildwheel` to build cross-platform wheels. It is triggered by `workflow_dispatch` (manual, requires a version input) or on GitHub release publish.

### Build Job (`build_wheels`)
- **Platforms**: ubuntu-22.04 (Linux x86_64), macos-14 (macOS arm64), windows-2022 (AMD64)
- **Python versions**: cp311, cp312, cp313
- **Smoke test**: Each built wheel imports `decidb`, runs `SELECT 42`, forces the
  bundled HiGHS backend through a typed `DECIDE x(BOOL)` query, checks its result,
  and prints the package version.
- **Artifacts**: Wheels are uploaded as GitHub Actions artifacts (7-day retention). Each OS produces 3 `.whl` files (one per Python version). The sdist is built separately on Linux.

There is no separate wheel-test job in the release workflow. The complete package test
suite remains available through the default `cibuildwheel` configuration in
`pyproject.toml`; the release workflow deliberately overrides it with the bounded smoke
test above. What CI does not cover is closed by the dry run below.

### Pre-publication validation (run before every production publish)

CI builds the sdist and greps its file list, but never installs or executes it. Since
`pip` falls back to the sdist on any interpreter without a matching wheel, that path
reaches real users untested unless it is checked by hand. Do this while the GitHub
release is still a draft — publishing is what triggers the PyPI upload, and **both PyPI
and TestPyPI are write-once**: a version number, once uploaded, can never be reused even
after deleting the files.

1. Dispatch the wheels workflow against TestPyPI with a throwaway version. Use one that
   collides with no plausible real release (`v0.99.0`), because the string is burned on
   success. `CMakeLists.txt` accepts only `vX.Y.Z` or `vX.Y.Z-N-gHASH`, so a `.post1`
   retry is not available.

   ```bash
   gh workflow run python-wheels.yml --repo huda-lab/decidb --ref master \
     -f version=v0.99.0 -f publish_target=testpypi
   ```

2. Install the wheel into a clean environment and solve one query of each decision type.
   `--only-binary` keeps a silent source build from masking a missing wheel. The
   `--extra-index-url` is required because TestPyPI carries no numpy or pandas.

   ```bash
   pip install --only-binary decidb \
     --index-url https://test.pypi.org/simple/ \
     --extra-index-url https://pypi.org/simple/ decidb==0.99.0
   ```

3. Install the sdist. Fetch the tarball by URL rather than with `pip download
   --no-binary`, which needs `setuptools` from an index TestPyPI does not carry and fails
   with a misleading `from versions: none`. The compile takes roughly 30 minutes and
   saturates every core.

   ```bash
   pip install <sdist-url-from-the-json-api>
   ```

The README is the PyPI long description and is baked into the artifacts at build time, so
its rendering is checked locally rather than by uploading — `readme_renderer` is the same
library the index uses:

```bash
uv run --with "readme_renderer[md]" python -c \
  "import readme_renderer.markdown as m; print(bool(m.render(open('tools/pythonpkg/README.md').read(), stream=None)))"
```

At v0.2.0 this path verified: versioned filenames on all 10 artifacts, a clean-environment
sdist install that compiled in 27 minutes and solved a typed DECIDE query through bundled
HiGHS, and green per-wheel smokes on Linux x86-64, macOS arm64 and Windows AMD64 across
Python 3.11–3.13.

**A draft release pins its own commit.** The release event builds the tag, and the tag is
created at the draft's `target_commitish`. Any commit made after the draft was built —
including a documentation-only one — requires retargeting, or the published package is
built from the stale commit:

```bash
gh release edit v0.2.0 --repo huda-lab/decidb --target $(git rev-parse HEAD)
```

The target must be the full 40-character SHA; an abbreviated one returns a bare
`422 Validation Failed`. Release notes are frozen text generated when the CLI build ran,
so edits to the notes template after that build do not reach the draft — regenerate them
with `gh release edit --notes-file` before publishing.

### Publish Job (`publish_pypi`)
- Only runs on release events or when `publish_target=pypi` is selected in a manual dispatch
- Depends on both `build_wheels` and `build_sdist` passing
- Uses trusted publishing (`id-token: write`)

`pyproject.toml` also configures `cibuildwheel` defaults (used when building locally with `cibuildwheel`):
- Runs `pytest` test suite after building.
- musllinux, i686, and aarch64 run a reduced "fast" test suite.

### Prerequisites for CI
- **HiGHS must be committed**: `third_party/highs/` must be tracked in git (not just present locally). The workflow checks out the repo with `submodules: recursive`, but HiGHS is not a submodule — it must be committed directly.
- **Version input/tag**: the workflow passes it to `setuptools_scm` as
  `SETUPTOOLS_SCM_PRETEND_VERSION` for wheels and the sdist. Local untagged builds still
  derive a development version from Git.

## 10. Testing the Python Package

### Test Location
All Python tests live in `tools/pythonpkg/tests/`:
```
tests/
├── conftest.py              # Fixtures, markers, extension skip logic
├── pytest.ini               # Warning filters
├── fast/                    # Fast test suite
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
- **Optional dependency tests** — Tests for `torch`, `tensorflow`, `polars`, `pyspark`, `adbc_driver_manager` are skipped if those packages aren't installed.

### Test Result Expectations

With all extensions bundled and `pandas<3.0`, the required tests pass. Tests for
optional dependencies may skip when those packages are not installed; treat any
failure or unexpected skip as a regression rather than relying on a fixed count.

### Key Test Fixtures (`conftest.py`)
- `duckdb_cursor` — Creates a fresh in-memory DeciDB connection per test
- `require` — Loads a DuckDB extension from build directory (for extension tests)
- `spark` — Creates a Spark-compatible session (requires spark_namespace helpers)
- `integers`, `timestamps` — Pre-populated table fixtures
- `NumpyPandas`, `ArrowPandas` — Pandas DataFrames with different backends for parametrized tests
