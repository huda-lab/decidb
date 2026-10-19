"""No test, corpus query, benchmark or script still writes a removed DECIDE spelling
(syntax_reference.md §10.3). The removed-syntax suite is the one place that may, and a
sqllogic case marked "removed spelling" is a deliberate one (`_old_spellings.scan_file`).
"""

import pathlib

import pytest

from ._old_spellings import scan_file

ROOT = pathlib.Path(__file__).resolve().parents[3]
EXEMPT = {"test/decide/tests/test_scope_removed_syntax.py", "test/decide/tests/_old_spellings.py",
          "test/decide/tests/test_no_old_spellings_remain.py"}
SURFACES = sorted(
    str(path.relative_to(ROOT)) for pattern in (
        "test/decide/**/*.py", "test/decidb/*.test", "test/decidb/*.sql", "test/common/test_decidb_*.cpp",
        "test/decide/golden/corpus.sql", "benchmark/decide/queries/*.sql", "benchmark/decide/queries/*.example",
        "benchmark/decide/*.py", "tools/pythonpkg/test_decidb.py", "run.sh")
    for path in ROOT.glob(pattern)
    if ".venv" not in path.parts and str(path.relative_to(ROOT)) not in EXEMPT)

@pytest.mark.parametrize("surface", SURFACES)
def test_no_old_spelling_remains(surface):
    hits = scan_file(ROOT / surface)
    assert not hits, "\n".join(f"{surface}:{line} {kinds} :: {snippet}" for line, kinds, snippet in hits)
