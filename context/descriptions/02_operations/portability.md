# Cross-platform portability

DeciDB is developed on macOS and released on Linux, macOS, and Windows. Only the
two release workflows ever compile Windows, so a portability bug survives every
local build and every test run, and then fails a release 25 minutes at a time.
Between v0.1.0 and v0.2.0 four accumulated over 355 commits.

## What breaks, and why it is invisible locally

| Class | Example | Why macOS misses it |
|-------|---------|---------------------|
| POSIX-only header | `#include <sys/resource.h>` | MSVC has no such header; clang does |
| POSIX-only function | `setenv`, `isatty`, `getrusage` | MSVC needs `_putenv_s`, `_isatty`, `GetProcessMemoryInfo` |
| Missing standard header | `std::isfinite` with only `<cstdlib>` | clang pulls `<cmath>` in transitively, GCC does not |
| Unity-build leakage | `BinderException` without its header | a neighbour in the same unity blob declares it |

The last one deserves emphasis: the Windows CLI job configures `DISABLE_UNITY=1`,
so every file compiles alone. A file that only builds because it shares a
translation unit with another is fine everywhere else and fails only there.

## The local check

```bash
python3 scripts/check_portability.py          # all checks, ~30s
python3 scripts/check_portability.py --fast   # skip standalone compiles, ~2s
```

It needs a `build/*/compile_commands.json`, which `make release` produces. The
checks are scoped to DeciDB's own sources — paths matching `decide` or `decidb`.
Upstream DuckDB files are excluded on purpose: upstream CI already builds them
on Windows, and including them produces roughly 55 false positives from files
that demonstrably compile, because upstream leans on transitive includes.

Verified against the real pre-fix sources: it flags all four bugs above.

**Known limitation.** The POSIX check skips any file that mentions `_WIN32`,
`DUCKDB_WINDOWS`, or `_MSC_VER` anywhere, so a file that guards one call and
leaves another bare is not reported. Comments and string literals are stripped
before matching, so a symbol merely named in prose is not a hit.

## The CI check

`.github/workflows/portability.yml`:

- **static-checks** — runs the script above on every push and pull request (~2
  minutes). Configures CMake without building, purely for `compile_commands.json`.
- **windows-compile** — a real MSVC build with `DISABLE_UNITY=1` on pushes to
  master, daily, and on demand (~25 minutes). Skipped on pull requests. This is
  the only thing that proves MSVC is happy; the static checks cannot see
  template resolution, two-phase lookup, or resource-file problems.

## Writing portable code here

Guard platform primitives the way `src/decidb/gurobi/gurobi_loader.cpp` does —
`#ifdef _WIN32` with a real Windows implementation on the other side, not a
silent no-op. Include the header that declares what you use rather than relying
on a neighbour or on clang's transitive includes; the specific exception header
(`duckdb/common/exception/binder_exception.hpp`), not just the umbrella
`duckdb/common/exception.hpp`.

Deliberate gap: `PeakProcessMemoryBytes()` in `physical_decide.cpp` returns 0 on
Windows, so the slow-solve report omits its peak-memory line there. Implementing
it means `GetProcessMemoryInfo` and a psapi link entry in both CMake and
`setup.py`. Tracked in `todo.md`.

## Resource files

`tools/shell/rc/duckdb.rc` compiles only on Windows and is not C++, so no static
check covers it. It references `logo/decidb.ico`; deleting or renaming that icon
breaks the Windows build alone. Regeneration instructions are in the header
comment of `logo/decidb-icon.svg`.
