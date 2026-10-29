# Work queue: every open task in the docs, by feature

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `language/guide.md`, `direct_solve/README.md`, `issues/bugs.md`.
Checks: `direct_solve/framework/todo.md` § How to work any task in these todo files.

Each batch finishes completely. One working tree: agents may edit disjoint batches side by side; one builds and tests
at a time. Lanes: F1 then F5; F2 then F3; F6; F7 then F8. F4 never beside F2 or F5. F9 runs alone.

## 1. Owner decisions (independent of each other; ask all first)

- [ ] F2: BUG-4 parser plan, BUG-5 route, LP-2 four choices
- [ ] F3: INF-3 six calls, INF-1 which check, INF-2 relation key label
- [ ] F4: CQ-1 five calls, CQ-3 names, CQ-4 encoding, CAN-2 four choices, OPT-2 indicator scope
- [ ] F5: CAN-1 linearizer and objective, OPT-3 helper definition
- [ ] F6: H-04 message order, H-03 where the reduction lives, CQ-2 six calls
- [ ] F7: BIN-1 three rulings
- [ ] F9: OPS-2 rename the names too, or not

## 2. F1 Numerics

- [ ] FRM-1 Tighten the Big-M of ABS, `<>`, hard MIN/MAX, IN — pipeline/06_model_formulation/todo.md § FRM-1

## 3. F2 Messages quote what the user wrote (needs: 1)

- [ ] BUG-7 One quoting function for every message — issues/bugs.md § BUG-7
- [ ] BUG-5 Relation names in stage-05 messages — issues/bugs.md § BUG-5
- [ ] BUG-4 Source spans instead of DuckDB's rendering — issues/bugs.md § BUG-4
- [ ] LP-2 Display text per occurrence; qualify over a join — pipeline/03_logical_plan/todo.md § LP-2
- [ ] BUG-8 One name form, one key text per clause — issues/bugs.md § BUG-8 (needs: LP-2)

## 4. F3 Diagnosis output (needs: 1, F2)

- [ ] INF-3 A `BETWEEN` is quoted whole — diagnostics/infeasible/todo.md § INF-3
- [ ] INF-1 An `=` on MIN/MAX is quoted as written — diagnostics/infeasible/todo.md § INF-1 (needs: INF-3)
- [ ] BUG-10 A quadratic clause reported once in the query scope — issues/bugs.md § BUG-10
- [ ] BUG-11 HiGHS's expanded repair carries a 1e-5 cushion — issues/bugs.md § BUG-11
- [ ] INF-2 A relation key's NULL reads like `'NULL'` — diagnostics/infeasible/todo.md § INF-2

## 5. F4 One clause scope and keyed model (needs: 1; never beside F2 or F5)

- [ ] CQ-1 One clause scope (D47) — issues/code_quality.md § CQ-1
- [ ] CQ-3 Name the keyed scope "keyed" (D46) — issues/code_quality.md § CQ-3 (needs: CQ-1)
- [ ] TST-2 Rename the tests and markers, in CQ-3's change — testing/todo.md § TST-2 (needs: CQ-1)
- [ ] CQ-4 One encoding of "count once" — issues/code_quality.md § CQ-4
- [ ] CAN-2 A keyed decision beside a reducer, no helper — pipeline/04_canonicalizer/todo.md § CAN-2
- [ ] OPT-2 `IN` under `per K:` once per class — pipeline/05_optimizer/todo.md § OPT-2

## 6. F5 Helper decisions (needs: 1, F1)

- [ ] OPT-1 A box per helper instance — pipeline/05_optimizer/todo.md § OPT-1
- [ ] CAN-1 A `BOOL` decision as a factor on a reducer — pipeline/04_canonicalizer/todo.md § CAN-1
- [ ] OPT-3 A quadratic SUM behind a helper decision — pipeline/05_optimizer/todo.md § OPT-3

## 7. F6 Direct solve (needs: 1)

- [ ] H-04 Keyed scope checks in the direct plan — direct_solve/framework/todo.md § H-04
- [ ] H-03 Read a data bound through the bound-side window — direct_solve/framework/todo.md § H-03
- [ ] CQ-2 One step chooses how a query is answered — issues/code_quality.md § CQ-2
- [ ] H-05 Read a reducer's `by` key into the IR — direct_solve/framework/todo.md § H-05 (needs: H-04)
- [ ] S1 reads `by` again — direct_solve/rules/s1_uniform_matroid/todo.md § Read a `by` key again (needs: H-05)
- [ ] R1 reads `by` again — direct_solve/rules/r1_continuous_knapsack/todo.md § Read a `by` key again (needs: H-05)
- [ ] H-06 The two-level objective — direct_solve/framework/todo.md § H-06
- [ ] H-07 `per K:` on a constraint — direct_solve/framework/todo.md § H-07 (needs: H-04)

## 8. F7 SQL semantics (needs: 1)

- [ ] BIN-1 Functions that behave unlike SQL: NaN and infinity — pipeline/02_binder/todo.md § BIN-1

## 9. F8 Solver runtime (needs: F7)

- [ ] SOL-1 Bound a HiGHS solve by `DECIDB_TIME_LIMIT` — pipeline/07_solver/todo.md § SOL-1
- [ ] BUG-3 Q10 on HiGHS at large is killed for memory (owner runs) — issues/bugs.md § BUG-3

## 10. F9 Housekeeping (needs: 1; runs alone)

- [ ] OPS-2 Normalize the DecidB spelling — operations/todo.md § OPS-2

## 11. Measurement (last; the owner runs them)

- [ ] TST-1 A test large enough to see an optimality gap — testing/todo.md § TST-1
- [ ] PERF-1 Measurement overhead, a correctness check, a second host — performance/todo.md § PERF-1
- [ ] PERF-2 Backend discovery versus parsing and planning — performance/todo.md § PERF-2
- [ ] PERF-3 Rank extraction work; scope-check spans in `extraction` — performance/todo.md § PERF-3
- [ ] PERF-4 Rank formulation families; the shared helper box — performance/todo.md § PERF-4
- [ ] PERF-5 Gurobi and HiGHS loading overhead — performance/todo.md § PERF-5
- [ ] PERF-6 Scale ladders on both backends — performance/todo.md § PERF-6
- [ ] PERF-7 Readback, temporary results and cleanup — performance/todo.md § PERF-7
- [ ] PERF-8 Where each workload stops fitting in memory — performance/todo.md § PERF-8

## 12. Waiting

- [ ] BUG-2 An off-mode run reported one outcome fewer (waits for a recurrence) — issues/bugs.md § BUG-2
- [ ] OPS-1 Peak process memory on Windows (needs a Windows host) — operations/todo.md § OPS-1
- [ ] H-02 Front end for a second syntax (owner decides if it applies) — direct_solve/framework/todo.md § H-02
- [ ] H-08 VALUE-shape clauses (owner names the rule and shape) — direct_solve/framework/todo.md § H-08
- [ ] A2, A3, A4 Start a class not yet built (owner picks) — direct_solve/todo.md
