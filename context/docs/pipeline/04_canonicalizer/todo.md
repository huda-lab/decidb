# Stage 04 — Canonicalizer: open work

Each item below changes which shape `ClassifyScoped` or `PeelScale` gives a comparison,
which only this stage decides (`done.md` §3.7, §3.10).

## CAN-1 — One polynomial form for decision products, and one shape check after it

- **Ruling (owner, 2026-11-06).**
  1. This stage writes every product of decisions, in every reducer body and per-row side, in one spelling. The term
     splitter (`decide_term_split.cpp`) becomes a reader of that spelling.
  2. The spelling is factored: a coefficient (constant or data) on the left times a product of linear forms, each
     raised to a whole number. `2*x*x` is `2 * POWER(x, 2)`, `w*x*x` is `w * POWER(x, 2)`, `(x-t)*(x-t)` is
     `POWER(x - t, 2)`. Squares are not multiplied out.
  3. A data coefficient on a square is solved by Gurobi only, as `x*y` is: its sign is not known before the query runs.
  4. Every degree is written out. The parser refuses only syntax, and the binder only binding (names, scopes,
     aggregation levels, function resolution, language not built yet) plus what only it can see. The algebra
     refusals run once, in a shape check right after canonicalization. The solver gate refuses only what the chosen
     backend cannot take. Solving degree above 2 is not part of this task.
  - (a) Each clause's shape is judged as written, after canonicalization and before any rewrite.
  - (b) One product group, `Q += l·rᵀ`, carries every degree-2 term from stage 05 through stages 06 and 08.
  - (c) Like terms merge over products: `x*x + x*x` is `2 * POWER(x, 2)`.
  - (d) `(b*x)*(b*x)` becomes `b*b * POWER(x, 2)`, a data coefficient (Gurobi only).
  - (e) `POWER(b, k)` over a BOOL decision is not rewritten to `b`.
  - (f) The one model-shape classifier SOL-4 needs is built here (`07_solver/todo.md` § SOL-4).
  - (g) Messages quote the written text. EXPLAIN's canonical view shows the canonical form.
  - (R-1) Degree above 2 is refused by the shape check, with the gate's sentence ("which no solver DeciDB supports
    can optimize").
  - (R-2) The shape check runs in `PlanSelectNode`, right after `FinalizeConstraintSourceInfo` and
    `CanonicalizeObjective`.
  - (R-3) Division by a decision moves to the shape check.
  - (R-4) A decision-bearing CASE is refused by the shape check. A CASE over data alone is accepted as a per-row
    coefficient, as COALESCE is.
  - (R-5) The `<>` rewrite (`decide_rewrite_notequal_avg.cpp`) reads degree from a small reader of the canonical form.
  - (R-6) The binder keeps its "not supported yet" refusals.
- **Ruling (owner, 2026-11-06), at the step-1 checkpoint.**
  - (A) Products are spelled coefficient first (`x * w` is `w * x`). Every message and DIAGNOSE quotes the written
    clause through its source location, never by searching the written text for the canonical text. EXPLAIN's ≡ line
    shows the canonical form, so a respelled clause prints one. The test that an untouched clause prints one line
    writes its clause in canonical order; a new case shows a respelled clause printing its ≡ line.
  - (B) The objective-term refusal quotes the written clause (`s * s`), as (A).
  - (C) A product of BOOL decisions (`b * b`, `(b+d)*(d+b)`) goes through the McCormick rewrite like any other; an
    oracle case checks it. (e) forbids only a special `POWER(b, k)` to `b` rewrite.
  - (D) An exponent that folds to a whole number (`POWER(x, 1)`, `CAST(2 AS INTEGER)`) is accepted. `POWER(x, 0)` is 1,
    as in SQL. The tests of the old refusal become oracle cases.
  - (E) (R-4) applies: the CASE-over-data tests become oracle cases; CASE over a decision keeps a refusal test.
  - (F) When a query has two refusals, the one the pipeline reaches first is reported.
  - (G) An operator outside the vocabulary over a decision reads "DECIDE: operator '%s' cannot take a DECIDE
    variable."
- **Ruling (owner, 2026-11-06), at the step-2 checkpoint.**
  - (H) `POWER(e, 0)` over a decision is 1, and the product form keeps the decisions of `e` at coefficient 0, as `0 * x`
    and `x - x` keep theirs. A reducer stays decision-bearing, so `SUM(POWER(x, 0)) <= 5` answers as `COUNT(*) <= 5`.
  - (I) The objective gets a source span, as each SUCH THAT clause has one, so its messages quote the written text.
  - (J) A bound operator carries its parsed operator's source location, where the binder makes it in one place; if it
    cannot, the missing caret is filed.
  - (K) The integer-step checks and the comparison-in-a-reducer refusal stay in the binder: moving them would change
    which queries are refused.
  - (L) Every shape-check refusal raises an Invalid Input error, as the gate does; it is not a binding error.
  - (M) As (G): a function over a decision reads "DECIDE: function '%s' cannot take a DECIDE variable.", and a division
    "DECIDE: cannot divide by a DECIDE variable." The CASE message keeps its advice.
- **Problem.**
  - Each algebra refusal has two to four recognizers, across the parsed tree, the bound tree, the optimizer and
    physical planning. Their gaps refuse legal queries:
    - `2*x*x` is refused as a "same-variable product", though `2*POWER(x,2)` is accepted;
    - `w*POWER(x,2)` is refused as degree above 2 (BUG-46), because a `SQUARE` term stores its scale as a constant.
  - Layer 2 marks "not a polynomial" with degree 3 (`DECIDE_UNCLASSIFIABLE_DEGREE`), so it cannot tell `x*x*y` from
    `EXP(x)`.
  - § 5 of `done.md` says the stage does not combine like terms, but rule C4 merges them.
- **Design, in build order (one commit each).**
  1. **Normal form (this stage).** `NormalizeProducts` is a bottom-up rewrite rule (as in DuckDB's
     `ArithmeticSimplificationRule`). It runs after `DesugarNorms` in `CanonicalizeComparison` and
     `CanonicalizeObjective`. For each decision-bearing `*` or POWER node:
     - flatten `*`, decision casts, unary minus and `POWER(b, k)` with a whole constant `k`;
     - a POWER whose base has degree 1 stays one linear form, so `POWER(b*x, 2)` keeps coefficient 1 and still runs on
       HiGHS;
     - merge equal decision-bearing factors into powers, comparing normalized factors with `Expression::Equals` and
       additive forms as multisets of signed atoms, so `(x+y)*(y+x)` is `POWER(x+y, 2)`;
     - compose data factors into the coefficient with `*` and never merge them, so SQL overflow behavior is unchanged;
     - never distribute over a sum.

     Any node other than `*`, a whole-constant POWER or data stays a whole term (`EXP(x)`, `POWER(x, 0.5)`, `x / y`,
     CASE), so the pass stays total. ABS, reducers and subqueries are factors that are never opened. Each rebuilt
     node is cast back to the type of the node it replaces, keeps its alias, and takes its `query_location`. The tree
     is rebuilt from the factor list in first-seen order, as a left-deep chain, so a second pass is a fixed point.
     `MergeLikeAtoms` keys on the normalized product without its constant, replacing `PeelBareDecisionFactors`' case
     list. PeelScale refuses a decision-bearing divisor as it does a multiplier. Optimizer rewrites that build a product
     use the product builder this stage exports.
  2. **Shape check.** `DecideShapeCheck` walks the canonical spelling once per clause and objective.
     - It records each clause's facts: degree, a decision-bearing term outside the IR's vocabulary, a data coefficient
       on a square, the sign of a square's coefficient, whether integer steps are legal.
     - It refuses, in one place:
       - a function or operator over a decision outside the vocabulary (ABS, reducers, norm/IN markers, powers of
         linear forms);
       - a POWER exponent that is not a whole constant;
       - division by a decision;
       - a decision-bearing CASE;
       - strict `<`, `>` or `<>` over a REAL decision, and a fractional operand against an integral decision;
       - degree above the highest any backend supports (2), with `ThrowDecideSolverGap`'s sentence.
     - Clauses the optimizer emits go through the same walk in verify mode beside `VerifyCanonical`.
     - Deleted:
       - `ValidatePowerExponent` and the exponent and base part of `ValidateQuadraticPower`;
       - the function and division branches of `ValidateDecideNoNonLinearScalar` and `ValidateSumArgumentInternal`;
       - the CASE refusal (`decide_binder.cpp:108`);
       - two of the three integer-step checks (`decide_binder.cpp:888-1207`, `decide_rewrite_scoped.cpp:160`);
       - `decide_degree.cpp`, except a small degree reader of the canonical form for `notequal_avg`, `define` and the
         splitter.
     - Kept in the binder:
       - a cast the user wrote;
       - an expression NULL whatever the decisions are;
       - a data aggregate times a decision;
       - its binding and "not yet" refusals.
  3. **Term splitter.**
     - One term kind: `{sign, coefficient (null = 1, never a decision), factors: (linear form, power)…, degree,
       opaque}`. `opaque` holds an ABS or reducer that direct solve reads before `RewriteAbs`.
     - Deleted: `DetectQuadraticPattern`, `ClassifyProduct`'s refusals, `UnsupportedProductFactor`, `square_scale`,
       and the reason and user-error fields.
     - Readers: R1 and S1 test `degree != 1`; singleton folding reads degree ≤ 1. The bilinear rewrite's `ToString`
       square detection goes.
  4. **One product group (stages 05 → 06 → 08).** `DecideProductGroup {left, right linear forms; square;
     weight_sign}`. A square is `l = w·f, r = f`; `w*x*y` is `l = w·x, r = y`.
     - It replaces the bilinear term structs, both `QuadraticGroup`s and the objective's `squared_terms`, through
       `solver_input.hpp`, `ilp_model_builder.cpp` and `physical_decide.cpp`.
     - The model builder runs one loop that symmetrizes `l·rᵀ`.
     - `ApplyScaleTo*` multiplies the scale into the coefficients, which deletes "a squared term cannot be multiplied
       by …" (`decide_linear_form.cpp:483-489`) and its plan-time fold.
  5. **Gate.** `decide_solver_gate.cpp` keeps only backend gaps: quadratic rows on HiGHS, convexity, MIQP, a singular
     Q.
     - Convexity is per group: a group is non-convex if it is not a square, or if its `weight_sign` (from
       `ScaleSignAtPlanTime`; 0 for data) fights the sense.
     - `HasCoupledQuadraticTerms` is judged per group.
     - `ClassifyDecideModel(const DecideModelShape &)` is the one classifier. The solver path's prepared model and
       direct solve's IR each fill a `DecideModelShape` from split terms (`AddTerm`). SOL-4 then reads it.
     - The diagnosis engines take `has_quadratic_obj` and `nonconvex_quadratic` from the per-group fact.
  6. **Direct solve.** A1 and N1 read a square through `DirectReadSquareTerm`, which takes a constant coefficient
     (`TryEvaluateFoldableDouble`) and declines a data one.
  7. **Contract.** The header and `done.md` § 1 state the rule that holds: every rule is an identity for every input
     (additive decomposition, moving terms, distributing a known factor over a sum, merging like terms, writing each
     product as a left coefficient times powers of its decision factors). The pass never multiplies out a power or a
     product of sums and never opens ABS, MIN/MAX or a subquery. It is total, pure, one spelling per meaning, and a
     fixed point. § 5 is corrected. `done.md` of stages 02, 05 and 07 and `language/errors.md` say where each refusal
     runs.
- **Done when.**
  - Each of `2*x*x`, `x*2*x`, `w*POWER(x,2)`, `w*x*x`, `POWER(x,2)*w` and `(x+y)*(y+x)` answers on Gurobi. Oracle cases
    cover them under a limit and under `MINIMIZE`.
  - `MINIMIZE SUM(3*x*x)` and `POWER(b*x, 2)` answer on forced HiGHS. A data weight on a square under forced HiGHS is
    refused by the gate.
  - `SUM(x*x*y) <= 10`, `POWER(x, 3)`, `EXP(x)`, `x / y` and a CASE over a decision are refused by the shape check,
    quoting the clause and underlining the term. A CASE over data alone answers.
  - Every refusal that moved refuses the same queries, with only its stage and class changed. Each changed test
    expectation is listed for the owner.
  - BUG-46 leaves `issues/bugs.md`. `language/functions.md`, `problem_types.md` and `errors.md` list the accepted forms
    and each refusal.
  - Suites: `make decide-test`; `DECIDB_VERIFY_SERIALIZER=1`; `DECIDB_FORCE_SOLVER=highs`;
    `DECIDB_TEST_DIRECT_SOLVE=off`; `unittest "[decidb]"`.
