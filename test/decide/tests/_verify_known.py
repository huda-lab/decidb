"""The marker for a difference DECIDB_EVALUATOR=verify knows of: `verify_known_difference(bug, groups)` names the record
groups, each `group` or `group:field prefix`, in which the second run may differ while the bug stands. Under verify a
marked test runs with DECIDB_VERIFY_ALLOW naming them and DECIDB_VERIFY_ALLOW_LOG, a file of its own, to which each
allowed difference that showed writes its bug (decide_evaluator.hpp); a marked bug that wrote nothing no longer
differs, and the test fails until its marker goes (conftest.py)."""


def allow_setting(marks) -> str:
    """DECIDB_VERIFY_ALLOW for `marks`, each (bug, groups)."""
    return ",".join(f"{bug}:{group}" for bug, groups in marks for group in groups)


def stale_markers(marks, logged) -> set:
    """The bugs of `marks` the log does not name: their difference is gone."""
    return {bug for bug, _ in marks} - set(logged)
