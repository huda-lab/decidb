"""The E/G/O construction must route rows and retain the deck's containers."""

import re
import os
import subprocess

from ._anr_cases import F1_MAX


def construction(cli, sql, chunk=2048):
    # These are explicitly requested internal dumps, so inspect them directly instead of the user-output guard.
    setting = f"SET decide_debug = 'tree,containers,chunk={chunk}'; "
    result = subprocess.run(
        [cli.exe, "-c", "SET threads=1; SET decide_direct_solve='off'; " + setting + sql],
        env={**os.environ, **(cli.env or {})},
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


#: F1's tree, nodes 8-10 being the objective, which p. 81 does not draw. The clause's text is one line here, as the
#: dump prints the query's own line breaks.
F1_TREE = """0 E root key=() per=row when=[] sigma=[ship] reads=[] columns=0 : ship <= 100
1 E root key=t.id per=key when=[] sigma=[by (t.rid), by (t.m), t.c] reads=[2,6] columns=2 : per t.id: sum(when t.p per t.cid: max(ship) by (t.cid)) by (t.rid) + 0.10 * sum(t.d) by (t.m) <= t.c
2 G key=t.rid agg=sum per=row when=[] sigma=[] reads=[3] columns=0 : sum(when t.p per t.cid: max(ship) by (t.cid)) by (t.rid)
3 E key=t.cid per=key when=[p] sigma=[max(ship) by (t.cid)] reads=[4] columns=0 : sum(when t.p per t.cid: max(ship) by (t.cid)) by (t.rid) operand
4 G key=t.rid, t.cid agg=max per=row when=[] sigma=[] reads=[5] columns=0 : max(ship) by (t.cid)
5 E key=() per=row when=[] sigma=[ship] reads=[] columns=0 : max(ship) by (t.cid) operand
6 G key=t.m agg=sum per=row when=[] sigma=[] reads=[7] columns=0 : sum(t.d) by (t.m)
7 E key=() per=row when=[] sigma=[t.d] reads=[] columns=1 : sum(t.d) by (t.m) operand
8 E objective key=() per=one when=[] sigma=[] reads=[9] columns=0 : sum(ship)
9 G key=() agg=sum per=row when=[] sigma=[] reads=[10] columns=0 : sum(ship)
10 E key=() per=row when=[] sigma=[ship] reads=[] columns=0 : sum(ship) operand
postorder=[0,5,4,3,2,7,6,1,10,9,8]
"""


def test_the_deck_example_builds_the_tree_of_page_81(decidb_cli):
    """F1's tree is the annotated tree of p. 81 of context/reference/New_syntax.pdf (E(TRUE, D.id) over the nest and the data
    sum, G(SUM, D.rid), E(S.p, S.cid), G(MAX, S.cid), E(TRUE, tid), G(SUM, S.m), E(TRUE, tid)), written as the dump
    writes a tree, which differs from the drawing in notation only:
    (1) the deck draws <=, + and * as nodes; the dump holds them in the E node's body;
    (2) the deck's sigma lists a node's per key and row id (D.id, tid); the dump prints them as `key=` and `per=`;
    (3) for a reducer read directly, the deck's sigma lists its by key (S.cid); the dump lists the reducer with its by;
    (4) the deck labels a nested G by its own key (S.cid); the dump writes the key from the root (t.rid, t.cid).
    Nodes 8-10, the objective, are not on p. 81."""
    tree, _ = construction(decidb_cli, F1_MAX.sql)
    assert re.sub(r"\n +", " ", tree) == F1_TREE


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
