-- Model-shape corpus for the canonicalization refactor.
--
-- Each query is here for its CONSTRAINT SHAPE, not its answer -- correctness is
-- covered by the oracle-verified pytest suite. This corpus exists to be run
-- under DECIDB_DUMP_MODEL so the built solver matrix can be diffed across a
-- refactor that claims not to change it. Equal optimal values are not evidence
-- of an equal model; equal dumps are.
--
-- The fixture is deliberately tiny (4 rows) so that shapes with Big-M encodings
-- (hard MIN/MAX, ABS under MAXIMIZE, <>) stay instant.
--
-- Usage: ./capture.sh <output-file>

CREATE TABLE items AS
SELECT * FROM (VALUES
    (1, 'A', 10.0, 2.0),
    (2, 'A', 20.0, 3.0),
    (3, 'B', 30.0, 4.0),
    (4, 'B', 40.0, 5.0)
) v(id, grp, price, weight);

CREATE TABLE groups AS
SELECT * FROM (VALUES ('A', 100.0), ('B', 200.0)) v(g, budget);

-- ---------------------------------------------------------------------------
-- Per-row constraint shapes
-- ---------------------------------------------------------------------------

-- 01 bare variable, upper bound
SELECT id, x FROM items DECIDE x(INT) SUCH THAT x <= 5 MAXIMIZE SUM(x);

-- 02 bare variable, lower bound
SELECT id, x FROM items DECIDE x(INT) SUCH THAT x >= 2 AND x <= 9 MINIMIZE SUM(x);

-- 03 side-swapped bare variable (constant on the left)
SELECT id, x FROM items DECIDE x(INT) SUCH THAT 5 >= x MAXIMIZE SUM(x);

-- 04 data coefficient on the decision variable
SELECT id, x FROM items DECIDE x(INT) SUCH THAT price * x <= 100 MAXIMIZE SUM(x);

-- 05 multi-variable, decision on both sides, with an LHS data offset
SELECT id, x, y FROM items DECIDE x(INT), y(INT)
SUCH THAT 10 - x <= y AND x <= 8 AND y <= 8 MAXIMIZE SUM(x) - SUM(y);

-- 06 per-row equality
SELECT id, x, y FROM items DECIDE x(INT), y(INT)
SUCH THAT x + y = 3 AND x <= 5 AND y <= 5 MAXIMIZE SUM(x);

-- 07 per-row with a constant folded on the left
-- Canonicalization changed this model, intentionally and for the better: before,
-- the emitted row was `x <= 6` but the absorbed column bound was ub=8 (read off
-- the un-migrated `x + 2 <= 8`), i.e. correct but loose. Canonical input lets the
-- absorption pass derive ub=6. Rows and query results were unchanged.
SELECT id, x FROM items DECIDE x(INT) SUCH THAT x + 2 <= 8 MAXIMIZE SUM(x);

-- 08 repeated reference to the same variable (see bounds-absorption bug)
SELECT id, x FROM items DECIDE x(INT) SUCH THAT 2*x + 3*x <= 10 MAXIMIZE SUM(x);

-- 09 BETWEEN
SELECT id, x FROM items DECIDE x(INT) SUCH THAT x BETWEEN 1 AND 4 MAXIMIZE SUM(x);

-- ---------------------------------------------------------------------------
-- Aggregate (reducer) constraint shapes
-- ---------------------------------------------------------------------------

-- 10 bare reducer
SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(x) <= 10 AND x <= 9 MAXIMIZE SUM(x);

-- 11 reducer with a data coefficient inside
SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(price * x) <= 100 AND x <= 9 MAXIMIZE SUM(x);

-- 12 reducer with an additive constant on the left (binder normalizes today)
SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(x) + 2 <= 8 AND x <= 9 MAXIMIZE SUM(x);

-- 13 side-swapped reducer
SELECT id, x FROM items DECIDE x(INT) SUCH THAT 10 >= SUM(x) AND x <= 9 MAXIMIZE SUM(x);

-- 14 scaled reducer
SELECT id, x FROM items DECIDE x(INT) SUCH THAT 2 * SUM(x) <= 10 AND x <= 9 MAXIMIZE SUM(x);

-- 15 two reducers, additive
SELECT id, x, y FROM items DECIDE x(INT), y(INT)
SUCH THAT SUM(x) + SUM(y) <= 10 AND x <= 9 AND y <= 9 MAXIMIZE SUM(x);

-- 16 two reducers, subtractive
SELECT id, x, y FROM items DECIDE x(INT), y(INT)
SUCH THAT SUM(x) - SUM(y) <= 3 AND x <= 9 AND y <= 9 MAXIMIZE SUM(x);

-- 17 data-only reducer on the right. This one moved at B.4/C.1 and the delta is an
-- accepted improvement: the bind-time hoist used to rewrite it to
-- `SUM(weight*x) - SUM(weight) <= 0`, and a zero bound gives the absorption pass
-- nothing to work with, so the columns kept the loose `x <= 9`. The reducer now stays
-- on the right, the row reads `rhs=14`, and absorption derives the correct
-- `ub = 14/2 = 7`. Same row, same results.
SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(weight * x) <= SUM(weight) AND x <= 9 MAXIMIZE SUM(x);

-- 18 reducer equality
SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(x) = 6 AND x <= 9 MAXIMIZE SUM(x);

-- 19 AVG
SELECT id, x FROM items DECIDE x(INT) SUCH THAT AVG(x) <= 2 AND x <= 9 MAXIMIZE SUM(x);

-- ---------------------------------------------------------------------------
-- WHEN / PER
-- ---------------------------------------------------------------------------

-- 20 constraint-level WHEN
SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(x) <= 5 WHEN grp = 'A' AND x <= 9 MAXIMIZE SUM(x);

-- 21 aggregate-local WHEN (comparison condition parenthesized before the bound)
SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(x) WHEN (grp = 'A') <= 5 AND x <= 9 MAXIMIZE SUM(x);

-- 21b two aggregate-local WHENs, additive
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT SUM(x) WHEN (grp = 'A') + SUM(x) WHEN (grp = 'B') <= 6 AND x <= 9 MAXIMIZE SUM(x);

-- 22 PER
SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(x) <= 4 PER grp AND x <= 9 MAXIMIZE SUM(x);

-- 23 PER + WHEN
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT SUM(x) <= 4 WHEN price > 15 PER grp AND x <= 9 MAXIMIZE SUM(x);

-- 24 per-row constraint under WHEN
SELECT id, x FROM items DECIDE x(INT) SUCH THAT x <= 2 WHEN grp = 'A' AND x <= 9 MAXIMIZE SUM(x);

-- ---------------------------------------------------------------------------
-- Optimizer-rewritten shapes (these emit auxiliary constraints)
-- ---------------------------------------------------------------------------

-- 25 MIN/MAX easy direction
SELECT id, x FROM items DECIDE x(INT) SUCH THAT MAX(x) <= 3 MAXIMIZE SUM(x);

-- 26 MIN/MAX easy direction, side-swapped
SELECT id, x FROM items DECIDE x(INT) SUCH THAT 3 >= MAX(x) MAXIMIZE SUM(x);

-- 27 MIN/MAX hard direction (Big-M indicators)
SELECT id, x FROM items DECIDE x(INT) SUCH THAT MAX(x) >= 3 AND x <= 5 MINIMIZE SUM(x);

-- 28 MIN easy direction
SELECT id, x FROM items DECIDE x(INT) SUCH THAT MIN(x) >= 2 AND x <= 5 MINIMIZE SUM(x);

-- 29 ABS in a constraint
SELECT id, x FROM items DECIDE x(INT) SUCH THAT ABS(x - 2) <= 1 MAXIMIZE SUM(x);

-- 30 ABS in a MINIMIZE objective
SELECT id, x FROM items DECIDE x(INT) SUCH THAT x <= 9 MINIMIZE SUM(ABS(x - 3));

-- 31 not-equal indicator
SELECT id, x FROM items DECIDE x(INT) SUCH THAT x <> 2 AND x <= 4 MAXIMIZE SUM(x);

-- 32 IN domain
SELECT id, x FROM items DECIDE x(INT) SUCH THAT x IN (1, 3, 5) MAXIMIZE SUM(x);

-- 32b IN domain with a negative minimum: the rewrite emits a floor-lowering
-- bound that absorption folds into the column box. Pins that the bound is
-- absorbed rather than emitted as a row.
SELECT id, x FROM items DECIDE x(INT) SUCH THAT x IN (-5, 3, 7) MINIMIZE SUM(x);

-- 33 quadratic constraint
SELECT id, x FROM items DECIDE x(REAL) SUCH THAT SUM(POWER(x - 2, 2)) <= 4 AND x <= 9 MAXIMIZE SUM(x);

-- 34 quadratic objective
SELECT id, x FROM items DECIDE x(REAL) SUCH THAT x <= 9 MINIMIZE SUM(POWER(x - 3, 2));

-- 35 bilinear (Boolean x continuous -> McCormick)
SELECT id, b, x FROM items DECIDE b(BOOL), x(INT)
SUCH THAT x <= 5 AND SUM(b * x) <= 8 MAXIMIZE SUM(b * x);

-- ---------------------------------------------------------------------------
-- Variable scopes
-- ---------------------------------------------------------------------------

-- 36 scalar (query-wide) variable as a shared bound
SELECT id, ship, cap FROM items DECIDE ship(INT), scalar cap(INT)
SUCH THAT ship <= cap AND ship <= 10 AND cap <= 8 MAXIMIZE SUM(ship) - 20 * cap;

-- 37 scalar variable inside a reducer constraint
SELECT id, ship, cap FROM items DECIDE ship(INT), scalar cap(INT)
SUCH THAT SUM(ship) <= 20 AND ship <= cap AND cap <= 6 MAXIMIZE SUM(ship);

-- 38 table-scoped variable with a join
SELECT i.id, keep FROM items i JOIN groups g ON i.grp = g.g
DECIDE i.keep(BOOL) SUCH THAT SUM(i.price * keep) <= 60 MAXIMIZE SUM(i.price * keep);

-- 39 qualified reducer over a joined relation
SELECT i.id, keep FROM items i JOIN groups g ON i.grp = g.g
DECIDE i.keep(BOOL) SUCH THAT SUM(i: keep) <= 2 MAXIMIZE SUM(i.price * keep);

-- ---------------------------------------------------------------------------
-- Shapes unlocked by Phase A (sign-aware ABS Big-M + composed MIN/MAX).
-- Canonicalization now places every decision term before these optimizer rewrites.
-- ---------------------------------------------------------------------------

-- 40 ABS on both sides: canonicalizes to `ABS(x-3) - ABS(x-9) <= 0`, so aux1
-- enters negated. Sign-aware classification Big-M's aux1 only; aux0 is pinned
-- transitively by the row. Feasible iff |x-3| <= |x-9|, i.e. x <= 6.
SELECT id, x FROM items DECIDE x(REAL)
SUCH THAT x <= 20 AND ABS(x - 3) <= ABS(x - 9) MAXIMIZE SUM(x);

-- 41 negated ABS written directly (reachable before Phase A; previously
-- misclassified as pinned because it sits on the upper-bounded side)
SELECT id, x, y FROM items DECIDE x(REAL), y(REAL)
SUCH THAT x <= 20 AND y <= 20 AND ABS(x - 3) - ABS(y - 9) <= 0 MAXIMIZE SUM(x) + SUM(y);

-- 42 negated reducer in an additive LHS. The equivalent
-- `SUM(x) <= SUM(y)` is accepted by the side-agnostic binder and canonicalizer.
SELECT id, x, y FROM items DECIDE x(INT), y(INT)
SUCH THAT SUM(x) - SUM(y) <= 0 AND x <= 5 AND y <= 3 MAXIMIZE SUM(x);

-- 43 composed MIN/MAX with a negated reducer. Rejected before Phase A with
-- "does not support subtraction in the LHS"; the MAX term now arrives with
-- sign -1, which flips its easy/hard classification (here: hard).
SELECT id, x, y FROM items DECIDE x(INT), y(INT)
SUCH THAT SUM(x) - MAX(y) <= 0 AND x <= 5 AND y <= 7 MAXIMIZE SUM(x);

-- 44 leading-negative reducer: canonicalizes to `0 - MAX(x) <= -3`, the
-- `0 - term` idiom the additive rebuild emits for a leading negative term.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT 3 - MAX(x) <= 0 AND x <= 9 MINIMIZE SUM(x);

-- ---------------------------------------------------------------------------
-- Cast-lid shapes (Phase B.2). DuckDB wraps a whole comparison side in a cast
-- whenever the side's natural type differs from the comparison's resolved type.
-- The canonicalizer descends binder-added decision casts and exposes the additive
-- spine before physical extraction consumes the canonical form.
-- ---------------------------------------------------------------------------

-- 45 two aggregate-local WHEN reducers, one negated, under a DECIMAL->DOUBLE
-- cast lid. All terms are decision-bearing, so the split changes placement for
-- none of them; it is the spine reaching them at all that is being pinned.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT (SUM(x) WHEN (price > 5)) - (SUM(x * price) WHEN (price > 5)) + 2 <= 8
AND x <= 4 MAXIMIZE SUM(x);

-- 46 mixed placement under a cast lid: a reducer (LEFT) beside a scalar
-- subquery (RIGHT). This is the one that changes the canonicalizer's DECISION
-- rather than just its reach -- without cast descent the side is a single LEFT
-- atom and the pass declines, so the migration happens downstream instead.
-- A subquery is used deliberately: a numeric offset in this position is peeled
-- by the parsed-level simplifier before binding, so it never arrives sealed.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT (SUM(x) WHEN (price > 5)) + (SELECT max(budget) FROM groups) <= 210
AND x <= 4 MAXIMIZE SUM(x);

-- ---------------------------------------------------------------------------
-- Scaled reducers (Phase B.3). A factor on a reducer is peeled outward by the
-- canonicalizer, which converges every spelling onto `scale * term`, then folded
-- and then STAYS outside: the physical layer multiplies it into the per-row
-- coefficients for SUM/AVG, or into the auxiliary's contribution for MIN/MAX.
-- Keeping it outside is what makes the factor's sign an optimization input rather
-- than a correctness one -- MIN/MAX are order statistics, so pushing a negative
-- factor through one turns it into the other.
-- 47-49 are shape-equivalent and must produce the SAME matrix as each other:
-- that is the whole point of converging the spelling.
-- ---------------------------------------------------------------------------

-- 47 factor on the left of the reducer
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT 2 * SUM(x * price) <= 400 AND x <= 9 MAXIMIZE SUM(x);

-- 48 factor on the right -- same model as 47
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT SUM(x * price) * 2 <= 400 AND x <= 9 MAXIMIZE SUM(x);

-- 49 division by a constant
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT SUM(x * price) / 2 <= 100 AND x <= 9 MAXIMIZE SUM(x);

-- 50 scaled MAX in a constraint. Rejected before B.3 ("non-aggregate term");
-- the fold leaves a bare MAX for the easy-direction per-row rewrite.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT 2 * MAX(x * weight) <= 20 AND x <= 9 MAXIMIZE SUM(x);

-- 51 scaled MAX beside a SUM -- the composed path, also rejected before B.3.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT SUM(x) + 2 * MAX(x * weight) <= 30 AND x <= 9 MAXIMIZE SUM(x);

-- 52 NEGATIVE factor on MAX. The aggregate must come out of the fold as a MIN.
-- Before B.3 this was a silent wrong answer, not a rejection.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT -2 * MAX(x) >= -8 AND x <= 9 MAXIMIZE SUM(x);

-- 53 negative factor on a MIN/MAX objective -- the reproduced wrong answer.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 5 AND SUM(x) <= 4 MINIMIZE -1 * MAX(x);

-- 54 scalar subquery as the factor: one value for the whole query, so legal,
-- even though its value is unknown at plan time and it looks like a plain
-- column ref after flattening.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT (SELECT max(budget) FROM groups) * SUM(x) <= 1000 AND x <= 9
MAXIMIZE SUM(x);

-- ---------------------------------------------------------------------------
-- Query-wide (`scalar`) decisions as terms of an aggregate constraint (K3).
-- Rejected before B.3 by the physical extractor; the model builder also had to
-- stop fanning a one-column variable out over the group's rows.
-- ---------------------------------------------------------------------------

-- 55 the paper's max_shortfall shape: a scalar slack absorbing the overflow
SELECT id, x, s FROM items DECIDE x(INT), scalar s(INT)
SUCH THAT x >= 3 AND x <= 3 AND SUM(x) - s <= 4 MINIMIZE s;

-- 56 scalar variable added to an aggregate, under PER
SELECT id, grp, x, s FROM items DECIDE x(INT), scalar s(INT)
SUCH THAT x <= 9 AND s >= 2 AND s <= 2 AND SUM(x) + s <= 8 PER grp
MAXIMIZE SUM(x);

-- ---------------------------------------------------------------------------
-- A factor whose SIGN is unknown at plan time (Phase B.3 / P4). An uncorrelated
-- scalar subquery is a legal factor but its value is not available until the
-- query runs. Because the factor stays OUTSIDE the reducer, the sign only
-- selects which linearization is cheaper -- never which is correct -- so these
-- take the exact (envelope + indicator) encoding rather than being rejected.
-- Before the fold was removed these failed with an internal assertion.
-- ---------------------------------------------------------------------------

-- 57 unknown-sign factor on a bare MAX: routed to the composed path, which pins
-- the auxiliary in BOTH directions. The single-term path cannot serve this: its
-- two encodings are opposite quantifiers and picking needs the sign.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND (SELECT max(budget) FROM groups) * MAX(x) <= 400
MAXIMIZE SUM(x);

-- 58 unknown-sign factor beside a SUM
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND SUM(x) + (SELECT max(budget) FROM groups) * MAX(x) <= 400
MAXIMIZE SUM(x);

-- 59 unknown-sign factor on a MIN/MAX objective. The flat objective path replaces
-- the objective with its auxiliary at coefficient 1.0 and has nowhere to put a
-- factor, so a scaled one goes through the composed objective path.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND SUM(x) <= 4
MINIMIZE (SELECT max(budget) FROM groups) * MAX(x);

-- ---------------------------------------------------------------------------
-- Reducers as a bound (Phase B.5). The right-hand side is evaluated to one value
-- per group by the reducer evaluator, which implements the construction order the
-- paper fixes in §3.2.2: when selection -> per partitioning -> qualifier
-- de-duplication -> aggregation.
--
-- These are pure additions: every model above stays byte-identical, because none
-- of these shapes could be expressed before. 60-63 previously failed at the
-- binder or the physical extractor; 62 previously returned a WRONG ANSWER.
-- ---------------------------------------------------------------------------

-- 60 MIN as a bound. This is the shape that exposed the asymmetry: `<= AVG(col)`
-- worked only because the bind-time hoist moved it left, where a data term is
-- reduced by *summing a column*. MIN cannot be reached by adding things up, so it
-- was refused with a message that presented the gap as a rule.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND SUM(x * price) <= MIN(price) * 4
MAXIMIZE SUM(x);

-- 61 MAX as a bound, per group. Each group's bound is its own MAX, which is what
-- "one value per group" means concretely -- a single global MAX would give B's
-- bound to A as well.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND SUM(x * price) <= MAX(price) * 2 PER grp
MAXIMIZE SUM(x);

-- 62 COUNT(*) under PER. Regression pin for a live wrong answer: count_star was
-- folded to the operator's TOTAL input cardinality, so both groups here received
-- the bound 4 instead of 2. The dump is the evidence -- the query still returned
-- rows before, just against the wrong model.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND SUM(x) <= COUNT(*) PER grp
MAXIMIZE SUM(x);

-- 63 a reducer mixed with an ordinary term. The reducer collapses to one value per
-- group and the rest of the expression is still evaluated per row, so nothing here
-- needs a special case -- which is the point of substituting the reducer with a
-- broadcast column rather than folding the whole side.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND SUM(x * price) <= MIN(price) + 90
MAXIMIZE SUM(x);

-- 64 a row-varying bound on a reduced constraint (D3). Paper §3.2.1 without `per`:
-- the clause generates one instance per tuple, all sharing the reducer, so the
-- conjunction is exactly `<= min(bound)`. A correlated subquery decorrelates into a
-- per-row column worth 100 for group A and 200 for B, so this binds at 100 -- not at
-- row 0's value, and not per row. A bare column here (`<= price`) is the same shape
-- but is still refused by the binder until C.2 opens that gate.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND SUM(x) <= (SELECT budget FROM groups WHERE g = items.grp)
MAXIMIZE SUM(x);

-- 65 the same bound under PER, where each group takes its own tightest value (100
-- for A, 200 for B) rather than one global minimum.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 90 AND SUM(x) <= (SELECT budget FROM groups WHERE g = items.grp) PER grp
MAXIMIZE SUM(x);

-- 66 an aggregate-local WHEN on the left, a row-varying bound on the right. The
-- local WHEN scopes its own reducer, never the per-tuple fan-out, so the bound is
-- the tightest over EVERY selected row (100) and not over the B rows the left side
-- keeps (200). Results do not reveal this -- x is capped at 9 either way -- so the
-- dump's `rhs` is the whole evidence, as with 62.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND (SUM(x) WHEN (grp = 'B'))
                     <= (SELECT budget FROM groups WHERE g = items.grp)
MAXIMIZE SUM(x);

-- 67 the reducer form of 66. The two halves of the right-hand side -- reducer
-- folding and row-varying reduction -- must answer "which rows?" the same way, so
-- this binds at MIN over all four rows (10), not over the B rows (30).
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND (SUM(x) WHEN (grp = 'B')) <= MIN(price)
MAXIMIZE SUM(x);

-- ---------------------------------------------------------------------------
-- Shapes unlocked by B.4 + C.1 (data reducers stay right; the bind-time hoist
-- is gone). The first three were BUILT by B.5 and unreachable -- the binder's
-- RHS check treated a WHEN predicate / PER key / relation alias as if it were a
-- value on the bound side and rejected the whole wrapper.
-- ---------------------------------------------------------------------------

-- 68 AVG mixed with another term on the bound side. Refused until now, and the
-- reason was a type rather than the arithmetic: the AVG->SUM rewrite redeclared the
-- node with SUM's integral type while its value stayed fractional. The rewrite now
-- skips decision-free aggregates, so `rhs` is 2*25 = 50 exactly.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND SUM(x * weight) <= 2 * AVG(price)
MAXIMIZE SUM(x);

-- 69 a reducer's OWN WHEN on the bound side. Aggregate-local, so it scopes only
-- that reducer: the bound is SUM(price) over the A rows (30), while the left side
-- still sums every row.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x <= 9 AND SUM(x) <= SUM(price) WHEN (grp = 'A')
MAXIMIZE SUM(x);

-- 70 a relation-qualified reducer as a bound. The join fans each group to two
-- rows, so the de-duplication is what makes `rhs` 3 (= (100+200)/100) rather than
-- 6 -- the same call the left side has always made.
SELECT i.id, keep FROM items i JOIN groups g ON i.grp = g.g
DECIDE i.keep(BOOL) SUCH THAT SUM(i: keep) <= SUM(g: budget) / 100
MAXIMIZE SUM(i.price * keep);

-- ---------------------------------------------------------------------------
-- A product with an additive factor inside a reducer body. The physical layer
-- distributes `weight*(price+x)` into `weight*price + weight*x` before it can
-- classify the terms, and the rebuilt product used to inherit the ORIGINAL
-- multiply's signature -- which was resolved for the wider `price+x`. Over
-- narrow DECIMAL columns that reinterprets the operands' physical
-- representation and yields a garbage bound (a feasible query reported
-- infeasible). Each entry below reached that rebuild through a different decline
-- of the parsed-level simplifier, which used to distribute the body first in the
-- common case. C.4 deleted that layer, so every shape here now reaches the
-- rebuild directly; the three entries are kept because they were written to
-- discriminate and still do.
-- ---------------------------------------------------------------------------

-- 71 nested product, reached via a non-constant bound. `SUM(weight*price)` is
-- 10*2 + 20*3 + 30*4 + 40*5 = 400, so `rhs` is 500 - 400 = 100.
SELECT id, x FROM items DECIDE x(BOOL)
SUCH THAT SUM(weight * (price + x)) <= (SELECT MAX(budget) + 300 FROM groups)
MAXIMIZE SUM(x * price);

-- 72 the subtraction arm of the same distribution: the addend carries a flipped
-- sign, so a rebuild that silently returns garbage would not be caught by the
-- sign bookkeeping alone.
SELECT id, x FROM items DECIDE x(BOOL)
SUCH THAT SUM(weight * (price - x)) >= (SELECT MIN(budget) + 200 FROM groups)
MAXIMIZE SUM(x * price);

-- 73 nested product under a scaled reducer -- the OTHER way the parsed-level
-- simplifier used to decline. Same body, same rebuild, reached without a
-- subquery on the bound side.
SELECT id, x FROM items DECIDE x(BOOL)
SUCH THAT 2 * SUM(weight * (price + x)) <= 1000
MAXIMIZE SUM(x * price);

-- ---------------------------------------------------------------------------
-- Data-only POWER as a coefficient (canonicalize.md C.4)
-- ---------------------------------------------------------------------------

-- 74 `POWER(price, 2)` references no decision variable, so it is a per-row
-- coefficient and the constraint is linear in x. Coefficients are 100/400/900/
-- 1600. Until C.4 no POWER node ever reached the binder here -- the parsed-level
-- simplifier lowered it to `price*price` through SymbolicC++ -- which hid a
-- binder gate that required every POWER base to reference a decision variable.
SELECT id, x FROM items DECIDE x(BOOL)
SUCH THAT SUM(x * POWER(price, 2)) <= 1000
MAXIMIZE SUM(x * weight);

-- ---------------------------------------------------------------------------
-- Side-agnostic constraint gate (canonicalize.md C.2)
--
-- The binder no longer requires the DECIDE expression on the left, and no longer
-- flips the comparison to put it there. DecideCanonicalizer owns that decision on
-- the bound tree, so these entries are here to pin two things at once: the shapes
-- that only C.2 admits, and that a reversed spelling converges on the SAME model
-- as the forward one rather than a mirrored variant of it.
-- ---------------------------------------------------------------------------

-- 75 a query-wide decision as the bound. Rejected before C.2 by the RHS
-- validator, which refused any bound containing a decision variable. Canonical
-- form is `SUM(x*price) - cap <= 0`, the row-invariant term B.3 landed.
SELECT id, x, cap FROM items DECIDE x(BOOL), scalar cap(INT)
SUCH THAT SUM(x * price) <= cap AND cap <= 60
MAXIMIZE 3 * SUM(x * price) - 2 * cap;

-- 76 the same constraint written backwards. The row is the exact NEGATION of
-- 75's -- every coefficient sign flipped and `<` become `>` -- not a copy of it,
-- and that is K4 rather than a shortfall: the relation the user typed is
-- preserved because diagnostics re-quote it, so `SUM - cap <= 0` and
-- `cap - SUM >= 0` are both canonical and stay distinct trees. The deleted binder
-- flip WAS a direction normalization, which is why it had to go rather than move.
-- What is pinned here is that the two spellings agree as models: same columns,
-- same bounds, and one row that is the other multiplied by -1.
SELECT id, x, cap FROM items DECIDE x(BOOL), scalar cap(INT)
SUCH THAT cap >= SUM(x * price) AND cap <= 60
MAXIMIZE 3 * SUM(x * price) - 2 * cap;

-- 77 paper Example 1 (§3.1): a row-varying data term left of the reducer AND a
-- decision on the bound. Canonical form is `-SUM(x) - cap <= -price`, so it
-- exercises C.2's gate and B.5's runtime reduction of the row-varying bound in
-- one query. `<=` takes the MIN over rows, i.e. the largest price binds.
SELECT id, x, cap FROM items DECIDE x(INT), scalar cap(INT)
SUCH THAT price - SUM(x) <= cap AND x <= 5
MINIMIZE cap;

-- 78 a reducer on both sides. `SUM(x*price) - SUM(y*price) <= 0` after
-- canonicalization -- no new machinery, but no single-sided gate could accept it.
SELECT id, x, y FROM items DECIDE x(INT), y(INT)
SUCH THAT SUM(x * price) <= SUM(y * price) AND SUM(y) <= 3 AND x <= 8 AND y <= 8
MAXIMIZE SUM(x * price);

-- 79 leading negative term on a reduced constraint. Independent of C.2's gate,
-- but C.2 is what made it reachable in practice: BuildAdditive used to spell a
-- leading negation `0 - term`, and the aggregate extractor rejected that
-- synthesized `0` as a non-aggregate term. It is a unary minus now.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT 0 - SUM(x) <= -6 AND x <= 5
MINIMIZE SUM(x);

-- Data-column coefficients on ABS. A column's sign is unknown at plan time, so
-- the ABS auxiliary cannot be assumed pinned and gets a Big-M indicator. These
-- were classified "pinned, no Big-M" until the unknown-sign fix, which made
-- `SUM(w * ABS(...))` unsound wherever `w` could be negative.

-- 80 non-negative coefficient. `weight` is positive in this fixture, but that is
-- a fact about the data, not about the plan, so Big-M is emitted anyway. This is
-- the conservative cost of the fix.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT SUM(weight * ABS(x - 2)) <= 20 AND x <= 9
MAXIMIZE SUM(x);

-- 81 genuinely mixed-sign coefficient: `price - 25` spans -15..15 over the
-- fixture. Without Big-M the negative rows' auxiliaries float free and the
-- constraint is weaker than written.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT SUM((price - 25) * ABS(x - 2)) <= 5 AND x <= 9
MAXIMIZE SUM(x);

-- The `<>` range collapse. `LHS <> K` is a disjunction only when K sits strictly
-- inside the range the LHS can reach; when it does not, one branch is dead and
-- the survivor is a plain inequality with no indicator and no Big-M. Query 31
-- above is the interior-K case that must keep the two-row encoding, so these two
-- pin the other direction. Only the rigid box (a variable's intrinsic domain)
-- may license the collapse — a bound the user wrote is loosenable under
-- infeasibility diagnosis, and query 84 pins that it does not license one.

-- 82 aggregate `<> 0` over BOOL decisions: SUM(x) cannot go below 0, so this is
-- exactly SUM(x) >= 1 — the "pick at least one" shape.
SELECT id, x FROM items DECIDE x(BOOL) SUCH THAT SUM(x) <> 0 MINIMIZE SUM(x);

-- 83 per-row `<> 0` over BOOL decisions: one tight `x >= 1` per row.
SELECT id, x FROM items DECIDE x(BOOL) SUCH THAT x <> 0 MINIMIZE SUM(x);

-- 84 the same feasible set as 82, but with the floor written out. `x >= 0` is
-- re-emitted as a loosenable row during diagnosis, so it may not be baked into
-- the constraint's shape: this must keep the two-row disjunction.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x >= 0 AND x <= 5 AND SUM(x) <> 0
MINIMIZE SUM(x);

-- 85 interior-K `<>` over BOOL decisions. Query 82 collapses and so emits no
-- Big-M at all; this one keeps the disjunction, which is what makes the constant
-- visible. A BOOLEAN's [0,1] ceiling is seeded into the absorbed box at stage 05,
-- so the tight per-group M here is single-digit. It read 1000000 (the fallback)
-- while the ceiling only reached the model builder, since every Big-M derivation
-- reads the box and treats >= 1e20 as unbounded.
SELECT id, x FROM items DECIDE x(BOOL) SUCH THAT SUM(x) <> 2 MINIMIZE SUM(x);

-- ---------------------------------------------------------------------------
-- Auxiliary boxes over a range open on one side
-- ---------------------------------------------------------------------------
-- Every MIN/MAX query above carries a finite bound, so none of them exercises a
-- half-open auxiliary box. These do. They answer only where the backend states
-- MIN/MAX natively -- a Big-M has no finite value over an open end and the
-- lowered arm refuses -- so on HiGHS they are the refusals, not models.

-- 86 constraint side, open above: `x >= 0` has a derived floor and no ceiling,
-- so every auxiliary is boxed [0, 1e30] rather than given up as free.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT MAX(x) >= 3 AND x >= 0
MINIMIZE SUM(x);

-- 87 the mirror, via a negative coefficient: the open ceiling on `x` opens the
-- expression's FLOOR and 0 becomes its ceiling. Sign has to be respected before
-- blaming a side, and a box read off the wrong end would cut off the optimum.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT MIN(-1 * x) <= -3 AND x >= 0
MINIMIZE SUM(x);

-- 88 objective side, which boxes through a different walk than the constraint
-- side and had the same defect independently.
SELECT id, x FROM items DECIDE x(INT)
SUCH THAT x >= 0 AND SUM(x) >= 2
MINIMIZE MAX(x * weight);

-- ---------------------------------------------------------------------------
-- Row-varying CONSTANTS inside an extremum, and extrema over group SUMS
-- ---------------------------------------------------------------------------
-- Two shapes the corpus was blind to until 2026-08-23, both of which set the
-- linking Big-M. A hard extremum's members used to be scaled off the DECISION
-- VARIABLES' reach alone, discarding each row's constant; a constant cancels
-- inside one row's `(aux - expr)` but not between two rows, so a family whose
-- rows carry DIFFERENT constants got an M too small to be valid. `price`
-- differs per row, so `x + price` is exactly that family.

-- 89 objective side. Without the budget the cap does not change the argmax, so
-- the row-limited SUM is what makes an invalid M visible in the dump.
SELECT id, price, x FROM items DECIDE x(INT)
SUCH THAT x >= 0 AND x <= 5 AND SUM(x) <= 5
MAXIMIZE MAX(x + price);

-- 90 composed side, which emits the envelope AND the closing family. A too-small
-- M contradicts the envelope outright, so this shape reported INFEASIBLE rather
-- than answering with the wrong number.
SELECT id, price, x FROM items DECIDE x(INT)
SUCH THAT x >= 0 AND x <= 5 AND SUM(x) <= 5
MAXIMIZE MAX(x + price) + 0.001 * SUM(x);

-- 91 an extremum over GROUP SUMS rather than over rows. Its members leave any
-- single row's range as soon as a group holds more than one row, so its M comes
-- from the group-sum family; it used to be the per-row span multiplied by the
-- row count, which is a bound on that family rather than a measurement of it.
SELECT grp, id, x FROM items DECIDE x(INT)
SUCH THAT x >= 0 AND x <= 5 AND SUM(x) <= 6
MAXIMIZE MAX(SUM(x)) PER grp;

-- 92 the MIN mirror of 91, whose hard direction is the other arm.
SELECT grp, id, x FROM items DECIDE x(INT)
SUCH THAT x >= 0 AND x <= 5 AND SUM(x) >= 4
MINIMIZE MIN(SUM(x)) PER grp;

-- ---------------------------------------------------------------------------
-- A plain column as the bound of a reduced constraint (C1/C2/C3)
-- ---------------------------------------------------------------------------
-- Until 2026-08-25 a bare column reaching IsAllowedDecisionFreeBoundExpression's
-- default case was refused, which blocked the paper's own running example below.

-- 93 the paper's Figure 1, verbatim data. Two PER'd constraints (line 8, line 9)
-- each bound by a plain column -- `stock`, `demand` -- collapsing to the tightest
-- per-group value (C1). The published output is 450 / 0 / 0, D1 open, D2 closed.
WITH Depots(depotID, stock, opening_cost) AS (
    VALUES ('D1', 800, 12000), ('D2', 500, 8000)
),
Routes(routeID, depotID, regionID, capacity, unit_cost) AS (
    VALUES ('T1', 'D1', 'R1', 500, 6),
           ('T2', 'D1', 'R2', 350, 6),
           ('T3', 'D2', 'R2', 300, 3)
),
Regions(regionID, demand, priority) AS (
    VALUES ('R1', 450, 'critical'), ('R2', 600, 'standard')
)
SELECT routeID, depotID, regionID, open, ship
DECIDE D.open(BOOL), T.ship(INT)
FROM Depots D JOIN Routes T USING (depotID) JOIN Regions R USING (regionID)
SUCH THAT
    ship BETWEEN 0 AND capacity * open AND
    SUM(ship) <= stock PER depotID AND
    SUM(ship) >= demand WHEN priority = 'critical' PER regionID
MINIMIZE SUM(unit_cost * ship) + SUM(D: opening_cost * open);

-- 94 `<>` with a bound that varies within a PER group (C3): every excluded value
-- is kept -- `SUM(x) <> 3 AND SUM(x) <> 7` for group 'a' -- rather than collapsed
-- to one, so one binary and one Big-M pair is allocated per distinct value.
SELECT id, grp, x FROM (VALUES (1, 'a', 3), (2, 'a', 7)) t(id, grp, cap)
DECIDE x(INT)
SUCH THAT x BETWEEN 0 AND 10
    AND SUM(x) >= 3 AND SUM(x) <= 7
    AND SUM(x) <> cap PER grp
MAXIMIZE SUM(x);
