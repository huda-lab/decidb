#!/usr/bin/env python3
"""Catch the breakages that only Windows or a non-clang toolchain would find.

Nothing builds Windows between releases, so portability bugs sit unnoticed for
hundreds of commits and then fail a release build 25 minutes at a time. This
runs the same checks locally in about a minute.

Three checks, each corresponding to a bug that actually reached CI:

  headers    A std:: symbol used without the header that declares it. Unity
             builds and clang's transitive includes both hide these; GCC and
             MSVC do not. (std::isfinite with only <cstdlib> broke Linux.)

  posix      A POSIX-only header or function in a file that Windows compiles.
             (<sys/resource.h> and setenv broke Windows.)

  standalone Every .cpp compiled alone with -fsyntax-only, which reproduces the
             Windows CLI job's DISABLE_UNITY=1. Without it a file can rely on a
             declaration that a unity-build neighbour happened to supply.
             (BinderException broke Windows this way.)

Usage:
    python3 scripts/check_portability.py             # all checks
    python3 scripts/check_portability.py --fast      # skip standalone compiles
"""
import argparse
import json
import pathlib
import re
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Scope: DeciDB's own code. Upstream DuckDB sources are excluded because
# upstream CI already builds them on Windows -- including them here produces
# ~55 false positives from files that demonstrably compile, since upstream
# leans on transitive includes that happen to work.
#
# test/persistence and test/extension are excluded from the Windows build by
# test/CMakeLists.txt, so their POSIX use is legitimate. test/common is not.
SEARCH_ROOTS = ["src", "test", "tools/shell", "tools/utils"]
OWNED = re.compile(r"(decide|decidb)", re.IGNORECASE)

HEADER_CHECKS = [
    ("cmath", r"std::(isfinite|isnan|isinf|fabs|floor|ceil|round|pow|sqrt|log|exp|trunc|copysign|nextafter|hypot)\b"),
    ("algorithm", r"std::(sort|stable_sort|max_element|min_element|find_if|count_if|reverse|unique|lower_bound|upper_bound|all_of|any_of|none_of|clamp)\b"),
    ("numeric", r"std::(accumulate|iota)\b"),
    ("cstring", r"std::(memcpy|memset|memcmp|strlen|strcmp)\b"),
    ("limits", r"std::(numeric_limits)\b"),
    ("memory", r"std::(unique_ptr|shared_ptr|make_unique|make_shared)\b"),
]

POSIX_HEADERS = r'#include\s*<(unistd|dirent|pthread|sys/(resource|time|wait|ioctl|mman))\.h>'
POSIX_FUNCS = (r'\b(getrusage|setenv|unsetenv|isatty|STDIN_FILENO|strcasecmp|strncasecmp'
               r'|popen|pclose|fork|usleep|nanosleep|gettimeofday|mkstemp|ftruncate'
               r'|dlopen|dlsym|dlclose|opendir|readdir|closedir)\b')

WIN_GUARDS = ("_WIN32", "DUCKDB_WINDOWS", "_MSC_VER")


def tracked_sources(dirs, suffixes=(".cpp", ".hpp")):
    out = subprocess.run(["git", "ls-files", *dirs], cwd=ROOT,
                         capture_output=True, text=True).stdout.split()
    return [f for f in out
            if f.endswith(suffixes) and "third_party" not in f and OWNED.search(f)]


def strip_comments(text):
    """Blank out comments and string literals, preserving line numbering, so a
    symbol named in prose is not reported as a call."""
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
    text = re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', lambda m: ' ' * len(m.group(0)), text)
    return text


def check_headers(files):
    bad = []
    for f in files:
        raw = (ROOT / f).read_text(errors="replace")
        includes = set(re.findall(r'#include\s*<([A-Za-z0-9_/.]+)>', raw))
        text = strip_comments(raw)
        for hdr, pat in HEADER_CHECKS:
            m = re.search(pat, text)
            if m and hdr not in includes:
                line = text[:m.start()].count("\n") + 1
                bad.append(f"{f}:{line}  std::{m.group(1)} without <{hdr}>")
    return bad


def check_posix(files):
    bad = []
    for f in files:
        raw = (ROOT / f).read_text(errors="replace")
        # A file that mentions a Windows guard anywhere is assumed to handle the
        # split deliberately; this check is for entirely unguarded use.
        if any(g in raw for g in WIN_GUARDS):
            continue
        text = strip_comments(raw)
        for pat, what in ((POSIX_HEADERS, "POSIX header"), (POSIX_FUNCS, "POSIX function")):
            m = re.search(pat, text)
            if m:
                line = text[:m.start()].count("\n") + 1
                bad.append(f"{f}:{line}  unguarded {what}: {m.group(0)}")
                break
    return bad


def check_standalone(files):
    cc_path = next((p for p in sorted(ROOT.glob("build/*/compile_commands.json"))), None)
    if cc_path is None:
        return ["no build/*/compile_commands.json -- run `make release`, or configure with "
                "cmake -B build/ci -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    cc = json.loads(cc_path.read_text())

    # Flags differ by area -- test targets need Catch's include path, which no
    # src/ entry carries. Keep the richest flag set seen per top-level area and
    # compile each file with the flags of its own area.
    by_area = {}
    for e in cc:
        args = shlex.split(e["command"]) if "command" in e else e["arguments"]
        f = [a for a in args if a.startswith(("-I", "-D", "-isystem"))]
        rel = e["file"].replace(str(ROOT) + "/", "")
        area = rel.split("/")[0]
        if len(f) > len(by_area.get(area, [])):
            by_area[area] = f
    if not by_area:
        return ["no usable compile_commands entry found"]
    fallback = max(by_area.values(), key=len)

    bad = []
    cpps = [f for f in files if f.endswith(".cpp")]
    for i, f in enumerate(cpps, 1):
        print(f"\r  standalone {i}/{len(cpps)}", end="", file=sys.stderr, flush=True)
        flags = by_area.get(f.split("/")[0], fallback)
        r = subprocess.run(["clang++", "-std=c++17", "-fsyntax-only", *flags, f],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            first = next((l for l in r.stderr.splitlines() if "error:" in l), r.stderr[:200])
            bad.append(f"{f}  {first.strip()}")
    print("\r" + " " * 40 + "\r", end="", file=sys.stderr)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fast", action="store_true", help="skip the standalone compiles")
    args = ap.parse_args()

    files = tracked_sources(SEARCH_ROOTS)
    print(f"checking {len(files)} DeciDB sources\n")

    failed = False
    checks = [("missing standard headers", check_headers, files),
              ("unguarded POSIX use", check_posix, files)]
    if not args.fast:
        checks.append(("standalone compile (DISABLE_UNITY=1)", check_standalone, files))

    for name, fn, arg in checks:
        problems = fn(arg)
        if problems:
            failed = True
            print(f"FAIL  {name}")
            for p in problems:
                print(f"        {p}")
        else:
            print(f"ok    {name}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
