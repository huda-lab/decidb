# Correctness — completed work

VAL-00 is complete as a **pre-feature, current-syntax solver baseline**:
[baseline.md](baseline.md) records admitted candidates, one-condition-away
misses, schemas, observed outputs and errors, and expected future path. Both
Gurobi and HiGHS were available for representative cases. This is not evidence
that direct selection works; no permanent direct-rule correctness test exists
yet. The experimental boundary probe and its limits are in
[design/experiments.md](../00_design/experiments.md). Promote this baseline to
executable path-and-result tests under [VAL-01](todo.md).

Historical scratch validators were research evidence only. They are not
permanent acceptance tests for direct selection.
