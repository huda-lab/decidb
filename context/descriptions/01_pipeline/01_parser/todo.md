# Stage 01 — Parser: open work

---

## Fix the `WHEN` grammar so `decide_grammar_repair.cpp` can be deleted

**Pointers**: `third_party/libpg_query/grammar/statements/select.y:441-477`
(`decide_constraint_item`) and `:291-322` (`decide_objective_item`);
`third_party/libpg_query/grammar/grammar.y` header comment (the `%expect 9`
budget); `src/decidb/parsed/decide_grammar_repair.cpp`.

`RepairDecideConstraintGrammar` and `RepairDecideObjectiveGrammar` exist only
because `a_expr WHEN_DECIDE b_expr` mis-associates in two ways:

1. the left `a_expr` absorbs `AND`, so `A AND B WHEN c` wraps the whole
   conjunction (and the same for `PER`);
2. `WHEN_DECIDE` has no declared precedence, so a comparison closes the `WHEN`
   early — `SUM(x) WHEN a > b` parses as `(SUM(x) WHEN a) > b`.

**Decision**: give `WHEN_DECIDE` a precedence below the comparison operators and
narrow the nonterminal on its left. Both repairs then become unreachable and the
file drops to just `ExpressionToDot` (see below).

**Why it is filed separately**: the nine conflicts are documented as deliberately
shift-resolved, so the change needs `make grammar-build` with bison 2.3 and a
regression pass over every DECIDE parse path — both clause orders, the
aggregate-local form, the qualified-reducer form, and `CASE` inside a DECIDE
clause. It has no canonicalization dependency in either direction.

**Test**: `test/decide/tests/test_when_grammar.py` plus the three repair shapes
(`A AND B WHEN c`, `A AND B PER col`, `MAXIMIZE SUM(x) WHEN a > b`) must produce
byte-identical models before and after. The `%expect` count must go down, not up.

**Done file**: `done.md` §4 — delete the association-repair table and the
`decide_grammar_repair.cpp` row from the source map.

---

## Move the parsed-tree rewrites out of `bind_select_node.cpp`

**Pointers**: `src/planner/binder/query_node/bind_select_node.cpp:1009-1109`.

Eight parsed-tree operations run there before any DECIDE binder does:
`RewriteScopedVarRefs`, `ValidateDecideNoExplicitDecisionCasts`,
`RewriteNormL0`, `RewriteNorm`, `RewriteInDomain`,
`ValidateDecideNoNonLinearScalar`, the two grammar repairs, and
`TagDecideSourceFragments`. All of them operate on `ParsedExpression`s and none
of them needs a bound tree, so they are stage 01 work living in a stage 02 file.
`RewriteInDomain` and the two `norm` rewrites also *synthesize* constraints,
which makes `bind_select_node.cpp` a producer of DECIDE syntax as well as its
consumer.

**Decision needed before starting**: whether these move to a new
`src/decidb/parsed/decide_desugar.cpp` alongside the grammar repair, or whether
`RewriteInDomain` / `RewriteNorm*` are better modelled as optimizer rewrites now
that `LogicalDecide::AddConstraint` exists as a canonicalizing entry point. The
second reading is not obviously wrong: they choose a *formulation* (binary
indicators, linking rows), which is stage 05's job, and they currently bypass the
canonicalization boundary entirely by emitting parsed nodes.

**Test**: all 80 golden models byte-identical; `test/decide/tests/test_norm*.py`
and the IN-domain cases unchanged.

**Done file**: `done.md` §4 — drop the "physically live in
bind_select_node.cpp" caveat and repoint the source map.

---

## `ExpressionToDot` has no live callers

**Pointers**: `src/decidb/parsed/decide_grammar_repair.cpp:232-338`;
call sites at `src/planner/binder/query_node/bind_select_node.cpp:1091, 1100,
1103, 1109`, all commented out.

~110 lines of Graphviz dumping for parsed expressions, reachable only by
uncommenting a debug line. It is genuinely useful when debugging an association
bug, which is the one thing this stage still has open.

**Decision**: keep it until the grammar fix above lands (it is the tool for
verifying that fix), then delete it with the rest of the file. If it should
survive, it needs a real entry point — a `PRAGMA` or a debug session setting —
rather than four commented-out lines.

**Test**: n/a for deletion.

**Done file**: `done.md` §4 — remove the closing paragraph.
