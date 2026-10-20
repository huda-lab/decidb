"""Per-test branch-coverage profiles for the direct-solve code (H-30).

Inert unless ``DECIDB_COVERAGE_DIR`` is set, so a normal ``make decide-test`` never loads it
(it is not registered in ``conftest.py``; ``run_coverage.sh`` passes ``-p coverage_plugin``).

With the variable set, the autouse fixture below gives every test its own LLVM profile.
The ``decidb`` binary of ``build/coverage`` writes one raw profile per process into the
test's own scratch folder (``%p`` is the process id, because one test may start the CLI
several times). When the test ends, the raw profiles are merged into one small
``profiles/<key>.profdata`` file and deleted, so the disk never holds more than a few raw
files per running worker. ``profiles/<key>.json`` records the node id, the outcome and the
wall time of the test, which ``coverage_report.py`` reads back.

Processes started outside a test (session fixtures, a stray CLI call) write to ``stray/``;
the report counts what lands there so a profile that belongs to no test is not silent.
"""

from __future__ import annotations

import functools
import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

import pytest

COVERAGE_DIR_ENV = "DECIDB_COVERAGE_DIR"


def _dir() -> Path | None:
    value = os.environ.get(COVERAGE_DIR_ENV)
    return Path(value) if value else None


def profile_key(nodeid: str) -> str:
    """File stem of a test's profile: unique, and still readable in a directory listing."""
    readable = "".join(c if c.isalnum() else "_" for c in nodeid.split("::", 1)[-1])[:60]
    return f"{readable}.{hashlib.sha1(nodeid.encode()).hexdigest()[:10]}"


@functools.lru_cache(maxsize=None)
def _llvm_profdata() -> str:
    return (
        os.environ.get("LLVM_PROFDATA")
        or subprocess.run(["xcrun", "-f", "llvm-profdata"], capture_output=True, text=True, check=True).stdout.strip()
    )


def pytest_configure(config):
    root = _dir()
    if root is None:
        return
    (root / "profiles").mkdir(parents=True, exist_ok=True)
    (root / "stray").mkdir(parents=True, exist_ok=True)
    os.environ["LLVM_PROFILE_FILE"] = str(root / "stray" / "%p.profraw")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    if report.when == "call" or (report.when == "setup" and report.outcome != "passed"):
        item._h30_outcome = report.outcome


@pytest.fixture(autouse=True)
def _per_test_coverage_profile(request):
    root = _dir()
    if root is None:
        yield
        return

    nodeid = request.node.nodeid
    key = profile_key(nodeid)
    raw_dir = root / "raw" / key
    shutil.rmtree(raw_dir, ignore_errors=True)
    raw_dir.mkdir(parents=True)
    previous = os.environ.get("LLVM_PROFILE_FILE")
    os.environ["LLVM_PROFILE_FILE"] = str(raw_dir / "%p.profraw")
    started = time.perf_counter()
    try:
        yield
    finally:
        seconds = time.perf_counter() - started
        if previous is None:
            os.environ.pop("LLVM_PROFILE_FILE", None)
        else:
            os.environ["LLVM_PROFILE_FILE"] = previous
        raws = sorted(raw_dir.glob("*.profraw"))
        out = root / "profiles" / f"{key}.profdata"
        merged = False
        if raws:
            merged = (
                subprocess.run(
                    [_llvm_profdata(), "merge", "-sparse", *map(str, raws), "-o", str(out)],
                    capture_output=True,
                ).returncode
                == 0
            )
        raw_bytes = sum(p.stat().st_size for p in raws)
        shutil.rmtree(raw_dir, ignore_errors=True)
        (root / "profiles" / f"{key}.json").write_text(
            json.dumps(
                {
                    "nodeid": nodeid,
                    "outcome": getattr(request.node, "_h30_outcome", "unknown"),
                    "seconds": round(seconds, 3),
                    "processes": len(raws),
                    "raw_bytes": raw_bytes,
                    "profdata_bytes": out.stat().st_size if merged else 0,
                    "merged": merged,
                }
            )
        )
