"""Canonical fixed-point coverage through emitted solver models.

The public SQL surface cannot invoke ``Canon`` twice directly.  The observable
fixed-point contract is that a spelling requiring canonicalization and the
already-canonical spelling it maps to emit byte-identical solver-neutral
models.  This is stronger than comparing optimal values: different rows and
bounds can share an optimum.

The pairs cover side swaps, exact casts, reducer scales, and WHEN/PER wrappers.
"""

import os
import subprocess

import pytest


def _dump_model(cli, sql, path):
    env = {**os.environ, **(cli.env or {}), "DECIDB_DUMP_MODEL": str(path)}
    result = subprocess.run(
        [cli.exe, cli.db, "-readonly", "-c", sql],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )
    errors = [
        line for line in result.stderr.splitlines()
        if line and not line.startswith("Warning:")
    ]
    assert not errors, "\n".join(errors)
    assert path.exists(), f"query emitted no model:\n{sql}"
    return path.read_text()


_PAIRS = [
    pytest.param(
        """
            SELECT id, x FROM (VALUES (1), (2)) t(id)
            DECIDE x(INT) SUCH THAT 5 >= x MAXIMIZE SUM(x)
        """,
        """
            SELECT id, x FROM (VALUES (1), (2)) t(id)
            DECIDE x(INT) SUCH THAT x <= 5 MAXIMIZE SUM(x)
        """,
        id="side_swap",
    ),
    pytest.param(
        """
            SELECT id, d, lim, x
            FROM (VALUES (1, 2.25::DECIMAL(6,2), 3.25::DECIMAL(8,2))) t(id,d,lim)
            DECIDE x(INT) SUCH THAT x <= 3 AND x + d <= lim MAXIMIZE SUM(x)
        """,
        """
            SELECT id, d, lim, x
            FROM (VALUES (1, 2.25::DECIMAL(6,2), 3.25::DECIMAL(8,2))) t(id,d,lim)
            DECIDE x(INT) SUCH THAT x <= 3 AND x <= lim - d MAXIMIZE SUM(x)
        """,
        id="exact_decimal_cast",
    ),
    pytest.param(
        """
            SELECT id, x FROM (VALUES (1), (2)) t(id)
            DECIDE x(INT) SUCH THAT x <= 9 AND SUM(x) * 2 <= 6 MAXIMIZE SUM(x)
        """,
        """
            SELECT id, x FROM (VALUES (1), (2)) t(id)
            DECIDE x(INT) SUCH THAT x <= 9 AND 2 * SUM(x) <= 6 MAXIMIZE SUM(x)
        """,
        id="reducer_scale",
    ),
    pytest.param(
        """
            SELECT id, grp, active, x
            FROM (VALUES (1,'a',true),(2,'a',false),(3,'b',true),(4,'b',true))
                 t(id,grp,active)
            DECIDE x(INT)
            SUCH THAT 3 >= SUM(x) WHEN active PER grp AND x <= 2
            MAXIMIZE SUM(x)
        """,
        """
            SELECT id, grp, active, x
            FROM (VALUES (1,'a',true),(2,'a',false),(3,'b',true),(4,'b',true))
                 t(id,grp,active)
            DECIDE x(INT)
            SUCH THAT SUM(x) <= 3 WHEN active PER grp AND x <= 2
            MAXIMIZE SUM(x)
        """,
        id="when_per_wrapper",
    ),
]


@pytest.mark.parametrize(("written", "canonical"), _PAIRS)
@pytest.mark.correctness
def test_canonical_spelling_is_model_fixed_point(decidb_cli, tmp_path, written, canonical):
    written_dump = _dump_model(decidb_cli, written, tmp_path / "written.dump")
    canonical_dump = _dump_model(decidb_cli, canonical, tmp_path / "canonical.dump")
    assert written_dump == canonical_dump
