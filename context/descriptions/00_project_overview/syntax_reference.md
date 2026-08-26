# Syntax Reference

## 1. The DECIDE Clause usage

Two clause orders are accepted; they parse to the same plan and neither is
preferred by the engine.

**Split order** (the paper's, Figure 1) — the declaration sits between `SELECT`
and `FROM`, the constraints and objective come after the joins:

```sql
SELECT ...
DECIDE [Table.]variable_name(type) [, [Table.]variable_name2(type2) ...]
FROM ...
[JOIN ...]
[WHERE ...]
SUCH THAT
    constraint_expression
    [AND constraint_expression2 ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

**Single-block order** — the whole clause sits after `WHERE`:

```sql
SELECT ...
FROM ...
[WHERE ...]
DECIDE [Table.]variable_name(type) [, [Table.]variable_name2(type2) ...]
SUCH THAT
    constraint_expression
    [AND constraint_expression2 ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

The declaration may appear in one position or the other, never both. A
declaration with no `SUCH THAT`, or a `SUCH THAT` with no declaration, is a
parser error.

## 2. Decision Variables

- Must be declared in the `DECIDE` list, and the type is **mandatory**: it is
  written in parentheses after the name, `x(INT)`. There are exactly three type
  names — `INT`, `BOOL`, `REAL`.
- Scope: Available in `SUCH THAT`, `MAXIMIZE/MINIMIZE`, and the `SELECT` list.
- There are three **variable scopes**, which decide how many solver columns one
  declaration yields:

  | Spelling | Scope | Columns |
  |---|---|---|
  | `x(INT)` | row-scoped (default) | one per result row |
  | `T.x(INT)` | table-scoped | one per distinct entity of `T` (§2.1) |
  | `scalar x(INT)` | query-wide | exactly one, for the whole query (§2.2) |
- **Type Declarations** (in DECIDE clause):
  - `x(INT)`: $x \in \{0, 1, 2, ...\}$ by default
  - `x(BOOL)`: $x \in \{0, 1\}$ (automatically adds bounds constraints)
  - `x(REAL)`: $x \in [0, \infty)$ by default (continuous)
- **Default lower bound is 0** for `INT` and `REAL`. This is a
  *default*, not a floor: a variable becomes **signed** (may take negative
  values) when the query gives it an explicit negative lower bound —
  `x >= -K`, `x BETWEEN -K AND K`, or a negative literal in an `IN` domain
  (`x IN (-5, 0, 5)`). A variable the query never lowers stays non-negative.
  There is no fully-free ($-\infty$) domain: signed variables always have a
  finite lower bound. See `03_expressivity/decide/done.md` → "Signed variables".

**Examples:**

```sql
DECIDE x(BOOL)           -- x is binary (0 or 1)
DECIDE x(INT)           -- x is integer, default domain {0, 1, 2, ...}
DECIDE x(REAL)              -- x is continuous, default domain [0, +inf)
DECIDE x(BOOL), y(INT), z(REAL)  -- multiple typed variables

-- Signed (negative-domain) variables: opt in with an explicit negative bound
DECIDE adj(REAL)            -- ... SUCH THAT adj BETWEEN -10 AND 10  → adj in [-10, 10]
DECIDE d(INT)           -- ... SUCH THAT d >= -5                  → d in [-5, +inf)
```

### 2.1 Table-Scoped Variables

By default, decision variables are **row-scoped**: the solver creates one variable per row in the input relation. When a query joins multiple tables, the input relation is the join result, and each result row gets its own independent variable.

**Table-scoped** variables are declared with a table qualifier: `DECIDE Table.var(TYPE)`. A table-scoped variable has ONE value per unique entity in the named source table. All result rows originating from the same entity share the same variable value (entity consistency).

- The table qualifier must match a table alias or table name in the `FROM` clause.
- Entity identification uses all columns from the source table as a composite key.
- Mixed queries can declare both row-scoped and table-scoped variables.
- Reduces solver variable count from `num_rows` (join result size) to `num_entities` (distinct entities in the source table).

### 2.2 Query-Wide (`scalar`) Variables

Some decisions are not attached to a tuple. `DECIDE scalar name(TYPE)` declares
**one** decision shared by the whole query, regardless of input cardinality.

```sql
SELECT r.regionID, ship, max_shortfall
FROM routes r
DECIDE ship(INT), scalar max_shortfall(INT)
SUCH THAT ship <= max_shortfall
MINIMIZE max_shortfall - SUM(ship)
```

- **Spelling.** The `scalar` keyword precedes the name; the type is still
  mandatory. `scalar` is an unreserved keyword, so it remains usable as an
  ordinary column, alias, or table name.
- **Never table-qualified.** `scalar T.x(INT)` is a contradiction and is
  rejected at parse time — a query-wide decision has no entity to attach to.
- **Output.** The assigned value is repeated on every output row, the same way a
  table-scoped assignment repeats on every row carrying that entity.
- **In constraints.** A query-wide decision may be compared against row-varying
  data (`ship <= max_shortfall`); the constraint is generated per row, and every
  generated row references the same solver column. That is what makes the
  decision shared rather than per-row. It may also be the **bound of a reduced
  constraint** — `SUM(ship) <= max_shortfall`, and the paper's Example 1
  `demand - SUM(ship) <= max_shortfall PER regionID` — since being row-invariant
  is exactly what lets it stand beside a reducer.
- **A reducer is rejected iff its body is row-invariant** — holds the same value
  on every row, with no data column and no row- or entity-scoped decision
  anywhere inside it. `SUM(cap)`, `AVG(cap)` and `SUM(cap1 + cap2)` are rejected:
  there is one column and nothing for the reducer to reduce over, so either
  plausible reading (coefficient `1` or coefficient `n`) is a different
  optimization problem. Use the bare name instead.
  A body that mixes a scalar with row-varying data is **not** row-invariant and
  is a standard scalar-times-vector reduction: `SUM(cost * cap)` means
  `(SUM cost) * cap`, and `SUM(x + cap)` means `SUM(x) + n*cap` where `n` is the
  number of counted rows (the group's, under `PER`; the qualified relation's
  distinct tuples, under `agg(D: ...)` — see §5.1). This is the same rule stated
  on a relation-qualified reducer's body.
- **In objectives.** A query-wide decision may appear bare, either alone
  (`MINIMIZE max_shortfall`) or as an additive term beside reducers
  (`MINIMIZE max_shortfall - SUM(ship)`) — its coefficient is applied **once**,
  not once per row — or inside a reducer whose body is not row-invariant
  (`MINIMIZE SUM(cost * cap)`), where it is weighted like any other term.

Tests: `test/decide/tests/test_scalar_scope.py`.

**SUM/AVG semantics**: Aggregates follow SQL semantics and sum over result rows, not entities. If an entity appears in 3 result rows (because it joined with 3 rows from another table), its variable contributes 3 times to a SUM. This matches what a user would expect from the join result.

**Example:**

```sql
-- Select nurses to keep, one decision per nurse even though each nurse
-- appears once per shift they are assigned to.
SELECT n.name, s.shift_date, keepN
FROM nurses n
JOIN shifts s ON n.id = s.nurse_id
DECIDE n.keepN(BOOL)
SUCH THAT
    SUM(keepN * s.hours) <= 100
MAXIMIZE SUM(keepN * n.skill_score)
```

Here `n.keepN` is table-scoped to `nurses`: if nurse Alice appears in 5 shift rows, all 5 rows share a single `keepN` variable. Without the `n.` prefix, each of the 5 rows would get its own independent variable.

**Limitations:**
- The table qualifier must refer to a table or alias present in the `FROM` clause.
- Entity keys are derived from all columns of the source table. There is no syntax to specify a custom key subset.

## 3. Constraints

Constraints must evaluate to a boolean. Multiple constraints are separated by `AND`.

- **Supported Operators**: `=`, `<`, `<=`, `>`, `>=`, `<>`.
  - `<>` (not-equal): Supported on both per-row and aggregate constraints. Rewritten to `LHS <= K-1 OR LHS >= K+1`, which (like strict `<` / `>`) is only valid when the LHS is integer-valued. Rejected at **bind** time with a `BinderException` whenever the compared side is not provably whole-numbered from its declared types — a REAL decision, or a column/literal of a fractional type (`DOUBLE`, `DECIMAL` with scale > 0). The message names the offender and the cast that fixes it. For `AVG(x) <> K` the denominator is hoisted to the RHS (emitted as `SUM(x) <> K*n`, per-group size for PER), keeping the LHS integer-valued.
    - Encoding depends on where `K` falls in the LHS's reachable range. When `K` is **interior**, the disjunction costs 1 auxiliary binary + 2 Big-M constraints. When the range lies wholly on one side of `K`, one branch is dead and a **single plain inequality** is emitted instead — `SUM(x) <> 0` over non-negative decisions is just `SUM(x) >= 1`, with no binary and no Big-M. When `K` is unreachable the constraint excludes nothing and is dropped. Only a variable's intrinsic domain (BOOL 0/1, default non-negativity) licenses the collapse; a bound you write yourself stays loosenable under `decide_diagnostics()` and keeps the disjunction.
  - An L0 count is integer-valued whatever it counts, so `norm(e, 0, M) < K` and `norm(e, 0, M) <> K` are accepted even when `e` references a REAL decision. Other norm orders (`1`, `2`, `'inf'`) are not counts and are not exempt.
  - `<` / `>` (strict): Rewritten internally to the integer-step form (`LHS < K` $\rightarrow$ `LHS <= ceil(K) - 1`), which is only valid when the compared side is provably whole-numbered. Use `<=` / `>=` instead when it is not.
    - **The rule is stated on declared types, and rejects at bind time**, before any data is read. A side is whole-numbered when every decision in it is `INT`/`BOOL` and every column, literal and reducer over them has a whole-numbered type: an integer type, or `DECIMAL` with scale 0. `DOUBLE`, `FLOAT` and `DECIMAL(p, s>0)` are not, *whatever values they happen to hold* — otherwise inserting one fractional row could make a working query illegal. The message names the offender and the cast that fixes it (`SUM(x * l_quantity::BIGINT) < 100`).
    - **Not refused**: the bound `K` itself (a fractional bound is handled by rounding, or for `<>` by dropping a tautology); a decision-free additive offset, which moves the bound rather than the lattice; `POWER(e, n)` with a whole, non-negative constant `n`; and an L0 count, which is integral whatever it counts. A fractional *multiplier* (`SUM(0.5 * x) < 5`) is refused, since it rescales the decision off the lattice.
- **Between**: `expr BETWEEN a AND b` $\rightarrow$ `expr >= a AND expr <= b`.
- **In**: `x IN (v1, ..., vK)` — works on both table columns and decision variables. On decision variables, rewritten to K binary indicator variables with cardinality + linking constraints. `IN` on aggregates (e.g., `SUM(x) IN (...)`) is not supported.
- **Linearity**: Most sub-expressions involving a decision variable must be linear.
  - `x * 5`: OK.
  - `x + y`: OK.
  - `x * column`: OK (column is constant per row).
  - `x * y`: OK — bilinear (Gurobi only for non-Boolean pairs; McCormick for Boolean×anything).
  - `POWER(x - target, 2)`: OK — quadratic constraint (Gurobi only, via `GRBaddqconstr`).
  - `x * x * x`: **ERROR** (triple+ products not supported).
- **Quadratic constraints**: `POWER(linear_expr, 2)` / `expr ** 2` / `(expr)*(expr)` in constraints enables QCQP. Gurobi only. Composes with WHEN, PER. See Section 3.1 below.
- **Subqueries**: Scalar subqueries (both uncorrelated and correlated) are allowed on the RHS of constraints. Correlated subqueries are decorrelated into joins, producing per-row values. For aggregate constraints, the subquery RHS must evaluate to the same scalar for all rows. Subqueries cannot reference DECIDE variables.

### 3.1 Quadratic Constraints (QCQP)

```sql
-- Per-row quadratic constraint
SUCH THAT POWER(x - target, 2) <= 9

-- Aggregate quadratic constraint (total budget)
SUCH THAT SUM(POWER(x - target, 2)) <= 1000

-- With PER grouping
SUCH THAT SUM(POWER(x - target, 2)) <= 50 PER department

-- Multiple syntax forms (all equivalent)
SUCH THAT POWER(x - t, 2) <= K
SUCH THAT (x - t) ** 2 <= K
SUCH THAT (x - t) * (x - t) <= K
```

**Gurobi only** — HiGHS does not support quadratic constraints. Negated and scaled forms are supported: `-POWER(expr, 2)`, `K * POWER(expr, 2)`.

### 3.1 Numeric precision and casts

A DECIDE model carries **one numeric domain: `DOUBLE`**, roughly 15 significant
digits. This is not a policy choice — the solver interface is `double` throughout
(`SolverInput` holds `vector<double>` for every bound and coefficient) and both
Gurobi and HiGHS take doubles, so every value in a model ends up there whatever
types the SQL expression carried.

Three consequences are explicit:

**Do not cast decision expressions in solver clauses.** `CAST`, `TRY_CAST`, and `::`
are rejected when their input contains a decision variable in `SUCH THAT` or the
objective. This is independent of the target type:

```sql
SUCH THAT CAST(x AS INTEGER) <= 3     -- rejected
SUCH THAT TRY_CAST(x AS DOUBLE) <= 3  -- rejected
MAXIMIZE SUM(x::DOUBLE)               -- rejected
```

Decision values already enter the solver as DOUBLE, so a cast to DOUBLE is redundant.
Other targets introduce SQL computations such as rounding that are not supported
solver algebra. Compare the decision directly and cast the data or bound instead.

**Data casts retain DuckDB semantics.** `x <= CAST(1.6 AS INTEGER)` has a bound of 2,
and `SUM(x * CAST(weight AS INTEGER))` uses the cast result as its coefficient.
DuckDB evaluates the complete typed data expression first; DeciDB converts only its
result to DOUBLE when building the solver model.

**Internal casts are transparent.** DuckDB may insert casts to reconcile SQL types,
for example when comparing `SUM(x)` with a DECIMAL column. Those binder-generated
wrappers over decision algebra are implementation detail and do not change the model.

This restriction does not affect ordinary SQL. A cast in a standard query or in the
post-solve `SELECT` list, including `SELECT CAST(x AS INTEGER)`, behaves normally.

**Beyond the domain.** Above `2^53` (about 9 quadrillion) a DECIDE model and
row-by-row SQL evaluation can disagree, because SQL rounds each side of a written
comparison independently while DeciDB folds the bound once. Such a query is outside
what the solver could represent in any case. This limit is pinned by
`test_canonicalize_cast.py::test_magnitudes_beyond_double_domain_are_a_documented_limit`.

## 4. Objective

- **Optional**: Omitting `MAXIMIZE`/`MINIMIZE` creates a feasibility problem — the solver finds any assignment satisfying all constraints. Both Gurobi and HiGHS support this.
- When present, must be a supported aggregate expression (`SUM(...)`, `AVG(...)`, `MIN(...)`, `MAX(...)`) or an additive expression composed of supported aggregate terms.
- Must involve at least one decision variable.
- Linear objectives: must be linear in decision variables.
- **Quadratic objectives (QP)**: `MINIMIZE SUM(POWER(linear_expr, 2))` is supported for convex quadratic programming. The inner expression must be linear in decision variables. Three equivalent syntax forms:
  - `POWER(expr, 2)` / `POW(expr, 2)` — function call
  - `expr ** 2` — exponentiation operator
  - `(expr) * (expr)` — identical multiplication (both sides must be the same expression)
  - Negated forms: `-POWER(expr, 2)`, `(-1) * POWER(expr, 2)` for concave QP (both solvers).
  - `MAXIMIZE SUM(POWER(expr, 2))` is non-convex (Gurobi only, via NonConvex=2).
  - Gurobi supports both continuous QP and mixed-integer QP (MIQP). HiGHS supports continuous QP only — integer/boolean variables with quadratic objectives require Gurobi.
- **Bilinear objectives and constraints (`x * y`)**: Products of two different DECIDE variables are supported in both objectives and constraints. Two categories:
  - **Boolean x anything** (McCormick linearization): When one factor is `BOOL`, the product is exactly linearized. Works with both Gurobi and HiGHS. Requires a finite upper bound on the non-Boolean variable — given explicitly (`x <= K`) or inferred by implied-bound propagation from a non-negative constraint like `SUM(x) <= K`. Bool x Bool uses simpler AND-linearization (no Big-M).
  - **General non-convex** (`Real*Real`, `Int*Int`, `Int*Real`): Produces indefinite Q matrix. Objectives: Gurobi only (NonConvex=2). Constraints: Gurobi only (via quadratic constraints). HiGHS rejects with clear errors.
  - Data coefficients are supported: `SUM(profit * b * x)`.
  - Composes with WHEN: `SUM(b * x) WHEN condition`.
  - Triple or higher products (`a * b * c`) are rejected.

## 5. Aggregations

- `SUM()` is the primary aggregate over decision variables.
- `AVG(expr)` is supported. Rewritten to `SUM(expr)` with RHS scaled by row count N at execution time. For objectives, `AVG` and `SUM` share the same argmax/argmin. For constraints, `AVG(expr) op K` becomes `SUM(expr) op K*N` where N is the row count (adjusted for WHEN/PER context).
- `SUM`/`AVG` constraint bodies may include data-only additive terms alongside decision terms, e.g. `SUM(x + cost) <= K` or `SUM(q * (price + x)) <= K`. DeciDB evaluates the data-only aggregate contribution over the active rows/group and subtracts it from the scalar RHS before building the solver row.
- `MIN(expr)` and `MAX(expr)` are supported. Easy cases (`MAX(expr) <= K`, `MIN(expr) >= K`) become per-row constraints with no auxiliary variables. Hard cases (opposite direction, equality) use a global auxiliary variable and Big-M binary indicators. In objectives, `MINIMIZE MAX(expr)` and `MAXIMIZE MIN(expr)` use a global auxiliary; `MAXIMIZE MAX(expr)` and `MINIMIZE MIN(expr)` additionally require Big-M indicators. Composes with WHEN.
- Aggregate-local filters are supported on individual aggregate terms: `SUM(expr) WHEN condition + SUM(expr2) WHEN condition2`. This is different from an expression-level `WHEN` on the whole constraint or objective.
- **A factor on a reducer** is supported for every aggregate kind and on either side: `2 * SUM(x*p)`, `SUM(x*p) * 2`, `SUM(x*p) / 2`, `2 * MAX(x*v)`, and inside a composed LHS (`SUM(x) + 2 * MAX(x*v)`). The factor must be **one value for the whole query** — a literal, an expression over literals, or an uncorrelated scalar subquery. A negative factor is handled correctly: `-2 * MAX(e)` is `-2·MIN(e)`, and the rewrite swaps `MIN`/`MAX` accordingly.
  - A **row-varying** factor is rejected on every aggregate kind: `weight * SUM(x)` errors with *"'weight' varies per row, so it cannot multiply SUM(x). Move it inside the aggregate, e.g. SUM(x * weight)."* A reducer produces one number, so there is no row whose `weight` would scale it — the same reason SQL requires `col` in the `GROUP BY` for `col * SUM(x)`. Write the per-row-coefficient form explicitly.
  - A **correlated** scalar subquery is row-varying by construction and is rejected on the same grounds, with a message that calls it a subquery rather than quoting the internal column name flattening gives it: *"this subquery returns a different value for each row, so it cannot multiply SUM(x). Move it inside the aggregate, e.g. SUM(x * (SELECT ...))."* Only **uncorrelated** scalar subqueries are legal factors.
  - A **decision** factor is rejected as bilinear rather than as a scale: `s * SUM(x)` (with `scalar s`) is a product of two decisions.
- A query-wide (`scalar`) decision may appear as a **bare additive term** of an aggregate constraint, since it is row-invariant: `SUM(ship) - max_shortfall <= cap` (paper §3.1). It contributes its coefficient once, not once per row. It may equally be written as the **bound** (`SUM(ship) <= max_shortfall`); which side it is on carries no meaning, because canonicalization moves every decision-bearing term to the left before anything downstream looks. A **row-scoped** decision as the bound of a reduced constraint (`SUM(x) <= y`) is rejected — there is no single `y` for a number that has no row.
- **Either side may carry the decision.** The constraint gate classifies both sides and accepts when either is a DECIDE expression, so `5 >= x`, `10 >= SUM(x)` and `cap >= SUM(x)` are the same constraints as their mirror images. A reducer may appear on **both** sides (`SUM(x*v) <= SUM(y*v)`, `SUM(x*v) <= MAX(x*w) + 30`). What is *not* symmetric is the bound rule: a side carrying no decision must still reduce to one value per group, whichever side it is written on. For `<=`/`<`/`>=`/`>` that reduction is the tightest of the bound's own per-row values (MIN for `<=`/`<`, MAX for `>=`/`>`) — a bare data column qualifies exactly like a scalar subquery (`SUM(ship) <= stock PER depotID`, the paper's own Figure 1). `=` refuses a bound that genuinely varies, naming it a contradiction (two different single numbers can't both be *the* sum). `<>` does not collapse at all: it keeps every value the bound took as its own exclusion (`SUM(x) <> 500 AND SUM(x) <> 350` within one group), not just the tighter of the two.
- `norm(expr, p)` expresses an L_p regularization term over a decision-variable expression (lasso/ridge-style). The user supplies the weight, e.g. `MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`. The binder retains it as a DECIDE marker and the optimizer selects the existing formulation, so it composes with WHEN/PER and works in both objectives and constraints (including norm-bounded constraints like `norm(e, 1) <= K`). Supported orders:
  - `norm(e, 1)` → `SUM(ABS(e))` — L1 (sparse-leaning; ABS linearization).
  - `norm(e, 2)` → `SUM(POWER(e, 2))` — squared L2 / ridge (convex QP).
  - `norm(e, 'inf')` → `MAX(ABS(e))` — L-infinity (max linearization).
  - `norm(e, 0[, M])` → **L0 / count of nonzeros**. Adds one 0/1 indicator `z` per row, linked so `z = 1` **iff** `|e| >= tolerance` (a forward Big-M link forces `z=1` when `e != 0`, a reverse link `ABS(e) >= tol*z` forces `z=0` when `e` is within tolerance); the term becomes `SUM(z)`, the **exact** count. Upgrades the model to a MILP. Because the count is exact it is sound in **every** context — `MINIMIZE`/penalty and `<= K` cap, **and** `>= K`, `= K`, `MAXIMIZE`. The Big-M bound is **inferred from the data** when omitted — `norm(e, 0)` derives a tight per-problem `M` at execution from variable bounds (implied-bound propagation) + data; pass `norm(e, 0, M)` to supply it explicitly. The reverse link reuses ABS linearization, so `e` must have finite bounds. The nonzero tolerance (default `1e-4`, must exceed the solver feasibility tolerance) is configurable via `SET decide_l0_tolerance = …`; a value forced into `(0, tolerance)` is reported infeasible (tolerance-ambiguous).

### 5.1 Relation-Qualified Reducers — `SUM(D: expr)`

By default a reducer combines one term per **join-result row**. A join repeats a table's
tuples once per match, so an unqualified reducer over a table-scoped decision weights that
decision by how many rows its entity joined with. A **relation-qualified reducer** reduces
over the qualified relation's tuple identities instead, contributing one term per tuple.

**Syntax**: `agg(Rel[, Rel, ...]: expr)`, where `agg` is `SUM`, `AVG`, `MIN` or `MAX`, and
each `Rel` is a table alias or table name bound in the `FROM` clause. Naming more than one
relation widens the tuple identity to their concatenation — see "Several relations at
once" below.

```sql
SELECT routeID, depotID, open, ship
DECIDE D.open(BOOL), T.ship(INT)
FROM Depots D JOIN Routes T USING (depotID)
SUCH THAT ship <= capacity * open AND SUM(ship) >= 300
MINIMIZE SUM(unit_cost * ship) + SUM(D: opening_cost * open);
```

A depot serving three routes appears in three result rows. `SUM(D: opening_cost * open)`
charges its opening cost **once**; the unqualified `SUM(opening_cost * open)` would charge
it three times. Both forms remain available and the unqualified one is unchanged.

- **Identity is the tuple, not the value.** Two depots with equal `opening_cost` are two
  terms. This is not `SUM(DISTINCT expr)`.
- **Scope is the surviving rows, not the base table.** The join and `WHERE` decide which
  tuples contribute; a depot filtered out has no term and no decision variable.
- **Everything inside must come from a qualified relation.** A column or decision from a
  relation *not* named in the qualifier is rejected at bind time — `SUM(D: opening_cost +
  unit_cost * ship)` names `T`'s columns and is refused rather than picking an arbitrary
  route per depot. A row-scoped decision is refused for the same reason: it is not
  determined by any qualified relation's key. Constants are fine. A query-wide (`scalar`)
  decision is exempt from this rule — it is row-invariant, so it contributes the same
  value to every tuple regardless of which relation "owns" it (`SUM(D: opening_cost *
  cap)` is legal). It is still refused when it is the *whole* body, since a row-invariant
  body has nothing left to de-duplicate or sum over (`SUM(D: cap)` is rejected — batch D's
  row-invariance rule, §2.2).
- **Construction order** is `WHEN` selection → `PER` partitioning → qualifier grouping and
  de-duplication → aggregation. De-duplication happens **inside** each `PER` group.
- **`AVG(D: expr)` divides by the number of distinct tuples**, not the number of rows.
- **`MIN`/`MAX` are unaffected by de-duplication** — every row of a tuple identity carries
  the same value, so dropping repeats cannot move an extremum. The qualifier is accepted
  and is a no-op for them.
- **Several relations at once — `SUM(D, T: expr)`.** The tuple identity widens to the
  concatenation of every named relation's own key, so a row is a duplicate only when it
  repeats on *all* of them together. Naming a relation removes the fan-out it would
  otherwise contribute to the reducer; a relation left unnamed still contributes its
  fan-out, uncollapsed — so with a third, unqualified relation `X` in the join,
  `SUM(D, T: expr)` differs from both `SUM(D: expr)` and the unqualified `SUM(expr)`.
  `expr` may draw columns and decisions from any of the named relations, not just one.
  The list is order-independent (`SUM(D, T: ...)` and `SUM(T, D: ...)` are the same
  reducer). One consequence follows directly: if `D` and `T` are the *only* two relations
  the query joins, naming both is a no-op — the composite key already is the join-result
  row, so `SUM(D, T: expr)` and unqualified `SUM(expr)` compute the same thing. The two
  forms only diverge once some relation the query joins is left out of the qualifier.

## 6. Conditional Expressions — `WHEN`

The `WHEN` keyword enables conditional constraints and conditional objectives. A `WHEN` clause causes the expression to apply only to rows where the condition evaluates to true. Rows where the condition is false or NULL are excluded.

**Syntax**: `expression WHEN condition`

### 6.1 WHEN on Constraints

```sql
SUCH THAT
    SUM(x * weight) <= 50 WHEN category = 'electronics' AND
    SUM(x * weight) <= 30 WHEN category = 'clothing' AND
    x <= 1
```

**Execution**: WHEN conditions create per-row boolean masks. For aggregate constraints (`SUM`), masked-out rows are excluded from the summation. For per-row constraints, masked-out rows skip constraint generation entirely.

### 6.2 WHEN on Objective

```sql
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'
MINIMIZE SUM(x * cost) WHEN region = 'US'
```

**Execution**: Objective coefficients for non-matching rows are zeroed out. The solver sees a standard ILP where only matching rows contribute to the objective value.

### 6.3 Aggregate-local WHEN

`WHEN` can also be attached directly to a single aggregate term inside an additive aggregate expression:

```sql
SUCH THAT
    SUM(x * hours) WHEN morning + SUM(x * hours) WHEN evening <= 40

MAXIMIZE
    SUM(x * profit) WHEN high_margin + SUM(x * bonus) WHEN strategic
```

Each aggregate-local `WHEN` filters only that aggregate's rows. Rows outside all local aggregate filters do not contribute to that expression, but they are not removed from the query or from unrelated constraints/objectives.

Comparison predicates in aggregate-local constraint `WHEN` conditions must be
parenthesized so the final comparison remains the constraint bound:

```sql
SUM(x * hours) WHEN (shift = 'morning') + SUM(x * hours) WHEN (shift = 'evening') <= 40
```

An objective has no trailing bound, so its single atomic comparison is
unambiguous and may omit the parentheses:

```sql
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'
```

Parenthesize objective conditions containing `NOT`, arithmetic, or compound
logic as well.

Do not combine expression-level `WHEN` with aggregate-local `WHEN` in the same constraint or objective; the binder rejects that shape to avoid ambiguous double-filter semantics.

### 6.4 Rules (applies to both constraints and objectives)

- `WHEN` is postfix: the expression comes first, then `WHEN condition`.
- The `WHEN` condition must reference only table columns, **not** decision variables.
- NULL conditions are treated as false (expression does not apply for that row).
- When a `WHEN` condition contains `AND`/`OR`, parentheses are required:
  ```sql
  SUM(x * weight) <= 20 WHEN (category = 'A' AND status = 'active')
  ```
- Conditions containing `NOT` or arithmetic also require parentheses, for
  example `WHEN (NOT active)` and `WHEN (a + b > 5)`.
- Expressions without `WHEN` apply unconditionally to all rows.

## 7. Group-Scoped Constraints — `PER`

The `PER` keyword generates one constraint per distinct value (or combination of values) of column(s).

**Syntax**: `SUM(expr) comparison rhs PER column` or `PER (col1, col2, ...)`

```sql
SUCH THAT
    SUM(x * hours) <= 40 PER empID
    SUM(x * hours) <= 40 PER (empID, department)
```

Column references in `PER` can be either bare (`PER empID`) or qualified by table name / alias (`PER employees.empID`, `PER e.empID`). Qualified and unqualified forms are equivalent; the qualifier is purely syntactic disambiguation, useful for JOIN queries where the grouping column would otherwise be ambiguous. Mixed forms (`PER (empID, dept.id)`) are accepted.

### 7.1 PER + WHEN Composition

WHEN filters rows first, then PER groups the remaining rows:

```sql
SUCH THAT
    SUM(x * hours) <= 30 WHEN title = 'Director' PER empID
    SUM(x * hours) <= 30 WHEN title = 'Director' PER (empID, department)
```

### 7.2 PER on Objective — Nested Aggregates

PER on objectives uses nested aggregate syntax to specify both per-group and across-group aggregation:

```sql
-- Nested aggregate: OUTER(INNER(expr)) PER col
MINIMIZE SUM(MAX(x * cost)) PER department
MAXIMIZE MIN(SUM(x * profit)) PER region
MINIMIZE MAX(SUM(x * hours)) PER empID
```

All 9 combinations of `SUM`/`MIN`/`MAX` for outer and inner aggregates are supported.

**Flat aggregate + PER**:
- `SUM(expr) PER col` or `AVG(expr) PER col`: Accepted (no-op — global sum equals sum of group sums).
- `MIN(expr) PER col` or `MAX(expr) PER col` (flat): **Error** — use nested form instead.

WHEN + PER composition is supported: `MINIMIZE MAX(SUM(x * hours)) WHEN active = 1 PER empID`.

### 7.3 Restrictions

- **Aggregate-only**: PER requires an aggregate constraint, such as `SUM(...)`, `AVG(...)`, or an additive expression of aggregate terms. Per-row constraints are rejected.
- **Column references only**: Each PER column must be a simple column reference, not an expression or decision variable.
- **Multi-column PER must be parenthesized**: use `PER (col1, col2)`, not `PER col1, col2`. Top-level `SUCH THAT` constraints are separated only by `AND`; a comma in the constraint list is rejected by the parser. The parentheses are required to disambiguate the grouping key from the surrounding constraint syntax.
- **Constant RHS**: The right-hand side must be constant across groups.
- **NULL handling**: Rows where any PER column is NULL are excluded.
