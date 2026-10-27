# IR: how a query becomes something a rule can read

A rule never reads the bound DECIDE tree or the SQL text. One front end reads the tree into an intermediate
representation (**IR**): the decision variables and their scopes, every constraint with its comparison, reducer,
`when` scope and right-hand side, the objective's terms and scope, and the scope checks the plan must run. This is the compiler split of front end,
IR and back end; the rules are the back end.

## Why a separate layer

- **Only one file knows the tree layout** (`direct_ir.cpp`). If the tree changes, the front end changes and the rules
  do not.
- **A new language changes one file.** A change to the bound tree changes the front end: it reads the constructs the rules know (a `when`, a reducer's own `when`) and marks the others not proved (a `by` key, `per K:` on a clause). The rules change only where the language's meaning changes (nothing is computed on a row a `when` drops).
- **Matching a familiar subtree is unsafe.** The solver's flattened model is built later and can silently drop a
  term. Reading the complete bound tree means a rule sees everything the user wrote.

## Conservative analysis

The front end models the whole language. Anything it cannot model is recorded as **not proved**, per constraint and
per objective part. A rule must decline a problem with a not-proved IR node in a clause it depends on, so a wrong
rewrite cannot follow from a gap in the front end. A decline carries no reason: the rule applies or it does not.

Not proved covers what the binder admits and no rule models: a per-row term the splitter cannot read, an order-0 norm,
a reducer's `by` key, a `per K:` in front of a clause, a clause whose reducer value became a helper decision
(`VALUE_REWRITE_TAG`), the two-level objective, and a folded bound that is not plain. A scope check with a key (one per
`by` group or per class) makes the whole IR not proved.
The reader trusts the invariants of the bound tree for everything else (one source child, the four reducers, comparison
and `IN` leaves, a `SUCH THAT` that is an `AND` of clauses (the binder rejects `OR` between constraints), at most one
`when` in front of a clause, a `SUCH THAT` clause) and states them as `D_ASSERT`s, because no query can break them
and untestable fallbacks would be dead code. A front end that admits a new shape teaches the reader to read it or to
mark it not proved.

## Things worth knowing

- A per-row bound has `variable_bound` set when the solver path folds it into its decision's box instead of emitting a
  model row (an unconditional comparison of one decision against a constant, affine spellings included, D41). Such a
  bound replaces the variable's default lower bound of 0; every other bound only intersects with it. The flag's
  `plain` half marks a bare decision against a cast-wrapped constant, the only fold the rules read; any other folded
  bound is not proved. The flag comes from one predicate shared with the solver (`DecideIsSingletonRowBound`), so the
  two cannot disagree.
- `row_checks` are the scope checks the DECIDE operator runs before it reads a value
  (`PhysicalDecide::RunScopeChecks`): a `when` that keeps no row (D4) and a reducer whose own `when` keeps no row (D8),
  each with its message from `decide_value_messages.hpp`. The rule driver puts them under every rule's plan
  (`AddRowChecks`: a window that counts the rows and a filter that raises), so a rule does not see them. A check with
  a key is not one of them yet.
- The term splitter that breaks `SUM(2*x - x)` into linear terms is shared with the solver path
  (`src/planner/decide/decide_term_split.cpp`), so the two paths cannot read one term differently.
- `norm(e, p)` is replaced by its definition when the query is canonicalized (L0 stays a marker). The binder tags the
  resulting `SUM`, and a rule that read the tag as a plain sum would answer wrongly, so a rule refuses anything it
  does not prove.
- Estimates (row counts) never prove an IR node.

Details: `direct_ir.cpp` and the IR types in `direct_rule.hpp`. The C++ cases in `test_decidb_direct_ir.cpp` list what
each IR node kind covers.
