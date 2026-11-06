# Report: direct solve takes A2, single-facility location

Scratchpad. The goal's queue is done; this file is its report, and the open lines at the end are what is left.
Delete this file once those lines move or ship.

## The goal in plain terms

Direct solve answers some DECIDE queries with ordinary DuckDB operators instead of a solver. A *rule* recognizes one
problem shape (a *class* in the catalogue) and writes the plan. A2 is the shape "one shared value closest to every
row": many rows, one `REAL` decision shared by all of them (`per ():`) or by each group of a key (`per k:`), placed at
the best center of the rows' values. Before this goal every such query went to Gurobi or HiGHS.

The work ran in four phases (G1 to G4). Each code phase ran as one *lane*: a Sonnet agent in its own git worktree,
whose diff I reviewed, gated with the full test matrix (auto, off, serializer and HiGHS suites plus C++), and
fast-forwarded onto `direct-solve-prototype`. The owner ruled the first decision (what to answer when no row is kept)
and delegated the rest; the decisions below are mine, made by the owner's written rules.

## What shipped

| Phase | What A2 now does | Commits |
|---|---|---|
| G1 | One query-wide decision under a squared loss, `SUM(POWER(c - v, 2))` in any orientation or scale: the mean, clamped into the interval | `ea039b00b8`, docs `8bd475acb3` |
| G2 | Keyed decisions (one center per class of the key, NULL keys as on the solver path) and bounds read from the data (per class, the tightest of each clause's ends) | `802b64228e`, `e0603a2690`, `e863cbcc1a`, docs `48f3ca7512` |
| G3 | A weight on the square, constant or read from the data, of any sign; the absolute loss (`SUM(ABS(c - v))`, `norm(c - v, 1)`: the weighted median); the maximum absolute loss (`MAX(ABS(c - v))`, `norm(c - v, 'inf')`: the midpoint); L0 declined | `589e387745`, `523eb30500`, `7ba18282a7`, `e70dd0287e`, `e4035614a5`, docs `09f510c7e9` |
| G4 | Measured against Gurobi and HiGHS up to `sf=1` (800,000 rows); every size agreed with both | queries and docs `f5cae3c2c9` |

The shared code A2 brought: the IR carries a keyed decision's key; A1 and A2 share one bound-clause layout; N1 and A2
share the slope fold; the square reader takes a weight read from the data; a new `ABS` reader; R1's running sum moved
to the shared fragments; an unused runtime check was deleted. Last gate, at `e4035614a5`: 7,523 passed in each of the
four suites, C++ 4,766 assertions in 86 cases. Nothing is pushed: the branch is 93 commits ahead of its remote.

Speed at 800,000 rows (A2 `done.md` § Performance):

| Query | Direct | Gurobi | HiGHS |
|---|---:|---:|---:|
| Absolute loss, one median per supplier | 0.14 s | 12.0 s (86x) | 62.4 s (461x) |
| Squared loss, one center per supplier | 0.06 s | 0.13 s (2.2x) | 0.39 s (6.5x) |
| Squared loss, one center for all rows | 0.09 s | 0.08 s (0.8x) | 0.07 s (0.8x) |

## Decisions I made for the owner

| Question | Decision | Reason |
|---|---|---|
| Several optimal points (a flat median, a zero weight) | The one nearest 0, for every loss; on a concave tie between `-U` and `U`, the lower end | Extends the owner's G1 ruling; A1 already breaks ties to the lower end |
| An empty interval over constant bounds | Raise the infeasible error in the plan | A1 does the same; the G1 brief's claim that A1 declines was wrong |
| H-47: a square's expanded constant past a DOUBLE answers under direct, refuses under the solver | Not a bug; `rules.md` now says a rule owes no refusal over a number only the solver's model computes | DuckDB evaluates the same objective to 0.0; matching would cost an extra pass and still not match |
| How the plan reads the key | The IR carries the key (reusing the `by`-key reader), replacing two unused fields | The layer that owns the key's collation and NULL mode |
| Constant and data bounds | One reading: a constant folds into the same `MAX`/`MIN` | No second path |
| Data weight on the square | Every sign: a total weight of 0 or less answers an end of the interval; an infinite best end is unbounded | Gurobi answers these, and the plan cannot fall back after it reads a weight |
| Data weight on the absolute loss | Declined, filed as A2-9 for the owner | The median is wrong for negative weights; the general fix costs two more windows |
| Maximum absolute loss | A positive constant weight, `MINIMIZE` only | Anything else is not the midpoint |
| L0 loss | Declined | Long proof for a rare loss (`rules.md`, keep the layer lean) |
| A NaN or infinite value under `ABS` | A2 raises the value error as written | The solver path's wording comes from its Big-M rewrite (a big-constant envelope that turns `ABS` into two bounds), filed as BUG-51 |
| Unbounded vs too large | Unbounded only for a concave or linear class with an infinite best end; a convex turning point past a DOUBLE clamps to a finite end, or is too large at an open end | A bounded convex problem is never unbounded (my review caught the lane raising it) |
| A1's check on kept rows | Dropped, and its helper deleted | Removing it failed none of 2,161 tests: the values are already NULL on dropped rows |
| G4's queries | Added a third, the keyed absolute loss | Its plan (a sort per class) is A2's costliest and shipped in G3 |
| The slow query-wide square | Filed as H-48, not fixed | The fix changes the shared plan shape of A2 and N1, beyond this goal |

## What is hacky or doubtful

- **The query-wide plan does per-class work on every row, on one thread.** The plan puts the class's sums in a window
  and then computes the answer and all its checks, one long `CASE`, on each of the 800,000 rows. A window with no key
  runs everything above it on one thread. This is why A2's simplest query is slower than the solver. The same
  projection also carries constant branches (`CASE WHEN false THEN true ...`) that DuckDB leaves unfolded. H-48 holds
  the measured cause and the fix.
- **The square's too-large check recomputes the clamp** that the shared tail also computes. It stays in the square's
  loss because only that loss can produce an infinite target, but it is the same expression twice.
- **`DirectLayoutQuadraticPart`** grew to 62 lines with 8 parameters (complexity 11); nothing else grew past the
  base, and A2's largest function is complexity 10.
- **Process slips, all caught before landing.** The G1 brief cited a wrong A1 precedent and a wrong NULL rule; the G2
  brief went out with a literal `$(LANE_RULES)` placeholder (sent in full right after); the lanes' focused gates missed
  the old-spelling lint (a `Summary()` with "per class" failed the full gate) and ranged Black formatting on six G3
  test lines (I fixed both). The `/phase` lane rules do not yet ask for `old_spelling` in a lane's `-k` pattern or
  a ranged format check; adding both would catch these in the lane.

## Unfixed issues

| Issue | Where | Fix | Size |
|---|---|---|---|
| A2's query-wide square is 0.8x the solver | `direct_solve/framework/todo.md` § H-48 (decision needed) | Aggregate per class over a materialized source, answer once per class, join back | 1 to 2 days, A2 and N1 |
| Data weights on the absolute loss | `rules/a2_single_facility_location/todo.md` § A2-9 (decision needed) | My recommendation: keep declining | none, or 1 day |
| Solver path words a bad `ABS` value as a bound `-v`, and an infinite one as infeasible | `issues/bugs.md` § BUG-51 | Check the value before the Big-M rewrite | half a day |
| Solver path reads a NaN on a row a sum's own `when` drops under `ABS` | `issues/bugs.md` § BUG-52 | The rewrite's bounds carry the reducer's `when` | half a day |
| Gurobi says unbounded on a boxed QP with constants near 1e200 | `issues/bugs.md` § Limitations | Inside Gurobi; a warning comes first | none |
| HiGHS refuses a square whose weight is read from the data | A2 `done.md` § Where it differs | By design: HiGHS takes convex models only, and the sign is unknown until the query runs | none |
| `when f: c >= -4 AND c <= cap` applies `when` to the first conjunct only | Syntax reference | Documented behavior; it surprised me in a G2 test | none |

## Open lines

Lanes: B1 and P1 side by side once H-48 is ruled.

### Owner decisions

- [ ] A2-9 Data weights on the absolute loss — direct_solve/rules/a2_single_facility_location/todo.md § A2-9
- [ ] H-48 Group-then-join for a class's answer — direct_solve/framework/todo.md § H-48

### B1. Solver-path bugs found on the way (independent of A2)

- [ ] BUG-51 A bad value under ABS worded as a bound — issues/bugs.md § BUG-51
- [ ] BUG-52 A sum's own when and ABS on a dropped row — issues/bugs.md § BUG-52

### P1. Plan shape found by the measurement (framework)

- [ ] H-48 A class's answer once per class, not once per row — direct_solve/framework/todo.md § H-48
