# Stage 04 — Canonicalizer: open work

Each item below changes which shape `ClassifyScoped` or `PeelScale` gives a comparison,
which only this stage decides (`done.md` §3.7, §3.10).

## CAN-1 — One polynomial form for decision products

- **Ruling (owner, 2026-11-06).**
  1. This stage writes every product of decisions, in every reducer body and per-row side, in one spelling. The term
     splitter (`decide_term_split.cpp`) becomes a reader of that spelling.
  2. The spelling is factored: a coefficient (constant or data) times a product of linear forms, each raised to a whole
     number. `2*x*x` is `2 * POWER(x, 2)`, `w*x*x` is `w * POWER(x, 2)`, `(x-t)*(x-t)` is `POWER(x - t, 2)`. Squares
     are not multiplied out.
  3. A data coefficient on a square is solved by Gurobi only, as `x*y` is: its sign is not known before the query runs.
  4. Every degree is written out. The solver gate refuses degree above 2, quoting the clause. Layer 2 refuses only what
     has no polynomial form (`EXP(x)`, `SQRT(x)`, `x / y`). Solving degree above 2 is not part of this task.
- **Problem.**
  - Two places read the spelling of a square or a product, and their gaps refuse legal queries:
    - `decide_degree.cpp` (`is_quadratic_form`, the `POWER` / `**` / `^` reading);
    - the term splitter (`DetectQuadraticPattern`, `ClassifyProduct`).

    `2*(x*x)` and `2*POWER(x,2)` are accepted. `2*x*x` is refused as a "same-variable product". `w*POWER(x,2)` is
    refused as degree above 2 (BUG-46), because a `SQUARE` term stores its scale as a constant (`square_scale`).
  - Layer 2 refuses degree above 2 at bind time. That refusal was first added so the message could name the clause as
    written. The solver gate now quotes the clause too (`05_optimizer/done.md` § The model-class gate), so the refusal
    can join the other solver-capability refusals.
  - § 5 of `done.md` says the stage does not combine like terms, but rule C4 merges them (`x + x` is `2 * x`).
- **Change.**
  - In this stage, rewrite each decision product in place in the tree:
    - flatten the factors;
    - merge equal factors into a power;
    - move each decision-free factor into the coefficient, on the left.

    ABS, MIN/MAX, subqueries and data casts are left as they are. The pass stays total and pure, and never leaves the
    tree.
  - The term splitter produces one term kind, a coefficient × a product of powers of linear forms, in place of
    `LINEAR` / `PRODUCT` / `SQUARE`. A tree outside the spelling is an engine bug (`D_ASSERT` / internal error), not a
    refusal. Delete `UnsupportedProductFactor` and the same-variable refusal. Delete the `ABS` factor branch if nothing
    reaches it after `RewriteAbs`.
  - Readers of split terms:
    - stage 05's linear form (constraint quadratic groups, objective);
    - `decide_rewrite_bilinear.cpp`, which must leave a term above degree 2 alone;
    - singleton-row folding;
    - direct solve (`direct_ir`, `direct_bound_reader`, A1, N1, R1, S1). These decline a data coefficient on a square.
  - The model class records the highest degree. `decide_solver_gate.cpp` refuses degree above 2 in the same sentence
    shape as the other gaps, and judges convexity from the coefficient: a constant whose sign agrees with the sense is
    convex, and a data coefficient is non-convex.
  - Layer 2 (`decide_degree.cpp`) stops refusing degree above 2 and keeps refusing non-polynomials. Its square
    detection goes if nothing reads it.
  - `done.md` states the contract that holds: total, pure, one spelling per meaning, never leaves the tree. That
    replaces "never opens a term". § 5 is corrected.
- **Before starting.** A `decidb-architect` review of this design. Questions for it:
  - Does the body rewrite keep the stage total, pure and a fixed point?
  - Which readers change?
  - Where does the model class learn the degree?
  - What remains of `decide_degree.cpp`?
- **Done when.**
  - `2*x*x`, `x*2*x`, `w*POWER(x,2)`, `w*x*x` and `POWER(x,2)*w` answer on Gurobi. Oracle cases cover them under a
    limit and under `MINIMIZE`.
  - `MINIMIZE SUM(3*x*x)` answers on forced HiGHS.
  - A data weight on a square under forced HiGHS is refused with the gate's text.
  - `SUM(x*x*y) <= 10` and `POWER(x, 3)` are refused by the gate, quoting the clause. The degree-above-2 tests keep
    their refusal, with the new text.
  - BUG-46 leaves `issues/bugs.md`. `language/functions.md`, `problem_types.md` and `errors.md` list the accepted forms
    and the refusal.
