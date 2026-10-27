# Language roadmap

DeciDB is moving to the language proposal (`context/New_syntax.pdf`, "the deck"). This page lists what the move has
not reached yet: deck features DeciDB does not implement, and current behavior the deck changes. Each item is planned
transition work, not a bug: today's behavior is deliberate, documented in [guide.md](guide.md), and either refuses a
query with a message or answers it correctly. None is scheduled; when one is, its work moves to the todo of the area
that owns it.

Already implemented from the deck: keyed declarations (`per K: x(TYPE)`, pp. 6–7), the prefix `when`/`per` and the
postfix `by` with their defaults (p. 16), the determination rule (pp. 26–31), deck examples 1–8 (pp. 17–24), and one
nested shape, the two-level objective `outer(per k: inner(e) by (k))`.

## Current behavior the deck changes

DeciDB refuses or raises in three places where the deck answers. Each guard was added for a reason that still holds
until the deck's behavior is built; the move replaces it.

### A qualified decision name (p. 7)

- **Today:** a declaration `DECIDE T.ship(INT)` and a reference `D.open` in `SUCH THAT` are refused; a decision is
  declared and referenced by its bare name (the removed-spelling rules for `T.x(TYPE)` in
  `decide_removed_spellings.cpp`, D56; `RefuseQualifiedDecisionReferences`, D43, S2).
- **Why today:** so a qualifier is never read as a scope, which the removed `T.x(TYPE)` spelling did.
- **Deck:** "Reason 3": a qualifier keeps its SQL role and only resolves the name; with the same scope, `T.ship(INT)`
  and `ship(INT)` differ only in name resolution.

### A `when` that keeps no rows (pp. 29–30)

- **Today:** `when region = 'Mars': sum(x) <= 5` over data with no such region raises before solving
  (`PhysicalDecide::RunScopeChecks`, D4; [guide.md §7.2](guide.md#72-null-and-empty-sets)).
- **Why today:** a typo in the condition would otherwise drop a constraint silently.
- **Deck:** the clause yields one instance per class of `R_θ / κ`; an empty `R_θ` has no classes, so the clause yields
  no constraints.

### An empty reducer group and a NULL value (pp. 45, 50, 56)

- **Today:** `per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity` raises when a depot has
  no priority shipment (deck example 7; `RunScopeChecks`, D8, D18). A NULL coefficient or bound raises, naming the
  column (`ExtractDoubleColumn`).
- **Why today:** a skipped NULL or an empty group can hide missing data; the error names the column to fix.
- **Deck:** a reducer over no rows is NULL, a reducer skips NULL inputs as SQL does, and a NULL that reaches the
  comparison unreduced skips that constraint instance. The deck states this with frame selectors, as the general NULL
  rule. The owner's audit of SQL NULL behavior is
  [../pipeline/02_binder/todo.md §2](../pipeline/02_binder/todo.md#2-functions-that-behave-unlike-sql).

## Nested reducers (ANR, pp. 25, 28–32)

The grammar `B ::= f(e…, G…)`, `E ::= [when θ][per K]: B`, `G ::= agg(E)[by (Γ)]` lets a reducer's argument hold
reducers to any depth. Example 9: `sum(max(ship) by (S.customerID)) by (D.depotID) <= D.priorityCapacity`.

- **Today:** one gate refuses it, *"DECIDE: an aggregate inside an aggregate is not supported yet"*
  (`ValidateScopePlacement`, `decide_binder.cpp`), except for the two-level objective.
- **What is ready:** the grammar is already recursive, and a reducer's scope lives as tags on its own bound aggregate
  (`filter`, `__qualified_by_<i>__`, `__group_key_<i>__`), so depth is encoded nowhere else in the binder.
- **The two-level objective's open restrictions.** It is the one nested shape built, and each of these is refused
  with "not supported yet" ([errors.md §2](errors.md#2-not-supported-yet)):
  - a `when` inside either reducer: needs a per-class filter in the two-level rewrite (stage 05) and in the per-class
    grouping at execution;
  - a `per` inside the inner reducer: needs the inner reducer's own key scope inside each class, and its determination
    check within the class;
  - a `by` key other than the `per` key: needs inner groups that differ from the outer classes in the two-level
    rewrite;
  - `avg` as the outer reducer: needs an average over classes, whose count is data;
  - the two-level objective beside another term or under a factor.
- **What admitting a shape takes:** lift the gate for it, and let the value rewrite (`RewriteScopedReducers`) recurse:
  one inner helper per class of the inner `by`, inside the outer group, defined before the outer reducer that reads
  it.
- **Open questions:** which shapes enter first (example 9 first); whether an inner helper's definition belongs to the
  outer clause's provenance.
- **Test when built:** example 9 and `sum(x - avg(x) by (g)) <= 5`, one nested shape per reducer kind, judged against
  `test/decide/tests/_brute_force.py` on both solvers; the gate's message kept for every shape not admitted.

## Variant signatures (pp. 34–40)

When a `per` key does not determine a body expression or a `by` key, the deck makes one constraint per distinct
combination (the class's *variant signatures*) instead of an error. Where every class has one signature, the result
equals the current semantics, so it is a conservative extension. DeciDB raises the determination error instead
([guide.md §7.1](guide.md#71-the-determination-rule)). `LogicalDecide::scope_checks` already lists the parts a
signature is built from.

## Frame selectors (pp. 42–56, 66–69)

Ordered navigation inside a constraint, independent of SQL windows:

- point selection `at(F [else v]: e)` with `F ::= first | last | [k] previous | [k] next`;
- range aggregation `agg(from F1 to F2 [every d] [else v | all]: e)`;
- each followed by `over (S [asc | desc] [cyclic] [within P])`.

Defaults: `k = 1`, `asc`, `within ()`, `every 1`, `else NULL`. A NULL a reducer consumes is ignored; a non-reduced NULL
reaching the comparison skips that constraint instance (p. 50). Well-definedness adds `R_θ ⊨ κ → π` for every frame
partition `π`. The deck's reasons for not reusing SQL windows: windows run after `GROUP BY`, and SQL has no cyclic
order, no stepped range and no fill or complete-frame boundary policy.

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

- **DIAGNOSE on a feasible query** returns one `feasible` row. A richer report (solve time, model size, gap), as a
  true `EXPLAIN ANALYZE` counterpart, is unscheduled.
