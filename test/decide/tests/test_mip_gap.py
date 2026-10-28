"""The `decide_mip_gap` setting: the relative gap a DECIDE solve may stop at.

The default, 0, proves the optimum. `test_entity_scope.py::test_entity_scoped_mixed_when_per` sets 1e-4 and is
oracle-compared on both backends.
"""

from __future__ import annotations

import pytest


@pytest.mark.error
def test_negative_mip_gap_rejected(decidb_cli):
    decidb_cli.assert_error("SET decide_mip_gap = -1e-4;", match=r"decide_mip_gap must be >= 0")
