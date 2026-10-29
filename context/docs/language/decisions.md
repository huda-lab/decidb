# Language decisions

Rulings on the DECIDE language that code and tests cite by number (`D5`, `S3`). Each row is the ruling as it stands.
A number not in the table is retired and is not reused. Add a new ruling with the next free number.

## Decisions

| # | Decision |
|---|---|
| D1 | `SUM(D: e)` means `sum(per D: e)` (S4) |
| D2 | The nested objective is written `outer(per k: inner(e) by (k))` |
| D3 | The clause shapes support `SUM`, `AVG`, `MIN` and `MAX` |
| D5 | A NULL in a written key column (a declaration's `per`, a constraint's `per`, `by`, a `per` inside a reducer) puts the row in no class or group of that key, as SQL's `=` has it (deck p. 30). The row keeps every other key's groups. A keyed decision has no value on that row: the output is NULL, a reducer skips it, an unreduced use skips the instance. A relation element (`per D`) compares NULL as a value. SQL's `GROUP BY` keeps NULL as one group; DECIDE does not, because a limit for an unknown group means nothing (owner, 2026-10-27/28) |
| D6 | `per g: sum(x) <= 10` is accepted; no `by` means `by ()` |
| D7 | Each declarator has its own key; a `per` does not carry over |
| D15 | A scope or `by` on a call other than `SUM`/`AVG`/`MIN`/`MAX` is refused with a message (D34 and D35 admit `by` on `count` and `norm`) |
| D19 | A written `per row:`, in front of a constraint, inside a reducer or before a declaration, is the default written out (`PG_AEXPR_SCOPE_PER_ROW`, `SCOPE_PER_ROW_TAG`) |
| D22 | A reducer or `norm` with no argument gets a binder message (`SUM needs one argument, found 'sum()'`) |
| D24 | The value rewrite handles exactly: a decision beside a reducer, different `by` keys, a row-varying factor, a reducer inside another expression, a `MIN`/`MAX` beside another term with a `by` or a non-constant bound, a decision beside a `MIN`/`MAX` in a constraint, a query-wide product beside a reducer |
| D27 | `EXPLAIN` names every auxiliary by what it stands for and prints one account line per rewrite under its clause |
| D28 | DIAGNOSE's `group` column holds the `per` key value: in the expanded slack scope a `by`-grouped clause reports its group's finding once per class of its `per` key, and once per kept row when it has no `per` key (ANR deck pp. 16–17, 110) |
| D30 | Every clause follows the scope rules, bare clauses included |
| D31 | There is one language; no clause carries a language tag |
| D32 | One echo for every clause: the qualifier in front, no synthesized `per row:`, `by ()` not printed. Quoted text keeps the user's case; text DeciDB writes (a rewritten row, an edit) uses lower-case reducers and keywords |
| D33 | The expanded DIAGNOSE scope reports per kept row for a clause with no `per` key (D28 read literally) |
| D34 | `count(*)` takes a `by`; `count(*) by (g)` counts a group's rows |
| D35 | `norm(e, p)` takes a `by` |
| D36 | A removed objective form with no exact equivalent gets its own message: a flat `MIN`/`MAX`/`AVG` with `PER` is told that an objective is produced once; an outer `AVG` is pointed to `sum` |
| D37 | Removed-spelling messages carry the fixed phrase "is removed" and quote the user's own expressions, built by the transformer from the parse tree |
| D38 | The colon form takes an exact key on `D`'s stored columns, the run-time data check and a keyed decision inside it, and echoes as written |
| D40 | `OR` between constraints is refused; a data-only reducer on the decision side is refused with a message; no internal name appears in "does not reference a decision" |
| D41 | An easy `min(x) >= K` / `max(x) <= K` means `x >= K` / `x <= K` on every row it reads; with no `when` and no reducer filter it is absorbed into the decision's box, so a negative `K` makes `x` signed. A constant lower bound replaces the default floor of 0 on the rows it keeps: every row without a `when` or a reducer filter, and only the kept rows or members with one (the other rows keep 0). A keyed or query-wide decision takes the replaced floor when any row it reads is kept. The bound is read by value: any affine or scaled spelling with a constant other side (`min(x) - 1 >= K - 1`, `2 * min(x) >= 2K`, `-min(x) <= -K`) is absorbed like the literal one, and so is a member scaled or shifted by constants (`max(-x) <= 2` is `x >= -2`, `max(2 * x) <= -10` is `x <= -5`) |
| D43 | `D.x` is refused at bind time when `D` is a `FROM` relation and `D.x` binds to no column (*"write open, not D.open; a decision is referenced by its bare name"*); in a `when`, `per` or `by`: *"D.open is a decision; when, per and by may only use data"* |
| D44 | Open: the NULL class label in DIAGNOSE's `group` column prints as the text `NULL` (`../diagnostics/infeasible/todo.md` § INF-2) |
| D45 | Every column prints as the user wrote it (`S.cap`, `cap`), at every stage: the binder cuts its spelling and tags the bound reference |
| D46 | Open: the scope enum becomes `ROW / KEYED / GLOBAL` with its values kept, and DIAGNOSE's label `keyed` (`../issues/code_quality.md` § CQ-3) |
| D47 | Open: one clause-scope struct and walker, one two-level renderer (`../issues/code_quality.md` § CQ-1) |
| D50 | `DECIDE scalar cap` and `DECIDE lineitem.x` (no type) get the removed-spelling error, naming a typed declaration; `per (): cap` gets "needs a type" |
| D51 | `SCALAR` is an unreserved keyword, used only by the error rule; `scalar` is an ordinary identifier |
| D52 | The paper's Figure 1 query runs (`test_reduced_bound_data_column.py`) |
| D54 | A parse tree stored with a removed spelling (a view, a macro) prints as written and is refused when bound, with the removed-spelling message |
| D55 | The postfix `WHEN` after a constraint takes an `a_expr`, so `WHEN NOT f` and `WHEN g IN (...)` get the named message |
| D56 | The `T.x` message names `per T:` and says that `per T` keys on every column of `T` |
| D60 | The scope oracle steps a strict sense onto whole numbers (`> K` → `floor(K) + 1`, `< K` → `ceil(K) - 1`), the same as stepping by one for a whole `K` |
| D62 | The removed-`PER` message names the new spelling only: a NULL key is no group under both spellings (D5), so it needs no NULL note |
| D63 | Determination messages print the colon form as written |
| D64 | A bound on one decision admits what DuckDB admits: a whole-numbered decision takes `n` exactly when the written comparison, evaluated by DuckDB with `n` in place of the decision, is true (`0.1 * x <= 0.3` admits 3, since `0.1` is DECIMAL; `0.1::DOUBLE * x = 0.3::DOUBLE` admits nothing). Every such bound folds into the decision's box, a data coefficient (`cap * x <= 10`) included; only a bound whose interval is a constant replaces the default floor of 0, on the rows it keeps (D41); a bound of data only intersects with it |
| D65 | An infinite bound keeps its SQL meaning: `x <= inf` and `x >= -inf` are no bound; `x >= inf`, `x <= -inf` and `x = ±inf` make the query infeasible on every backend, and `DIAGNOSE` names the clause. A literal `x >= -'inf'` frees `x` below, as `x >= -1e30` does |
| D66 | A right side of magnitude 1e20 or more in the open direction (`x <= 1e30`) is no bound |
| D67 | A NULL data value is skipped where it is read (deck pp. 50, 56): a reducer skips a row whose summand is NULL, as SQL does; a NULL that reaches a constraint unreduced skips that instance; a NULL objective term is dropped before like terms merge. A NULL written in the query beside a decision, and NaN, stay errors |
| D68 | An empty group skips its constraint (deck p. 56): a reducer over no rows is NULL, and a NULL that reaches the comparison skips the instance. A `when` that keeps no row yields no constraint; a group in which some reducer reads no row yields none; an objective term over no rows is dropped. `count(*)` over no rows is 0, as in SQL (owner, 2026-10-28) |
| D69 | A key groups rows under its column's collation, as `GROUP BY` does (`PushCollation` on each key element): `per K:` classes, `by` groups, bound-side reducers and keyed decisions alike. A label quotes the group's least raw spelling, so it does not depend on row order (owner, 2026-10-27) |

## Spelling calls

| # | Call |
|---|---|
| S1 | `per row` is accepted as the written-out default |
| S2 | Decisions are referenced by bare name; `D.open` is refused (D43) |
| S3 | With no `per`, a constraint is per row, so `sum(x) <= cap` takes the tightest `cap`; with an explicit `per`, a varying bound is the determination error |
| S4 | `SUM(D: e)` uses the same data check as `sum(per D: e)` |
| S6 | `per (a, b):` is refused with a message; parentheses mean only `per ()` |
| S7 | Inside a DECIDE clause, `when`, `per` and `by` after a `)` are keywords; a column with such a name is quoted there |
| S8 | `per` keys are columns and relations; expressions are allowed only in `by` |
| S9 | An input with no rows gives an empty result |
| S10 | Data-only reducers stay on the bound side of a comparison |
| S11 | A reducer over a lone query-wide decision (`SUM(cap)`) is refused |
