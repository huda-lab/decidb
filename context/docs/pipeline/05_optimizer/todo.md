# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## OPT-21 — The solver path depends on no direct-solve code

- **Problem.** `DecideOptimizer::RetainRowChecks` builds a whole `DirectFrontEnd` to copy one vector; a check passes
  through `DecideScopeCheck`, `DirectRowCheck` and an `Expression`; `DecideScopeCheckKind` has one value. Pass 0
  (`decide_rewrite_rhs_bounds.cpp`), `decide_rewrite_abs.cpp`, `decide_singleton_row_folding.cpp` and
  `decide_optimizer.cpp` include `direct/` headers for general helpers (`ExpressionBuilder`, the number-message and
  kept-row helpers).
- **Change.** One vector of value checks on `LogicalDecide`; the kind enum and both wrapper types go. The general
  helpers move to `src/optimizer/decide/`; no solver-path file includes a `direct/` header.
- **Needs.** FW-5 (it edits pass 0's bound records).
- **Done when.** `grep -rln "decide/direct/" src/optimizer/decide/*.cpp` lists only the dispatch file;
  `./test/decide/run_tests.sh -n 2 -k "bound_side_tables or abs or nan"` passes.

## OPT-24 — A rewritten MIN/MAX clause is scoped once per instance

- **Problem.** `sum(a*x) + max(b*x) <= 40` builds 13 rows; the same clause with a subquery bound builds 25
  (`context/opa_rewrite/repro/minmax_two_models.sql`): the composed route scopes the rewritten clause again for each
  instance.
- **Change.** The rewritten clause is scoped once per instance; the duplicate rows go.
- **Done when.** Both spellings build the same rows; `./test/decide/run_tests.sh -n 2 -k "minmax or max"` passes.

## OPT-25 — MIN/MAX inside a sum takes the value route

- **Problem.** MIN/MAX has five routes into one emitter (`EmitExtremumLink`): easy single term, hard single term,
  composed constraint, composed or flat objective, and the value route; the canonicalizer picks by shape. "composed"
  is on 364 lines in 17 files.
- **Change.** A decision reducer beside other terms takes the value route; the composed route and its two side
  vectors on `LogicalDecide` go (about 700 lines). `tools/ab_bench.py` shows no workload moving beyond about 10%.
- **Needs.** OPT-24.
- **Done when.** `grep -rn "composed" src` finds no route; `./test/decide/run_tests.sh -n 2 -k "minmax or max"`
  passes.

## OPT-26 — The hard single term and the flat objective take the value route

- **Problem.** After OPT-25, two routes beside the value route remain: the hard single term and the flat objective.
- **Change.** Both take the value route; only the easy single term keeps its own row.
- **Needs.** OPT-25.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "minmax or max or objective"` passes.

## OPT-27 — ABS is lowered in one place

- **Problem.** `RewriteAbs` emits two comparison rows as expressions; stage 06 finds them back by index
  (`BuildAbsTagMap`), recovers the operand from one of them and clones them. ABS is the only construct whose
  indicator is allocated in stage 05, so a rewrite pass reads `use_native_constructs`; native against lowered is
  decided in three places under two policies. `SolverConstructSupport::bilinear` is never read.
- **Change.** Stage 05 records each ABS (helper, operand, exact or not) and stage 06 emits every row: stage 05 picks
  the shape, stage 06 the encoding. `bilinear` goes. About 150 lines go.
- **Done when.** `grep -rn "BuildAbsTagMap" src` finds nothing; `./test/decide/run_tests.sh -n 2 -k "abs"` passes,
  and with `DECIDB_FORCE_SOLVER=highs`.

## OPT-28 — Stage 05's passes share one enumerator and one polarity walker

- **Problem.** Stage 05 is 19 ordered passes over one tree and six side containers: 16 loops over side vectors,
  three polarity walkers (`WalkPolarity`, `CollectReducerSites`, `WalkComposedLhs`), two `ReadAffine`.
- **Change.** One enumerator of every body, used by every pass; `CollectReducerSites` folds onto `WalkPolarity`.
- **Needs.** OPT-25 (it deletes `WalkComposedLhs`).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "rewrite or polarity or minmax"` passes.

## OPT-29 — The node-local rewrites run in one post-order traversal

- **Problem.** `RewriteBilinear` runs three times and `TagAbsForBigM` twice; the objective is re-canonicalized nine
  times on a query no pass alters.
- **Change.** One post-order traversal applies the node-local constructs, so no pass reruns.
- **Needs.** OPT-28.
- **Done when.** Each pass runs once per query (`DECIDB_PROFILE=1`); `./test/decide/run_tests.sh -n 2 -k "bilinear
  or abs or rewrite"` passes.

## OPT-30 — Two value checks of one comparison

- **Problem.** `AddAbsValueCheck` (`decide_rewrite_abs.cpp`) drops a duplicate by comparing two checks including their
  place, and `AddValueCheck` (`decide_rewrite_rhs_bounds.cpp`) compares them its own way. Two checks that differ only in
  place are one check with two carets.
- **Change.** One comparison, shared by both, which ignores the place.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "abs or bound_side_tables"` passes.

## OPT-32 — Optimizer labels are built on every query

- **Problem.** `minmax_clause_labels`, `aux_var_expressions` and `link.label` are display state built for every clause,
  in time proportional to the clauses, and no `diagnose` gate stands before them.
- **Change.** They are built under `EXPLAIN` and `DIAGNOSE` only.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "explain or diagnose"` passes with no golden change.
