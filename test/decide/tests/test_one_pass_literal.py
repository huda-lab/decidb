"""The deck's one pass, checked by its own properties (plan §3): what the descent reads and where rows go.

L6 (Descent): a child's reach is its parent's, and each `when` part runs once per row reaching it. Read off the
containers dump: each node's rows in and out, its children, and the rows each `when` part was read on.
"""

import os
import re
import subprocess

import pytest

from ._anr_cases import CASES
from .test_one_pass_tree import construction, nodes

#: The deck's cases that run, each read at chunk size 1 (the deck's row-at-a-time animation) and 2048.
RUNNING = [case for case in CASES if case.sql and not getattr(case, "error", None)]


def reach(node):
    """A node's children and its `when` parts' row counts, from its `reach` line."""
    match = re.search(r"^  reach root=([01]) children=\[([\d,]*)\] when=\[([\d,]*)\]$", node["body"], re.MULTILINE)
    assert match, node
    children = [int(c) for c in match[2].split(",") if c]
    when = [int(c) for c in match[3].split(",") if c]
    return children, when


def is_root(node):
    return re.search(r"^  reach root=1 ", node["body"], re.MULTILINE) is not None


def all_nodes(containers):
    """Every node, F included, which `nodes` leaves out."""
    pattern = re.compile(r"^node (\d+) ([EGOF]) rows (\d+) -> (\d+) [^\n]*\n", re.MULTILINE)
    found = list(pattern.finditer(containers))
    return {
        int(match[1]): {
            "kind": match[2],
            "incoming": int(match[3]),
            "outgoing": int(match[4]),
            "body": containers[match.end() : found[i + 1].start() if i + 1 < len(found) else len(containers)],
        }
        for i, match in enumerate(found)
    }


def check_descent(containers, num_rows):
    graph = all_nodes(containers)
    parents = {n: [] for n in graph}
    for n, node in graph.items():
        for child in reach(node)[0]:
            parents[child].append(n)
    # The paths from the roots to each node, as the descent counts them (a DAG: departure 15).
    paths = {}

    def count(n):
        if n not in paths:
            paths[n] = (1 if is_root(graph[n]) else 0) + sum(count(p) for p in parents[n])
        return paths[n]

    for n, node in graph.items():
        children, when = reach(node)
        assert count(n) >= 1, (n, containers)
        if count(n) == 1 and is_root(node):
            # A root no reader routes: every row descends from it.
            assert node["incoming"] == num_rows, (n, containers)
        elif count(n) == 1:
            # One path: its reach is its parent's.
            assert node["incoming"] == graph[parents[n][0]]["outgoing"], (n, containers)
        else:
            # More than one path: each (row, enclosing slot) once, however many paths route it.
            base = num_rows if is_root(node) else 0
            assert node["incoming"] <= base + sum(graph[p]["outgoing"] for p in parents[n]), (n, containers)
        if when:
            # The first part reads every row reaching the node, each later one at most the rows before it kept.
            assert when[0] == node["incoming"], (n, containers)
            assert all(later <= earlier for earlier, later in zip(when, when[1:])), (n, containers)
            assert node["outgoing"] <= when[-1], (n, containers)
        else:
            assert node["outgoing"] <= node["incoming"], (n, containers)


@pytest.mark.parametrize("chunk", [1, 2048])
@pytest.mark.parametrize("case", RUNNING, ids=lambda case: case.name)
def test_a_childs_reach_is_its_parents(decidb_cli, case, chunk):
    """L6 over every deck case: roots read every row, a child reads exactly the rows its parent passes on."""
    _, containers = construction(decidb_cli, case.sql, chunk)
    check_descent(containers, len(case.data))


def test_the_deck_example_routes_as_pages_83_to_91(decidb_cli):
    """F1's E(S.p, S.cid) reads its `when` on all six rows and keeps five (row 4, p = F, stops there, p. 89); its
    G(MAX) below reads those five and no other."""
    from ._anr_cases import F1_MAX

    _, containers = construction(decidb_cli, F1_MAX.sql, 1)
    graph = all_nodes(containers)
    gated = [n for n, node in graph.items() if reach(node)[1]]
    assert len(gated) == 1, containers
    node = graph[gated[0]]
    assert (reach(node)[1], node["incoming"], node["outgoing"]) == ([6], 6, 5), containers
    (child,) = reach(node)[0]
    assert graph[child]["incoming"] == 5, containers


@pytest.mark.parametrize(
    "when, counts, kept",
    [
        pytest.param("t.a > 0 AND t.b > 0", [4, 2], 1, id="an-and-reads-its-second-part-on-what-the-first-kept"),
        pytest.param("t.a > 0 OR t.b > 0", [4], 3, id="an-or-is-one-part"),
    ],
)
def test_each_when_part_reads_only_the_rows_before_it_kept(decidb_cli, when, counts, kept):
    """L1 and L6: an AND's parts gate in turn, so its second part never reads a row its first dropped."""
    sql = f"""
        SELECT id, x FROM (VALUES (1, 1, 1), (2, 1, 0), (3, 0, 1), (4, 0, 0)) t(id, a, b)
        DECIDE x(INT)
        SUCH THAT x <= 3 AND sum(when {when}: x) <= 2
        MAXIMIZE sum(x)"""
    _, containers = construction(decidb_cli, sql, 1)
    graph = all_nodes(containers)
    gated = [node for node in graph.values() if reach(node)[1]]
    assert len(gated) == 1, containers
    assert reach(gated[0])[1] == counts, containers
    assert gated[0]["outgoing"] == kept, containers
    check_descent(containers, 4)


def animation(cli, sql):
    """The tree, and per row the containers after it (DECIDB_DUMP_STEPS at chunk size 1): per step, node -> line."""
    env = {
        **os.environ,
        **(cli.env or {}),
        "DECIDB_DUMP_TREE": "1",
        "DECIDB_DUMP_STEPS": "1",
        "DECIDB_DESCENT_CHUNK": "1",
    }
    result = subprocess.run(
        [cli.exe, "-c", "SET threads=1; SET decide_direct_solve='off'; " + sql],
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, result.stderr
    tree = result.stderr.split("DECIDB_TREE_BEGIN\n", 1)[1].split("DECIDB_TREE_END", 1)[0]
    steps = []
    for step in re.findall(r"DECIDB_STEP_BEGIN \d+\n(.*?)DECIDB_STEP_END", result.stderr, re.DOTALL):
        steps.append(dict(re.findall(r"^node (\d+) [EGOF]: ?(.*)$", step, re.MULTILINE)))
    return tree, steps


def node_of(tree, pattern):
    (match,) = re.findall(r"(?m)^(\d+) " + pattern, tree)
    return match


#: Pp. 86-91, one row a page: the root's set (d, region, mode, capacity), E(S.p, S.cid)'s classes under each region,
#: G(MAX, S.cid)'s rows per (region, customer), and the data G(SUM, S.m)'s rows per mode. Rows count from 0, the deck's
#: tid from 1; row 3 (tid 4, p = F) reaches the data reducer and stops at E(S.p) (p. 89).
F1_ANIMATION = [
    ("(d1) -> {0:(East, Air, 30)}", "(East)/(c1) -> {0:(East, c1)}", "(East, c1) -> {0}", "(Air) -> {0}"),
    ("(d1) -> {0:(East, Air, 30)}", "(East)/(c1) -> {0:(East, c1)}", "(East, c1) -> {0, 1}", "(Air) -> {0, 1}"),
    (
        "(d1) -> {0:(East, Air, 30), 2:(West, Ground, 50)}",
        "(East)/(c1) -> {0:(East, c1)} (West)/(c2) -> {2:(West, c2)}",
        "(East, c1) -> {0, 1} (West, c2) -> {2}",
        "(Air) -> {0, 1} (Ground) -> {2}",
    ),
    (
        "(d1) -> {0:(East, Air, 30), 2:(West, Ground, 50)}",
        "(East)/(c1) -> {0:(East, c1)} (West)/(c2) -> {2:(West, c2)}",
        "(East, c1) -> {0, 1} (West, c2) -> {2}",
        "(Air) -> {0, 1} (Ground) -> {2, 3}",
    ),
    (
        "(d1) -> {0:(East, Air, 30), 2:(West, Ground, 50)} (d2) -> {4:(West, Air, 45)}",
        "(East)/(c1) -> {0:(East, c1)} (West)/(c2) -> {2:(West, c2)} (West)/(c4) -> {4:(West, c4)}",
        "(East, c1) -> {0, 1} (West, c2) -> {2} (West, c4) -> {4}",
        "(Air) -> {0, 1, 4} (Ground) -> {2, 3}",
    ),
    (
        "(d1) -> {0:(East, Air, 30), 2:(West, Ground, 50)} (d2) -> {4:(West, Air, 45)}",
        "(East)/(c1) -> {0:(East, c1)} (West)/(c2) -> {2:(West, c2)} (West)/(c4) -> {4:(West, c4)}",
        "(East, c1) -> {0, 1} (West, c2) -> {2} (West, c4) -> {4, 5}",
        "(Air) -> {0, 1, 4, 5} (Ground) -> {2, 3}",
    ),
]


@pytest.mark.parametrize("inner", ["max", "sum"])
def test_the_descent_replays_the_decks_animation_row_by_row(decidb_cli, inner):
    """Pp. 83-91 at chunk size 1: after each row the containers hold what the deck's page for that row holds."""
    from ._anr_cases import F1_MAX, F1_SUM

    tree, steps = animation(decidb_cli, (F1_MAX if inner == "max" else F1_SUM).sql)
    nodes_shown = (
        node_of(tree, r"E root key=t\.id "),
        node_of(tree, r"E key=t\.cid [^\n]*\[p\] "),
        node_of(tree, rf"G key=t\.rid, t\.cid agg={inner} "),
        node_of(tree, r"G key=t\.m agg=sum "),
    )
    assert len(steps) == len(F1_ANIMATION), steps
    for row, (step, expected) in enumerate(zip(steps, F1_ANIMATION)):
        assert tuple(step[n] for n in nodes_shown) == expected, (row, step)


def test_the_descent_replays_the_frame_animation_row_by_row(decidb_cli):
    """Pp. 100-105: each row joins its product's timeline at its period, A's two and B's four."""
    from ._anr_cases import F2_PREVIOUS

    tree, steps = animation(decidb_cli, F2_PREVIOUS.sql)
    frame = node_of(tree, r"O key=t\.product ")
    assert [step[frame] for step in steps] == [
        "(A) -> {0@1}",
        "(A) -> {0@1, 1@2}",
        "(A) -> {0@1, 1@2} (B) -> {2@1}",
        "(A) -> {0@1, 1@2} (B) -> {2@1, 3@2}",
        "(A) -> {0@1, 1@2} (B) -> {2@1, 3@2, 4@3}",
        "(A) -> {0@1, 1@2} (B) -> {2@1, 3@2, 4@3, 5@4}",
    ], steps
