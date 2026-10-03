# First S1 Rule — completed work

The opt-in S1 slice recognizes one row-scoped Boolean, finite consistently
foldable numeric upper, lower, equality, or intersecting interval bounds,
or multiple numeric source-valued bounds together with foldable bounds,
optional per-row zero/one pins, and one or more signed, unfiltered linear
`SUM(coefficient * x)` objective terms with an optional finite constant offset.
The count term may be `SUM(x)` or an exact unit product such as `SUM(1*x)`.
It admits global bounds and bounds keyed by source-column `PER` keys, with an
optional deterministic source-only `WHEN`, either top-level or aggregate-local
on `SUM(x)`. Every cardinality clause
must describe identical membership. NULL `PER` keys and rows outside the
aggregate's `WHEN` bypass its bound but may still have a per-row pin. Pins
accept exact zero/one comparisons with an optional deterministic source-only
`WHEN`; overlapping contradictory pins raise infeasibility. A nonempty source
with no eligible row for a scoped constraint reports the solver's
empty-aggregate error. Each source-valued bound checks every row for NULL or
NaN before score evaluation, including bypassed rows, then converts to DOUBLE
and takes a group minimum for upper bounds or maximum for lower bounds.
Deterministic, nonthrowing source-only numeric expressions now include
`COALESCE` and `TRY_CAST` in this proof; their evaluated result, including a
failed `TRY_CAST`, receives the same all-row NULL/NaN validation. The limits
intersect; a global variation count makes an equality error take
precedence over errors in later clauses, even when it occurs in another group.
Source-valued RHS under aggregate-local `WHEN` reduces over all group rows,
including filtered-out rows. Each dynamic bound's proof distinguishes that
RHS membership from the count's filtered membership.
It rejects unsupported shapes before changing the DECIDE plan. On a scoped
input, generated operators check that some row is eligible before evaluating
scores. They then evaluate each typed coefficient, convert it to DOUBLE, add
signed terms in solver order, check the resulting score, rank free rows first
within their groups, and emit every source row
with an `INTEGER` 0/1 decision. Fixed selected rows reduce each group's
remaining lower and upper limits. A positive or source-valued lower bound adds a free-row count
to the rank window and reports DECIDE infeasibility when a nonempty eligible
group has too few available rows. Without pins, it counts all rows.
Scoped plans add an unpartitioned non-NULL count of eligible rows for the
empty-aggregate check. This order preserves the solver's empty-aggregate
error ahead of a bad score when both are present. Strict and fractional bounds normalize to inclusive
integer counts; impossible normalized intervals raise infeasibility on an
eligible nonempty input and preserve the empty-input result.
The result boundary retains the rank dependency at zero upper bound and when
`x` is unprojected.

RULE-04: a 5,000-row late invalid coefficient reports NULL/non-finite or the
underlying deterministic cast error under outer limits, `COUNT(*)`, and parent
filters. An input `WHERE` can remove it; outer `LIMIT 0` does not execute it.
The focused cases are in `test/decide/tests/test_direct_solve.py`.

RULE-01: the matcher reads exact facts for the complete bound trees, requires
one attributed source clause per factor, and rejects unsupported factors,
mismatched scope, aggregate-local filters, qualifiers, frames, and unsupported
expressions. The historical M1–M17 baseline and current near-miss matrix
cover the first-slice facts; the current matrix replaces promoted cases.

RULE-02: the rule's typed proof is mandatory
for construction. The current one-condition-away cases each assert a
reasoned `require` miss and unchanged solver plan in `auto`; extra unknown
wrapper cases test fail-closed fact handling. The proof carries variable
identity/domain, exact cardinality interval, objective sense/terms, guard obligation,
and output bindings. The builder checks the required fields before constructing
relational operators. C++ contract tests call Match and Prove on real bound
DECIDE plans, check proof output identity and explanation, and verify that
missing provenance, unrepresentable bounds, and volatile coefficients fail before
Rewrite. The current built-in optimizer audit is in
[optimizer_audit.md](../00_design/optimizer_audit.md).

RULE-03: ordinary bound projections, guard filters, and a window emit
one `INTEGER` 0/1 assignment per source row. Independent enumeration and
parent-context tests cover both senses, zero/worsening scores, ties, empty
input, grouped and `WHEN` membership, NULL keys, intersecting cardinality
clauses, per-row pins, and infeasible nonempty inputs. The shared result
boundary preserves output position and type. The full S1 class still needs
further fixed and scoped forms. The production performance gate remains open;
this is still a one-rule prototype.
