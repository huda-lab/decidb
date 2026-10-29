# Rules: what a rule is, how to add one, what it must match

A rule is one problem class from the catalogue (S1, A1, ...). Each lives in `rules/<class>/` in these docs and in its
own `<id>_<slug>_rule.cpp` in the code (`s1_uniform_matroid_rule.cpp`; the slugs are in `../../glossary.md` § Problem
classes). The folder holds `definition.md` (the class: worked example, recognition, supported variants, relational
construction, correctness argument, eligibility conditions), `todo.md` and, once something ships, `done.md`.

## Catalogue status

An **Exact** class has a complete mathematical construction for inputs that satisfy its eligibility conditions. A
**Prototype** class also has an exact construction, but its recognition or numeric certification is not complete enough
for production use. If any required condition cannot be proved, the query stays on the solver path. The SQL in each
`definition.md` shows the assignment construction for its worked example; a rule must also preserve everything under
"What every rule must match" below.

## Lifecycle

**CheckCondition → Apply**, then the engine maps the substitute onto DECIDE's outputs through its slot map. A rule also
has a `Summary()`, the fixed line `EXPLAIN` shows for it.

- **CheckCondition** checks the shape first, then certifies every constraint, objective part, decision, runtime value
  check and output. It returns the applicability condition `Apply` reads, or nothing. A not-proved IR node means the
  rule does not apply; it is not an error. Shape and certification are one step because a DECIDE query offers each
  rule a single node, so a separate cheap pattern step (as in Cascades, where one pattern is tried on many subtrees)
  saves nothing.
- **Rules are disjoint.** The rule driver runs CheckCondition for every rule, and the rules are told apart by the
  IR they require: S1 needs a `SUM(x)` count bound, A1 forbids any aggregate constraint. At most one rule applies to a
  query. If two or more do, the rule driver raises an internal error naming them, because the overlap is a bug in the
  rules and no ordering is safe to pick. The rule driver test covers this with stub rules.
- **Apply** builds the substitute, a relational plan. An unexpected failure here is an internal error, never a silent
  fallback.

## Adding a rule

1. Create `src/optimizer/decide/direct/<id>_<slug>_rule.cpp`. Keep the rule and its helpers in `namespace
   direct_<id>_<slug>` (unity builds merge anonymous namespaces); inside it the class is `Rule` and its `EXPLAIN` value
   is `RULE_NAME`, the upper-cased `<ID>_<SLUG>`. Declare `Make<Id><Slug>Rule()` (`MakeS1UniformMatroidRule()`) in
   `direct_rules.hpp`, which declares every rule's factory, and list the file in the folder's `CMakeLists.txt`.
2. Register it with one line in `direct_registry.cpp`. No other engine file changes.
3. Read only the IR (`direct_rule.hpp`). The driver offers a rule only an IR that `DirectIRIsReadable` accepts, so a
   rule does not re-check the IR's statuses, offset or sense; any other not-proved node is a decline. The rule must be
   disjoint from every registered rule: it requires an IR property that rules proving a nearby shape forbid.
4. Use the shared predicates in `direct_expression.cpp` to decide whether a coefficient or bound is admissible
   (`DirectProveValue`). Take the empty-aggregate guard, the all-rows breaker, the scope projection
   (`DirectProjectScope`), the check of a per-row bound read from the data (`DirectCheckRowBounds`) and
   `DirectWindowExtremum` from `direct_plan_fragments.cpp`, the
   number checks and error predicates from `direct_runtime_checks.cpp`, and the per-row bounds on a decision
   (`DirectIsSingletonRow`, `DirectReadBounds`, `DirectClauseBounds`, `DirectRowBox`, `DirectBoxIsEmpty`) and the
   linear terms of an objective part or a budget (`DirectReadLinearPart`) from `direct_bound_reader.cpp`. Build
   every expression with the `ExpressionBuilder` (`direct_expression.hpp`); build a node by hand only when the builder
   has no operation for it (a window expression), and say so in a comment. A helper only your rule uses stays in
   your rule's own files until a second rule needs it. Inside the rule's namespace, names carry no `Direct` or class
   prefix (`Condition`,
   `PlanBuilder`); names in `namespace duckdb` keep `Direct`.
5. State the plan's slot map (where each DECIDE output lives), which decision outputs may be skipped, and its validated
   columns.
6. Add the standard tests (below), then `rules/<id>_<slug>/done.md`. The class's `definition.md` already exists; correct
   it where the code proves something different, and leave the rest as the definition.

## What every rule must match from the solver

- **Every input row is read.** A bad value on the last of 5,000 rows must raise even under `LIMIT 1` or
  `COUNT(*)`. A plan that could stream wraps its checks in `DirectValidationBreaker`; a rank window already reads
  everything. Only `LIMIT 0` may skip the read.
- **An error on the same inputs.** Where the solver raises, direct solve raises: invalid data-valued bounds and fixed
  values on the rows that read them; invalid objective coefficients (NULL, NaN, infinity); infeasibility. Nothing is
  computed on a row a `when` drops (syntax reference §6.3): a value there must not raise, on either path
  (`DirectOnKeptRows`, `DirectCheckOnKeptRows`; `test_direct_source_values.py` checks each place a rule masks). Silently answering where the solver refuses is a wrong answer. What
  the rule does **not** owe: the solver's choice among several errors in one query (any one of the applicable errors is
  enough), or the ` at row N` the solver adds to a NaN or infinite value, because N is a scan position a plan cannot
  name. Every other message the solver words for the user is the solver's text (see User-facing wording).
- **One numeric domain.** Values are evaluated with DuckDB's semantics, then converted to DOUBLE. Counts are exact only
  up to 2^53, and a rule declines anything it cannot represent exactly.
- **Results.** Every source column in order, then one column per decision: `INTEGER` 0/1 for `BOOL`, `BIGINT` for
  `INT`, `DOUBLE` for `REAL`. Duplicate rows stay separate rows. When several assignments are optimal, a rule may
  return a different one than the solver.
- **User-facing wording.** Name the object the user wrote and the smallest edit. Internal detail belongs in `EXPLAIN` and
  `decide_diagnostics()`. A runtime error uses the solver's text, built by the function the solver calls, never a copy of
  the string: `decide_value_messages.hpp` (NULL, and NaN or infinite, values), `decide_numeric_policy.hpp` (a whole
  number beyond the exact range). A NULL value names the columns that are NULL on the failing row, so the plan carries
  the value's source columns to its check and builds the message per row. A new rule builds its errors with these
  functions and adds a test that compares the full text of `require` with `off`
  (`test_direct_user_facing.py`).

## Keep the layer lean

A rule exists to be a light shortcut. Before adding code to a rule or to the shared engine, check it against these:

- **Does it protect the answer or the speedup?** If not (error order, a nicer diagnostic), leave it out. Error text is
  the exception: it comes from the solver's own functions (see User-facing wording).
- **Is the proof short?** If admitting a shape needs a long proof, a new range argument or a parity check against
  solver quirks, decline the shape. The solver handles it correctly, only more slowly. A decline is cheap and a bug
  in a proof is a wrong answer.
- **Does a second rule need it?** Keep helpers inside the rule until a second rule uses them, then move them to the
  shared files (plan fragments, runtime checks, bound reader).
- **Is the test proportionate?** One test per behavior. Do not pin an incidental detail, such as which of two errors
  comes first.

## The standard tests every rule ships

All run in `test/decide/tests/`. The output of any rule must be checkable by these; if a query shape has no standard
test, adding one is a task in that rule's `todo.md`.

1. **Conformance suite** (`test_direct_rule_conformance.py`): add a `RuleFixture` in `_direct_rule_fixtures.py`. It
   checks schema and rows, the all-rows read, prepared-plan selection, serializer round trip, `EXPLAIN` and profiling,
   the `require` error, `off`, a forced backend, `DIAGNOSE`, and near misses. Each near miss must decline under
   `require` and answer under `auto` exactly as it does under `off`; one plain form per rule must apply.
2. **Differential oracle test** (`test_direct_<id>_<slug>.py`: `test_direct_s1_uniform_matroid.py`,
   `test_direct_a1_separable_box.py`): the rule adds its own file with its own case table, a function that builds the
   oracle model from the rows, and a function that checks one DeciDB assignment against its constraints. The shared
   scaffold is `_direct_oracle_common.py`: the fixture that lifts a forced backend, `run` and `outcome`, the `VALUES`
   list builder, and `agree_with_oracle`, which runs the oracle model, the solver path (`off`) and the direct path
   (`require`, so a query the rule does not apply to fails) and compares them. Keep test names stable, because the
   oracle cache is keyed on the test node id and the test function's source. All three must agree: both DeciDB runs must
   satisfy every constraint and reach the oracle's objective, or all must say infeasible. Inputs with no oracle model
   (bad data) go in the error table, where `off` and `require` must both fail (the choice among several errors need not
   match; a NULL or NaN message must, apart from ` at row N`), and values the oracle cannot hold (infinities, 2^53) go
   in the boundary table, where direct is compared with the solver only. Add the shape inside parent queries too.
3. **C++ cases** (`test/common/`) for the rule's applicability condition and for the IR it relies on. Each decline
   branch of `CheckCondition` gets a case in `test_decidb_direct_decline.cpp`, which hands the rule the IR of a small
   planned query; `make decide-coverage` shows each new case reaching an arm no other case of the rule reaches.

Run after any change: `make decide-test`, `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` (the solver path alone),
`DECIDB_VERIFY_SERIALIZER=1 make decide-test`, `DECIDB_FORCE_SOLVER=highs make decide-test`, and
`build/release/test/unittest "[decidb]"`. Each takes about 30 seconds. `test/decide/run_direct_matrix.sh` runs all five
and prints one summary line each (`auto: 3089 passed, 0 xfailed, 0 failed`); run `make release` first. The exit code is 0
only when no check fails.
