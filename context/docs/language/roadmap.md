# Language roadmap

DeciDB is moving to the language proposal ([`context/New_syntax.pdf`](../../New_syntax.pdf), "the deck"). This page
lists what the move has not reached yet: deck features DeciDB does not implement, and current behavior the deck changes.
Each item is planned transition work, not a bug: today's behavior is deliberate, documented in [guide.md](guide.md), and
either refuses a query with a message or answers it correctly. None is scheduled; when one is, its work moves to the
todo of the area that owns it.

Already implemented from the deck: keyed declarations (`per K: x(TYPE)`, pp. 6–7), the prefix `when`/`per` and the
postfix `by` with their defaults (p. 16), variant signatures (pp. 34–40), deck examples 1–9 (pp. 17–25), NULL keys
and empty groups (pp. 30, 50, 56), reducers nested to any depth (pp. 25, 28–32), the two-level objective
`outer(per k: inner(e) by (k))` among them, and frame selectors, points and ranges (pp. 42–56).

The rulings for variant signatures, nesting and frame selectors are settled ([decisions.md](decisions.md) D70–D86) and
encoded by the test oracle `test/decide/tests/_anr_oracle.py` ([../testing/scope_suite.md](../testing/scope_suite.md));
the sections below say how far the engine builds them.

## Current behavior the deck changes

DeciDB refuses in one place where the deck answers. The guard was added for a reason that still holds until the
deck's behavior is built; the move replaces it.

### A qualified decision name (p. 7)

- **Today:** a declaration `DECIDE T.ship(INT)` and a reference `D.open` in `SUCH THAT` are refused; a decision is
  declared and referenced by its bare name (the removed-spelling rules for `T.x(TYPE)` in
  `decide_removed_spellings.cpp`, D56; `RefuseQualifiedDecisionReferences`, D43, S2).
- **Why today:** so a qualifier is never read as a scope, which the removed `T.x(TYPE)` spelling did.
- **Deck:** "Reason 3": a qualifier keeps its SQL role and only resolves the name; with the same scope, `T.ship(INT)`
  and `ship(INT)` differ only in name resolution.

## Frame selectors (pp. 42–56, 66–69)

Ordered navigation inside a constraint, independent of SQL windows:

- point selection `at(F [else v]: e)` with `F ::= first | last | [k] previous | [k] next`;
- range aggregation `agg(from F1 to F2 [every d] [else v | all]: e)`;
- each followed by `over (S [asc | desc] [cyclic] [within P])`.

Defaults: `k = 1`, `asc`, `within ()`, `every 1`, `else NULL`. A NULL a reducer consumes is ignored; a non-reduced NULL
reaching the comparison skips that constraint instance (p. 50). Well-definedness adds `R_θ ⊨ κ → π` for every frame
partition `π`. The deck's reasons for not reusing SQL windows: windows run after `GROUP BY`, and SQL has no cyclic
order, no stepped range and no fill or complete-frame boundary policy.

- **Today:** every spelling parses, binds and is formulated (`DecideFrameExpression`, `LogicalDecide::frames`, guide
  §5.6): a point is read in place, a range over data is a value of the row, and a range over decisions a helper
  decision per window (D78).
- **What is left:** their composition with variants and nesting (D82); a frame that may read nothing inside a reducer
  over decisions, the member dropped (D84); a frame in a bound that takes its group's tightest value (D85) or inside a
  reducer of a bound; a frame over decisions in a `MIN`/`MAX` of several terms.

## Conditional generation with `if` (pp. 58–59)

`[when θ][per K][if b]: B`, where `b` is a Boolean over decisions: each instance is `b ⇒ B`. `when` filters known
data; `if` guards with an unknown. Requires `R_θ ⊨ κ → b`. Example: enforce a depot's capacity only if its decided
`status` is `'open'`.

## Declaration bounds and domains (pp. 9, 11)

- **Bounds at the declaration:** `decide ship(INT) between 0 and T.capacity per T.routeID`, or `x(TYPE) <= e`, where
  `e` is a constant or a column determined by the declaration's key. Today bounds are written in `SUCH THAT` and
  folded into the box.
- **Semi-continuous and semi-integer:** `SEMIREAL` / `SEMIINT` with a required `between x and y`: the value is 0 or
  in `[x, y]`.
- **Finite text domains:** `x(TEXT) in ['closed', 'open', 'repair']`, referable in later clauses (with `if`).

## Objectives (pp. 62–63)

- **Lexicographic:** `minimize e1 then minimize e2 then maximize e3`, optimized in priority order.
- **`satisfy`:** find the first feasible solution, spelled explicitly. Today a query with no objective is a
  feasibility problem.
- An objective's omitted `per` is `per ()`; no other outer `per` is allowed. DeciDB already treats the objective as
  produced once.

## The one-pass algorithm (pp. 79–119)

The deck's evaluation strategy for nested ANR: build an annotated AST whose scoped expressions carry their signature
(per key, direct tuple terms, direct `by` keys), fill it top-down in one pass over the joined rows, then evaluate
bottom-up into constraints. Runtime containers follow the node role: a reducer is a map from `by` key to operand; a
scoped expression is a vector for `per row`, otherwise a set of distinct signatures. Frames are built the same way
over partition and order. `MAX` inside `SUM` becomes helper variables (`x₁,₂ = max(x₁, x₂)`); `SUM` inside `SUM`
flattens. DeciDB evaluates through DuckDB plans and window columns instead.

## Beyond the deck (pp. 2, 76)

Decision-driven structures (decision-dependent frames, domains such as permutations, sets and intervals,
order-dependent aggregates such as quantiles, rankings and CVaR), other data-driven structures (conic and SAT
optimization, constraint-programming globals such as `allDifferent`), and sound translations to other solver families
(CP-SAT, Hexaly, IBM CP). SOCP is below.

## SOCP

Constraints of the form `‖Ax + b‖ ≤ cᵀx + d`, a true Euclidean norm bound.

- **Why deferred.** The practical case already works: `‖e‖ ≤ R` and `‖e‖² ≤ R²` have the same solutions, and the
  squared form is `norm(e, 2) <= R*R`, a quadratic constraint. SOCP adds only the square-root spelling and the
  non-convex `>=` direction, which must be refused anyway.
- **What an implementation must settle.**
  - *Spelling.* `norm(e, 2)` already means the **squared** L2 norm. A square-root norm needs a distinct spelling, and
    two side by side is a hazard for users; that is the main design cost.
  - *Direction.* `‖e‖ ≤ b` is convex; `‖e‖ ≥ b` is not and is refused at bind time, naming the clause.
  - *Backend.* Gurobi expresses cones through convex quadratic constraints; HiGHS has LP, MIP and QP only. A
    Gurobi-only class fits how quadratic constraints, non-convex QP and MIQP work: add a flag to `SolverModelClass`
    (`decide_solver_capabilities.hpp`) and let `RequireDecideSolverSupport` refuse at plan time.
- Revisit when a real query needs something the squared form cannot express.

## Not from the deck

- **A backend that takes only convex quadratic rows.** Gurobi solves a non-convex quadratic row at its default
  settings, so DECIDE does not classify row convexity; the model class records it for the objective only
  (`decide_solver_capabilities.hpp`). A backend that refuses non-convex rows needs that classification, for
  user-written rows and for the rows a defined MIN/MAX body emits (`../pipeline/05_optimizer/done.md`, "Non-linear
  reducer bodies").
- **DIAGNOSE on a feasible query** returns one `feasible` row. A richer report (solve time, model size, gap), as a
  true `EXPLAIN ANALYZE` counterpart, is unscheduled.
