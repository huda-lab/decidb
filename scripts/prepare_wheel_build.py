#!/usr/bin/env python
"""Prepare amalgamation files for wheel/sdist builds.

This script generates sources.list, includes.list, and the duckdb_build/
directory needed by setup.py. It replaces the inline Python blocks that
were duplicated in the CI workflow.

Usage:
    # For wheel builds (platform-aware jemalloc detection):
    python scripts/prepare_wheel_build.py

    # For sdist builds (always include jemalloc sources):
    python scripts/prepare_wheel_build.py --include-jemalloc
"""
import argparse
import os
import platform
import sys


def main():
    parser = argparse.ArgumentParser(description="Prepare wheel build files")
    parser.add_argument(
        "--include-jemalloc",
        action="store_true",
        help="Always include jemalloc (use for sdist to cover all platforms)",
    )
    args = parser.parse_args()

    # Must run from tools/pythonpkg
    script_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "pythonpkg")
    script_path = os.path.normpath(script_path)
    os.chdir(script_path)

    sys.path.insert(0, os.path.join(script_path, "..", "..", "scripts"))

    amalgamation_path = os.path.join(script_path, "..", "..", "scripts", "amalgamation.py")
    if not os.path.isfile(amalgamation_path):
        print("ERROR: Amalgamation script not found at", amalgamation_path)
        sys.exit(1)

    import package_build
    import amalgamation  # noqa: F401 — imported for side effects

    extensions = ["core_functions", "parquet", "tpch", "icu", "json"]

    if args.include_jemalloc:
        # sdist: include jemalloc so the tarball works on all platforms
        extensions.append("jemalloc")
    elif platform.system() == "Linux" and platform.architecture()[0] == "64bit":
        # wheels: only include jemalloc on 64-bit Linux (matches setup.py)
        extensions.append("jemalloc")

    use_short_paths = platform.system() == "Windows"
    target_dir = os.path.join(script_path, "duckdb_build")
    source_list, include_list, _original_sources = package_build.build_package(
        target_dir, extensions, False, 0, "duckdb_build", use_short_paths
    )

    duckdb_sources = [os.path.sep.join(package_build.get_relative_path(script_path, x).split("/")) for x in source_list]
    duckdb_sources.sort()

    duckdb_includes = ["duckdb_build/" + x for x in include_list]
    duckdb_includes.append("duckdb_build")

    os.chdir(script_path)

    with open("sources.list", "w", encoding="utf8") as f:
        for s in duckdb_sources:
            f.write(s + "\n")

    with open("includes.list", "w", encoding="utf8") as f:
        for i in duckdb_includes:
            f.write(i + "\n")

    print(f"Generated sources.list and includes.list successfully")
    print(f"Sources: {len(duckdb_sources)}, Includes: {len(duckdb_includes)}")


if __name__ == "__main__":
    main()
