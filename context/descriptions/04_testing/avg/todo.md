# AVG Aggregate Test Coverage — Todo

No open gaps. All AVG scenarios listed in `done.md` are covered.

The previously xfailed `AVG(x) <> K` pair (`test_avg_not_equal_boolean`,
`test_avg_not_equal_with_when`) now passes: the AVG denominator is hoisted to
the RHS inside the deferred NE expansion rather than distributed into LHS
coefficients. See `physical_decide.cpp` — gated by
`EvaluatedConstraint::ne_avg_rhs_scale`, applied per-group so PER + AVG + `<>`
scales by per-group size.
