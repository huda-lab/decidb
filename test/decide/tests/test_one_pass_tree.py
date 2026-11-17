"""The E/G/O construction must route rows and retain the deck's containers."""

import re
import os
import subprocess

import pytest

from ._anr_cases import F1_MAX


def construction(cli, sql, chunk=2048):
    # These are explicitly requested internal dumps, so inspect them directly instead of the user-output guard.
    env = {
        **os.environ,
        **(cli.env or {}),
        "DECIDB_DUMP_TREE": "1",
        "DECIDB_DUMP_CONTAINERS": "1",
        "DECIDB_DESCENT_CHUNK": str(chunk),
    }
    result = subprocess.run(
        [cli.exe, "-c", "SET threads=1; SET decide_direct_solve='off'; " + sql],
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, result.stderr
    containers = result.stderr.split("DECIDB_CONTAINERS_BEGIN\n", 1)[1].split("DECIDB_CONTAINERS_END", 1)[0]
    tree = result.stderr.split("DECIDB_TREE_BEGIN\n", 1)[1].split("DECIDB_TREE_END", 1)[0]
    return tree, containers


def nodes(containers):
    """Read the dump's actual node state, without inferring it from the answer."""
    pattern = re.compile(
        r"^node (\d+) ([EGO]) rows (\d+) -> (\d+) groups=(\d+) entries=(\d+) instances=(\d+) text=([^\n]*)\n",
        re.MULTILINE,
    )
    found = list(pattern.finditer(containers))
    return [
        {
            "id": int(match[1]),
            "kind": match[2],
            "incoming": int(match[3]),
            "outgoing": int(match[4]),
            "groups": int(match[5]),
            "entries": int(match[6]),
            "instances": int(match[7]),
            "text": match[8],
            "body": containers[match.end() : found[i + 1].start() if i + 1 < len(found) else len(containers)],
        }
        for i, match in enumerate(found)
    ]


def test_deck_data_reducer_retains_six_values_in_two_mode_groups(decidb_cli):
    """Slide 91 stores Air's four values and Ground's two; it has not folded them."""
    _, containers = construction(decidb_cli, F1_MAX.sql)
    state = nodes(containers)
    mode = [n for n in state if n["kind"] == "G" and "key=(Air)" in n["body"] and "key=(Ground)" in n["body"]]
    assert len(mode) == 1, containers
    assert (mode[0]["groups"], mode[0]["entries"]) == (2, 6), containers
    operand = [n for n in state if n["kind"] == "E" and n["text"] == mode[0]["text"] + " operand"]
    assert len(operand) == 1, containers
    assert operand[0]["entries"] == 6, containers
    # The stored columns still contain the six input demands, not Air=75/Ground=15.
    data = re.findall(r"data=\(([^)]*)\)", operand[0]["body"])
    assert data == ["20", "15", "10", "5", "25", "15"], containers


_PINNED_ROWS = (
    "(VALUES (1, 'a', 'East', 'c1', TRUE, 20, 30), (2, 'a', 'East', 'c1', TRUE, 15, 30), "
    "(3, 'a', 'West', 'c2', FALSE, 10, 50), (4, 'b', 'West', 'c3', TRUE, 25, 45)) t(id, g, r, k, p, d, c)"
)

#: Three pinned dumps verify that tree construction preserves each node's routing and signature structure.
PINNED_DUMPS = {
    "a-nest-under-an-operator": (
        "x(INT) SUCH THAT x <= 100 AND per t.g: sum(when t.p per t.k: max(x) by (t.k)) by (t.r) "
        "+ 0.10 * sum(t.d) by (t.r) <= t.c MAXIMIZE sum(x)",
        """0 E root key=() per=row when=[] sigma=[x] reads=[] columns=0 : x <= 100
1 E root key=t.g per=key when=[] sigma=[by (t.r), t.c] reads=[2,6] columns=2 : per t.g: sum(when t.p per t.k: max(x) by (t.k)) by (t.r) + 0.10 * sum(t.d) by (t.r) <= t.c
2 G key=t.r agg=sum per=row when=[] sigma=[] reads=[3] columns=0 : sum(when t.p per t.k: max(x) by (t.k)) by (t.r)
3 E key=t.k per=key when=[p] sigma=[max(x) by (t.k)] reads=[4] columns=0 : sum(when t.p per t.k: max(x) by (t.k)) by (t.r) operand
4 G key=t.r, t.k agg=max per=row when=[] sigma=[] reads=[5] columns=0 : max(x) by (t.k)
5 E key=() per=row when=[] sigma=[x] reads=[] columns=0 : max(x) by (t.k) operand
6 G key=t.r agg=sum per=row when=[] sigma=[] reads=[7] columns=0 : sum(t.d) by (t.r)
7 E key=() per=row when=[] sigma=[t.d] reads=[] columns=1 : sum(t.d) by (t.r) operand
8 E objective key=() per=one when=[] sigma=[] reads=[9] columns=0 : sum(x)
9 G key=() agg=sum per=row when=[] sigma=[] reads=[10] columns=0 : sum(x)
10 E key=() per=row when=[] sigma=[x] reads=[] columns=0 : sum(x) operand
postorder=[0,5,4,3,2,7,6,1,10,9,8]
""",
    ),
    "a-keyed-term-beside-a-reducer": (
        "x(REAL), per t.g: y(REAL) SUCH THAT x <= 9 AND y <= 3 AND sum(x) by (t.g) + y <= 5 MAXIMIZE sum(x) + sum(y)",
        """0 E root key=() per=row when=[] sigma=[x] reads=[] columns=0 : x <= 9
1 E root key=() per=row when=[] sigma=[y] reads=[] columns=0 : y <= 3
2 E root key=t.g per=key when=[] sigma=[sum(x) by (t.g), y] reads=[3] columns=0 : sum(x) by (t.g) + y <= 5
3 G key=t.g agg=sum per=row when=[] sigma=[] reads=[4] columns=0 : sum(x) by (t.g)
4 E key=() per=row when=[] sigma=[x] reads=[] columns=0 : sum(x) by (t.g) operand
5 E objective key=() per=one when=[] sigma=[] reads=[6,8] columns=0 : sum(x) + sum(y)
6 G key=() agg=sum per=row when=[] sigma=[] reads=[7] columns=0 : sum(x)
7 E key=() per=row when=[] sigma=[x] reads=[] columns=0 : sum(x) operand
8 G key=() agg=sum per=row when=[] sigma=[] reads=[9] columns=0 : sum(y)
9 E key=() per=row when=[] sigma=[y] reads=[] columns=1 : sum(y) operand
postorder=[0,1,4,3,2,7,6,9,8,5]
""",
    ),
    "a-frame-over-decisions": (
        "x(INT) SUCH THAT x <= t.c AND x >= at(previous: x) over (t.id within t.g) MAXIMIZE sum(x)",
        """0 E root key=() per=row when=[] sigma=[x, t.c] reads=[] columns=1 : x <= t.c
1 E root key=() per=row when=[] sigma=[x, at(previous: x) over (t.id within t.g)] reads=[2] columns=0 : x >= at(previous: x) over (t.id within t.g)
2 O key=t.g per=row when=[] sigma=[x] reads=[] columns=0 : at(previous: x) over (t.id within t.g)
3 E objective key=() per=one when=[] sigma=[] reads=[4] columns=0 : sum(x)
4 G key=() agg=sum per=row when=[] sigma=[] reads=[5] columns=0 : sum(x)
5 E key=() per=row when=[] sigma=[x] reads=[] columns=0 : sum(x) operand
postorder=[0,2,1,5,4,3]
""",
    ),
}


@pytest.mark.parametrize("case", list(PINNED_DUMPS))
def test_the_construction_writes_the_tree_it_wrote(decidb_cli, case):
    decide, expected = PINNED_DUMPS[case]
    tree, _ = construction(decidb_cli, f"SELECT id FROM {_PINNED_ROWS} DECIDE {decide}")
    assert tree == expected
