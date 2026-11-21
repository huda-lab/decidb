# Errors and refusals

Every message DECIDE raises for a query it will not run. A message states the kind of error and where it is in the
query, and stops there: it does not say how to fix the query. `DIAGNOSE` is the feature that proposes edits, and these
messages are all still raised under it, since nothing was solved and there is nothing to diagnose. The data checks that
run before solving are in [guide.md §7.3](guide.md#73-data-checks).

## How DECIDE throws an error

These are the rules for every error DECIDE raises. Code and tests follow them; nothing else about a message is owed.

1. **The kind is the DuckDB exception class, chosen by the cause.**
   - `ParserException`: text that is not DECIDE syntax. A removed spelling is a plain syntax error.
   - `BinderException`: a query that is wrong in the language. A nesting limit the language keeps says "is not
     allowed".
   - `NotImplementedException`: a shape the language allows but DeciDB does not handle, or a model this installation
     cannot solve. It says "is not supported" (`… on this installation` for the second, with no solver named).
   - `InvalidInputException`: the data or the outcome of solving: NaN, two values, infeasible, unbounded, a limit.
   - `InternalException`: DeciDB's own bug. No query causes one.
2. **The text is one plain statement of what is wrong, in SQL terms, and stops.** No fix (`DIAGNOSE` proposes edits),
   no solver names, no internal names, no prefix. It may quote a piece of the query, as the code has it at hand.
3. **A caret is given when it is at hand.** If the expression the error is raised on already has a location, pass it.
   Otherwise raise without one. Never thread, store, serialize, merge or compute a location to get a caret. A message
   about a setting, the environment or a solve outcome has no place in the query.
4. **Raise where the problem is found.** Nothing collects, ranks or orders errors. When a query has several, which
   one is reported is not owed.
5. **The direct path and the solver path raise the same kind.** Their texts match where that falls out of shared
   code; no code exists to make them match.
6. **A message with the right kind is finished.** No task is filed to improve a wording or a caret.
7. **Tests assert the kind, and the place when the message gives one**, with `assert_error_at`
   (`../testing/oracle.md` §3.6), never the full text. A test that compares two runs compares their kind or first
   line. A change to error paths alone runs only the error tests it touches.

**Ruling (owner, 2026-11-19).** Name the kind of error and give details when possible; no hoops to throw verbose
errors. The two paths need not print the same text.

**Raised** is the stage that throws: **parse** (the grammar and the transformer), **bind** (the binder, while
expressions are bound), **plan** (the canonicalizer, the shape check, the rewrites, the solver choice and the physical
plan: everything before a row is read) and **run** (execution, and the formulation it calls).

## 1. Parse time

| Situation | Kind | Message | Caret at |
|---|---|---|---|
| `per` other than `per ()` on the objective | Parser | `an objective cannot have a per key` | `per` |
| A parenthesized key (`per (g):`) | Parser | `a per key cannot be in parentheses` | the `(` |
| An expression in a `per` key | Parser | `'q' in a per key is not a column or a relation` | the element |
| An expression in a frame's `within` | Parser | `'q' in within is not a column or a relation` | the element |
| A declaration with no type | Parser | `decision 'x' has no type` | the name |
| A `DECIDE` with no `SUCH THAT`, or an objective where its constraints belong | Parser | `DECIDE has no SUCH THAT clause` | `DECIDE` |
| A `SUCH THAT` with no `DECIDE` | Parser | `SUCH THAT has no DECIDE clause` | `SUCH THAT` |
| A second `DECIDE` clause | Parser | `DECIDE appears twice` | the second `DECIDE` |
| A comma between constraints | Parser | `SUCH THAT constraints cannot be separated by a comma` | the comma |
| A qualifier of a reducer that is not one relation name (`sum(a.b: x)`) | Parser | `'q' before ':' in a reducer is not a relation` | the qualifier |
| A frame's `k` or `every` that is not a positive whole number | Parser | `'0' in a frame is not a positive whole number` | the frame |
| A frame position that is not `first`, `last`, `[k] previous` or `[k] next` | Parser | `'prev' is not a frame position` | the word |
| A frame word that is not `every` or `cyclic` where one stands | Parser | `'cyclc' is not a frame keyword` | the word |
| A range frame over more than one expression | Parser | `a range frame reduces one expression, 'sum' has 2` | the reducer |
| A name on a frame's expression (`a := e`) | Parser | `an expression in a frame cannot have a name` | the frame |
| A declaration right after another with no comma, a spelling the grammar does not have, a frame selector outside a `DECIDE` clause, `by` after anything but a function call (a column, a literal, `CASE`, `CAST`, `::`, `COALESCE`, parentheses), `WHERE` after a `DECIDE` clause | Parser | `syntax error at or near "…"` (DuckDB's own) | the token |

## 2. Bind time

Binder Error unless marked **NI** (Not implemented Error). The rows marked **D80** are the nesting limits the language
keeps ([guide.md §5.5](guide.md#55-reducers-inside-reducers--the-path-rule)).

### Constraints and the objective

| Situation | Kind | Message | Caret at |
|---|---|---|---|
| `OR` between constraints | NI | `OR between SUCH THAT constraints is not supported` | the `OR` |
| A comparison, `BETWEEN` or conjunction that reads a decision, used as a number (`x <= (x > 1)`, a `CASE WHEN x > 1`) | Binder | `'q' is a comparison used as a number` | the inner comparison |
| A constraint that is none of the comparisons (`x IS NULL`, `x LIKE 'a'`, `x + 1`) | NI | `'x IS NULL' is not supported` | the constraint |
| A bare decision as a constraint (`SUCH THAT x`) | Binder | `'q' is not a comparison` | the term |
| `IN` on a reducer | NI | `IN on 'q' is not supported` | the `IN` |
| An `IN` list that reads a decision | Binder | `an IN list reads a decision` | the `IN` |
| A reducer that is not `SUM`, `AVG`, `MIN`, `MAX`, `norm` or `COUNT`, or one with `FILTER` | NI | `'q' is not supported in a DECIDE clause` | the aggregate |
| `COUNT` over a decision | NI | `COUNT over a decision is not supported` | the call |
| A reducer or `norm` with the wrong number of arguments, at any position (`x <= max(v, 2)`: DuckDB's two-argument `MAX` is not a reducer) | Binder | `'sum()' has 0 arguments` (`1 argument`; `'max(v, 2)' has 2 arguments`) | the call |
| A window function anywhere in a clause | Binder | `a window function is not allowed in a DECIDE clause` | the window call |
| A `norm` whose order is not a constant | Binder | `the order of 'q' is not a constant` | the order |
| A `norm` order other than 0, 1, 2 or `'inf'` | NI | `norm order '3' is not supported` | the order |
| A bound on a `norm` that is not of order 0, or one that is not a positive constant | Binder | `only norm order 0 takes a bound` · `the bound of 'q' is not a positive constant` | the argument |
| A subquery in a clause that is not scalar, or that reads a decision | Binder | `a subquery in a DECIDE clause is not scalar` · `a subquery in a DECIDE clause reads a decision` | the subquery |
| A reducer over query-wide decisions only, wherever it stands (a side, beside another term, under a `by`, in the objective); a `norm` and a range frame's reducer are not refused | Binder | `'cap' is a query-wide decision, so 'sum(cap)' has no rows to reduce` | the reducer |
| A cast over a decision | Binder | `'x::DOUBLE' casts a decision` | the cast |
| A NULL beside a decision | Binder | `'x + NULL' is NULL whatever the decisions are` | the expression |
| A decision whose name is a column's or another decision's | Binder | `decision 'x' has the name of a column` · `decision 'x' is declared twice` | the declaration |
| A name DECIDE reserves | Binder | `'q' is not allowed as a name in DECIDE` | the name |

### Scopes, keys and relations

| Situation | Kind | Message | Caret at |
|---|---|---|---|
| A column of an outer query (`LATERAL`, a scalar subquery in another query's select list), in any clause or the `WHERE` | Binder | `'o.k' refers to an outer query` | the column |
| `by` after a call that is not a reducer | Binder | `by (...) does not follow a reducer` | the `by` |
| `when` or `per` inside a call that is not a reducer | Binder | `when or per inside 'q', which is not a reducer` | the call |
| A decision in a `when`, a key or a `by`, in the `WHERE`, or in a frame's order key or `within` | Binder | `'x' is a decision, which when, per and by cannot read` (`WHERE`; `a frame's order key and within`) | the decision |
| A window function in a `when`, a key, a `by` or a frame's order key | Binder | `a window function is not allowed in when, per, by or a frame's order key` | the window call |
| A reducer in a `when`, a key, a `by` or a frame's order key | Binder | `a reducer is not allowed in when, per, by or a frame's order key` | the reducer |
| A volatile function (`random()`) in a clause | Binder | `a volatile function is not allowed in a DECIDE clause` | the call |
| An unknown key element | Binder | `'depot' is not a column or a relation of the FROM clause` | the element |
| `rowid` or a generated column in a key | Binder | `'g.b' is not a stored column` | the element |
| An unqualified `FULL OUTER JOIN USING` column in a key | Binder | `'depotID' is merged by FULL OUTER JOIN USING` | the element |
| A colon form naming a relation not in `FROM` | Binder | `'X' is not a relation of the FROM clause` | the qualifier |
| A relation qualifier on a call that is not a reducer | Binder | `'q' cannot be qualified by a relation` | the call |
| A scope or the colon form inside an ordinary subquery written in a clause | NI | `'q' inside a subquery is not supported` | the scope |
| **D80** `norm` inside a reducer, a reducer inside `norm` | Binder | `norm inside a reducer is not allowed` · `a reducer inside norm is not allowed` | the call |

### Frames

| Situation | Kind | Message | Caret at |
|---|---|---|---|
| **D80** A frame in a `when`, a `per`, a `by`, a `within` or an order key | Binder | `a frame selector is not allowed in when, per, by, within or an order key` | the frame |
| **D80** A reducer or a frame inside a frame's payload or fallback | Binder | `a reducer inside 'q' is not allowed` · `a frame selector inside 'q' is not allowed` | the inner node |
| A frame's `else` that reads a decision while its payload reads none | NI | `else 'x' reads a decision while the payload reads none, which is not supported` | the `else` value |
| A frame's `else` that is not a number | Binder | `else ''a'' is not a number` | the `else` value |
| A range's `else` over a collated payload, whose `min` or `max` compares text under the collation | Binder | `else '0' stands in a collated text payload` | the `else` value |
| A range frame reducing with a function other than `sum`, `avg`, `min`, `max`, `count` | NI | `a range frame reducing with 'median' is not supported` | the frame |

A frame's `else` that is text carries its own quotes, so the sentence doubles them (`else ''a'' is not a number`).

### `DIAGNOSE`

| Situation | Kind | Message | Caret at |
|---|---|---|---|
| `DIAGNOSE` on a query with no `DECIDE` clause | Binder | `DIAGNOSE needs a query with a DECIDE clause` | the query |
| `DIAGNOSE` on a query with more than one `DECIDE` clause | NI | `DIAGNOSE on more than one DECIDE clause is not supported` | the query |

## 3. Plan time

The messages raised after binding, while the plan is built and optimized, before any row is read. A caret is printed
where the message names a clause or a node the user wrote.

| Situation | Kind | Message | Caret at |
|---|---|---|---|
| An `INT` or `REAL` decision as a factor on a reducer, or a `BOOL` one on a reducer inside another expression | Binder | `'s' is an INT decision, which cannot scale a reducer` (`a REAL`, `a BOOL`; `divide`; `a reducer inside another expression`; a compound factor reads `'b - c' reads a decision, which cannot scale a reducer`) | the factor |
| A row-varying factor on a reducer in the objective (a column, or a data reducer with a `by`) | Binder | `'weight' varies per row, which cannot scale a reducer in the objective` (`'avg(v) by (g)' varies per row, …`) | the factor |
| A bound that is NULL | Binder | `the bound 'q' is NULL` | the bound |
| A comparison that reads no decision (`v <= 5`, `SUM(t.v) + 1 <= 500`, `abs(v) <= 5`), or an objective whose body reads none (`1 + 2`, `sum(v) + 1`) | Binder | `'v <= 5' reads no decision` | the comparison, or the objective body |
| `AVG` under `<>` beside another term, or with its own `when` or `per` | NI | `AVG in a <> constraint beside another term, or with its own when or per, is not supported` | the comparison |
| `<>` over a square or product of decisions not linearized first (a product with a `BOOL` is) | NI | `<> over a product or a power of decisions is not supported` | the comparison |
| An `ABS` over a square or product of decisions not linearized first (a product with a `BOOL` is) | NI | `ABS over a product or a power of decisions is not supported` | the ABS |
| **D80** A square or product of decisions inside nested reducers that no body definition names | Binder | `'q' squares or multiplies decisions inside nested reducers, which is not allowed` | the reducer |
| A square, or a product of two decisions, outside a reducer in the objective, unless the two are distinct query-wide decisions or one is `BOOL` | NI | `'x * y' multiplies decisions outside a reducer, which is not supported` | the product |
| **D80** A square or product of decisions in a frame's payload or `else` (`x * x`, `power(x, 2)`, `x ** 2`) | Binder | `'x * x' squares or multiplies decisions inside a frame, which is not allowed` | the inner node |
| A frame's payload with an `ABS` over a decision | NI | `'abs(x - 3)' inside a frame is not linear in the decisions, which is not supported` | the node |
| `<`, `>` or `<>` on a side that can take fractional values: a `REAL` decision, a fractional coefficient (`SUM(x * v)` over a `DOUBLE` `v`), an offset under a non-linear term, or a helper standing for a reducer | NI | `'<' compares 'x', which can take fractional values` (`'>' compares 'sum(ship + 0.5) by (D.region)', …`) | the comparison |
| A `CASE`, `if` or `iif` over a decision (a `CASE` over data is a per-row coefficient) | NI | `CASE over a decision is not supported` (`IF`, `IIF` as written) | the conditional |
| A function over a decision outside `+`, `-`, `*`, `/` by data, `POWER`, `ABS` and the reducers (`sqrt`, `exp`, `floor`) | NI | `function 'sqrt' over a decision is not supported` | the call |
| An operator over a decision outside that vocabulary (`%`, `COALESCE`, `IS NULL`) | NI | `operator '%' over a decision is not supported` | the operator |
| `POWER(e, n)` over a decision with `n` not a whole number known before the query runs | NI | `the exponent '0.5' of POWER over a decision is not a constant whole number` | the `POWER` |
| Division by a decision | NI | `division by a decision is not supported` | the `/` |
| A term of degree above 2 (three decisions in one product, `POWER(x, 3)`) | NI | `a term of degree 3 is not supported` | the term |
| A model class this installation cannot solve | NI | one text per cause, each ending `is not supported on this installation` (below) | the clause, the reducer or the objective |
| A bound that cannot hold for the decision's domain: a `BOOL` above 1 or below 0 (`x >= 2`), or an upper bound below the default floor of 0 | Invalid Input | `the constraint cannot hold: 'x' is a BOOL decision` · `the constraint cannot hold: 'x' is never negative` | the clause |
| `decide_direct_solve` is `require` | Invalid Input | `decide_direct_solve is 'require' and the query is DIAGNOSE` · `decide_direct_solve is 'require' and DECIDB_FORCE_SOLVER is set` · `decide_direct_solve is 'require' and no direct rule applies to this query` | none |
| `decide_optimizer` in `disabled_optimizers` | Invalid Input | `DECIDE needs the optimizer 'decide_optimizer', which is disabled` | none |
| `DECIDB_FORCE_SOLVER` names a solver DeciDB does not know, or one this installation lacks | Invalid Input | `DECIDB_FORCE_SOLVER names no solver: 'x'` · `DECIDB_FORCE_SOLVER names a solver this installation does not have: 'x'` | none |

**The causes of a solver gap.** Each reads `<cause> is not supported on this installation`. The installation, not a
solver, is the place, so a user on Gurobi gets an answer to the same query.

| Cause | Caret at |
|---|---|
| `squaring or multiplying decisions in a constraint` | the first such clause |
| `multiplying two decisions in the objective` | the objective |
| `maximizing a squared decision` · `minimizing a negated squared decision` | the objective |
| `squaring a decision that is not REAL` | the objective |
| `squaring an expression of more than one decision` | the objective |
| `a square scaled by a value whose sign is known only when the query runs` | the objective |
| `MIN or MAX over a squared or multiplied expression` | the reducer |

## 4. Run time

Raised while rows are read or the model is built. A message that quotes a value names no row: a row's position in the
input depends on the scan.

| Situation | Kind | Message | Caret at |
|---|---|---|---|
| A NaN in the data, an infinite coefficient (a factor on an aggregate that divides by zero included), or an infinite value under `ABS`, `MIN` or `MAX` | Invalid Input | `'v' is NaN or infinite on some rows` (`'1 / (SELECT 0)' is NaN or infinite on some rows`) | the column or expression as written, or the nearest located expression above a value DeciDB built |
| A bound that is NaN on some rows | Invalid Input | `the bound 'v' is NaN on some rows` | the bound |
| A constant coefficient that SQL computes as NaN or infinite (`x / 0`, `2e308`, `'Infinity'`) | Invalid Input | `'1 / 0' is NaN or infinite` | the constant |
| A number past a DOUBLE that DeciDB derives across rows (`SUM(b*x)` over rows of one keyed decision, `POWER(b*x, 2)`, a bound or range DeciDB computes from the decisions' bounds) | Invalid Input | `the constraint computes a number too large for a DOUBLE` (`the objective …`) | the clause |
| An `=` bound that takes two values in a `by` group | Invalid Input | `the = bound 'cap' takes two values (5 and 7) in the group category = 'Food'` (a bound over the whole query names no group) | the bound |
| The objective takes two values or instances over its kept rows | Invalid Input | `'D.capacity' takes more than one value over the kept rows (20, 40), so the objective would be produced more than once` (below) | the field, the key element, the declaration or the frame |
| A range frame reading more values than `decide_frame_limit` | Invalid Input | `the frame reads more than 4 values over its ranges, above decide_frame_limit (4)` | the frame |
| A frame's selected position holding rows with two signatures of its payload (D73) | Invalid Input | `the frame finds 2 rows at t.product = 'A', t.period = 1` | the frame |
| Under `per K`, a class whose rows lie in two timelines of a frame its body reads, or an objective's rows in two of a frame at its root (D75) | Invalid Input | `per t.category does not determine the timeline t.product: t.category = 'Food' spans t.product = 'A', 'B'` (the objective: `the objective (per ()) does not determine the timeline t.product: its rows span t.product = 'A', 'B'`) | the frame |
| A variable with no finite bound under a `MIN`/`MAX`, `ABS` or `<>` in the direction that needs one, or a `norm(e, 0)` | Invalid Input | `'x' has no finite bound, which ABS needs` (`MIN or MAX`, `<>`, `norm(e, 0)`) | the clause |
| The other factor of a product with a `BOOL` decision | Invalid Input | `'q' has no finite upper bound, which its product with a BOOL decision needs` | the clause |
| A whole-numbered decision that solves past 2^53 | Invalid Input | `decision 'x' takes the value 100000000000000000, past 2^53, the largest whole number DECIDE returns exactly` | the declaration |
| A row or objective whose numbers lie too far apart for the installation to load | NI | `the constraint holds numbers too far apart in size, which is not supported on this installation` (`the objective …`) | the clause |
| The query has no feasible point | Invalid Input | `the DECIDE query is infeasible` | none |
| The objective improves without end | Invalid Input | `the DECIDE query is unbounded` | none |
| The solver cannot tell the two apart | Invalid Input | `the DECIDE query is infeasible or unbounded` | none |
| A time or iteration limit with no answer to return | Invalid Input | `the DECIDE query reached the time limit` · `the DECIDE query reached the iteration limit` | none |
| The solver stopped with another status | Invalid Input | `the DECIDE query could not be solved: <status>` (the status text is omitted where the solver gives none) | none |
| The Gurobi environment or licence cannot start | Invalid Input | `the Gurobi environment could not start: <reason>` | none |
| An interrupt before an answer | Interrupt | `DECIDE stopped at your request before finding a solution.` (DuckDB's interrupt) | none |

An error in a Gurobi model load (`Failed to add … to Gurobi`, `GRBaddqconstr`) is an Internal Error: a query never
causes it.

**What the objective takes two values over.** The phrase `so the objective would be produced more than once` is fixed;
names and values come from the query and the data, the least and the greatest as `ORDER BY` places them (NULL last), so
the message does not depend on row order. The first part names the field that differs:

- a column or expression: `'D.capacity' takes more than one value over the kept rows (20, 40)`;
- a `by` key: `'D.region' in by (D.region) takes more than one value over the kept rows ('East', 'West')`;
- a decision: `the decision 'ship' (declared per row) has more than one instance over the kept rows`;
- a frame: `'at(first: x) over (t.period within t.product)' reads more than one position over the kept rows (t.period
  = 1, 2)`, `… reads more than one timeline over the kept rows (t.product = 'A', 'B')` or `… falls back to more than one
  value over the kept rows`.

## 5. No place in the query

A message about a setting, the environment or the solve itself has no caret. Each names the setting or the component,
states what is wrong, and stops.

| Message | Kind |
|---|---|
| A setting out of range: `decide_frame_limit must be >= 1; got 0.`, `decide_mip_gap must be >= 0; got -1.`, `decide_l0_tolerance must be at least 1e-5; got 1e-7`, `diagnose_decide_infeasible_slack_scope must be 'query' or 'expanded'; got 'x'`, and the same form for the other `diagnose_decide_*` settings | Invalid Input |
| `decide_direct_solve must be off, auto, or require` | Invalid Input |
| The messages of `decide_direct_solve = 'require'`, `DECIDB_FORCE_SOLVER` and a disabled `decide_optimizer` (§3) | Invalid Input |
| The solver outcomes and the Gurobi environment (§4) | Invalid Input |

The `decide_debug` setting reads its own items (`decide_debug: unknown item 'x'`); it is a developer setting and states
what it rejects.

## Warnings

A warning is not an error and leaves the answer unchanged. The number-range warning is one line on stderr, once per
query: `DECIDE: a very large or very small number may make this answer inaccurate.` The two warnings about what the
data left of a clause are in [guide.md §7.4](guide.md#74-what-the-data-left-of-a-clause).

## Notes

DeciDB prevents no overflow, as DuckDB's DOUBLE arithmetic does not. Large finite numbers solve after the number-range
warning (`../pipeline/07_solver/done.md`), and the solver's verdict on them stands. An infinite bound is no bound. A
coefficient that is NaN or infinite where DeciDB reads it raises one of the first three run-time rows: the first when a
column holds it, quoting the column or expression as the user wrote it (`2 * b` names `b`); the third when no column
holds it but SQL's arithmetic made it so (`1 / 0` is infinite in DuckDB). A coefficient has no separate message for NaN
and for infinity. The too-large row is for a number no expression of the query holds: it names the clause, and it
applies only where DeciDB accumulates across rows (`x*1e308 + x*1e308`), since a literal past a DOUBLE as written
(`2e308 * x`) is an infinite constant.

A clause that comes from a table macro's body has a position relative to the body, and a clause with no text of its
own prints no caret.

**Ruling (owner, 2026-11-18).** A message states the kind of error and where it is in the query, and stops (R12,
R19). A removed spelling is an ordinary syntax error (R13). A NaN or infinite value says so and names where it stands
(R14). An `=` bound that takes two values names its `by` group (R15).

**Ruling (owner, 2026-11-19).** Every message of the tables above carries the raise call's own position
(`query_location`), as DuckDB's casts do. A direct-solve rule raises where the value is found, so a direct plan and
the solver path print the same text and the same caret.

**Ruling (owner, 2026-11-19).** A `too large for a DOUBLE` message has its caret on the clause and never quotes the
clause: `the constraint computes a number too large for a DOUBLE`, `the objective …`.

**Ruling (owner, 2026-11-19).** A constraint that is none of the comparisons the rows above name is `'q' is not
supported` (Not implemented Error) at its node.

**Ruling (owner, 2026-11-19).** A comparison that reads no decision is `'q' reads no decision` (Binder Error), quoting
the whole comparison. A side that reads a decision in a shape the model does not take is named by its own row of §3: a
`CASE WHEN c > 1 THEN x ELSE 0 END` compared with a bound is `CASE over a decision is not supported`.

**Ruling (owner, 2026-11-19).** `else ''a'' is not a number` keeps the doubled quote: the fallback text carries its own
quotes and the sentence adds its own.

**Ruling (owner, 2026-11-19).** The decision-as-factor message names the decision's type (`'s' is an INT decision, which
cannot scale a reducer`; `a REAL`, `a BOOL`; `divide`; `a reducer inside another expression`), and a compound factor
reads `'b - c' reads a decision, which cannot scale a reducer`. Minimizing a negated square is its own cause
(`minimizing a negated squared decision`). An objective that takes two instances of a decision has its caret on the
declaration.

**Ruling (owner, 2026-11-19).** `the DECIDE query could not be solved: <status>` names the solver's own status text where
there is one. `the constraint holds numbers too far apart in size` has one text for every cause. The number-range
warning does not tell the user what to change. The settings in §5 and the `DIAGNOSE` form of `decide_direct_solve =
'require'` read as the tables give them.
