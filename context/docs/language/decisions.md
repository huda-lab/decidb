# Language decisions

Rulings on the DECIDE language that code and tests cite by number (`D8`, `S3`). Each row is the ruling as it stands.
A number not in the table is retired and is not reused. Add a new ruling with the next free number.

## Decisions

| # | Decision |
|---|---|
| D1 | `SUM(D: e)` means `sum(per D: e)` (S4) |
| D2 | The nested objective is written `outer(per k: inner(e) by (k))` |
| D3 | The clause shapes support `SUM`, `AVG`, `MIN` and `MAX` |
| D4 | A `when` that keeps no rows is an error, with or without a reducer in the clause |
| D5 | NULL is its own class for a declaration's `per`, a constraint's `per` and `by`. Open: to be reversed ([execution todo §2](../pipeline/08_execution/todo.md#2-a-null-key-is-not-a-class-reverses-d5)) |
| D6 | `per g: sum(x) <= 10` is accepted; no `by` means `by ()` |
| D7 | Each declarator has its own key; a `per` does not carry over |
| D8 | A reducer with no rows for some class is an error, not a skipped constraint |
| D15 | A scope or `by` on a call other than `SUM`/`AVG`/`MIN`/`MAX` is refused with a message (D34 and D35 admit `by` on `count` and `norm`) |
| D18 | The D8 message names the `by` group the reducer reads (`has no rows for D.region = 'North'.`); with `by ()` it ends `has no rows.` |
| D19 | A written `per row:`, in front of a constraint, inside a reducer or before a declaration, is the default written out (`PG_AEXPR_SCOPE_PER_ROW`, `SCOPE_PER_ROW_TAG`) |
| D22 | A reducer or `norm` with no argument gets a binder message (`SUM needs one argument, found 'sum()'`) |
| D24 | The value rewrite handles exactly: a decision beside a reducer, different `by` keys, a row-varying factor, a reducer inside another expression, a `MIN`/`MAX` beside another term with a `by` or a non-constant bound, a decision beside a `MIN`/`MAX` in a constraint, a query-wide product beside a reducer |
| D27 | `EXPLAIN` names every auxiliary by what it stands for and prints one account line per rewrite under its clause |
| D28 | DIAGNOSE's `group` column holds the `per` key value: in the expanded slack scope a `by`-grouped clause reports its group's finding once per class of its `per` key, and once per kept row when it has no `per` key (ANR deck pp. 16–17, 110) |
| D30 | Every clause follows the scope rules, bare clauses included |
| D31 | There is one language; no clause carries a language tag |
| D32 | One echo for every clause: lower-case reducers and keywords, the qualifier in front, no synthesized `per row:`, `by ()` not printed |
| D33 | The expanded DIAGNOSE scope reports per kept row for a clause with no `per` key (D28 read literally) |
| D34 | `count(*)` takes a `by`; `count(*) by (g)` counts a group's rows |
| D35 | `norm(e, p)` takes a `by` |
| D36 | A removed objective form with no exact equivalent gets its own message: a flat `MIN`/`MAX`/`AVG` with `PER` is told that an objective is produced once; an outer `AVG` is pointed to `sum` |
| D37 | Removed-spelling messages carry the fixed phrase "is removed" and quote the user's own expressions, built by the transformer from the parse tree |
| D38 | The colon form takes an exact key on `D`'s stored columns, the run-time data check and a keyed decision inside it, and echoes as written |
| D40 | `OR` between constraints is refused; a data-only reducer on the decision side is refused with a message; no internal name appears in "does not reference a decision" |
| D41 | An easy `min(x) >= K` / `max(x) <= K` means `x >= K` / `x <= K` on every row it reads; with no `when` and no reducer filter it is absorbed into the decision's box, so a negative `K` makes `x` signed. The bound is read by value: any affine or scaled spelling with a constant other side (`min(x) - 1 >= K - 1`, `2 * min(x) >= 2K`, `-min(x) <= -K`) is absorbed like the literal one, and so is a member scaled or shifted by constants (`max(-x) <= 2` is `x >= -2`, `max(2 * x) <= -10` is `x <= -5`) |
| D43 | `D.x` is refused at bind time when `D` is a `FROM` relation and `D.x` binds to no column (*"write open, not D.open; a decision is referenced by its bare name"*); in a `when`, `per` or `by`: *"D.open is a decision; when, per and by may only use data"* |
| D44 | Open: the NULL class label in DIAGNOSE's `group` column prints as the text `NULL` (`../diagnostics/infeasible/todo.md` §2) |
| D45 | Open: EXPLAIN and DIAGNOSE print a non-key column by its bare name over a join ([logical plan todo §2](../pipeline/03_logical_plan/todo.md#2-columns-print-by-their-bare-name-over-a-join)) |
| D46 | Open: the scope enum becomes `ROW / KEYED / GLOBAL` with its values kept, and DIAGNOSE's label `keyed` ([code quality](../issues/code_quality.md#name-the-keyed-scope-keyed-everywhere)) |
| D47 | Open: one clause-scope struct and walker, one two-level renderer (`../issues/code_quality.md`) |
| D50 | `DECIDE scalar cap` and `DECIDE lineitem.x` (no type) get the removed-spelling error, naming a typed declaration; `per (): cap` gets "needs a type" |
| D51 | `SCALAR` is an unreserved keyword, used only by the error rule; `scalar` is an ordinary identifier |
| D52 | The paper's Figure 1 query runs (`test_reduced_bound_data_column.py`) |
| D54 | A parse tree stored with a removed spelling (a view, a macro) prints as written and is refused when bound, with the removed-spelling message |
| D55 | The postfix `WHEN` after a constraint takes an `a_expr`, so `WHEN NOT f` and `WHEN g IN (...)` get the named message |
| D56 | The `T.x` message names `per T:` and says that `per T` keys on every column of `T` |
| D60 | The scope oracle steps a strict sense onto whole numbers (`> K` → `floor(K) + 1`, `< K` → `ceil(K) - 1`), the same as stepping by one for a whole `K` |
| D62 | The removed-`PER` message adds the NULL note: *"a NULL g is now its own group, so write when g is not null per g: to leave those rows out"* |
| D63 | Determination and D8 messages print the colon form as written |
| D64 | A bound on one decision admits what DuckDB admits: a whole-numbered decision takes `n` exactly when the written comparison, evaluated by DuckDB with `n` in place of the decision, is true (`0.1 * x <= 0.3` admits 3, since `0.1` is DECIMAL; `0.1::DOUBLE * x = 0.3::DOUBLE` admits nothing). Every such bound folds into the decision's box, a data coefficient (`cap * x <= 10`) included; only an unconditional literal bound replaces the default floor of 0 (D41) |
| D65 | An infinite bound keeps its SQL meaning: `x <= inf` and `x >= -inf` are no bound; `x >= inf`, `x <= -inf` and `x = ±inf` make the query infeasible on every backend, and `DIAGNOSE` names the clause. A literal `x >= -'inf'` frees `x` below, as `x >= -1e30` does |
| D66 | A right side of magnitude 1e20 or more in the open direction (`x <= 1e30`) is no bound |

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
| S9 | An input with no rows gives an empty result; D4 is about a `when` that removes every row of a non-empty input |
| S10 | Data-only reducers stay on the bound side of a comparison |
| S11 | A reducer over a lone query-wide decision (`SUM(cap)`) is refused |
