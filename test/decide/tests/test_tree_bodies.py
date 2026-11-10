"""The tree's bodies are stage 05's own terms (language/guide.md §6.4). Stage 05 lowers every clause once, for flat, and
names on each reducer's terms the tree's G they range over; the tree binds its clause roots to stage 05's clauses,
copies the terms and finds the node each is computed on. Each test reads DECIDB_DUMP_BODIES and names the branch it
reaches."""

import os
import re
import subprocess

import pytest

ROWS = "(VALUES (1, 'a', 3.0, 2.0, true), (2, 'a', 5.0, 1.0, false), (3, 'b', 1.0, 4.0, true)) t(id, g, c, w, p)"

TERM = re.compile(r"^\s+(term|inner) (\d+) var=(-?\d+) child=(-?\d+) site=(-?\d+) program=(-?\d+) label=(.*)$")


def run(cli, sql, **env):
    """The query with its tree and bodies dumped. The dumps print internal names, so the
    binary runs here rather than through the wrapper's user-output guard."""
    environment = {
        **os.environ,
        **(cli.env or {}),
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
                    "program": int(term[6]),
                    "label": term[7],
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


NULL_ROWS = "(VALUES (1, 'a', 3.0, 2.0, true), (2, 'a', NULL, 1.0, false), (3, 'b', 1.0, 4.0, true)) t(id, g, c, w, p)"


def decide(rows, decisions, clauses, objective="MAXIMIZE sum(x)"):
    return f"SELECT id FROM {rows} DECIDE {decisions} SUCH THAT {clauses} {objective}"


#: One query per branch of the program compile (CompileDecideTree), with the line of the bodies dump it must print.
#: Programs read their node's layout: its carried columns (#0, ...), then its program columns.
PROGRAMS = {
    # A coefficient reads a source column its site carries, by its place among them.
    "a-carried-column": (
        decide(ROWS, "x(REAL)", "x <= 9 AND sum(t.c * x) by (t.g) <= 5"),
        r"program \d+ COEFFICIENT expr=CAST\(#0 AS DOUBLE\) written=CAST\(c AS DOUBLE\) block=TERMS ",
    ),
    # A query-wide scale is read on the operand of the reducer it scales, which does not carry it: a program column,
    # which the coefficient's input then finds there.
    "a-program-column": (
        decide(ROWS, "x(REAL)", "x <= 9 AND (SELECT 2.0) * sum(t.c * x) by (t.g) <= 5"),
        r"column #1 reads input \d+\n  program 0 COEFFICIENT expr=\"\*\"\(CAST\(#1 AS DOUBLE\), CAST\(#0 AS DOUBLE\)\)"
        r"[^\n]*\n  program 1 VALUE expr=CAST\(#1 AS DOUBLE\) ",
    ),
    # A clause's bound is a program on its root.
    "a-bound": (
        decide(ROWS, "x(REAL), y(REAL)", "x <= 9 AND y <= 9 AND x + y <= t.c", "MAXIMIZE sum(x + y)"),
        r"program \d+ BOUND expr=CAST\(#0 AS DOUBLE\) written=CAST\(c AS DOUBLE\) block=BOUND group=0 position=0 ",
    ),
    # A bound that reads a frame and may differ within a group is the tightest one there (D85): a role of its own.
    "a-tightest-bound": (
        decide(ROWS, "x(REAL)", "x <= 9 AND sum(x) by (t.g) <= at(previous else 10: t.c) over (t.id within t.g)"),
        r"program 0 TIGHTEST_BOUND .* block=BOUND group=0 position=0 ",
    ),
    # An admitted bound computes a and r, then each end over the row and those two, read after the layout: r is #2
    # beside the one carried column (a, a constant, is in the ends already). It names its bound.
    "admitted-ends": (
        decide(ROWS, "x(REAL)", "x <= t.c"),
        r"bound rhs=1 lower=4 upper=5\n(?:.*\n)*  program 4 BOUND expr=[^\n]*\(\(#2 IS NULL\)\)[^\n]* "
        r"written=CAST\(c AS DOUBLE\) block=BOUND group=0 position=4 owner=0 arguments=2,3\n",
    ),
    # A member test is a program on its node.
    "a-member-test": (
        decide(NULL_ROWS, "x(REAL)", "x <= 9 AND sum(t.c * x) <= 5"),
        r"program \d+ TEST expr=\(#0 IS NOT NULL\) written=\(c IS NOT NULL\) block=FILTERS group=0 position=0 owner=-1",
    ),
    # A member test ranks in its reducer's filter, by the first term its node holds: w's linear term (0), not its
    # squared terms (2 and 3), which follow every linear term (DecideProgramCompiler::AddTests).
    "a-member-test-ranks-with-its-first-term": (
        decide(
            NULL_ROWS,
            "x(REAL), y(REAL)",
            "x <= 9 AND y <= 9",
            "MINIMIZE sum(power(x - t.c, 2) + t.w * x) + sum(t.c * y)",
        ),
        r"program \d+ TEST expr=\(#1 IS NOT NULL\) written=\(w IS NOT NULL\) block=FILTERS group=0 position=1 ",
    ),
    # A member test that reads a data reducer reads it as a hole, after the node's carried columns.
    "a-member-test-reads-a-data-reducer": (
        decide(ROWS, "x(REAL)", "x <= 9 AND sum(t.w * x * max(t.c) by (t.g)) <= 9"),
        r"  hole #2 reads node \d+\n(?:.*\n)*  program 2 TEST expr=\(CAST\(CASE  WHEN \(\(#1 IS NULL\)\) THEN \(NULL\) "
        r"ELSE #2 END AS DOUBLE\) IS NOT NULL\) ",
    ),
    # A read of a data reducer is a hole of its node's layout, after its carried and program columns, which the
    # bound and the admitted end that both read it share: the G's value in the slot the entry's row reads.
    "a-data-reducer": (
        decide(ROWS, "x(REAL)", "x <= 9 AND x <= max(t.c) by (t.g)"),
        r"  hole #1 reads node \d+\n(?:.*\n)*  program 1 BOUND expr=CAST\(CASE  WHEN \(\(#0 IS NULL\)\) THEN \(NULL\) "
        r"ELSE #1 END AS DOUBLE\) [^\n]*\n(?:.*\n)*  program 3 VALUE expr=CAST\(CASE  WHEN \(\(#0 IS NULL\)\) THEN "
        r"\(NULL\) ELSE #1 END AS DOUBLE\) ",
    ),
    # A data reducer's arguments are programs on its operand, which the G folds per slot.
    "a-data-reducers-arguments": (
        decide(ROWS, "x(REAL)", "x <= 9 AND x <= max(t.c) by (t.g)"),
        r"node \d+ entries\n  program 0 VALUE expr=#0 written=c block=FILTERS group=0 position=0 owner=-1\n",
    ),
    # A data check's message is a program on its hidden root.
    "a-data-check": (
        decide("(VALUES (1, 'a', 3.0::DOUBLE)) t(id, g, c)", "x(REAL)", "x <= max(t.c) by (t.g)"),
        r"node 0 entries\n  program 0 VALUE expr=CASE  WHEN \(\(#0 IS NULL\)\) THEN \(NULL\) ELSE CASE  WHEN "
        r"\(isnan\(#1\)\) THEN ",
    ),
    # A read of a frame is a hole of its reader's layout, the column of the frame's O at the reader's row.
    "a-frame": (
        decide(ROWS, "x(REAL)", "x <= 9 AND x - at(previous: x) over (t.id) <= 1"),
        r"  hole #0 reads node \d+ column 0\n  program 0 COEFFICIENT [^\n]*\n  program 1 COEFFICIENT expr=#0 "
        r"written=at\(previous: x\) over \(t.id\)__frame_column__ ",
    ),
    # A reducer over data reading a frame is its G's fold, whose operand reads the frame's one value as Lower read it.
    "a-reducer-over-a-frame": (
        decide(ROWS, "x(REAL)", "x <= 9 AND x <= sum(at(previous else 1: t.c) over (t.id) * t.w) by (t.g)"),
        r"  hole #0 reads node \d+\n  program 0 COEFFICIENT [^\n]*\n  program 1 BOUND expr=CAST\(#0 AS DOUBLE\) "
        r"(?:.*\n)*  hole #1 reads node \d+ column 0\n  program 0 VALUE expr=\"\*\"\(#1, ",
    ),
    # A point's O computes its payload and its `else` on its anchors, which its columns read.
    "a-points-payload-and-else": (
        decide(ROWS, "x(REAL)", "x <= 9 AND x <= at(previous else t.w: t.c) over (t.id)"),
        r"node \d+ entries\n  program 0 VALUE expr=#0 written=c [^\n]*\n  program 1 VALUE expr=#1 written=w ",
    ),
    # A range's O computes its reducer's argument on its anchors.
    "a-ranges-argument": (
        decide(ROWS, "x(REAL)", "x <= 9 AND x <= sum(from previous to next: t.c) over (t.id)"),
        r"node \d+ entries\n  program 0 VALUE expr=#0 written=c block=FILTERS ",
    ),
    # The objective's squared terms follow its terms.
    "objective-squares": (
        decide(ROWS, "x(REAL)", "x <= 9", "MINIMIZE sum(power(x - t.c, 2)) + sum(t.w * x)"),
        r"program 1 COEFFICIENT expr=CAST\(#0 AS DOUBLE\) written=CAST\(c AS DOUBLE\) block=TERMS group=0 position=2 ",
    ),
    # A helper's members are computed on its operand, a frame helper's else terms on its O after its members.
    "helper-members": (
        decide(ROWS, "x(REAL)", "x <= 9 AND sum(max(t.w * x) by (t.g)) <= 5"),
        r"members rank=links:0\n  term 0 var=0 child=-1 site=(\d+) program=0 [^\n]*\nnode \1 entries\n"
        r"  program 0 COEFFICIENT expr=CAST\(#0 AS DOUBLE\) written=CAST\(w AS DOUBLE\) block=TERMS ",
    ),
    "frame-else": (
        decide(ROWS, "x(REAL)", "x <= 9 AND x <= sum(from previous to next else t.w: x) over (t.id)"),
        r"  fallback\n    term 0 var=-1 child=-1 site=(\d+) program=1 [^\n]*\n(?:.*\n)*?  program 1 COEFFICIENT "
        r"expr=#\d+ [^\n]* block=TERMS group=0 position=1 ",
    ),
}


#: The branches whose query squares or multiplies decisions, which needs Gurobi: the solver check refuses such a query
#: before the tree's bodies are built, so these run through decidb_cli_gurobi, which skips without it.
GUROBI_PROGRAMS = {
    # A product's coefficient of 1 is left out by stage 05, so it has no program; a data one has.
    "a-product-of-one": (
        decide(ROWS, "x(REAL), y(REAL)", "x <= 9 AND y <= 9 AND x * y <= 10", "MAXIMIZE sum(x + y)"),
        r"product 0 vars=0,1 child=-1 site=\d+ program=-1",
    ),
    "a-product-coefficient": (
        decide(ROWS, "x(REAL), y(REAL)", "x <= 9 AND y <= 9 AND t.c * x * y <= 10", "MAXIMIZE sum(x + y)"),
        r"program \d+ COEFFICIENT expr=CAST\(#0 AS DOUBLE\) written=CAST\(c AS DOUBLE\) block=PRODUCTS ",
    ),
    # A square's terms are programs on its reducer's operand; its query-wide factor is read on the input's first row,
    # so it has none.
    "a-square": (
        decide(ROWS, "x(REAL)", "x <= 9 AND (SELECT 2.0) * sum(power(x - t.c, 2)) <= 50"),
        r"square 0 child=(\d+) sign=1\n    inner 0 var=0 child=\1 site=(\d+) program=0 [^\n]*\n(?:.*\n)*"
        r"node \2 entries\n(?:  .*\n)*  program 0 COEFFICIENT [^\n]* block=SQUARES group=0 position=1 ",
    ),
    # The objective's products are their own block.
    "objective-product": (
        decide(ROWS, "x(REAL), y(REAL)", "x <= 9 AND y <= 9", "MAXIMIZE sum(t.c * x * y)"),
        r"program 0 COEFFICIENT expr=COALESCE\(CAST\(#0 AS DOUBLE\), 0.0\) [^\n]* block=PRODUCTS group=0 position=0 ",
    ),
}


def assert_compiled(cli, sql, line):
    result = run(cli, sql)
    assert "DECIDB_BODIES_BEGIN" in result.stderr, result.stderr
    bodies = result.stderr.split("DECIDB_BODIES_BEGIN\n", 1)[1].split("DECIDB_BODIES_END", 1)[0]
    assert re.search(line, bodies), bodies


@pytest.mark.parametrize("case", list(PROGRAMS))
def test_a_program_is_compiled_on_its_site(decidb_cli, case):
    assert_compiled(decidb_cli, *PROGRAMS[case])


@pytest.mark.parametrize("case", list(GUROBI_PROGRAMS))
def test_a_quadratic_program_is_compiled_on_its_site(decidb_cli_gurobi, case):
    assert_compiled(decidb_cli_gurobi, *GUROBI_PROGRAMS[case])
