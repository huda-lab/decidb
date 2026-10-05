# Facts: how a query becomes something a rule can read

A rule never reads the bound DECIDE tree or the SQL text. One adapter reads the tree into a neutral set of
**facts**: the decision variables and their scopes, every constraint with its comparison, reducer, `PER`/`WHEN`
scope and right-hand side, and the objective's terms and scope.

## Why a separate layer

- **Only one file knows the tree layout** (`direct_problem.cpp`). If the tree changes, one adapter changes and
  the rules do not.
- **A second language front end can reuse every rule.** The planned "ANR" adapter would produce the same facts from
  the new language. That is a design bet; no second adapter exists yet, so it is untested.
- **Matching a familiar subtree is unsafe.** The solver's flattened model is built later and can silently drop a
  term. Reading the complete bound tree means a rule sees everything the user wrote.

## Unknown fails closed

The adapter models the whole language. Anything it cannot model is recorded as **unknown**, per constraint and per
objective part, with a reason. A rule must decline a problem with an unknown fact in a clause it depends on.
`require` mode prints that reason, for example `constraint_shape: expected SUM(x) with unit contribution`.

## Things worth knowing

- The term splitter that breaks `SUM(2*x - x)` into linear terms is shared with the solver path
  (`src/planner/decide/decide_term_split.cpp`), so the two paths cannot read one term differently.
- `norm(e, p)` is replaced by its definition when the query is canonicalized (L0 stays a marker). Before this,
  S1 read the binder's tagged `SUM` as a plain sum and returned wrong answers, so a rule must refuse anything it
  does not prove.
- Estimates (row counts) never certify a fact.

Details: `direct_problem.cpp` and the fact types in `direct_rule.hpp`. The C++ cases
`test_decidb_direct_facts.cpp` list what each fact kind covers.
