# Errors, refusals and removed spellings

Every message DECIDE raises for a query it will not run, grouped by why. Messages quote the user's SQL, never an
internal name, and say what to write instead. All of them are still raised under `DIAGNOSE`: nothing was solved, so
there is nothing to diagnose. The data checks that run before solving are in [guide.md §7.3](guide.md#73-data-checks).

## 1. Errors

| Situation | Raised | Message |
|---|---|---|
| The objective takes two values or instances over its kept rows | run, before solving | [guide.md §7.3](guide.md#73-data-checks) |
| A reducer in a `when` | run, before solving | `DECIDE: an aggregate cannot be used here. ...` |
| `per` other than `()` on the objective | parse | `an objective is produced once: write per (): or leave per out` |
| A parenthesized key | parse | `write per a, b: without parentheses; parentheses are only for per ()` |
| An expression in a `per` key | parse | `a per key lists columns or relations; put expressions in by (...)` |
| A declaration with no type | parse | `DECIDE variable "x" needs a type; write x(INT), x(BOOL) or x(REAL)` |
| A comma between constraints | parse | `comma-separated SUCH THAT constraints are not supported; use AND between constraints` |
| `by` after anything but a function call (a column, literal, `CASE`, `CAST`, `::`, `COALESCE`, parentheses, …), or a scope inside such a form | parse | syntax error |
| A removed spelling | parse (`D.x`: bind) | names the new spelling (§3) |
| `OR` between constraints | bind | `SUCH THAT: OR is not supported.` |
| A comparison, `BETWEEN` or conjunction that reads a decision, used as a number (`x <= (x > 1)`) | bind | `SUCH THAT does not support '(x > 1)'` (`SUM expression` / `MAXIMIZE` inside a reducer / the objective) |
| A column of an outer query (`LATERAL`, a scalar subquery in another query's select list), in any clause or the `WHERE` | bind | `DECIDE: 'k' refers to an outer query, which a DECIDE cannot read; to solve per group, join that table into this query's FROM and write the clause with per` |
| `by` after a call other than `SUM`, `AVG`, `MIN`, `MAX`, `norm`, `count` | bind | `DECIDE: by (...) must directly follow SUM, AVG, MIN, MAX, norm or count` |
| `when` or `per` inside a call other than `SUM`, `AVG`, `MIN`, `MAX` | bind | `DECIDE: when and per inside a call need SUM, AVG, MIN or MAX; write the scope in front of the constraint or inside a reducer` |
| A decision in a `when`, a key or a `by` | bind | `DECIDE: x is a decision; when, per and by may only use data` |
| A window function in a `when`, a key or a `by` | bind | `DECIDE: a window function such as 'row_number() OVER ()' is not supported in when, per or by` |
| A reducer in a `when` or a `by` key | bind | `DECIDE: a reducer such as 'sum(cap)' is not supported in when, per or by` |
| A volatile function (`random()`) in a constraint, the objective or a `by` key | bind | `DECIDE: random() is not supported in a constraint` (`the objective`, `a by key`) |
| An unknown key element (keys resolve in their own query's `FROM`) | bind | `DECIDE: depot is neither a column nor a relation of the FROM clause` |
| `rowid` or a generated column in a key | bind | `DECIDE: g.b is not a stored column; a per key lists columns or relations of the FROM clause` (`a within key` in a frame) |
| A frame's `k` or `every` that is not a positive whole number | parse | `... must be a positive whole number` |
| A frame word misspelled (`prev`, `evry`, `cyclc`) | parse | `DECIDE: a frame position is first, last, [k] previous or [k] next, found prev` (`expected every d ...`, `expected cyclic or within ...`) |
| An expression in a frame's `within` | parse | `DECIDE: within lists columns or relations, as a per key does` |
| A range frame over more than one expression, or a reducer other than `sum`, `avg`, `min`, `max`, `count` | parse / bind | `sum(from first to last: e) reduces one expression, found 2` · `DECIDE: a range frame reduces with sum, avg, min, max or count, found ...` |
| A frame's `else` that is not a number | bind | `DECIDE: in at(previous else 'a': x) over (t.period), else 'a' is not a number` |
| A decision in a frame's order key or `within` | bind | `DECIDE: x is a decision; a frame's order key and within may only use data` |
| A frame selector in a DECIDE clause that an outer query binds (a correlated DECIDE subquery whose frame reads an outer column) | bind | `DECIDE: a frame selector is not supported here: it reads the rows of the DECIDE query whose constraints or objective it stands in, and only those` |
| A frame selector outside a DECIDE clause (a `SELECT` list, a `WHERE`): the parser reads it in a clause only | parse | `Parser Error: syntax error at or near "over"` |
| An unqualified `FULL OUTER JOIN USING` column in a key | bind | `DECIDE: depotID is merged by a FULL OUTER JOIN USING; write S.depotID or D.depotID in the per key` |
| A reducer or `norm` with no argument | bind | `SUM needs one argument, found 'sum()'` · `NORM needs an expression and an order, as in norm(e, 1), found 'norm()'` |
| A colon form naming a relation not in `FROM` | bind | `Relation 'X' is not in the FROM clause, so a reducer cannot be qualified by it.` |
| A comparison whose reducers, under arithmetic, read no decision (`SUM(t.v) + 1 <= 500`) | bind | `SUCH THAT clause: '(sum(l_quantity) + 1)' reads no DECIDE variable; compare a decision with it` |
| A reducer over query-wide decisions only, as a constraint's whole decision side | bind | `cap is a query-wide decision, so SUM(cap) has nothing to aggregate over; use cap on its own` |
| An `INT` or `REAL` decision as a factor on a reducer, or a `BOOL` one on anything but a bare reducer | bind | `DECIDE constraint: 's' is a decision, so it cannot multiply SUM(x). Only data or a BOOL decision can scale SUM/AVG/MIN/MAX.` (objective: `... Only constants, query-wide values or a BOOL decision can scale ...`) |
| A row-varying factor on a reducer in the objective | bind | `DECIDE objective: 'weight' varies per row, so it cannot multiply SUM(x). Move it inside the aggregate, e.g. SUM(x * weight).` (a correlated subquery is named "this subquery") |
| A data-only reducer scaling a decision (`x * avg(cap)`, `sum(x) / sum(cap)`) | bind | `An aggregate over table columns ... cannot multiply / divide / be divided together with a decision variable` |
| `<`, `>` or `<>` on a side that is not whole-numbered | bind | names the offender and the cast that fixes it |
| `<`, `>` or `<>` on a helper decision whose value can be fractional | bind | `Comparison '>' is not supported here: 'sum(ship + 0.5) by (D.region)' can take fractional values in this constraint, and stepping the bound is exact only on whole numbers. Compare with '>=' instead.` |
| A strict bound on a `MIN`/`MAX` whose body can be fractional once its constant addends move to the bound | bind | `Comparison '>' is not supported here: 'max(x + w)' has type DECIMAL(21,1) ... Compare with '>=' instead.` |
| A NULL beside a decision | bind | `DECIDE: '(x + NULL)' is NULL whatever the decisions are; replace the NULL with a value, as in COALESCE(column, 0)` |
| A cast over a decision | bind | refused in `SUCH THAT` and the objective ([guide.md §3.2](guide.md#32-numeric-precision-and-casts)) |
| A non-linear function over a decision | bind | `Scalar function 'sqrt' over a DECIDE variable is not supported: it would make the model non-linear. Only ABS() and POWER(..., 2) can wrap a decision variable.` |
| `POWER(e, n)` over a decision with `n` other than the constant 2 | bind | names the exponent |
| Division by a decision | bind | `Division by a DECIDE variable is not supported` |
| Three or more decisions in one product | bind | `Triple or higher-order products of DECIDE variables are not supported (total degree > 2)` |
| A `BOOL` bound that cannot hold (`x >= 2`) | before reading rows | `DECIDE optimization is infeasible: x >= 2 cannot hold for a BOOL variable.` |
| A NaN in the data, or an infinite coefficient | run | `DECIDE: v is NaN or infinite on some rows. Skip those rows with 'when isfinite(v):'.` (a bound: `DECIDE: the bound v is NaN on some rows. Skip those rows with 'when NOT isnan(v):'.`) |
| A number past a DOUBLE that DeciDB derives (`SUM(b*x)` over rows of one keyed decision, `POWER(b*x, 2)`, a Big-M or range over the decisions' bounds) or a literal coefficient (`2e308*x`) | run | `DECIDE: SUM(b * x) <= 10 computes a number too large for a DOUBLE. Scale its data down.` (names the clause, or the objective) |
| An infinite bound under `ABS`, `MIN`, `MAX` or `<>` | run | `DECIDE: the bound v is infinite on some rows, and this clause needs a finite one. Skip those rows with 'when isfinite(v):'.` |
| An unbounded member under a lowered hard `MIN`/`MAX`, `<>`, Big-M `ABS` or McCormick product | run | names the variable and the bound to add |
| A model class the available solver cannot run (non-convex QP, MIQP, quadratic constraints on HiGHS) | plan | names what the query does and that it needs Gurobi |
| A range frame reading more values than `decide_frame_limit` | run, before solving | `DECIDE: sum(from first to last: t.v) over (t.period within t.product) reads more than 4 values over its ranges, above decide_frame_limit (4). Narrow the range, or raise the setting.` |
| A frame's selected position holding rows with two signatures of its payload (D73) | run, before solving | `DECIDE: at(previous: x) over (t.period within t.product) finds 2 rows at t.product = 'A', t.period = 1; a position must hold one row.` |
| Under `per K`, a class whose rows lie in two timelines of a frame its body reads (D75) | run, before solving | `DECIDE: per t.category does not determine the timeline t.product of at(first: t.v) over (t.period within t.product). t.category = 'Food' spans t.product = 'A', 'B'.` |
| A square or product under a `MIN`/`MAX` on HiGHS | plan | `DECIDE: 'max(power(x, 2))' takes the maximum of a squared or multiplied expression, which needs Gurobi — not available on this machine. Install Gurobi, or replace power(x, 2) with an expression linear in the decision variables.` |

DeciDB prevents no overflow, as DuckDB's DOUBLE arithmetic does not. Large finite numbers solve after the large-number
warning (`../pipeline/07_solver/done.md`), and the solver's verdict on them stands. An infinite bound is no bound. A
coefficient that is NaN or infinite where DeciDB reads it raises one of the two messages above: the first when a column
holds it, quoting that column's value even inside a merged or scaled coefficient (`2 * b` names `b`), TOO_LARGE naming
the clause otherwise.

## 2. Not supported yet

Each shape is refused with a message, never answered wrongly. Rows marked **D80** are the nesting limits the language
keeps ([guide.md §5.5](guide.md#55-reducers-inside-reducers--the-path-rule)). The others are limits on what DeciDB
formulates; their open work is in the owning stage's `todo.md`.

| Shape | Message |
|---|---|
| **D80** `norm` inside a reducer | `DECIDE: norm inside a reducer is not supported yet` |
| **D80** A reducer inside `norm` | `DECIDE: a reducer inside norm is not supported yet` |
| **D80** Inside a reducer, a reducer times a decision | `DECIDE: '(x - 1) * max(x) BY (t.g)' multiplies a reducer by a decision; only data can scale a reducer` |
| **D80** Inside a reducer, `abs` over a reducer an enclosing `when`, `per` or `by` chooses the rows of | `DECIDE: abs over a reducer inside another reducer is not supported yet: abs(max(x) BY (t.g))` |
| **D80** A square or product of decisions inside nested reducers that no body definition names: a nest of sums in the objective not read in place | `DECIDE: 'sum(x * x) by (t.g)' squares or multiplies decisions inside nested reducers, which is not supported yet` |
| A frame inside a reducer over data, on a bound or inside a reducer over decisions | `DECIDE: at(previous: t.v) over (t.period within t.product) inside sum(per t.product: at(previous: t.v) over (t.period within t.product)), a reducer over data, is not supported yet` |
| A frame in the bound of a clause over groups that takes its group's tightest value (D85): one that does not read one row per group | `DECIDE: at(previous: t.cap) over (t.period within t.g) on the bound side of sum(x) by (t.g) is not supported yet` |
| A frame's payload not linear in the decisions (`abs`, a decision divisor) | `DECIDE: 'abs((x - 3))' inside at(...) is not linear in the decisions, which is not supported yet; write it outside the frame` |
| A frame's `else` that reads a decision while its payload reads none | `DECIDE: in at(previous else s: t.demand) over (t.period), else s reads a decision while the payload reads none, which is not supported yet` |
| **D80** A frame in a `when`, `per`, `by`, `within` or an order key | `DECIDE: a frame selector is not allowed in when, per, by, within or an order key` |
| **D80** A reducer or a frame inside a frame's payload or fallback | `DECIDE: a reducer inside at(...) is not supported; reduce outside the frame` · `DECIDE: a frame selector inside at(...) is not supported` |
| **D80** A square or product of decisions in a frame's payload | `DECIDE: 'x * x' squares or multiplies decisions inside at(...), which is not supported yet` |
| **D80** A frame over decisions times a decision, or squared | `DECIDE: '(x * at(previous: x) over (t.period))' multiplies a decision read at another row by a decision, which is not supported yet` |
| `AVG` under `<>` beside another term, or with its own `when`/`per` | `DECIDE: AVG in a <> constraint is not supported yet ...` |
| `<>` over a square or product of decisions not linearized first (a product with a `BOOL` is) | `DECIDE: <> over a product or a power of decisions is not supported yet: power(x, 2) <> 9` |
| `<>` on a `MIN`/`MAX`, alone or composed | `DECIDE does not support <> comparison with MIN/MAX aggregates.` |
| A square, or a product of two decisions, outside a reducer in the objective, unless the two are distinct query-wide decisions or one is `BOOL` | `DECIDE objective: 'x * y' multiplies decisions outside a reducer, which is not supported yet` |
| A scope inside an ordinary SQL subquery written in the clause | `DECIDE: 'WHEN condition:' is not supported yet` (or `'PER key:'`, `'PER ():'`, `'PER ROW:'`, `'BY (key)'`) |
| `CASE WHEN` inside a constraint or objective | one message naming the `when`/`per` scopes and the CTE alternative |
| `IN` on a reducer | refused |

A reducer over data only stays on the bound side of a comparison.

## 3. Removed spellings

Each is a parse error whose message carries the phrase `is removed` and names the spelling to write, built from the
query's own text so it pastes back (`D.x` is the exception: a Binder Error). A condition is quoted whole as written, a
name that needs quotes is quoted, and a removed form inside a subquery of a clause is refused the same way. A
`WHEN` after a reducer followed by a bare comparison (`SUM(x) <= SUM(cap) WHEN cap > 2`) was never valid and stays a
plain syntax error, as does a removed form after a clause that already has a scope in front. Messages below are for
`FROM ... t(id, cap, g)`.

| Removed | Write instead | Message (key phrase) |
|---|---|---|
| `T.x(TYPE)` | `per T: x(TYPE)` | `the declaration t.x(INT) is removed; write per t: x(INT) (per t keys on every column of t; write per t.k: x(INT) to key on one column k)` |
| `T.x` (no type) | `per T: x(INT)` / `(BOOL)` / `(REAL)` | `the declaration t.x is removed; write per t: x(INT), per t: x(BOOL) or per t: x(REAL) (per t keys on every column of t)` |
| `scalar x(TYPE)`, `scalar T.x(TYPE)` | `per (): x(TYPE)` | `scalar s(INT) is removed; write per (): s(INT)` |
| `scalar x` (no type) | `per (): x(INT)` / … | `scalar s is removed; write per (): s(INT), per (): s(BOOL) or per (): s(REAL)` |
| `C PER g` · `C PER (g, h)` · `C PER g, h` | `per g, h: C`, with `by (g, h)` after each reducer | `the postfix PER is removed; write per g: in front of the constraint and by (g) after each of its reducers` |
| `C PER g`, `C` without a reducer | `C` | `the postfix PER is removed; a constraint with no reducer already holds on every row, so drop the PER` |
| `C WHEN c` | `when c: C` | `the postfix WHEN is removed; write when g = 'A': in front of the constraint` |
| `C WHEN c PER g` | `when c per g: C`, with `by (g)` | `the postfix PER is removed, as is the WHEN before it; write when g = 'A' per g: ...` (with the NULL note) |
| `SUM(e) WHEN c` | `sum(when c: e)`; after a reducer with a scope or `by`, the condition joins it | `the WHEN after sum(x) is removed; write sum(when g = 'A': x)` |
| `COUNT(*) WHEN c` · `norm(e, p) WHEN c` | `sum(when c: 1)` · `when c:` in front | `... write sum(when g = 'A': 1)` · `... norm takes no when inside it: write when g = 'A': in front of the constraint when the condition holds for all of it` |
| `SUM(D: e) WHEN c` | `sum(when c per D: e)` | `the WHEN after sum(t: x) is removed; write sum(when g = 'A' per t: x)` |
| `MAXIMIZE E WHEN c` | `MAXIMIZE when c: E` | `the postfix WHEN is removed; write MAXIMIZE when g = 'A': sum(x)` |
| `MINIMIZE OUTER(INNER(e)) PER g` | `MINIMIZE outer(per g: inner(e) by (g))`; a `WHEN` inside goes in front, the colon form inside stays (`max(per t: x)`) | `the postfix PER is removed; write MINIMIZE sum(per g: max(x) by (g))` |
| `MINIMIZE c * OUTER(INNER(e)) PER g` | the nested reducer where it stands | `the postfix PER is removed; write the nested reducer in 2 * sum(max(x)) where it stands as outer(per g: inner(e) by (g)), as in MINIMIZE sum(per g: max(e) by (g))` |
| `MAXIMIZE SUM(e) PER g` | `MAXIMIZE sum(e)` | `the postfix PER is removed; an objective is produced once: write MAXIMIZE sum(x)` |
| `MAXIMIZE MAX(e) PER g` | `MAXIMIZE max(e)`, or `sum(per g: max(e) by (g))` | `... an objective is produced once: write MAXIMIZE max(x), or MAXIMIZE sum(per g: max(x) by (g)) to combine one value per class` |
| `MAXIMIZE AVG(e) PER g` | `MAXIMIZE sum(per g: avg(e) by (g))`, or `MAXIMIZE avg(e)` | `the postfix PER is removed; write MAXIMIZE sum(per g: avg(x) by (g)) to add up one average per class as before, or MAXIMIZE avg(x) for one average over all rows` |
| `D.x`, a decision with its relation | `x` | `DECIDE: write open, not t.open; a decision is referenced by its bare name` · in a `when`, `per` or `by`: `DECIDE: t.open is a decision; when, per and by may only use data` |

- `D.x` is refused only when `D` is a relation of the `FROM` clause and `D.x` names none of its columns, so a struct
  field or a real column `D.x` is unaffected (D43).
- A view or macro stored with a removed spelling prints as written and is refused when bound (D54).
- The colon form `SUM(D: e)` is current syntax ([guide.md §5.1](guide.md#51-the-colon-form--sumd-e)).

Tests: `test/decide/tests/test_scope_removed_syntax.py`; the scanner `test_no_old_spellings_remain.py` (with
`_old_spellings.py`) keeps these spellings out of every other test; the removed-spelling cases of
`test/decidb/test_scope_parser.test`.
