# Router — todo

**All router work has shipped** — see `done.md` for how the classifier, the inf/unb
check-ray routing, and the infeasible (elastic) and time_limit (slow) terminals work,
plus the decision-tree unit tests.

Genuinely-deferred follow-ups (Bucket B elastic-as-classifier, diverging-bound →
unbounded hand-off) are tracked where their engine lives, in `slow/todo.md`. (The
once-deferred instant Ctrl-C interrupt has shipped — Gurobi stops mid-solve via a
watcher thread; HiGHS stays boundary-only, a backend limitation documented in
`slow/done.md`.)
