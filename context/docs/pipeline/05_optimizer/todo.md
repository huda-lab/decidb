# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## OPT-30 — A shared ABS takes the encoding its first reader needs

- **Problem.** A wrong answer. With `decide_direct_solve='off'` and `t` = ids 1..3, `x <= 3 AND x >= 0 AND x + sum(abs(x
  - 3)) <= 100 AND x + sum(abs(x - 3)) >= 50 MAXIMIZE sum(x)` returns `x = 3` on every row (the left side is at most
  12); with the two clauses swapped it is infeasible, on Gurobi and HiGHS. `TagAbsForBigM` decides whether an ABS needs
  its exact (Big-M) encoding from the clause it is written in (`decide_rewrite_abs.cpp`); the value rewrite then shares
  one helper across both clauses (`decide_rewrite_scoped.cpp`, `Helpers::Find`) and keeps the first clause's body, whose
  ABS is pushed down only, so the second clause reads a lower envelope alone. Definition families share bodies the same
  way (`decide_tree_definition.cpp`).
- **Change.** `RewriteAbs` decides each ABS's encoding once, walking every body from its owner with the push the owner
  gives it: a comparison from its relation, the objective down, a definition from its relation, a value link from its
  joined need flags (both sides mean Big-M). Both `TagAbsForBigM` passes, `TagAbsSubtreeForBigM` and
  `ABS_NEEDS_BIGM_TAG` go. The pattern is an inherited attribute read after the shared helpers have joined their
  readers.
- **Ruling (owner, 2026-11-21).** This design, not a flag OR-ed inside `Helpers::Find`, which leaves definitions open.
  It lands after OPT-25: until then a hard single MIN/MAX term reaches `RewriteAbs` as a tagged `SUM`
  (`EmitHardMinMaxClause`) and a flat MIN/MAX objective as a `SUM` with its extremum in `flat_objective_agg`, so a
  walk from the owners would read them as sums and give a hard extremum's members the cheap encoding. After OPT-25
  every MIN/MAX is a value link whose sides record its readers' push. The wrong answer stays open until then
  (`../../issues/bugs.md`).
- **Needs.** OPT-25.
- **Done when.** The clause-order pair, in both orders, agrees with the oracle (infeasible); `./test/decide/run_tests.sh
  -n 2 -k "abs"` passes, and with `DECIDB_FORCE_SOLVER=highs`.

## OPT-31 — A square outside a reducer in the objective is refused

- **Problem.** `MINIMIZE power(sum(x) - 3, 2)` (the squared distance of a total from a target) and `MINIMIZE (y -
  3) * (y - 3)` over a query-wide `y` are convex QPs, but the objective's linear form refuses any square outside a
  reducer (`decide_linear_form.cpp`, `'…' multiplies decisions outside a reducer, which is not supported`). The same
  square solves as a constraint (`power(sum(x) - 3, 2) <= 4`), and a product of two query-wide decisions is accepted
  in the objective.
- **Change.** A square of a linear expression outside a reducer becomes a quadratic objective term, as `sum(power(x -
  3, 2))` is: a reducer inside it becomes a helper (the value route), and the solver checks judge convexity and the
  installation as for any quadratic objective.
- **Needs.** BIND-4 (which removes the binder's `power` refusal) and OPT-25.
- **Done when.** Both examples agree with the oracle on Gurobi and HiGHS (continuous); `./test/decide/run_tests.sh -n
  2 -k "quadratic or objective"` passes.

## OPT-24 — A rewritten clause has one instance per distinct signature

- **Problem.** The value route gives a clause with no `per` one instance per row (`Helpers::ClassKeyOf`,
  `decide_rewrite_scoped.cpp`; a tree root with no `per`, `decide_tree.cpp`), so identical rows repeat: `sum(a*x) +
  max(b*x) <= (SELECT 40)` builds 25 rows where 13 hold the same model
  (`context/opa_rewrite/repro/minmax_two_models.sql`, which labels the 25-row model "value route"), and `sum(a*x) +
  sum(b*x) by (g) <= 40` builds 12 rows of which 3 are distinct. Under `per ():` the same clause builds the 13-row
  model.
- **Change.** A rewritten clause has one instance per distinct signature of what it reads; the duplicate rows go.
- **Ruling (owner, 2026-11-21).** This replaces the task's first diagnosis, which blamed the composed route.
- **Ruling (owner, 2026-11-21): one rule in every shape.** The model states each distinct constraint once. Today the
  aggregate shape merges equal instances (`per t.g: sum(x) + s <= t.b` over three classes with one `b` builds 1 row;
  guide §6.2, D6) while the tree keeps one per class (`per t.g: s + u <= t.b` builds 3 identical rows, the value route
  12 for the repro). The tree's roots key their instances by signature alone, not by class and signature, so the row
  shape merges too. Guide §7.1's "Equal bodies in two classes stay two instances, two identical model rows" becomes
  one model row, and `test_equal_values_in_two_classes_stay_two_instances` expects 1 model row (its oracle's 2
  instances stay). A clause that reads a per-row decision differs on every row and keeps its path at no new cost. A
  DIAGNOSE test whose output changes goes to the owner.
- **Ruling (provisional, god for the owner, 2026-11-21): `per K` merges on every field.** A `per K` root's signature
  leaves out the fields its key holds (`KeyHoldsField`: `length(t.g)` under `per t.g`, a `by` key inside the key), so
  dropping the class from the instance key merged unequal classes (42 failures, wrong answers in `test_anr_nested`). A
  root's identity across classes reads every field it reads, collected once; the naming list (variants, EXPLAIN
  ANALYZE) is that list minus the fields the key holds. If that is not a small change, two collected lists will do.
  Owner to confirm.
- **Done when.** Both spellings of the repro build the same rows; `./test/decide/run_tests.sh -n 2 -k "minmax or value
  or scoped"` passes.

## OPT-25 — MIN/MAX over decisions takes one route

- **Problem.** One MIN/MAX shape reaches the model by up to three routes, chosen by details of the clause: `sum + max <=
  K` is composed or value by whether the bound is a literal, has a `by` or has a decision beside it
  (`decide_canonicalizer.cpp`); a lone term with a factor of unknown sign, by its bound; an objective MIN/MAX is flat
  without a factor and composed with one (`MAXIMIZE max(b*x)` builds 14 rows, `MAXIMIZE 2 * max(b*x)` 26); beside a SUM
  it is composed unless under a function, a decision or a nest; the hard single term has its own path; `max(e) = K` is
  split across the easy and hard routes; the easy/hard classification is read twice (`IsCheapLoneExtremum`).
- **Change.** The deck's step-5 rule: a MIN or MAX over decisions is a helper with one-sided defining rows, chosen by
  the direction its reader pushes it (an epigraph auxiliary, Plaisted–Greenbaum). `RewriteScopedReducers` replaces every
  MIN/MAX site with a helper, in every clause shape and in the objective; SUM and AVG stay in place in an aggregate row
  and become helpers in a VALUE-shape clause as today. The canonicalizer's escalation to VALUE and `TakesComposedMinMax`
  go, with the composed and flat routes: `decide_rewrite_minmax.cpp` but the easy branch, `linearization_minmax.cpp`'s
  composed and hard emitters, `LogicalDecide`'s composed vectors and `flat_objective_*`, `minmax_clause_labels`, the
  tree's composed and flat roots, the model emitter's `Composed`, and the composed loops of the other passes.
  `EmitExtremumLink` keeps one caller. Re-measure the lines (the first count, about 700, predates the review's estimate
  of more than 1,000).
- **The one exception.** The easy identity `max(e) <= K` (every member `e <= K`; `=` splits into `<=` and `>=` as
  BETWEEN does) stays a stage-04 rewrite to a per-member clause, which folding takes into the bounds (D41). It is an
  identity applied before routing, not a route: on the helper, `max(b*x) <= 8` over 12 rows costs one variable and 13
  rows where it costs none, and leaves `x` its wider bounds.
- **Rulings (owner, 2026-11-21).** This design; the hard single term and the flat objective (once OPT-26) are part of
  it; `<>` on MIN/MAX is accepted on the helper, with oracle tests; the hard term's widening of the family for repair
  advice is not carried over unless a DIAGNOSE test shows the advice get worse, which goes to the owner.
- **Benchmark.** Against Gurobi, before and after: `benchmark/decide/queries/q4_minmax_nested.sql`,
  `q9_maximize_max.sql`, and the repro scaled up (`context/opa_rewrite/tools/ab_bench.py` holds no MIN/MAX workload).
- **Needs.** OPT-24.
- **Done when.** `grep -rn "composed" src` finds no route; `./test/decide/run_tests.sh -n 2 -k "minmax or max or
  objective"` passes, and with `DECIDB_FORCE_SOLVER=highs`.

## OPT-27 — ABS is lowered in one place

- **Problem.** `RewriteAbs` emits two comparison rows as expressions; stage 06 finds them back by index
  (`BuildAbsTagMap`), recovers the operand from one of them and clones them. ABS is the only construct whose
  indicator is allocated in stage 05, so a rewrite pass reads `use_native_constructs`; native against lowered is
  decided in three places under two policies. `SolverConstructSupport::bilinear` is never read.
- **Change.** Stage 05 records each ABS (helper, operand, exact or not) and stage 06 emits every row: stage 05 picks
  the shape, stage 06 the encoding. `bilinear` goes. About 150 lines go.
- **Needs.** OPT-30, which decides each ABS's encoding once in stage 05.
- **Done when.** `grep -rn "BuildAbsTagMap" src` finds nothing; `./test/decide/run_tests.sh -n 2 -k "abs"` passes,
  and with `DECIDB_FORCE_SOLVER=highs`.

## OPT-28 — Stage 05's passes share one enumerator and one polarity walker

- **Problem.** Stage 05 is 19 ordered passes over one tree and six side containers: 16 loops over side vectors, three
  polarity walkers (`WalkPolarity`, `CollectReducerSites`, `WalkComposedLhs`), two `ReadAffine`.
- **Change.** `LogicalDecide::EnumerateBodies(callback(DecideBody&))`, where a body carries its expression, push, rows,
  source alias and scope, with one fixed case per container, as DuckDB's `LogicalOperatorVisitor::EnumerateExpressions`
  has one per operator; every pass that walks bodies uses it. `CollectReducerSites` folds onto `WalkPolarity`.
  Bilinear's `ReadAffine` reads each factor with `DecideTermSplitter::Split` instead of its own reader.
- **Rulings (owner, 2026-11-21).** A BOOL decision factor counts as non-negative in every reader of a push (it is 0 or
  1); today `CollectReducerSites` counts it non-negative and `WalkPolarity` unknown. The splitter replaces bilinear's
  `ReadAffine`.
- **Needs.** OPT-25 (it deletes `WalkComposedLhs`).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "rewrite or polarity or minmax"` passes.

## OPT-29 — No rewrite pass reruns without a cause

- **Problem.** `RewriteBilinear` runs three times and `TagAbsForBigM` twice (OPT-30 deletes it); the objective is
  canonicalized 9 times, 1 at bind and 8 in stage 05, and 4 of those follow passes that only add tags the canonicalizer
  does not read (bilinear, ABS).
- **Change.** The 4 reinstalls after tag-only passes go. The second bilinear run goes and `RewriteScopedReducers` hands
  the objective's products to the bilinear rewrite always; the third becomes a worklist hook in `FindAbsAndAllocateAux`
  that tags a product once the ABS helper it reads exists (as LLVM's InstCombine revisits a new value's users).
- **Ruling (owner, 2026-11-21).** Not one post-order traversal: the ABS encoding, value helpers, definitions, MIN/MAX,
  `<>` and IN depend on the whole clause or query, not on one node and its children.
- **Needs.** OPT-28.
- **Done when.** `RewriteBilinear` runs once (`DECIDB_PROFILE=1`); `./test/decide/run_tests.sh -n 2 -k "bilinear or abs
  or rewrite"` passes.
