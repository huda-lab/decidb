# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## FORM-8 — One record per kind of model row

- **Problem.** `EvaluatedConstraint` has about ten construct-specific fields, so readers carry skip lists
  (`IsPlainLinearRow`). The provenance triple (source clause, removal group, repair group) is declared in 10 structs and
  copied field by field at three sites; one (`GeneralConstraintSpec` into its provenance) drops the group key, group
  name and aggregate flag, which no query reads on such a row.
- **Change.** One list per kind of row, as `SolverModel` and the backend APIs keep, and one origin value.
  - `<>` rows go to their own list (`NotEqualClause`: the row, `no_exclusion`, the AVG scale), and so do quadratic rows
    (`QuadraticClause`: the row and its product groups). A `<>` waits in its list until its lowering expands it, as
    LLVM's LegalizeDAG legalizes a pseudo-operation. Every linear row stays in `constraints`: a clause's, a per-row
    bound, an ABS envelope, an elastic folded row. The model dumps keep their row order.
  - `ne_clause_idx` and `ne_clause_sources` go: the list says `<>`, and `source_clause_id` names the clause.
  - `abs_aux_idx` goes: a row that defines an auxiliary says so (`defined_var`, from the DEFINES tag);
    `FindUnboundedContributor` takes the column it skips; `PlaceHelpers` reads each list in a loop of its own, as it
    reads `bilinear_links`, and `IsPlainLinearRow` goes.
  - One origin value, `DecideRowOrigin` (source clause, removal group, repair group) in `decide_source_info.hpp`, held
    by composition and copied whole. `LogicalDecide::BilinearLink`, a copy of `BilinearLinkSpec`, goes. `RawConstraint`
    and `GeneralConstraintSpec` carry a `ConstraintProvenance`.
  - The clause-level diagnosis facts (`kind`, `rhs_is_shared_scalar`, `rhs_label`, `avg_scaled`) move into a
    `ConstraintProvenance` the builder copies; the per-term and per-group arrays stay beside what they index.
  - Dead code goes: `EvaluatedConstraint::rhs_mechanism_offset` and four `RawConstraint` fields only a test writes, the
    `<>` product check in `LinearizeNotEqual` (stage 05 refuses that query), and the builder's repair-group fallback
    with the unit checks that reach only it.
  - The elastic pass and `DemandedAuxReach` keep reading `<>` and quadratic rows, through one walker over the three
    lists; whether they should is parked in `../../diagnostics/infeasible/todo.md`.
  - DuckDB keeps one list of a class hierarchy (`BoundConstraint`); these kinds live for different spans (a `<>` is
    used up before the build, a quadratic row goes to another backend list), which one list would test for at every
    reader.
- **Steps.** One commit each: the origin; the `<>` list; the quadratic list; the ABS mark; the diagnosis facts.
- **Ruling (owner, 2026-11-22).** Approved as designed (reviewed by decidb-architect the same day).
- **Done when.** `grep -rn "IsPlainLinearRow" src` finds nothing; every model dump is byte-identical to the base's;
  `./test/decide/run_tests.sh -k "minmax or abs or diagnose or not_equal or quadratic"` and `unittest "[decidb]"` pass.
