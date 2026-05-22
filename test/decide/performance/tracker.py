"""Performance data collection for DECIDE tests."""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path


def _git_commit() -> str:
    """Return the short git commit hash, or 'unknown'."""
    try:
        return (
            subprocess.check_output(
                ["git", "rev-parse", "--short", "HEAD"],
                stderr=subprocess.DEVNULL,
            )
            .decode()
            .strip()
        )
    except Exception:
        return "unknown"


@dataclass
class PerfRecord:
    """A single performance measurement."""
    test_name: str
    timestamp: str
    git_commit: str
    decidb_wall_time_s: float
    oracle_build_time_s: float
    oracle_solve_time_s: float
    num_input_rows: int
    num_solver_variables: int
    num_solver_constraints: int
    objective_value: float | None
    solver_backend: str
    status: str
    comparison_status: str = ""
    decide_vector: list[float] | None = None


@dataclass
class PerfTracker:
    """Collects performance records across a pytest session."""

    records: list[PerfRecord] = field(default_factory=list)
    _results_dir: Path = field(
        default_factory=lambda: Path(__file__).resolve().parent.parent / "results"
    )

    def record(
        self,
        test_name: str,
        decidb_wall_time_s: float,
        oracle_build_time_s: float,
        oracle_solve_time_s: float,
        num_input_rows: int,
        num_solver_variables: int,
        num_solver_constraints: int,
        objective_value: float | None,
        solver_backend: str,
        status: str = "pass",
        comparison_status: str = "",
        decide_vector: list[float] | None = None,
    ) -> None:
        self.records.append(
            PerfRecord(
                test_name=test_name,
                timestamp=datetime.now(timezone.utc).isoformat(),
                git_commit=_git_commit(),
                decidb_wall_time_s=decidb_wall_time_s,
                oracle_build_time_s=oracle_build_time_s,
                oracle_solve_time_s=oracle_solve_time_s,
                num_input_rows=num_input_rows,
                num_solver_variables=num_solver_variables,
                num_solver_constraints=num_solver_constraints,
                objective_value=objective_value,
                solver_backend=solver_backend,
                status=status,
                comparison_status=comparison_status,
                decide_vector=decide_vector,
            )
        )

    def save_json(self) -> Path | None:
        """Write records to a timestamped JSON file in results/."""
        if not self.records:
            return None
        self._results_dir.mkdir(parents=True, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = self._results_dir / f"perf_{ts}.json"
        path.write_text(
            json.dumps([asdict(r) for r in self.records], indent=2) + "\n"
        )
        return path
