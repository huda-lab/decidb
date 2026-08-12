"""Oracle result cache for DECIDE tests.

Caches solver objective values keyed by test node ID and a hash of the
test source code + database checksum.  On cache hit, all model-building
calls (add_variable, add_constraint, set_objective) become no-ops and
solve() returns the cached SolverResult instantly.

Cache file: results/oracle_cache.json
Invalidation:
  - Per-test: hash of inspect.getsource(test_fn) + db checksum
  - Global:   db checksum (size + mtime) stored at top level
  - GC:       stale entries pruned on full (unfiltered) test runs that passed

Under xdist every worker builds its own OracleCache and sees only the slice of
tests it was handed, so no worker may write the shared file: the GC would prune
every entry the other workers owned and the last writer would win.  Workers
instead hand back a ``worker_report()`` over xdist's ``workeroutput`` channel,
which the controller folds together with ``merge_worker_reports()``.

The report travels with the worker's shutdown event rather than through a file
it drops on the way out.  That distinction matters for the GC: a worker that
crashes and gets replaced delivers no report, the controller sees the gap, and
pruning is skipped — whereas a missing file is indistinguishable from a worker
that legitimately touched nothing, and silently costs a chunk of the cache.
"""

from __future__ import annotations

import hashlib
import inspect
import json
import os
from pathlib import Path
from typing import Any, Callable

from solver.base import OracleSolver
from solver.types import ObjSense, SolverResult, SolverStatus, VarType

_CACHE_PATH = Path(__file__).resolve().parent / "results" / "oracle_cache.json"


def _file_checksum(path: str) -> str:
    st = os.stat(path)
    return hashlib.sha256(f"{st.st_size}:{st.st_mtime_ns}".encode()).hexdigest()[:16]


def _source_hash(fn: Callable, db_checksum: str) -> str:
    src = inspect.getsource(fn)
    return hashlib.sha256(f"{db_checksum}\n{src}".encode()).hexdigest()[:16]


# ---------------------------------------------------------------------------
# Cache storage
# ---------------------------------------------------------------------------

class OracleCache:
    """Reads/writes oracle results from a JSON file on disk."""

    def __init__(self, db_path: str):
        self._db_checksum = _file_checksum(db_path)
        self._data: dict[str, Any] = {
            "db_checksum": self._db_checksum,
            "entries": {},
        }
        self._accessed: set[str] = set()
        #: Entries solved during this session, as opposed to loaded from disk.
        #: This is what a worker hands back; see ``worker_report``.
        self._stored: dict[str, Any] = {}
        self._dirty = False
        self._load()

    def _load(self) -> None:
        if not _CACHE_PATH.exists():
            return
        try:
            raw = json.loads(_CACHE_PATH.read_text())
        except (json.JSONDecodeError, KeyError):
            return
        if raw.get("db_checksum") != self._db_checksum:
            self._dirty = True
            return
        self._data = raw

    def lookup(self, test_id: str, test_fn: Callable) -> SolverResult | None:
        h = _source_hash(test_fn, self._db_checksum)
        self._accessed.add(test_id)
        entry = self._data["entries"].get(test_id)
        if entry is None or entry.get("input_hash") != h:
            return None
        raw_vv = entry.get("variable_values", {})
        return SolverResult(
            status=SolverStatus[entry["status"]],
            objective_value=entry.get("objective_value"),
            variable_values={k: float(v) for k, v in raw_vv.items()},
        )

    def store(self, test_id: str, test_fn: Callable, result: SolverResult) -> None:
        h = _source_hash(test_fn, self._db_checksum)
        self._accessed.add(test_id)
        entry = {
            "input_hash": h,
            "status": result.status.name,
            "objective_value": result.objective_value,
            "variable_values": result.variable_values,
        }
        self._data["entries"][test_id] = entry
        self._stored[test_id] = entry
        self._dirty = True

    def save(self, gc: bool = False) -> None:
        if gc:
            pruned = {
                k: v
                for k, v in self._data["entries"].items()
                if k in self._accessed
            }
            if len(pruned) != len(self._data["entries"]):
                self._data["entries"] = pruned
                self._dirty = True
        if not self._dirty:
            return
        _CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
        _CACHE_PATH.write_text(
            json.dumps(self._data, indent=2, sort_keys=True) + "\n"
        )

    def worker_report(self) -> dict[str, Any]:
        """This worker's contribution, for the controller to merge.

        Carries only entries solved during this session — the controller reads
        the shared file itself, so re-sending what was loaded from it would just
        move the whole cache across the wire N times.  ``accessed`` is always
        included, even when nothing was solved: it is what lets the controller
        GC against the union across workers rather than one worker's slice.
        """
        return {
            "db_checksum": self._db_checksum,
            "stored": self._stored,
            "accessed": sorted(self._accessed),
        }


def merge_worker_reports(
    db_path: str, reports: list[dict[str, Any]], *, gc: bool
) -> int:
    """Fold xdist worker reports into the shared cache.  Returns entries kept."""
    cache = OracleCache(db_path)
    for report in reports:
        # A report built against a different database is stale by definition —
        # drop it rather than mixing checksums into one file.
        if report.get("db_checksum") != cache._db_checksum:
            continue
        stored = report.get("stored") or {}
        if stored:
            cache._data["entries"].update(stored)
            cache._dirty = True
        cache._accessed.update(report.get("accessed", []))
    cache.save(gc=gc)
    return len(cache._data["entries"])


# ---------------------------------------------------------------------------
# Transparent solver wrapper
# ---------------------------------------------------------------------------

class CachedOracleSolver:
    """Drop-in replacement for OracleSolver that caches solve() results.

    On create_model(), checks the cache:
      - Hit:  all subsequent model-building calls are no-ops;
              solve() returns the cached SolverResult.
      - Miss: delegates everything to the real solver;
              solve() stores the result before returning.

    If no real solver is available and the cache misses, the test is skipped.
    """

    def __init__(
        self,
        real_solver: OracleSolver | None,
        test_id: str,
        test_fn: Callable,
        cache: OracleCache,
    ):
        self._real = real_solver
        self._test_id = test_id
        self._test_fn = test_fn
        self._cache = cache
        self._cached_result: SolverResult | None = None
        self._model_active = False
        # A single test may call create_model() more than once (e.g.
        # parametric loops over multiple budgets). We give each model a
        # unique cache key by suffixing the call count so earlier solves
        # don't shadow later ones.
        self._model_count = 0
        self._current_cache_key = test_id

    def create_model(self, name: str) -> None:
        self._model_count += 1
        self._current_cache_key = (
            self._test_id if self._model_count == 1
            else f"{self._test_id}#{self._model_count}"
        )
        cached = self._cache.lookup(self._current_cache_key, self._test_fn)
        if cached is not None:
            self._cached_result = cached
            self._model_active = False
            return
        self._cached_result = None
        if self._real is None:
            import pytest
            pytest.skip(
                f"No solver available and no cached oracle for {self._current_cache_key}"
            )
        self._real.create_model(name)
        self._model_active = True

    def add_variable(
        self,
        name: str,
        var_type: VarType = VarType.BINARY,
        lb: float = 0.0,
        ub: float | None = None,
    ) -> None:
        if self._model_active:
            self._real.add_variable(name, var_type, lb, ub)

    def add_constraint(
        self,
        coeffs: dict[str, float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        if self._model_active:
            self._real.add_constraint(coeffs, sense, rhs, name)

    def set_objective(
        self,
        coeffs: dict[str, float],
        sense: ObjSense,
    ) -> None:
        if self._model_active:
            self._real.set_objective(coeffs, sense)

    def set_quadratic_objective(
        self,
        linear: dict[str, float],
        quadratic: dict[tuple[str, str], float],
        sense: ObjSense,
        constant: float = 0.0,
    ) -> None:
        if self._model_active:
            self._real.set_quadratic_objective(linear, quadratic, sense, constant)

    def add_quadratic_constraint(
        self,
        linear: dict[str, float],
        quadratic: dict[tuple[str, str], float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        if self._model_active:
            self._real.add_quadratic_constraint(linear, quadratic, sense, rhs, name)

    def add_indicator_constraint(
        self,
        indicator_var: str,
        indicator_val: int,
        coeffs: dict[str, float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        if self._model_active:
            self._real.add_indicator_constraint(
                indicator_var, indicator_val, coeffs, sense, rhs, name,
            )

    def solve(self, time_limit: float = 60.0) -> SolverResult:
        if self._cached_result is not None:
            return self._cached_result
        if self._real is None:
            import pytest
            pytest.skip(
                f"No solver available and no cached oracle for {self._current_cache_key}"
            )
        result = self._real.solve(time_limit)
        self._cache.store(self._current_cache_key, self._test_fn, result)
        return result

    def solver_name(self) -> str:
        if self._cached_result is not None:
            return "cached"
        if self._real is not None:
            return self._real.solver_name()
        return "cached"
