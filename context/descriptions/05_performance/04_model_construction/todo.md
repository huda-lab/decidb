# Model construction

Relate model time and boundary memory jumps to actual row/nonzero expansion and simultaneously live representations. Rank the expensive formulation families. Preserve provenance, diagnosis and backend contracts in any later reduction.

**A helper decision's box, per instance.** A helper decision of the value rewrite (`syntax_reference.md` §6.4) has one instance per `by` group but one column box: `DeriveReducerValueBounds` (`src/decidb/formulation/linearization_reducer_value.cpp`) boxes it by the cover of every group's reach, and a `MIN`/`MAX` helper's closing Big-M reads that box end against its own group's far end. When groups reach very different ranges, every instance gets the widest group's box, so the narrow groups' Big-M rows are looser than their data needs and the LP relaxation is weaker. A box per instance would tighten it, but the model contract keeps one bound pair per decision (`SolverInput::lower_bounds` / `upper_bounds`), so it needs per-instance bounds in `01_pipeline/06_model_formulation`. Measure first: a `max(e) by (g)` helper pinned from both sides beside a per-row decision (`x + 1 = max(x * v) by (g)`), two groups whose reach differs by orders of magnitude, node count and solve time with the shared box against a hand-boxed model. Record the finding in `done.md`.


**A range read as a running total.** A range frame's window lists every value it reads
(`DecideFrameMap`, `physical_decide_frames.cpp`), so a range whose width grows with the
timeline -- `sum(from first to next: x) over (t.period)`, a running total -- reads n²/2
values over n rows: 4,000 rows in two timelines build and solve in 3.9 s (2026-10-30, Gurobi,
release build), and 10,000 in one timeline pass `decide_frame_limit`'s default (10,000,000
values) and are refused. A rolling window of fixed width stays linear (100,000 rows in 1 s
over decisions, 0.13 s over data), and so does a whole-timeline range (one helper per
timeline: 0.23 s). Two cures, each with the suites unchanged and a model dump showing the
smaller model: over data, prefix sums for `sum`, `avg` and `count` (a window's value is a
difference of two prefixes), in `ReduceRangeValues`; over decisions, a helper defined from
its neighbour's (`h_a = h_{a-1} + x_a` for `from first to ...`, `h_a = h_{a-1} + x_{a+k} -
x_{a-k-1}` for a sliding sum), which is the ANR plan's phase 7 (inlining and range
recursion). Over data, a range with no `every`, no `cyclic` and no `else` over a timeline
no `when` thins is also SQL's own `GROUPS BETWEEN k PRECEDING AND j FOLLOWING`, which
DuckDB's segment-tree window aggregate computes in O(n log n). Measure with
`profile_pipeline.py` spans for the frame maps and the value links.

**A position parted by an `else` that reads the anchor.** Where a range reaches an `else`
that reads the anchor's own row (`else t.w`), each anchor of the position is a window of its
own and reads the position's list again (`DecideFrameMap`), so k tied anchors cost k times
the position's values: 2,000 rows tied in two positions build 4,000,000 members, which the
limit now counts. For SUM and AVG the `else` is linear and could stand beside the helper
instead -- the helper over the payload alone, one per position, plus `missing / n` times the
`else` read at the anchor as a term of the row, as a point frame's `FALLBACK` terms are --
leaving the parting to MIN and MAX; over data, one aggregate state per list combined into
each parted window before its `else` is fed. Pin with a model dump.
