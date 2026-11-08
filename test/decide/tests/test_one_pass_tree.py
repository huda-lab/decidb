"""The E/G/O construction must route rows and retain the deck's containers."""

import re
import os
import subprocess

import pytest

from ._anr_cases import F1_MAX, F1_SUM, F2_PREVIOUS


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


@pytest.mark.parametrize("case", [F1_MAX, F1_SUM, F2_PREVIOUS], ids=lambda case: case.name)
def test_deck_construction_is_independent_of_chunk_size(decidb_cli, case):
    expected = construction(decidb_cli, case.sql, 1)
    for chunk in [7, 2048]:
        assert construction(decidb_cli, case.sql, chunk) == expected


@pytest.mark.parametrize("predicate,kept", [("t.a OR t.b", 3), ("t.a AND t.b", 0)])
def test_descent_respects_boolean_structure_and_sql_null(decidb_cli, predicate, kept):
    sql = f"""SELECT id,x FROM
        (VALUES (1,true,false),(2,false,true),(3,NULL,true),(4,false,NULL)) t(id,a,b)
        DECIDE x(REAL) SUCH THAT x<=9 AND when {predicate}: x<=3
        MAXIMIZE sum(x) ORDER BY id"""
    _, containers = construction(decidb_cli, sql)
    counts = [(int(a), int(b)) for a, b in re.findall(r"rows (\d+) -> (\d+)", containers)]
    assert (4, kept) in counts, containers


@pytest.mark.parametrize("case", [F1_MAX, F1_SUM], ids=lambda case: case.name)
def test_deck_data_reducer_retains_six_values_in_two_mode_groups(decidb_cli, case):
    """Slide 91 stores Air's four values and Ground's two; it has not folded them."""
    _, containers = construction(decidb_cli, case.sql)
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


def test_deck_previous_frame_builds_two_complete_ordered_timelines(decidb_cli):
    """Slide 105 has A positions 1,2 and B positions 1,2,3,4 before selection."""
    _, containers = construction(decidb_cli, F2_PREVIOUS.sql)
    frames = [node for node in nodes(containers) if node["kind"] == "O"]
    assert len(frames) == 1, containers
    frame = frames[0]
    assert (frame["groups"], frame["entries"]) == (2, 6), containers
    assert "key=(A)" in frame["body"] and "key=(B)" in frame["body"], containers
    timelines = re.findall(r"timeline \d+ \{([^}]*)\}", frame["body"])
    assert timelines == ["1:1;2:1;", "1:1;2:1;3:1;4:1;"], containers


def test_a_null_per_key_has_no_entry_but_does_not_stop_its_reducer(decidb_cli):
    """The NULL-per row contributes 7 to sum(d), despite producing no E instance."""
    sql = """SELECT id,x FROM (VALUES (1,1,2),(2,NULL,7)) t(id,g,d)
        DECIDE per (): x(REAL)
        SUCH THAT per t.g: x <= sum(t.d)
        MAXIMIZE x ORDER BY id"""
    _, containers = construction(decidb_cli, sql)
    state = nodes(containers)
    roots = [n for n in state if n["kind"] == "E" and n["incoming"] == 2 and n["entries"] == 1]
    assert roots, containers
    assert any(n["outgoing"] == 2 for n in roots), containers
    operands = [n for n in state if n["kind"] == "E" and n["text"].endswith(" operand")]
    assert any(n["entries"] == 2 and "data=(7)" in n["body"] for n in operands), containers


def test_shared_frame_inserts_each_source_row_once(decidb_cli):
    """Both halves of MAX equality read the same O, which retains two entries."""
    sql = """SELECT id,x FROM (VALUES (1,1,5),(2,2,7)) t(id,s,d)
        DECIDE x(INT)
        SUCH THAT x<=9 AND max(x) = at(first:t.d) over (t.s)
        MAXIMIZE sum(x) ORDER BY id"""
    tree, containers = construction(decidb_cli, sql)
    state = nodes(containers)
    # Shared nodes are referenced by more than one tree edge. Regardless of
    # the readers' order, a reducer owns one input entry per source row.
    referenced = re.findall(r"reads=\[([^]]*)\]", tree)
    child_ids = [int(n) for group in referenced for n in re.findall(r"\d+", group)]
    shared = {node for node in child_ids if child_ids.count(node) > 1}
    shared_frames = [node for node in state if node["kind"] == "O" and node["id"] in shared]
    assert len(shared_frames) == 1, tree
    frame = shared_frames[0]
    assert (frame["incoming"], frame["outgoing"], frame["entries"]) == (2, 2, 2), containers


#: A grouped root with no `per` holds one entry per group of its `by` key (decide_tree.cpp, GroupClass), where it held
#: one per row, each reading its group's whole value. Each case reaches a cause no other case reaches.
GROUPED_ROOT_CASES = {
    # GroupClass from Build: a composed extremum is one model row.
    "a-composed-extremum": ("max(t.w * x) - min(x) <= 5", 1),
    # An easy extremum's member test, w > 1, is read by its operand alone. The root has no column of w, which kept an
    # entry per value of it in group a, nor a field, which made each an instance.
    "an-easy-extremum-with-a-member-test": ("max(when t.w > 1: x) by (t.g) <= 2", 2),
}


@pytest.mark.parametrize("case", list(GROUPED_ROOT_CASES))
def test_a_grouped_root_holds_one_entry_per_group(decidb_cli, case):
    clause, groups = GROUPED_ROOT_CASES[case]
    sql = f"""SELECT id, x FROM (VALUES (1, 'a', 1.0), (2, 'a', 2.0), (3, 'b', 3.0)) t(id, g, w) DECIDE x(INT)
        SUCH THAT x <= 3 AND {clause} MAXIMIZE sum(x) ORDER BY id"""
    _, containers = construction(decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"}), sql)
    roots = [node for node in nodes(containers) if node["text"] == clause]
    assert [(node["entries"], node["instances"]) for node in roots] == [(groups, groups)], containers


def test_a_frame_with_no_else_keeps_one_entry_per_variant(decidb_cli):
    """A set E's entry carries where its row reads a frame (decide_descent.cpp, Visit): at the anchor's own row only
    for the frame's `else`, which reads it there (language/guide.md §5.6), else at its timeline. So `per ()` over
    `at(last: t.v)` holds one entry. One per row made each insert scan every earlier one, and a grouped clause fold
    every row: `sum(x) <= at(last: t.v) over (t.p)` over 40,000 rows took 218 s against 0.4 s."""
    clause = "per (): y <= at(last: t.v) over (t.p)"
    sql = f"""SELECT id, y FROM (VALUES (1, 1, 4), (2, 2, 6), (3, 3, 7)) t(id, p, v) DECIDE per (): y(INT)
        SUCH THAT y <= 9 AND {clause} MAXIMIZE y ORDER BY id"""
    _, containers = construction(decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"}), sql)
    roots = [node for node in nodes(containers) if node["text"] == clause]
    assert [(node["entries"], node["instances"]) for node in roots] == [(1, 1)], containers


def settled(cli, sql, text):
    """Each entry's settled instance in the values dump of the node written `text`, `-` where it holds none."""
    env = {**os.environ, **(cli.env or {}), "DECIDB_DUMP_VALUES": "1"}
    result = subprocess.run(
        [cli.exe, "-c", "SET threads=1; SET decide_direct_solve='off'; " + sql],
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, result.stderr
    values = result.stderr.split("DECIDB_VALUES_BEGIN\n", 1)[1].split("DECIDB_VALUES_END", 1)[0]
    return re.findall(r" instance (\S+)", values.split(f" {text}\n", 1)[1].split("\nnode ", 1)[0])


#: E settles into variants (decide_tree_evaluation.cpp, Scope): an entry whose variant an earlier entry holds reads
#: what that one reads. An `else` keeps each anchor's entry apart until O settles, and `first` never reaches it, so
#: each timeline is one variant. Each case reaches one arm.
SETTLED_CASES = {
    # A clause in the grouped shape folds each variant of its group once: group a's second entry holds none.
    "grouped": ("sum(x) by (t.g) <= at(first else t.w: t.v) over (t.p within t.g)", ["0", "-", "1"]),
    # Any other E maps each entry to its variant's instance, which a reducer over it reads once.
    "per-one": ("per (): y <= at(first else t.w: t.v) over (t.p)", ["0", "0", "0"]),
}


@pytest.mark.parametrize("case", list(SETTLED_CASES))
def test_an_entry_settles_into_its_variant(decidb_cli, case):
    clause, instances = SETTLED_CASES[case]
    sql = f"""SELECT id, x FROM (VALUES (1, 'a', 1, 4, 5), (2, 'a', 2, 6, 1), (3, 'b', 3, 7, 2)) t(id, g, p, v, w)
        DECIDE x(INT), per (): y(INT) SUCH THAT x <= 3 AND y <= 9 AND {clause} MAXIMIZE sum(x) + y ORDER BY id"""
    assert settled(decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"}), sql, clause) == instances


_PINNED_ROWS = (
    "(VALUES (1, 'a', 'East', 'c1', TRUE, 20, 30), (2, 'a', 'East', 'c1', TRUE, 15, 30), "
    "(3, 'a', 'West', 'c2', FALSE, 10, 50), (4, 'b', 'West', 'c3', TRUE, 25, 45)) t(id, g, r, k, p, d, c)"
)

#: The tree dump of three queries as the construction wrote it before stage 05 lowered the clauses between its two
#: halves (DecideTreeConstruction): Lower now lowers a node's children from the operator's own expressions, and the
#: nodes, edges, sigma and order it writes are the same.
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
    tree, _ = construction(
        decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"}), f"SELECT id FROM {_PINNED_ROWS} DECIDE {decide}"
    )
    assert tree == expected
