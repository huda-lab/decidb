"""The tree's bodies are stage 05's own terms (language/guide.md §6.4). Stage 05 lowers every clause once, for flat, and
names on each reducer's terms the tree's G they range over; the tree binds its clause roots to stage 05's clauses,
copies the terms and finds the node each is computed on. Each test reads DECIDB_DUMP_BODIES under
DECIDB_EVALUATOR=tree and names the branch it reaches."""

import os
import re
import subprocess

ROWS = "(VALUES (1, 'a', 3.0, 2.0, true), (2, 'a', 5.0, 1.0, false), (3, 'b', 1.0, 4.0, true)) t(id, g, c, w, p)"

TERM = re.compile(r"^\s+(term|inner) (\d+) var=(-?\d+) child=(-?\d+) site=(-?\d+) label=(.*)$")


def run(cli, sql, **env):
    """The query under the tree evaluator with its tree and bodies dumped. The dumps print internal names, so the
    binary runs here rather than through the wrapper's user-output guard."""
    environment = {
        **os.environ,
        **(cli.env or {}),
        "DECIDB_EVALUATOR": "tree",
        "DECIDB_DUMP_TREE": "1",
        "DECIDB_DUMP_BODIES": "1",
        **env,
    }
    return subprocess.run(
        [cli.exe, "-c", "SET threads=1; SET decide_direct_solve='off'; " + sql],
        env=environment,
        capture_output=True,
        text=True,
        timeout=120,
    )


def dumps(cli, sql, **env):
    """The node texts of the tree dump, by index, and the bodies dump's blocks: node -> (head, terms)."""
    result = run(cli, sql, **env)
    assert result.returncode == 0, result.stderr
    tree = result.stderr.split("DECIDB_TREE_BEGIN\n", 1)[1].split("DECIDB_TREE_END", 1)[0]
    texts = {int(m[1]): m[2] for m in re.finditer(r"(?m)^(\d+) [EGO] .* : (.*)$", tree)}
    text = result.stderr.split("DECIDB_BODIES_BEGIN\n", 1)[1].split("DECIDB_BODIES_END", 1)[0]
    blocks = {}
    for line in text.splitlines():
        head = re.match(r"^node (\d+) (.*)$", line)
        if head:
            terms = []
            blocks[int(head[1])] = (head[2], terms)
            continue
        term = TERM.match(line)
        if term:
            terms.append(
                {
                    "var": int(term[3]),
                    "child": int(term[4]),
                    "site": int(term[5]),
                    "label": term[6],
                }
            )
    return texts, blocks


def node(texts, text):
    """The one node whose text is `text`."""
    found = [index for index, written in texts.items() if written == text]
    assert len(found) == 1, (text, texts)
    return found[0]


def objective_terms(blocks):
    """The objective's body."""
    found = [terms for head, terms in blocks.values() if head.startswith("objective")]
    assert len(found) == 1, blocks
    return found[0]


def clause_block(blocks, root):
    head, terms = blocks[root]
    assert head.startswith("clause "), head
    return terms


def test_a_constraint_reducers_terms_name_its_operand(decidb_cli):
    """ExtractReducerConstraintTerms' stamp: the reducer's term ranges over its G, computed on the G's operand."""
    sql = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND sum(t.c * x) by (t.g) <= 5 MAXIMIZE sum(x)"
    texts, blocks = dumps(decidb_cli, sql)
    g = node(texts, "sum(t.c * x) by (t.g)")
    terms = clause_block(blocks, node(texts, "sum(t.c * x) by (t.g) <= 5"))
    assert [(t["var"], t["child"], t["site"]) for t in terms] == [(0, g, node(texts, f"{texts[g]} operand"))]


def test_lower_reads_the_operators_own_reducers_below_an_operator(decidb_cli):
    """Lower lowers a node's children from the operator's own expressions, so the reducer under `+` is the one stage 05
    lowers: with Lower copying first and recursing into the copy, stage 05's hook finds no node for it. The term beside
    the reducer has no child and is computed on its root."""
    sql = f"""SELECT id, x, y FROM {ROWS} DECIDE x(REAL), per t.g: y(REAL)
        SUCH THAT x <= 9 AND y <= 3 AND sum(x) by (t.g) + y <= 5 MAXIMIZE sum(x) + sum(y)"""
    texts, blocks = dumps(decidb_cli, sql)
    root = node(texts, "sum(x) by (t.g) + y <= 5")
    g = node(texts, "sum(x) by (t.g)")
    terms = clause_block(blocks, root)
    assert [(t["var"], t["child"], t["site"]) for t in terms] == [
        (0, g, node(texts, "sum(x) by (t.g) operand")),
        (1, -1, root),
    ]


def test_an_easy_extremums_terms_name_its_operand(decidb_cli):
    """ExtractEasyExtremumTerms' stamp: one row per member, so both terms range over the MAX's G."""
    sql = f"""SELECT id, x, y FROM {ROWS} DECIDE x(REAL), y(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND max(x + y) by (t.g) <= t.c MAXIMIZE sum(x + y)"""
    texts, blocks = dumps(decidb_cli, sql)
    g = node(texts, "max(x + y) by (t.g)")
    operand = node(texts, "max(x + y) by (t.g) operand")
    terms = clause_block(blocks, node(texts, "max(x + y) by (t.g) <= t.c"))
    assert [(t["var"], t["child"], t["site"]) for t in terms] == [(0, g, operand), (1, g, operand)]


def test_each_objective_reducer_names_its_operand(decidb_cli):
    """ExtractReducerObjectiveTerms' stamp: an objective of several reducers, each term on its own G."""
    sql = (
        f"SELECT id, x, y FROM {ROWS} DECIDE x(REAL), y(REAL) SUCH THAT x <= 9 AND y <= 9 MAXIMIZE sum(x) + 2 * sum(y)"
    )
    texts, blocks = dumps(decidb_cli, sql)
    terms = objective_terms(blocks)
    assert [(t["var"], t["child"]) for t in terms] == [(0, node(texts, "sum(x)")), (1, node(texts, "sum(y)"))]


def test_a_lone_objective_reducer_names_its_operand(decidb_cli):
    """AnalyzeObjective's stamp: an objective that is one reducer."""
    sql = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 3 MAXIMIZE sum(t.w * x)"
    texts, blocks = dumps(decidb_cli, sql)
    g = node(texts, "sum(t.w * x) operand") - 1
    assert texts[g] == "sum(t.w * x)"
    terms = objective_terms(blocks)
    assert [(t["var"], t["child"], t["site"]) for t in terms] == [(0, g, g + 1)]


def test_an_inlined_nest_ranges_over_the_inner_reducer(decidb_cli):
    """The objective's nest is inlined for flat and kept for the tree (D5): stage 05 lowers the inlined reducer, which
    the retained branch of Lower maps to the nest, and ReducerChild descends to the inner G, whose entries are the rows
    the inlined sum reads."""
    sql = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 3 MAXIMIZE sum(per t.g: sum(t.w * x) by (t.g))"
    texts, blocks = dumps(decidb_cli, sql)
    inner = node(texts, "sum(t.w * x) by (t.g)")
    terms = objective_terms(blocks)
    assert [(t["var"], t["child"], t["site"]) for t in terms] == [(0, inner, node(texts, f"{texts[inner]} operand"))]


def test_a_folded_bound_with_repair_rows_binds_a_singleton_row(decidb_cli):
    """A bound folded into the box that keeps its repair rows is stage 05's singleton row, numbered after every
    constraint (flat's repair_group_id)."""
    sql = f"SELECT id, x, y FROM {ROWS} DECIDE x(INT), y(INT) SUCH THAT x >= 4 AND x <= 9 AND x + y <= 20 MAXIMIZE sum(x + y)"
    texts, blocks = dumps(decidb_cli, sql)
    assert blocks[node(texts, "x + y <= 20")][0] == "clause 0 rank=constraints:0"
    assert blocks[node(texts, "x >= 4")][0] == "clause 1 rank=constraints:1"
    assert blocks[node(texts, "x <= 9")][0] == "clause 2 rank=constraints:2"


def test_a_folded_bound_without_repair_rows_binds_no_clause(decidb_cli):
    """A BOOLEAN domain gets no repair rows, so its folded bound is no clause of stage 05's."""
    sql = f"SELECT id, x, b FROM {ROWS} DECIDE x(INT), b(BOOL) SUCH THAT x <= 4 AND b <= 1 AND x + b <= 5 MAXIMIZE sum(x + b)"
    texts, blocks = dumps(decidb_cli, sql)
    assert node(texts, "b <= 1") not in blocks, blocks
    assert blocks[node(texts, "x + b <= 5")][0] == "clause 0 rank=constraints:0"


def test_a_reducer_the_tree_has_no_node_for_is_internal(decidb_cli):
    """ReducerChild's missing node, reached by DECIDB_EVALUATOR_INJECT=g1, which skips its lookup."""
    sql = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND sum(t.c * x) by (t.g) <= 5 MAXIMIZE sum(x)"
    result = run(decidb_cli, sql, DECIDB_EVALUATOR_INJECT="g1")
    assert "INTERNAL Error: DECIDE tree: stage 05 lowers a reducer the tree has no node for" in result.stderr


def test_a_value_links_members_are_computed_on_its_operand(decidb_cli):
    """A helper's G holds its reducer's members, computed on the G's operand and ranked with the value links."""
    sql = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND sum(max(x) by (t.g)) <= 5 MAXIMIZE sum(x)"
    texts, blocks = dumps(decidb_cli, sql)
    link = node(texts, "max(x) by (t.g)")
    head, terms = blocks[link]
    assert head.startswith("members rank=links:"), head
    assert [(t["var"], t["site"]) for t in terms] == [(0, node(texts, "max(x) by (t.g) operand"))]


def test_a_frame_links_members_and_else_are_computed_on_its_frame(decidb_cli):
    """A range frame's helper is its O, which holds the frame's positions: its members and its `else` are computed on
    the O itself."""
    sql = f"""SELECT id, x FROM {ROWS} DECIDE x(REAL)
        SUCH THAT x <= 9 AND x <= sum(from previous to next else 1: x) over (t.id) MAXIMIZE sum(x)"""
    texts, blocks = dumps(decidb_cli, sql)
    frame = node(texts, "sum(from previous to next else 1: x) over (t.id)")
    head, terms = blocks[frame]
    assert head == "members rank=links:0"
    assert [t["site"] for t in terms] == [frame, frame]


def test_a_composed_clause_raises_after_the_objective(decidb_cli):
    """A root takes the rank of the stage-05 list its body comes from: flat evaluates the composed clauses after the
    objective, so the objective's error is the one raised."""
    sql = """SELECT id, x FROM (VALUES (1, -1.0, -1.0), (2, 1.0, 2.0)) t(id, a, b) DECIDE x(REAL)
        SUCH THAT x <= 4 AND sum(x) + max(ln(t.a) * x) <= 100 MAXIMIZE sum(sqrt(t.b) * x)"""
    result = run(decidb_cli, sql)
    assert "cannot take square root of a negative number" in result.stderr, result.stderr
    bodies = result.stderr.split("DECIDB_BODIES_BEGIN\n", 1)[1].split("DECIDB_BODIES_END", 1)[0]
    assert re.search(r"(?m)^node \d+ composed 0 rank=composed:0$", bodies), bodies


def test_an_objective_of_composed_terms_alone_has_no_objective_body(decidb_cli):
    """With nothing beside its MIN/MAX terms, the objective stage 05 lowers is empty; the composed terms take the
    objective's slot, after every composed clause."""
    sql = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND x >= 1 MAXIMIZE max(x) + min(x)"
    texts, blocks = dumps(decidb_cli, sql)
    assert not any(head.startswith("objective") for head, _ in blocks.values()), blocks
    members = [head for head, _ in blocks.values() if head.startswith("members")]
    assert members == ["members rank=composed:0", "members rank=composed:0"], blocks


def test_flat_dumps_the_bodies_of_the_tree_a_dump_requests(decidb_cli):
    """Under flat, a dump variable builds the tree beside flat's plan, and the shadow descent prints its bodies too."""
    sql = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND sum(t.c * x) by (t.g) <= 5 MAXIMIZE sum(x)"
    result = run(decidb_cli, sql, DECIDB_EVALUATOR="flat")
    assert result.returncode == 0, result.stderr
    bodies = result.stderr.split("DECIDB_BODIES_BEGIN\n", 1)[1].split("DECIDB_BODIES_END", 1)[0]
    assert re.search(r"(?m)^node \d+ clause 0 rank=constraints:0$", bodies), bodies
