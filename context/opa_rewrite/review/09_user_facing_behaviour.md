<!-- Full report of a review agent, saved 2026-11-16. Model output, not verified unless the main report (../handoff.md) marks a finding as re-checked. -->

# A ("ANR") vs B ("OPA"): user-facing comparison

## Verdict from the user's chair

B is the build I would rather use. Across roughly 4,500 queries, I found no case where B crashes, raises an INTERNAL error, or returns an answer I could show to be wrong while A does better. A has six such classes: two INTERNAL errors (one kills the whole session), three wrong answers and one false "infeasible". Where both solve a query, their answers are row-for-row identical, with one equal-objective tie-break exception. B is worse for a user in two places, both judgement calls: it drops a "constrains nothing" warning in one shape, and its EXPLAIN omits lines A prints. Both builds share one wrong answer against the guide (`per g: x <= 2` over a NULL key) and several message-quality problems.

Three things to know before trusting this:
- **No independent oracle.** Where A and B agree, correctness is unverified apart from a few hand checks. I tried to run the repo's fuzz judge (`python -m fuzz` with `DECIDB_EXE_PATH` per binary); the permission classifier blocked it and I did not work around it.
- **B is A plus 140 commits** (A's HEAD `300e387bb1` is an ancestor of B's `7837e300be`). Some differences are bug fixes that arrived in those commits, not evaluator behaviour as such; `test_fuzz_defects.py` exists only in B's tree.
- **Query sources.** Besides hand-written queries I used repo text as query sources: string literals from `test/decide/tests/*.py`, `golden/corpus.sql`, the `sql` field of `tests/expected/*.json`, the fault catalogue of `test_error_precedence.py`, and 2,000 scripts from the `test/decide/fuzz` generators (text only, generated before the block).

No repo file was edited. Everything ran on default settings and the default solver backend.

## Query families

| # | Family | Queries | A | B | Agree? |
|---|---|---|---|---|---|
| 1 | Plain: per-row + sum objective, `per K:`, `when`, keyed, `per ()`, split order, feasibility-only, BETWEEN/IN | 10 | answers | identical | yes |
| 2 | Nested reducers depth 2 and 3, constraints and objective, deck example 9 | 17 + 72 ANR fixtures | answers; one genuine infeasible | identical | yes |
| 3 | Variant signatures, `=` bound with two values, objective produced twice, colon form over a join | 15 | answers and errors | identical | yes (EXPLAIN text differs, D11) |
| 4 | Frames: `at(previous else 0)`, ranges, `every`, `all`, cyclic, `within`, frame limit | 24 | answers, errors, warnings | identical | yes |
| 5 | NULLs: keys in `by`/`per`/`within`, coefficients, bounds, empty groups, `when` dropping all rows | 38 | answers | identical on these | yes here; **no** on three further shapes (D1, D3, D4) |
| 6a | Row error on a dropped vs a kept row (`ln(0)`, `sqrt(-1)`, bad cast, `/0`) | 30 | no error when dropped, error when kept | same errors | yes; B adds a caret on cast errors (D10) |
| 6b | Two errors in one query | 158 mine + 1,294 from the precedence catalogue | deterministic | deterministic | **15 of 1,258 ordered pairs differ**, plus one harvested case (D7) |
| 6c | NaN, infinity, infeasible, unbounded | in 6a | messages | identical | yes |
| 7a | EXPLAIN / EXPLAIN ANALYZE, nested and framed | 22 | text | text | **no** (D11) |
| 7b | DIAGNOSE: infeasible, unbounded, feasible, expanded scope, nested, framed | 12 + 2 on 6,000 rows | findings | identical | yes |
| 8 | Threads 1 vs 8, stored 6,000-row table (3 row groups) plus 850 generated scripts | 28 × 3 tables + 850 | no sensitivity | no sensitivity | yes |
| 9 | Row order: same data inserted ascending, reversed, shuffled | 28 × 3 | ties and some messages follow row order | same, except one tie | one equal-objective difference (D12) |
| 10 | Deck examples 1–8 over a join, LEFT JOIN variants | 13 | answers | identical | yes |
| 11 | `golden/corpus.sql` | 113 | answers | identical | yes |
| 12 | Literals harvested from test files | 426 | 2 INTERNAL, 3 wrong, 1 false infeasible | answers | **6 differ** (D2–D6) |
| 13 | SQL from `tests/expected/*.json` | 221 | — | — | **4 differ** (D7, D8, D11) |
| 14 | Generated scripts (nests, frames, bounds, order_keys) | 2,000 | 6 INTERNAL | answers | **22 scripts differ** = 8 cases (D1, D9) |

## Disagreements in full

### D1. A raises an INTERNAL error and invalidates the session: a keyed decision absent on a NULL-key row, read by a range frame
- **Severity:** blocker on A. **Confidence:** certain.
- **Location:** thrown at `/Users/hatim/Code/decidb/.claude/worktrees/sami-anr/src/include/duckdb/decidb/formulation/ilp_model.hpp:134`.

```sql
CREATE TABLE k(id INT, p VARCHAR, s INT, v INT);
INSERT INTO k VALUES (1, NULL, 1, 6), (2, 'A', 2, 5), (3, 'A', 3, 5), (4, 'B', 4, 2);
SELECT id, x, y FROM k t DECIDE per t.p: x(REAL), y(REAL)
SUCH THAT x = t.v AND y <= 100 AND y >= max(from previous to next: x) over (t.s) + 1
MINIMIZE sum(y) ORDER BY id;
SELECT count(*) AS rows_still_readable FROM k;
```
- **A:**
  ```
  INTERNAL Error: DECIDE: a column was asked of instance 2 of decision 0, which has none: it has no value, or is written into its readers
  Stack Trace: ...
  FATAL Error: Failed: database has been invalidated because of a previous fatal error. The database must be restarted prior to being used again.
  ```
  The follow-up `SELECT count(*)` fails with the same FATAL error.
- **B:** `1|NULL|6.0`, `2|5.0|6.0`, `3|5.0|6.0`, `4|2.0|6.0`, then `rows_still_readable = 4`.
- **Same on:** `sum(from first to last: x)` and `min(from previous to next: x)`; generated scripts `frames_037`, `frames_047`, `frames_070`.
- **Which is right:** B. I checked B's `sum` and `min` variants by hand against guide §5.6 (a NULL among the visited positions is skipped). A point frame (`at(first: x)`) and `at(previous else 0: x)` work on both.
- **Why it matters:** one NULL in a key column costs the A user their in-memory session.

### D2. A raises an INTERNAL error on `sum(abs(sum(abs(x))))`
- **Severity:** blocker on A. **Confidence:** certain.
```sql
SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT)
SUCH THAT x <= 3 AND sum(abs(sum(abs(x)))) <= 4 MAXIMIZE sum(t.id * x) ORDER BY id;
```
- **A:** `INTERNAL Error: DECIDE term 'abs(x)' holds a decision variable under an expression that is not linear algebra` plus a stack trace. This one is raised at plan time and does not invalidate the session.
- **B:** `1|0`, `2|2`. Correct by hand: the clause is `2·(|x1|+|x2|) <= 4`.
- The `>= 4 MINIMIZE` twin behaves the same way (B: `1|2`, `2|0`).

### D3. A returns a wrong answer: `per K: sum(x) by (G) <= literal` constrains groups no class of K reads
- **Severity:** blocker on A (silent wrong answer). **Confidence:** certain.
```sql
CREATE TABLE w5(id INT, g VARCHAR, k INT, cap INT);
INSERT INTO w5 VALUES (1,'a',1,2),(2,'a',NULL,2),(3,'b',NULL,2),(4,'b',NULL,2);
SELECT id, g, k, x FROM w5 t DECIDE x(INT)
SUCH THAT x <= 100 AND per t.k: sum(x) by (t.g) <= 3 MAXIMIZE sum(x) ORDER BY id;
```
- **A:** `1|a|1|3`, `2|a|NULL|0`, `3|b|NULL|3`, `4|b|NULL|0`
- **B:** `1|a|1|3`, `2|a|NULL|0`, `3|b|NULL|100`, `4|b|NULL|100`
- **Which is right:** B. Guide §6.2: "a row whose written `per` column is NULL gets no constraint from the clause". Group `b` has no row with a `k`, so nothing generates a constraint on it.
- With a data bound (`<= t.cap`) A is also right, so on A the answer depends on whether the bound is a literal.
- **Related repros:**
  - `test_fuzz_defects.py:22`: A `1,1,0`, B `1,5,5`.
  - The `>= 5 MINIMIZE` form over `(1,NULL,1,5),(2,'a',NULL,5)`: A sets `x2 = 5`, B `0,0`.

### D4. A returns a wrong answer: `min(per g: x)` and `max(per g: x)` read the row whose `g` is NULL
- **Severity:** blocker on A. **Confidence:** certain.
```sql
SELECT id, x FROM (VALUES (1,'b'),(2,'a'),(3,NULL)) t(id, g) DECIDE x(INT)
SUCH THAT x <= 2 AND min(per t.g: x) <= 0 MAXIMIZE sum((4 - t.id) * x) ORDER BY id;
```
- **A:** `1|2`, `2|2`, `3|0` (uses the NULL-g row to satisfy the minimum).
- **B:** `1|2`, `2|0`, `3|2`.
- **Which is right:** B. Guide §4/§5.3: a row whose inner `per` key is NULL is in no class.
- A is inconsistent with itself: `sum(per t.g: x)` and `avg(per t.g: x)` skip that row on both builds.
- `max(per t.g: x) >= 2 MINIMIZE ...` differs the same way (A `0,0,2`; B `0,2,0`).

### D5. A returns an answer that violates its own constraint: BOOL product over a signed decision
- **Severity:** blocker on A. **Confidence:** certain (arithmetic check).
```sql
SELECT id, x, b, y FROM (VALUES (1), (2)) t(id) DECIDE x(INT), b(BOOL), per (): y(BOOL)
SUCH THAT x <= 3 AND when t.id = 1: x >= -5 AND sum(b * x) <= 2 * y - 6
MINIMIZE 10 * y + SUM(x) ORDER BY id;
```
- **A:** `1|-5|1|0`, `2|0|0|0`. Here `sum(b*x) = -5` and `2*0 - 6 = -6`, and `-5 <= -6` is false.
- **B:** `1|-5|1|1`, `2|0|0|1`. Here `-5 <= -4` holds, with objective 5.

### D6. A reports a false "infeasible": BOOL factor on a reducer over signed decisions
- **Severity:** blocker on A. **Confidence:** certain.
```sql
SELECT id, x, b FROM (VALUES (1), (2)) t(id) DECIDE x(INT), per (): b(BOOL)
SUCH THAT x >= -5 AND x <= 3 AND b * sum(x) + 3 <= 1 MINIMIZE SUM(x) - 10 * b ORDER BY id;
```
- **A:** `Invalid Input Error: DECIDE optimization is infeasible: b * sum(x) + 3 <= 1 cannot hold because (b * sum(x)) is never negative.`
- **B:** `1|-5|1`, `2|-5|1`.
- A's stated reason is false: `x >= -5` makes `x` signed. The A user is told to give up on a solvable query.

### D7. Which error is reported first: the NaN a range frame reads
- **Severity:** minor. **Confidence:** certain.
- 15 of 1,258 ordered fault pairs differ, and every one involves a NaN read by a range frame over data.
```sql
CREATE TABLE e(id INT, tl VARCHAR, q INT, fval DOUBLE, root DOUBLE, weight DOUBLE);
INSERT INTO e VALUES (1,'A',1,'nan',-1,'nan'),(2,'A',2,9,1,1),(3,'B',1,9,1,1),(4,'B',2,9,1,1);
-- (a)
SELECT id, x FROM e t DECIDE x(REAL) SUCH THAT x <= 9
  AND x <= min(from first to last: t.fval) over (t.q within t.tl)
  AND abs(sqrt(t.root) * x - 1) <= 30 MINIMIZE sum(x);
-- (b)
SELECT id, x FROM e t DECIDE x(REAL) SUCH THAT x <= 9
  AND sum(t.weight * x) <= 100
  AND x <= min(from first to last: t.fval) over (t.q within t.tl) MINIMIZE sum(x);
```

| | A | B |
|---|---|---|
| (a) | `DECIDE: min(from first to last: t.fval) over (t.q within t.tl) reads a NaN on some rows. Skip those rows with a 'when' on the column it reads.` | `Out of Range Error: cannot take square root of a negative number` |
| (b) | the same frame NaN message | `DECIDE: t.weight is NaN or infinite on some rows. Skip those rows with 'when isfinite(t.weight):'.` |

- **Pattern:** on A the frame NaN beats everything except four fault kinds. On B it additionally loses to row errors under `abs`, the objective-produced-twice check, the two-rows-at-a-position check and a constant's error, and follows written order against a plain NaN coefficient or bound.
- **Second instance** (`tests/expected/run-db9e6789c98d2726.json`): `x <= sqrt(t.v - 5) AND sum(from last to last: ln(t.w - 1.5) * x) over (...) <= 7`. A reports the logarithm error from the second clause; B reports the square-root error from the first.
- **Which is better:** neither is wrong, since both errors are real. B is slightly more predictable. A's frame message is the more actionable text, because it names the clause and B's `Out of Range Error` names nothing.

### D8. Collated keys: which spelling a message names
- **Severity:** minor (A's second message is misleading). **Confidence:** certain.
```sql
CREATE TABLE c(id INT, g VARCHAR COLLATE NOCASE, p INT, v DOUBLE, cap INT);
INSERT INTO c VALUES (1,'b',1,4,3),(2,'B',1,5,4),(3,'b',2,6,3),(4,'a',1,7,9);
SELECT id, x FROM c t DECIDE x(REAL) SUCH THAT x <= 9
  AND x <= at(previous else 0: t.v) over (t.p within t.g) MAXIMIZE sum(x);
SELECT id, x FROM c t DECIDE x(REAL) SUCH THAT x <= 9 MAXIMIZE sum(x) by (t.g);
```

| | A | B |
|---|---|---|
| frame | `... finds 2 rows at t.g = 'b', t.p = 1; ...` | `... finds 2 rows at t.g = 'B', t.p = 1; ...` |
| objective | `'t.g' in by (t.g) takes more than one value over the kept rows ('B', 'b'), ...` | `... ('a', 'b'), ...` |

- **Which is right:** B. Guide §6.2 (D69) says a message names the group by its least spelling. A's objective message names `'B'` and `'b'`, which are one value under NOCASE; the two real values are `a` and `b`.
- The timeline-span message differs the same way (`'b', 'a'` on A, `'B', 'A'` on B).

### D9. The "constrains nothing" warning: instance count, and one shape where B says nothing
- **Severity:** minor (count); major-leaning for the missing warning. **Confidence:** certain on the outputs, likely on which is right.
- **Location:** `/Users/hatim/Code/decidb/.claude/worktrees/sami-opa/src/execution/operator/decide/physical_decide.cpp:994` (A: line 3655 of the same file in `sami-anr`).

**(a) Count.**
```sql
CREATE TABLE w2(id INT, g VARCHAR, k INT, cap INT);
INSERT INTO w2 VALUES (1,'a',1,NULL),(2,'b',NULL,5);
SELECT id, x FROM w2 t DECIDE x(INT)
SUCH THAT x <= 100 AND per t.k: sum(x) by (t.g) >= t.cap MINIMIZE sum(x) ORDER BY id;
```
- **A:** `DECIDE warning: 'per t.k: sum(x) by (t.g) >= t.cap' constrains nothing: each of its 2 instances has no value, so each is skipped.`
- **B:** `... constrains nothing: its one instance has no value, so it is skipped.`
- EXPLAIN ANALYZE mirrors this: `↳ 2 of 2 instances have no value and are skipped` on A, `↳ its one instance has no value and is skipped` on B.
- B's count matches the language: `per t.k` has one class. A counts `by` groups of the kept rows, including one no class reads.

**(b) B silent.**
```sql
CREATE TABLE w3(id INT, g VARCHAR, k INT, cap INT);
INSERT INTO w3 VALUES (1,NULL,1,5),(2,'a',NULL,NULL);
SELECT id, x FROM w3 t DECIDE x(INT)
SUCH THAT x <= 100 AND per t.k: sum(x) by (t.g) >= t.cap MINIMIZE sum(x) ORDER BY id;
```
- **A:** answer `0,0` plus `DECIDE warning: ... constrains nothing: its one instance has no value, so it is skipped.`
- **B:** answer `0,0` and no warning.
- Also seen on generated script `bounds_185`.
- **Which is better:** here A is. The clause constrains nothing on both, and only A says so. A's "one instance" is arguably the wrong one (group `a`, which `per t.k` never generates). B appears to treat a class whose `by` key is NULL as holding no instance. Guide §7.4 does not settle this case.
- **Options:**
  - Warn whenever a clause with at least one class ends with no model row. Most helpful, needs a count wording for "one class, no group".
  - Keep B's silence and document it in §7.4 beside the "NULL on every row" case. Simplest, but a user gets no signal.
  - Keep A's behaviour. It warns, with a count that does not match the language.

### D10. Conversion errors: B points at the offending expression in more places
- **Severity:** minor (B better). **Confidence:** certain.
```sql
CREATE TABLE s(id INT, txt VARCHAR, v INT); INSERT INTO s VALUES (1,'5',1),(2,'',2),(3,'7',3);
SELECT id, x FROM s DECIDE x(INT) SUCH THAT x <= 10 AND when CAST(txt AS INT) > 3: x <= 2 MAXIMIZE sum(v * x) ORDER BY id;
```
- **A:** `Conversion Error: Could not convert string '' to INT32`
- **B:** the same line, followed by
  ```
  LINE 1: ...LECT id, x FROM s DECIDE x(INT) SUCH THAT x <= 10 AND when CAST(txt AS INT) > 3: x <= 2 MAXIMIZE sum(v * x) ORDER BY...
                                                                        ^
  ```
- B adds the caret for a cast in a clause's `when`, a `by` key, a reducer's own `when` and the objective's `when`. A adds it only for a cast in a bound.
- Neither build locates `ln` or `sqrt` errors (shared surprise S6).

### D11. EXPLAIN and EXPLAIN ANALYZE text
- **Severity:** minor. **Confidence:** certain. All four are wording and format calls.
- Setup used for (a) and (b): `CREATE TABLE t(id INT, g VARCHAR, h VARCHAR, k INT, v INT, cap INT, p BOOLEAN)` with 12 rows.

**(a) Data bounds under `per` / `by`.** `EXPLAIN SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= 10 AND per t.g: sum(x) <= t.k * 20 MAXIMIZE sum(v * x);`
- **A:** `↳ per t.g: sum(x) <= "t.k * 20"`, and the plan tree gains `CTE direct_bound`, `UNGROUPED_AGGREGATE`, `CROSS_PRODUCT` and `CTE_SCAN` under DECIDE.
- **B:** `↳ per t.g: sum(x) <= min(CASE  WHEN ((g IS NULL)) THEN (NULL) ELSE k * 20 END)`, and the plan is just `DECIDE` over `SEQ_SCAN`.
- For `sum(x) by (t.g) <= t.cap`: A prints `CASE WHEN ((g IS NULL)) THEN (NULL) ELSE #4 END`; B prints `... ELSE min(t.cap) END`.
- B's plan is cleaner, and its text shows that the tightest value is taken. A's `#4` means nothing to a user. Both leak an internal NULL guard, against guide §9 ("never an internal name"). B also drops the `t.` qualifier inconsistently (`k * 20`, `cap`, but `min(t.cap)`).

**(b) Nested reducers in a constraint.** `per t.g: sum(per t.h: max(x) by (t.g, t.h)) by (t.g) <= t.cap / 2`
- **A** prints, for the outer sum, `↳ sum(per t.h: max(x) by (t.g, t.h)) by (t.g) becomes a helper decision per group`, and under ANALYZE `↳ ... is written where it is read, with no column`.
- **B** prints neither line for the outer sum; the inner `max` lines are identical.
- A's two lines contradict each other at first read. B is shorter but silent about the outer level.
- B still prints every level for the same nest in the objective, so B is inconsistent between constraint and objective.

**(c) `abs`.** `EXPLAIN ... SUCH THAT x <= 30 AND abs(x - 20) <= 5 ...`
- **A** adds `↳ abs(x - 20) - x >= -20` and `↳ abs(x - 20) + x >= 0 + 20`.
- **B** stops at `↳ abs(x - 20) becomes an auxiliary variable`.
- Guide §9 promises "the rows the solver receives", so B no longer matches the doc. A's `>= 0 + 20` is itself unpolished.

**(d)** The skipped-instance line of D9.

- **Options:**
  - Keep B's shorter text and update guide §9.
  - Restore the solver rows and the outer-helper line on B.
  - In either case, render a data bound as the written expression plus a plain note ("tightest value per group") instead of a CASE guard or `#4`.

### D12. Different but equally optimal answer in one row order
- **Severity:** minor. **Confidence:** certain.
- **Query** (6,000-row stored table, rows inserted in a shuffled order):
  ```sql
  SELECT id, x FROM big_shuf DECIDE x(INT)
  SUCH THAT x <= 10 AND per g: sum(per h: max(x) by (g, h)) by (g) <= 30
  MAXIMIZE sum(v * x) ORDER BY id;
  ```
- 33 rows differ, all in group `g47`: A opens `h2`, B opens `h3`. Each choice is worth 4,400.
- Both are feasible and both reach objective 1,047,360, which I checked in SQL.
- On this query B returns the same assignment for all three row orders; A's changes on the shuffled table.
- On the other eight queries I compared row by row, the two builds are identical, and both vary with row order the same way.
- Setup script: `/private/tmp/claude-501/-Users-hatim--claude-spares-spare-Ungos1/c596838a-2de5-42c6-8006-2a00850e535d/scratchpad/user/cmp/mkdb.sql`.

## Things both builds do that would surprise a user

### S1. `per g: x <= 2` constrains the row whose `g` is NULL (wrong answer against the guide)
- **Severity:** blocker-class on both. **Confidence:** certain.
```sql
CREATE TABLE n(id INT, g VARCHAR, cap INT, v INT);
INSERT INTO n VALUES (1,'a',5,3),(2,NULL,7,5),(3,'b',4,6);
SELECT id, g, x FROM n DECIDE x(INT) SUCH THAT x <= 10 AND per g: x <= 2 MAXIMIZE sum(v * x) ORDER BY id;
```
- Both return `1|a|2`, `2|NULL|2`, `3|b|2`. Guide §6.2 says the NULL-key row gets no constraint, so row 2 should be 10.
- Both get it right as soon as the clause changes slightly: `when id > 0 per g: x <= 2` gives `2|NULL|10`, and `per g: x <= cap` gives `2|NULL|10`.
- `per g: x >= 2 MINIMIZE` is wrong the same way.
- Also wrong: `per g, h:`, `2 * x <= 4`, `x = 2` and `BETWEEN`. `x <> 10` is right.
- **Options:** fix the literal-bound path to honour the NULL-key rule, or change the guide. The second leaves three spellings of one clause giving two answers.

### S2. The reported error does not follow written order
- **Severity:** minor on both. **Confidence:** certain.
- The first-written fault wins in 641 of 1,258 ordered pairs on A and 644 on B.
- Example on both: an `=`-bound-with-two-values error beats an earlier-written `ln(0)` clause, and a `b >= 2` BOOL-domain error beats everything.
- The choice is deterministic and thread-independent on both.

### S3. Stored row order decides some messages
- **Severity:** minor on both. **Confidence:** certain.
- Tested on the 6,000-row table in three insertion orders, identically on both builds and at 1 and 8 threads.
- `x <= ln(lnarg) + sqrt(sqarg) + 10` reports the logarithm error in one order and the square-root error in another.
- `sum(nan_b * x + nan_a * y)` names `nan_a` or `nan_b` depending on order.
- The timeline-span message names `g = 'g1'`, `'g0'` or `'g31'` depending on order.
- The `=`-bound, objective-twice and two-rows-at-a-position messages are stable.

### S4. Output row order without ORDER BY varies run to run
- **Severity:** minor on both. **Confidence:** certain.
- `SELECT id, x FROM (VALUES (1,'EU',2024),(2,'US',2025)) t(id, region, yr) DECIDE x(BOOL) SUCH THAT per region, yr: sum(x) by (region, yr) >= 1 MAXIMIZE SUM(x);` printed `2|1` before `1|1` in 8 of 60 runs on each build.
- It was stable at `SET threads=1`.

### S5. Warnings depend on the internal solve path
- **Severity:** minor on both. **Confidence:** likely.
- `... SUCH THAT x <= 4 MAXIMIZE when g = 'zzz': sum(v * x)` returns all zeros with no "objective is 0 whatever the decisions" warning.
- Guide §7.4 says a direct-solve answer prints nothing, which I take to be the reason; I did not confirm the path with EXPLAIN. A user cannot tell which path ran.

### S6. Row errors carry no location
- `Out of Range Error: cannot take logarithm of zero` names no clause, column or row on either build.

### S7. Smaller message and output frictions on both
- The `=`-bound message says "takes more than one value here (6 and 10)" without naming the group.
- A timeline-span message prints `per t.cost ... t."cost" = 2`, quoting the column in one place only.
- DIAGNOSE:
  - Prints `group = id = '11'` for an integer.
  - Prints `group = g = 'NULL'` beside `suggested_change = when g IS NULL: x <= <cap>`.
  - Reports `row = 0`, a 0-based position the user cannot map to their data.
  - Suggests `when g = 'c'` where the query wrote `t.g`.
- EXPLAIN ANALYZE prints no variants line for `per t.g: sum(x) <= t.k * 20` although class `a` holds two values of `k`; it does print one for `per t.g: y <= t.k + 3` over a keyed `y`. Guide §7.1 reads as if both should.
- `x <= count(from first to previous: t.demand) over (...)` is unbounded, because the first position's count has no value rather than 0. This is documented, but not what a SQL user expects of `count`.

## Overall judgement

**B behaves better for a user.**

- **Direction: high confidence.** All six of A's crash and wrong-answer classes are reproduced with small repros, and B's answer was checked by hand in each. No B crash or B-only wrong answer turned up in about 4,500 queries, including 1,000 generated scripts at two thread counts.
- **Completeness: medium confidence.** Agreement between the two builds is not correctness, and S1 shows they can agree on a wrong answer.

B is better on: D1–D6 (crashes and wrong answers), D8 (collated spellings), D10 (cast-error caret), D9a (instance count), D11a (cleaner plan) and D12 (row-order stability on one query).

B is worse or arguable on: D9b (lost warning), D11b and D11c (EXPLAIN lines dropped, and guide §9 no longer matches), and D7 (the frame NaN message, which names its clause, now loses to bare `Out of Range Error`s).

## Not covered

- An oracle-judged run, as noted in the verdict.
- The HiGHS backend (`DECIDB_FORCE_SOLVER=highs`) and `decide_direct_solve='off'` beyond a handful of queries.
- Quadratic, `norm` and bilinear shapes beyond what the harvested test literals contain.
- Prepared statements, views, macros, serialization, time-limit and Ctrl-C paths.
- Nesting deeper than 3.
- `EXPLAIN (FORMAT JSON)` beyond one query.
- Whether the 6,000-row scans truly ran in parallel: the tables have three row groups, but I could not observe it directly.
- Tests that build SQL with f-strings (most of `test_scope_*`, `test_anr_nested.py`, `test_anr_frames.py`, `test_null_values.py`, `test_collated_keys.py`) were not harvested; those areas are covered only by my own queries, the 72 ANR fixtures and the generated scripts.

One housekeeping note: I wrote one stray file to `/tmp/x` by mistake early on and deleted it immediately.

## Open design calls for the user

1. **S1:** should `per g: x <= 2` skip NULL-key rows as the guide says, or should the guide change?
2. **D9b:** should a clause whose only classes read a NULL `by` key warn that it constrains nothing, and with what count?
3. **D11:** should EXPLAIN keep the solver rows for `abs` and the outer-helper lines for nests (A, and guide §9) or the shorter B text? How should a data bound be rendered so that neither `#4` nor a CASE guard shows?
4. **D7 / S2:** is "which error first" something to promise (written order), or to leave as is and say so in `errors.md`?
5. **S3:** should the row-error and timeline-span messages be made independent of row order, as the objective check already is?
6. **S5:** should warnings be printed on the direct-solve path too?
7. **S6:** should DuckDB row errors raised inside a clause be wrapped with the clause text, as B now does with a caret for casts?

## Files

Everything is under `/private/tmp/claude-501/-Users-hatim--claude-spares-spare-Ungos1/c596838a-2de5-42c6-8006-2a00850e535d/scratchpad/user/cmp/`.

- **Hand-written cases:** `cases/p01_plain.sql` through `cases/p19_explain3.sql`.
- **Minimal repros of the disagreements:** `cases/p15_repro2.sql`, `cases/p16_repro3.sql`, `cases/p18_repro4.sql`, `cases/p14_repro.sql` (D7, D8, D10).
- **Runners:** `iso.py` (each labelled block in a fresh process on both builds; the easiest way to re-run a repro), `run.sh`, `thr.sh` with `thr_queries.sql` and `mkdb.sql`.
- **Error-pair grids:** `cases/p07_pairs.sql`, `cases/p13_precedence.sql`, with winners in `out/p13.A.sum` and `out/p13.B.sum`.
- **Repo-sourced queries:** `harv/`, `harv2/`, `fuzz/q/`; kept differing outputs in `out/harv/`, `out/harv2/`, `fuzz/out1/`.
