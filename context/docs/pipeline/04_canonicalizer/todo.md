# Stage 04 — Canonicalizer: open work

Open work in the canonical forms and the stages that read them. Finished work is in [done.md](done.md); a rewrite
belongs in this stage only when it meets [done.md](done.md) § What a rule must be.

## CAN-2 — One objective sense

- **Problem.** The objective's sense travels beside its tree (`DecideSense`), and about 25 files combine it with term
  signs, each on its own: the solver gate, model building, the backends, diagnosis, four optimizer rewrites and every
  direct-solve rule. Two of them combine it wrongly. O1 declines `MAXIMIZE -k*ABS(a - b)`, which is
  `MINIMIZE k*ABS(a - b)`: it reads the sense and the sign apart at `o1_monge_assignment_rule.cpp:287` and together at
  :291. A2 requires `MINIMIZE` without reading the part's sign (`a2_single_facility_location_rule.cpp:555`).
- **Change.** The canonicalizer writes every objective as a minimization: `MAXIMIZE f` is `MINIMIZE -f`, with the sign
  carried by each term (`Decompose` starts at -1), never by a factor. `LogicalDecide` keeps the written direction as
  `written_sense`, which only messages, EXPLAIN and a reported value read, and `SetObjective` keeps the minimization,
  so the pass stays a fixed point. FEASIBILITY has no objective and stays as it is. Every other reader of the sense
  goes.
  - A value a `MAXIMIZE` query shows is the negated model value (`0.0 - v`): the timeout's best objective so far
    (`physical_decide.cpp:1737`) and DIAGNOSE's achievable objective (`decide_diagnostic_engines.cpp:1074`).
  - Messages quote the objective as written.
  - **Ruling (owner, 2026-11-10).** EXPLAIN renders the canonical layer in the written sense, so the flip alone adds
    no `≡` line.
- **Needs.** CAN-3.
- **Done when.** O1 answers `MAXIMIZE -k*ABS(a - b)` under `require` (an oracle case in
  `test_direct_o1_monge_assignment.py`); the EXPLAIN of a `MAXIMIZE` query shows no canonical line for the flip; a
  timed-out `MAXIMIZE` query and DIAGNOSE on an infeasible `MAXIMIZE` query report the objective with its written sign;
  `./test/decide/run_tests.sh -n 2 -k "maximize or minmax or extremum or per_objective or o1_monge or explain or
  diagnos"` and `build/release/test/unittest "[decidb]"` pass.

## CAN-3 — The MIN/MAX rewrites read the sign

- **Problem.** The MIN/MAX rewrites read the objective's root as written, so one problem gets two formulations:
  `MAXIMIZE MIN(x)` takes the flat path (`RewriteMinMaxObjectiveTree`, `decide_rewrite_minmax.cpp:663`) and
  `MINIMIZE -MIN(x)` the composed one (`RewriteComposedMinMaxObjectiveTop`, :365). The flat path declines a factor,
  and the two-level path writes its inner sum over the whole objective, so a sign in front of its reducer is lost:
  once CAN-2 writes `MAXIMIZE MAX(per k: ...)` as `MINIMIZE -MAX(per k: ...)`, that query would minimize. On a
  constraint, a lone negated reducer carries its sign as a `-1` factor (`FoldNegationIntoFactor`,
  `decide_canonicalizer.cpp:1011`), which is multiplied in the reducer's SQL type and can overflow at the type's
  minimum where the written query does not.
- **Change.** Every MIN/MAX path, in the objective and in constraints, takes its direction from the sense times the
  term's sign times its factor's sign, as `WalkComposedLhs` (:147) does, so equal problems get one formulation
  whatever their spelling. **Ruling (owner, 2026-11-10).** Negation is a term's sign everywhere:
  `FoldNegationIntoFactor` goes.
- **Done when.** `MAXIMIZE MIN(x)` and `MINIMIZE -MIN(x)` build the same model (`DeciDBCli.dump_model`) and answer as
  the oracle does; `./test/decide/run_tests.sh -n 2 -k "minmax or extremum or per_objective or canonicalize_sign"`
  passes.

## CAN-4 — One spelling per function

- **Problem.** DuckDB registers one function under several names, and two bound calls of it differ only in the name:
  `pow`, `power` and `^` for `**`; `abs` for `@`; `add`, `subtract` and `multiply` for `+`, `-` and `*`. Each consumer
  matches names on its own, so one function is read differently in different places:
  - the shape check accepts all four power names but refuses `@(x - v)` and `multiply(x, v)` over a decision, while
    `ABS(x - v)` and `x * v` answer;
  - `ScalarFunction::operator==` compares names, so `(x - pow(v, 2)) * (x - power(v, 2))` is a product of two
    decisions, not a square: HiGHS refuses it, though it solves the same product written with `pow` twice;
  - O1 reads only `power` and `abs` in data (`o1_monge_assignment_rule.cpp:159-162`), so it declines a square written
    `**`, `^` or `pow`;
  - the binder's whole-number check for a strict comparison (`ValidateDecideIntegralComparisonOperands`,
    `decide_declarations_binder.cpp:281`) runs before the canonicalizer and keeps its own list of names
    (`decide_binder.cpp:1012`): `x + 0.5 < 3` answers, `add(x, 0.5) < 3` is refused.
- **Change.** A rule at the head of the canonicalizer, beside `DesugarNorms`, writes every call in a clause's algebra
  (both sides, the objective and reducer bodies, data included; not `when` conditions or keys) under the primary name
  of its function, and keeps its source location, so messages quote what the user wrote.
  - **Ruling (owner, 2026-11-10).** The relation comes from DuckDB's registration, where an alias is registered with
    its primary's implementation (`DUCKDB_SCALAR_FUNCTION_ALIAS` in `function_list.cpp`). DeciDB keeps no alias list.
    The canonical names are DuckDB's primaries (`**`, `@`, `+`, `-`, `*`), and every consumer that matches a function
    by name matches those.
  - **Ruling (owner, 2026-11-10).** An alias of a function DECIDE accepts over a decision is accepted too, as SQL
    treats it as the same function.
  - **Ruling (owner, 2026-11-10).** The shape check decides whether a strict comparison is over whole numbers, on the
    canonical spelling, and the binder's check goes (§6 of [done.md](done.md): a refusal of the algebra belongs in
    `DecideShapeCheck`). A query with two errors may report the other one first.
- **Done when.** Each against the oracle or the solver path: O1 answers squares written `**`, `^` and `pow` under
  `require`; `@(x - v)` and `multiply(x, v)` answer as `ABS(x - v)` and `x * v`; HiGHS solves
  `(x - pow(v, 2)) * (x - power(v, 2))`; `add(x, 0.5) < 3` answers as `x + 0.5 < 3`;
  `./test/decide/run_tests.sh -n 2 -k "o1_monge or canonicaliz or shape or power_over_data or abs_linearization or
  integral or whole"` and `build/release/test/unittest "[decidb]"` pass.
