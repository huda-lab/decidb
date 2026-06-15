# Query Diagnostics — Unbounded (implemented)

**INF_OR_UNBD disambiguation (Gurobi).** When Gurobi presolve returns the
ambiguous `GRB_INF_OR_UNBD`, DeciDB re-solves with `DualReductions=0`, yielding a
definitive `INFEASIBLE` or `UNBOUNDED` (the extra solve only happens in this rare
ambiguous case). `src/decidb/gurobi/gurobi_solver.cpp:202-219`; residual throw if
the disambiguation itself fails at `246-253`. Landed in commit `9d4bbd59f0`.

So U2/U3 receive an already-disambiguated status on Gurobi. The portable HiGHS
equivalent (obj=0 probe) is U1 in `todo.md`.

Otherwise: unbounded currently throws a static paragraph
(`gurobi_solver.cpp:237-245`). Ray extraction / diagnosis is not implemented.
