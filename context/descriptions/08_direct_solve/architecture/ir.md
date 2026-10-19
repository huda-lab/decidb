# IR: how a query becomes something a rule can read

A rule never reads the bound DECIDE tree or the SQL text. One front end reads the tree into an intermediate
representation (**IR**): the decision variables and their scopes, every constraint with its comparison, reducer,
`PER`/`WHEN` scope and right-hand side, and the objective's terms and scope. This is the compiler split of front end,
IR and back end; the rules are the back end.

## Why a separate layer

- **Only one file knows the tree layout** (`direct_ir.cpp`). If the tree changes, the front end changes and the rules
  do not.
- **A second language front end can reuse every rule.** The planned "ANR" front end would produce the same IR from the
  new language. That is a design bet; no second front end exists yet, so it is untested.
- **Matching a familiar subtree is unsafe.** The solver's flattened model is built later and can silently drop a
  term. Reading the complete bound tree means a rule sees everything the user wrote.

## Conservative analysis

The front end models the whole language. Anything it cannot model is recorded as **not proved**, per constraint and
per objective part, with a reason. A rule must decline a problem with a not-proved IR node in a clause it depends on,
so a wrong rewrite cannot follow from a gap in the front end. `require` mode prints that reason, for example
`constraint_shape: expected SUM(x) with unit contribution`.

## Things worth knowing

- A per-row bound has `variable_bound` set when the solver path folds it into its decision's box instead of emitting a
  model row (an unconditional comparison of the bare decision against a constant). Such a bound replaces the
  variable's default lower bound of 0; every other bound only intersects with it. The flag comes from one predicate
  shared with the solver (`DecideIsSingletonRowBound`), so the two cannot disagree.
- The term splitter that breaks `SUM(2*x - x)` into linear terms is shared with the solver path
  (`src/planner/decide/decide_term_split.cpp`), so the two paths cannot read one term differently.
- `norm(e, p)` is replaced by its definition when the query is canonicalized (L0 stays a marker). The binder tags the
  resulting `SUM`, and a rule that read the tag as a plain sum would answer wrongly, so a rule refuses anything it
  does not prove.
- Estimates (row counts) never prove an IR node.

Details: `direct_ir.cpp` and the IR types in `direct_rule.hpp`. The C++ cases in `test_decidb_direct_ir.cpp` list what
each IR node kind covers.
