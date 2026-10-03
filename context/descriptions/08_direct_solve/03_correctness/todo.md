# Correctness — open work

Correctness tests assert both **path selection** and **result semantics**. Tied
decision vectors need not match; schema, cardinality, feasibility, and primary
objective must. Use an independent solver oracle for representative cases so
two DeciDB paths cannot share one unnoticed mistake.

The first-slice correctness gate is met: all admitted test cases preserve the
observable contract, all tested non-admitted cases use the unchanged solver
path, and serializer/prepared-plan coverage confirms the result boundary in
multiple execution shapes. See [done.md](done.md) for the one forced-HiGHS
failure unrelated to this S1 rule.
