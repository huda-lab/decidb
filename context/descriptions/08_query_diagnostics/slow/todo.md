# Query Diagnostics — Slow (remaining work)

The slow-solve engine is shipped; see `done.md` for current behavior. This file keeps one
follow-up idea that is not specified yet.

## Diverging incumbent at timeout — possible unboundedness signal

A very small time limit can stop an unbounded model before the solver proves
`UNBOUNDED`. If the `TIME_LIMIT` result carries a runaway objective / bound or
an absurd optimality percentage, the slow report should not frame that as a
"usable solution."

Later work should decide the threshold, wording, variable hinting, and backend
test shape. The idea is just to reframe the timeout report + `decide_diagnostics()`
relation as: the objective appears to be running away, the model may be
unbounded, and the user may need to add a finite upper bound such as
`SUCH THAT x <= <cap>`. The ray-based unbounded engine cannot be reused directly
because `TIME_LIMIT` carries no ray.
