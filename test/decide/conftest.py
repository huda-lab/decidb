"""Shared fixtures and configuration for DECIDE tests.

Provides a CLI wrapper for the native decidb executable, a vanilla duckdb
connection for oracle data fetching (via dbgen-generated TPC-H data), an
oracle solver instance (with transparent result caching), and performance
tracking.

Architecture
------------
- **DecidB (DECIDE queries)**: native ``build/release/decidb`` executable
  invoked via subprocess (``DecidBCli``).  Reads ``decidb.db``.
- **Oracle (data fetching)**: vanilla ``duckdb`` Python package reading a
  separately generated TPC-H database (``_tpch_oracle.duckdb``).  The oracle
  database is created once per session via ``CALL dbgen(sf=0.01)`` and cached
  on disk.  This keeps the oracle completely independent of DecidB.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import duckdb
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from decidb_cli import DecidBCli
from solver.factory import get_solver
from oracle_cache import OracleCache, CachedOracleSolver
from performance.tracker import PerfTracker
from performance.reporter import print_perf_table

_TPCH_SF = 0.01

# ---------------------------------------------------------------------------
# Locate decidb.db and the decidb executable
# ---------------------------------------------------------------------------

_DECIDB_DB_CANDIDATES = [
    Path(__file__).resolve().parent.parent.parent / "decidb.db",
    Path(__file__).resolve().parent.parent.parent / "build" / "decidb.db",
]

_DECIDB_EXE_CANDIDATES = [
    Path(__file__).resolve().parent.parent.parent / "build" / "release" / "decidb",
]

_ORACLE_DB_PATH = Path(__file__).resolve().parent / "_tpch_oracle.duckdb"


def _find_decidb_db() -> Path | None:
    env = os.environ.get("DECIDB_DB_PATH")
    if env:
        p = Path(env)
        return p if p.exists() else None
    for p in _DECIDB_DB_CANDIDATES:
        if p.exists():
            return p
    return None


def _find_decidb_exe() -> Path | None:
    env = os.environ.get("DECIDB_EXE_PATH")
    if env:
        p = Path(env)
        return p if p.exists() else None
    for p in _DECIDB_EXE_CANDIDATES:
        if p.exists():
            return p
    return None


def _ensure_oracle_db() -> Path:
    """Generate the vanilla TPC-H database if it doesn't already exist."""
    if _ORACLE_DB_PATH.exists():
        return _ORACLE_DB_PATH
    conn = duckdb.connect(str(_ORACLE_DB_PATH))
    try:
        conn.execute("INSTALL tpch; LOAD tpch;")
        conn.execute(f"CALL dbgen(sf={_TPCH_SF})")
    finally:
        conn.close()
    return _ORACLE_DB_PATH


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture(scope="session")
def decidb_db_path():
    """Path to the TPC-H database file.  Skips the session if not found."""
    path = _find_decidb_db()
    if path is None:
        pytest.skip("decidb.db not found — set DECIDB_DB_PATH or build first")
    return str(path)


@pytest.fixture(scope="session")
def decidb_exe_path():
    """Path to the native decidb executable."""
    path = _find_decidb_exe()
    if path is None:
        pytest.skip(
            "decidb executable not found — build first or set DECIDB_EXE_PATH"
        )
    return str(path)


@pytest.fixture(scope="session")
def decidb_cli(decidb_exe_path, decidb_db_path):
    """Session-wide CLI wrapper for the native decidb executable.

    Queries are executed via subprocess, allowing multi-core execution.
    """
    return DecidBCli(decidb_exe_path, decidb_db_path)


@pytest.fixture(scope="session")
def decidb_cli_highs(decidb_exe_path, decidb_db_path):
    """CLI wrapper that forces DecidB to use the HiGHS backend.

    Backed by the ``DECIDB_FORCE_SOLVER=highs`` env var read in
    ``src/decidb/utility/ilp_solver.cpp``. Use in tests that must verify
    HiGHS-specific error paths (e.g. non-convex QP rejection) on hosts
    where Gurobi is also linked.
    """
    return DecidBCli(
        decidb_exe_path, decidb_db_path, env={"DECIDB_FORCE_SOLVER": "highs"},
    )


@pytest.fixture(scope="session")
def decidb_cli_gurobi(decidb_exe_path, decidb_db_path, _raw_oracle_solver):
    """CLI wrapper that forces DecidB to use the Gurobi backend.

    Skips the test if Gurobi is not available on this host. Availability
    is probed indirectly via ``_raw_oracle_solver``: the oracle factory
    requires gurobipy, which in turn requires a working Gurobi install,
    the same dependency DecidB's Gurobi backend has.
    """
    if _raw_oracle_solver is None:
        pytest.skip("Gurobi not available — decidb_cli_gurobi requires it")
    return DecidBCli(
        decidb_exe_path, decidb_db_path, env={"DECIDB_FORCE_SOLVER": "gurobi"},
    )


@pytest.fixture(scope="session")
def _oracle_db_path():
    """Generate (once) and return the path to the vanilla TPC-H database."""
    return str(_ensure_oracle_db())


@pytest.fixture(scope="function")
def duckdb_conn(_oracle_db_path):
    """Per-test read-only connection to the vanilla TPC-H database."""
    conn = duckdb.connect(_oracle_db_path, read_only=True)
    yield conn
    conn.close()


@pytest.fixture(scope="session")
def _raw_oracle_solver():
    """Underlying ILP solver, or None if unavailable."""
    try:
        return get_solver()
    except ImportError:
        return None


@pytest.fixture(scope="session")
def _oracle_cache(decidb_db_path, request):
    """Session-wide oracle result cache.  GC runs only on full (unfiltered) runs."""
    cache = OracleCache(decidb_db_path)
    yield cache
    is_full_run = (
        not getattr(request.config.option, "markexpr", "")
        and not getattr(request.config.option, "keyword", "")
    )
    cache.save(gc=is_full_run)


@pytest.fixture(scope="function")
def oracle_solver(request, _raw_oracle_solver, _oracle_cache):
    """Cache-aware oracle solver for the current test.

    On first run (or after a test/db change) the real solver executes and
    the result is written to results/oracle_cache.json.  Subsequent runs
    return the cached value without invoking the solver at all.
    """
    if _raw_oracle_solver is None and _oracle_cache is None:
        pytest.skip("No ILP solver available and no oracle cache")
    return CachedOracleSolver(
        _raw_oracle_solver,
        request.node.nodeid,
        request.function,
        _oracle_cache,
    )


@pytest.fixture(scope="session")
def perf_tracker():
    """Session-wide performance tracker.  Saves JSON + prints table on teardown."""
    tracker = PerfTracker()
    yield tracker
    path = tracker.save_json()
    print_perf_table(tracker)
    if path:
        print(f"  Performance data saved to: {path}\n")
