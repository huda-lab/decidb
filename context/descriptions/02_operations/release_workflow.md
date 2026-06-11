# GitHub Actions Release Workflow

`DecidBRelease.yml` builds release CLI binaries for Linux, macOS, and Windows and creates a draft GitHub release. It is **manually triggered** (`workflow_dispatch`).

A separate workflow, `python-wheels.yml`, builds the Python wheels: it runs on `workflow_dispatch` (with a TestPyPI dry-run target) and auto-publishes to PyPI + attaches wheels to the release when the GitHub release is **published**. See `pip.md` for the package itself.

## Triggering

Actions tab → **DecidB Release Build** → Run workflow:

| Input | Description | Default |
|-------|-------------|---------|
| `version` | Release version tag (e.g., `v0.1.0-beta`) | Required |
| `platforms` | Comma-separated: `linux`, `macos`, `windows` | `linux` |
| `create_release` | Whether to create a draft GitHub release | `true` |

## Job Structure

Three parallel platform builds (each gated on `platforms` containing its name) feed a final `create-release` job:

```
linux-x64 (ubuntu-latest)   macos-universal (macos-14)   windows-x64 (windows-2022)
            └───────────────────────┼───────────────────────┘
                                    ▼
                       create-release (ubuntu-latest)
```

Each platform job: checkout (full history) → setup Python 3.12 → build → verify (`decidb -c "PRAGMA platform;"`, `decidb --version`) → package → upload artifact (7-day retention).

**Only CLI executables are packaged** — library packages are intentionally skipped ("build from source if needed"):

| Platform | Artifacts |
|----------|-----------|
| Linux | `decidb_cli-linux-amd64.zip`, `decidb_cli-linux-amd64.gz` |
| macOS | `decidb_cli-osx-universal.zip`, `decidb_cli-osx-universal.gz` |
| Windows | `decidb_cli-windows-amd64.zip` |

### Per-platform build notes

- **Linux** (`ubuntu-latest`): builds inside the `manylinux2014_x86_64` Docker image for glibc 2.17+ compatibility (CentOS 7+, Ubuntu 18.04+). Installs `perl-IPC-Cmd` for the OpenSSL build, then runs `make`.
- **macOS** (`macos-14`): installs Ninja + `libomp` (OpenMP, required by HiGHS), uses ccache. Builds a universal binary (`OSX_BUILD_UNIVERSAL=1`) with libomp include paths passed via `EXTRA_CMAKE_VARIABLES` (`-I/opt/homebrew/opt/libomp/include`; on Intel Macs the prefix is `/usr/local`). Verified with `file build/release/decidb`.
- **Windows** (`windows-2022`): installs Ninja via choco, uses ccache, builds under MSVC vars (`vcvars64.bat`) with `cmake -G Ninja -B build/release -DCMAKE_BUILD_TYPE=Release -DDISABLE_UNITY=1` then `ninja -C build/release`. Output: `build/release/decidb.exe`.

### create-release job

Runs `if: always() && inputs.create_release`, downloads all artifacts, generates release notes (download table, quick start, system requirements, unsigned-binary warnings), then:

```bash
gh release create ${version} \
  --repo ... --target ${github.sha} \
  --title "DecidB ${version}" --notes-file release_notes.md \
  --draft artifacts/**/*.zip artifacts/**/*.gz
```

- `--target ${{ github.sha }}` pins the tag to the **exact commit that was built**.
- No `|| true`: if the tag/release already exists the job **fails loudly** rather than leaving a stale release in place.
- The release is a **draft** — review and publish manually. Publishing it triggers `python-wheels.yml` to push to PyPI and attach wheels.

## Key Environment Variables

| Variable | Purpose |
|----------|---------|
| `OVERRIDE_GIT_DESCRIBE` | Sets the version string embedded in binaries (from the `version` input) |
| `GH_TOKEN` | GitHub token for creating releases |
| `OSX_BUILD_UNIVERSAL` | Enables universal binary build on macOS |
| `EXTRA_CMAKE_VARIABLES` | Extra CMake flags (used for macOS libomp paths) |
| `GEN=ninja` | Use Ninja instead of Make (Makefile option) |
| `CMAKE_BUILD_PARALLEL_LEVEL` | Number of parallel build jobs |

## How the Build System Works

The workflow's `make` runs the root Makefile's `release` target, equivalent to:

```bash
mkdir -p build/release && cd build/release
cmake -DCMAKE_BUILD_TYPE=Release ../..
cmake --build . --config Release
```

Build outputs:

| Platform | Executable | Libraries |
|----------|------------|-----------|
| Linux | `build/release/decidb` | `build/release/src/libduckdb.so`, `libduckdb_static.a` |
| macOS | `build/release/decidb` | `build/release/src/libduckdb.dylib` |
| Windows | `build/release/decidb.exe` | `build/release/src/duckdb.dll`, `duckdb.lib` |

**Note**: The executable is named `decidb`, but internal libraries retain `libduckdb` naming for DuckDB extension/API compatibility.

## Known Build Warnings (cosmetic, safe to ignore)

- SymbolicC++ (macOS/Linux): `symbolic/product.h:375` dangling-else, `symbolic/sum.h:422` empty if-body — third-party.
- DeciDB (macOS): `decide_binder.hpp:42` missing `override`, `logical_operator_type.cpp:10` unhandled `LOGICAL_DECIDE` enum in switch.

## Notes

- **Unsigned binaries**: releases are not code-signed (unlike upstream DuckDB); users see OS security warnings on first run.
- **System requirements**: Linux glibc 2.17+; macOS 11.0+; Windows 10+ with VC++ Redistributable 2019+.
