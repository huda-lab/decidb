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
