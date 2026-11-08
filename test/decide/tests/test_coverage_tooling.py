"""The coverage toolchain's lookups and the benchmark scripts' host probes, on whatever host runs the tests."""

import argparse
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
BENCHMARK_DIR = REPO / "benchmark" / "decide"
TRUE = shutil.which("true")
FALSE = shutil.which("false")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sys.path.insert(0, str(BENCHMARK_DIR))
REPORT = _load("coverage_report_under_test", REPO / "test" / "decide" / "coverage_report.py")
PIPELINE = _load("profile_pipeline_under_test", BENCHMARK_DIR / "profile_pipeline.py")
DIRECT = _load("direct_solve_run_under_test", BENCHMARK_DIR / "direct_solve" / "run.py")

# `time -p` is the macOS fallback where a sandbox denies `time -l`: it reports no memory.
reports_memory = pytest.mark.skipif(PIPELINE.bench.get_time_flag() == "-p", reason="this /usr/bin/time reports no RSS")


@pytest.fixture
def coverage_build(tmp_path, monkeypatch):
    """A build directory whose compiler is a script that answers -print-prog-name with $COMPILER_ANSWER."""
    compiler = tmp_path / "fake clang++"
    compiler.write_text('#!/bin/sh\necho "$1" > "$(dirname "$0")/asked"\necho "$COMPILER_ANSWER"\n')
    compiler.chmod(compiler.stat().st_mode | stat.S_IXUSR)
    (tmp_path / "CMakeCache.txt").write_text(
        f"CMAKE_C_COMPILER:FILEPATH=/no/cc\nCMAKE_CXX_COMPILER:FILEPATH={compiler}\n"
    )
    monkeypatch.setattr(REPORT, "COVERAGE_BUILD", tmp_path)
    REPORT.llvm_tool.cache_clear()
    yield tmp_path
    REPORT.llvm_tool.cache_clear()


def test_llvm_tool_is_the_one_the_build_compiler_names(coverage_build, monkeypatch):
    monkeypatch.setenv("COMPILER_ANSWER", TRUE)
    assert REPORT.llvm_tool("llvm-cov") == TRUE
    assert (coverage_build / "asked").read_text().strip() == "-print-prog-name=llvm-cov"


def test_llvm_tool_the_compiler_cannot_find_is_an_error_naming_it(coverage_build, monkeypatch):
    # Branch: `path is None`. A compiler that finds nothing answers with the bare name.
    monkeypatch.setenv("COMPILER_ANSWER", "llvm-tool-not-installed")
    with pytest.raises(RuntimeError, match="finds no llvm-tool-not-installed"):
        REPORT.llvm_tool("llvm-tool-not-installed")


# `_read_meta` finds the library with one glob, `libduckdb.*`, which names no platform: one suffix stands for both.
@pytest.mark.parametrize("library", ["libduckdb.dylib"])
def test_cpp_profile_reads_the_shared_library_of_either_platform(tmp_path, monkeypatch, library):
    monkeypatch.setattr(REPORT, "COVERAGE_BUILD", tmp_path)
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / library).touch()
    (tmp_path / "src" / "libduckdb_static.a").touch()
    (tmp_path / "cpp").mkdir()
    (tmp_path / "cpp" / "cpp.profdata").touch()
    assert REPORT._read_meta(tmp_path)[REPORT.CPP_NODEID]["binary"] == tmp_path / "src" / library


def _coverage_build_recipe():
    return (REPO / "Makefile").read_text().split("\ndecide-coverage-build:\n", 1)[1].split("\ndecide-coverage:", 1)[0]


def test_makefile_instruments_exactly_the_files_the_report_reads():
    patterns = re.findall(r"src:\*/([^\\']+)", _coverage_build_recipe())
    instrumented = {path for pattern in patterns for path in REPO.glob(pattern) if path.suffix == ".cpp"}
    assert instrumented == set(REPORT.SCOPE)


def test_a_compiler_that_rejects_the_coverage_flags_stops_before_configuring():
    # Branch: the probe in decide-coverage-build failing. Without it cmake runs and leaves a CMakeCache.txt.
    build_dir = f"build/coverage_probe_{os.getpid()}"
    try:
        done = subprocess.run(
            [
                "make",
                "-s",
                "decide-coverage-build",
                "COVERAGE_CC=false",
                "COVERAGE_CXX=false",
                f"COVERAGE_BUILD_DIR={build_dir}",
            ],
            cwd=REPO,
            capture_output=True,
            text=True,
        )
        assert done.returncode != 0
        assert "false cannot build with the coverage flags" in done.stderr
        assert not (REPO / build_dir / "CMakeCache.txt").exists()
    finally:
        shutil.rmtree(REPO / build_dir, ignore_errors=True)


@reports_memory
def test_pipeline_profiler_measures_memory_with_this_hosts_time(tmp_path, monkeypatch):
    # The flag and the parser have to agree on this host: a BSD flag makes GNU time refuse to run the command at all.
    monkeypatch.setattr(PIPELINE, "RAW", tmp_path)
    monkeypatch.setattr(PIPELINE.bench, "DECIDB_EXE", TRUE)
    query = tmp_path / "q.sql"
    query.write_text("SELECT 1")
    row = PIPELINE.run_one("Q0", query, "highs", 0)
    assert row["status"] == "ok"
    assert row["peak_rss_mib"] > 0


@reports_memory
def test_direct_runner_measures_memory_with_this_hosts_time(monkeypatch):
    monkeypatch.setattr(DIRECT, "BINARY", TRUE)
    _, peak, status, _, _ = DIRECT.run("db", "direct", "SELECT 1", 30)
    assert status is None
    assert peak > 0


def test_direct_runner_does_not_read_the_query_in_times_report_as_an_error(monkeypatch):
    # Branch: the split ending what the process wrote. GNU time's report repeats the command line, query included.
    monkeypatch.setattr(DIRECT, "BINARY", TRUE)
    assert DIRECT.run("db", "direct", "SELECT 'Error'", 30)[2] is None


def test_direct_runner_reports_a_failed_process(monkeypatch):
    monkeypatch.setattr(DIRECT, "BINARY", FALSE)
    assert DIRECT.run("db", "direct", "SELECT 1", 30)[2] == "error"


def test_cpu_model_reads_cpuinfo_where_there_is_one(tmp_path):
    cpuinfo = tmp_path / "cpuinfo"
    cpuinfo.write_text("processor\t: 0\nmodel name\t: Some CPU\nprocessor\t: 1\nmodel name\t: Some CPU\n")
    assert DIRECT.cpu_model(cpuinfo) == "Some CPU"


def test_cpu_model_is_unknown_when_cpuinfo_does_not_name_it(tmp_path):
    # Branch: no `model name` line, as on ARM Linux.
    cpuinfo = tmp_path / "cpuinfo"
    cpuinfo.write_text("processor\t: 0\nBogoMIPS\t: 50.00\n")
    assert DIRECT.cpu_model(cpuinfo) is None


def test_cpu_model_asks_sysctl_without_cpuinfo(tmp_path, monkeypatch):
    # Branch: no /proc/cpuinfo, as on macOS.
    monkeypatch.setattr(DIRECT, "command_output", lambda command, cwd=None: " ".join(command))
    assert DIRECT.cpu_model(tmp_path / "missing") == "sysctl -n machdep.cpu.brand_string"


def test_manifest_records_the_machine(monkeypatch, tmp_path):
    monkeypatch.setattr(DIRECT, "BINARY", Path(TRUE))
    written = DIRECT.manifest(argparse.Namespace(), "stamp", tmp_path / "out.csv")
    assert written["cpu"]
    assert written["memory_bytes"] > 2**20
    json.dumps(written)
