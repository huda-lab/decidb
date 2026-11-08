# Stage 08 — Execution: open work

Open work in the physical operator. Finished work is in [done.md](done.md).

## BLOAT-24 — `PhysicalDecide::ParamsToString` copies `LogicalDecide`'s

- **Problem.** `PhysicalDecide::ParamsToString` (`physical_decide.cpp`) repeats the body of
  `LogicalDecide::ParamsToString` (`logical_decide.cpp`) line for line over the same fields: the variable list, the
  plan names, the rewrite accounts and the clause layers. A change to how `EXPLAIN` prints a clause must be made twice.
- **Change.** One free function renders the parameters from the fields both operators carry; both `ParamsToString`
  overrides call it.
- **Done when.** `EXPLAIN` output and every golden are unchanged, and `make decide-test` passes.
