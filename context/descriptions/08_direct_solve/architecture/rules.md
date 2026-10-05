# Rules: what a rule is, how to add one, what it must match

A rule is one problem class from the catalogue (S1, A1, ...). Each lives in `rules/<class>/` in these docs and in
its own `<name>_rule.cpp` in the code. The folder holds `definition.md` (the class: worked example, recognition,
supported variants, relational construction, correctness argument, eligibility conditions), `todo.md` and, once
something ships, `done.md`.

## Catalogue status

An **Exact** class has a complete mathematical construction for inputs that satisfy its eligibility conditions. A
**Prototype** class also has an exact construction, but its recognition or numeric certification is not complete enough
for production use. If any required condition cannot be proved, the query stays on the solver path. The SQL in each
`definition.md` shows the assignment construction for its worked example; a rule must also preserve everything under
"What every rule must match" below.

## Lifecycle

**Match → Prove → Cost → Explain → Rewrite**, then the engine maps the result onto DECIDE's output slots.

- **Match** binds the shape. **Prove** certifies every constraint, objective part, decision, runtime value check
  and output. An unknown fact is a miss, not an error.
- **Cost** sees only the estimated source row count and runs after proof. The cheapest proved rule wins; a tie goes
  to the first registered. No second rule exists yet, so this has never chosen between two.
- **Rewrite** builds the relational plan. An unexpected failure here is an internal error, never a silent fallback.

## Adding a rule

1. Create `src/optimizer/decide/direct/<name>_rule.cpp`. Keep helpers in `namespace direct_<name>` (unity builds
   merge anonymous namespaces). Declare `Make<Name>Rule()` in a header, list the file in the folder's `CMakeLists.txt`.
2. Register it with one line in `direct_registry.cpp`. No other engine file changes.
3. Read only the facts (`direct_rule.hpp`). Use `FirstUnknownReason()` to name a blocking clause.
4. Use the shared predicates in `direct_expression.cpp` to decide whether a coefficient or bound is admissible, and
   the shared builders in `direct_builder.cpp` for scope rules, the empty-aggregate error, bound validation and the
   all-rows barrier.
5. State the plan's output slots, which decision outputs may be skipped, and its validation slots.
6. Add the standard tests (below), then `rules/<name>/done.md`. The class's `definition.md` already exists; correct it
   where the code proves something different, and leave the rest as the definition.

## What every rule must match from the solver

- **Every input row is read.** A bad value on the last of 5,000 rows must raise even under `LIMIT 1` or
  `COUNT(*)`. A plan that could stream wraps its checks in `DirectValidationBarrier`; a rank window already reads
  everything. Only `LIMIT 0` may skip the read.
- **An error on the same inputs.** Where the solver raises, direct solve raises: empty scoped aggregate; invalid
  data-valued bounds and pins (including rows a `WHEN` or NULL `PER` key excludes); invalid objective coefficients
  (NULL, NaN, infinity); infeasibility. Silently answering where the solver refuses is a wrong answer. What the rule
  does **not** owe: the solver's message text, or the solver's choice among several errors in one query. Any one
  of the applicable errors is enough, and a generic message that names the clause is enough.
- **One numeric domain.** Values are evaluated with DuckDB's semantics, then converted to DOUBLE. Counts are exact only
  up to 2^53, and a rule declines anything it cannot represent exactly.
- **Results.** Every source column in order, then one column per decision: `INTEGER` 0/1 for `BOOL`, `BIGINT` for
  `INT`, `DOUBLE` for `REAL`. Duplicate rows stay separate rows. When several assignments are optimal, a rule may
  return a different one than the solver.
- **User-facing wording.** Name the object the user wrote and the smallest edit. Internal detail belongs in `require`
  reasons and `EXPLAIN`. Wording is not copied from the solver. If a message needs a plan of its own to build (for
  example, one that names which columns of a computed value are NULL), use the plainer message.

## Keep the layer lean

A rule exists to be a light shortcut. Before adding code to a rule or to the shared engine, check it against these:

- **Does it protect the answer or the speedup?** If not (error order, message wording, a nicer diagnostic), leave
  it out.
- **Is the proof short?** If admitting a shape needs a long proof, a new range argument or a parity check against
  solver quirks, decline the shape. The solver handles it correctly, only more slowly. A decline is cheap and a bug
  in a proof is a wrong answer.
- **Does a second rule need it?** Keep helpers inside the rule until a second rule uses them, then move them to the
  shared builders.
- **Is the test proportionate?** One test per behavior. Do not pin an incidental detail, such as which of two errors
  comes first.

## The standard tests every rule ships

All run in `test/decide/tests/`. The output of any rule must be checkable by these; if a query shape has no standard
test, adding one is a task in that rule's `todo.md`.

1. **Contract suite** (`test_direct_rule_contract.py`): add a `RuleFixture` in `_direct_rule_fixtures.py`. It checks
   schema and rows, the all-rows read, prepared-plan selection, serializer round trip, `EXPLAIN` and profiling,
   `require` reasons, `off`, a forced backend, `DIAGNOSE`, and near misses. Each near miss must also answer under `auto`
   exactly as it does under `off`.
2. **Three-way table** (`test_direct_three_way.py`): add rows for the rule's shapes. Each row runs on an independent
   solver (`oracle_solver`) built straight from the rows, on the solver path (`off`), and on the direct path
   (`require`, so a miss fails). All three must agree: both DeciDB runs must satisfy every constraint and reach the
   oracle's objective, or all must say infeasible. Inputs with no oracle model (bad data) go in the error table, where
   `off` and `require` must both fail (the message and the choice among several errors need not match), and values
   the oracle cannot hold (infinities, 2^53) go in the boundary table, where direct is compared with the solver only. Add the shape inside parent queries too.
3. **Differential fuzz** (`test_direct_fuzz.py`, comparator in `_direct_differential.py`): supply a seeded query
   generator. Direct (`require`) and solver (`off`) must succeed or fail together (any error counts) and agree on
   row count and primary objective. It finds interactions between shapes that no table lists.
4. **C++ cases** (`test/common/`) for the rule's proof and for facts it relies on.

Run after any change: `make decide-test`, `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` (the solver path alone),
`DECIDB_VERIFY_SERIALIZER=1 make decide-test`, `DECIDB_FORCE_SOLVER=highs make decide-test`, and
`build/release/test/unittest "[decidb]"`. Each takes about 30 seconds.
