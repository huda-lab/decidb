# Errors, refusals and removed spellings

Every message DECIDE raises for a query it will not run, grouped by why. Messages quote the user's SQL, never an
internal name, and say what to write instead. All of them are still raised under `DIAGNOSE`: nothing was solved, so
there is nothing to diagnose. The data checks that run before solving are in [guide.md §7.3](guide.md#73-data-checks).

## 1. Errors

| Situation | Raised | Message |
|---|---|---|
| A `per` key does not determine a value | run, before solving | [guide.md §7.3](guide.md#73-data-checks) |
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
| `rowid` or a generated column in a key | bind | `DECIDE: g.b is not a stored column; a per key lists columns or relations of the FROM clause` |
| An unqualified `FULL OUTER JOIN USING` column in a key | bind | `DECIDE: depotID is merged by a FULL OUTER JOIN USING; write S.depotID or D.depotID in the per key` |
| A reducer or `norm` with no argument | bind | `SUM needs one argument, found 'sum()'` · `NORM needs an expression and an order, as in norm(e, 1), found 'norm()'` |
| A colon form naming a relation not in `FROM` | bind | `Relation 'X' is not in the FROM clause, so a reducer cannot be qualified by it.` |
| A reducer over query-wide decisions only, as a constraint's whole decision side | bind | `cap is a query-wide decision, so SUM(cap) has nothing to aggregate over; use cap on its own` |
| A decision as a factor on a reducer | bind | `DECIDE constraint: 's' is a decision, so it cannot multiply SUM(x). Only data can scale SUM/AVG/MIN/MAX.` (objective: `... Only constants and query-wide values can scale ...`) |
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
| An unbounded member under a lowered hard `MIN`/`MAX`, `<>`, Big-M `ABS` or McCormick product | run | names the variable and the bound to add |
| A model class the available solver cannot run (non-convex QP, MIQP, quadratic constraints on HiGHS) | plan | names what the query does and that it needs Gurobi |
| A square or product under a `MIN`/`MAX` on HiGHS | plan | `DECIDE: 'max(power(x, 2))' takes the maximum of a squared or multiplied expression, which needs Gurobi — not available on this machine. Install Gurobi, or replace power(x, 2) with an expression linear in the decision variables.` |

## 2. Not supported yet

Each shape is refused with a message, never answered wrongly. Rows marked **roadmap** are deck features the move to
the new language has not reached ([roadmap.md](roadmap.md#nested-reducers-anr-pp-25-2832)). The others are limits on
what DeciDB formulates; their open work is in the owning stage's `todo.md`.

| Shape | Message |
|---|---|
| **roadmap** A reducer inside a reducer, except the two-level objective | `DECIDE: an aggregate inside an aggregate is not supported yet` |
| **roadmap** The two-level objective beside another term or under a factor | the same |
| **roadmap** A `per` inside the two-level objective's inner reducer | `DECIDE: per inside the inner reducer of min(per g: sum(...) by (g)) is not supported yet` |
| **roadmap** A `when` inside either reducer of the two-level objective | `DECIDE: when inside the outer reducer of min(per g: ...) is not supported yet; write it in front of the objective` (inner reducer likewise) |
| **roadmap** A two-level `by` key other than its `per` key | `DECIDE: in min(per g: sum(...) by (id)) the by key must be the per key; any other is not supported yet` |
| **roadmap** An outer `AVG` in the two-level objective | `DECIDE: avg over the classes of avg(per g: sum(...) by (g)) is not supported yet; write sum, min or max over them` |
| A product or square of decisions inside a reducer that a helper decision replaces | `DECIDE: 'sum(ship * ship)' is not supported yet in this constraint: a product or square of decisions inside a reducer is accepted only where the constraint's reducers share one by key, under a whole-query factor, with nothing beside them but data and query-wide decisions, each on its own` |
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
| `MINIMIZE OUTER(INNER(e)) PER g` | `MINIMIZE outer(per g: inner(e) by (g))`; an outer `AVG` becomes `sum`; a `WHEN` inside goes in front | `the postfix PER is removed; write MINIMIZE sum(per g: max(x) by (g))` |
| `MINIMIZE OUTER(INNER(D: e)) PER g` · `MINIMIZE c * OUTER(INNER(e)) PER g` | no spelling yet | `the postfix PER is removed; the two-level form ... takes no per inside its inner reducer yet` · `... takes one reducer call inside the outer one ... is not supported yet` |
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
