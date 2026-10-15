# Glossary

One name per concept. Each row gives the name direct solve uses, the standard idea it is, where that idea comes from,
and the identifiers it covers. The **Retired** column lists the old words and identifiers; the final sweep greps for
every one of them. When a name here and a name in the code differ, the code is behind and this page wins.

Conventions that apply to every row:

- Free functions and types in `namespace duckdb` keep the `Direct` prefix, as DuckDB names a feature inside its one
  namespace. Inside a rule's own namespace (`direct_s1_uniform_matroid`, `direct_a1_separable_box`) names carry no
  prefix and no class id: the namespace already says both.
- A problem class keeps its id (A1, S1, ...) as a prefix and adds the textbook name of its **problem**, not of the
  algorithm that solves it. Where operations research and databases disagree, the optimization term wins.
- User-visible text follows the names: `EXPLAIN` keys and values, `require` reasons, the syntax reference and the
  website. The serialized extension name (`decidb_direct_solve_result_v3`, constant `DIRECT_RESULT_EXTENSION`) stays, so a
  stored plan still reads back.

## The feature

| Name | Standard idea | Source | Covers | Retired |
|---|---|---|---|---|
| direct solve | A rule-based rewrite of a DECIDE query into a relational plan that computes the optimum: a problem-specific algorithm, expressed as a plan transformation | Operations research (specialized algorithms); query optimizers (rule-based rewrite) | `decide_direct_solve`, `EXPLAIN` key prefix `Direct solve`, `Direct*` identifiers, `08_direct_solve/` | none |
| problem class | A problem with a known specialized algorithm; one rule implements one class | Optimization texts | `rules/<id>_<slug>/` | "class" and "rule" used for each other |

## Intermediate representation (front end)

| Name | Standard idea | Source | Covers | Retired |
|---|---|---|---|---|
| IR | Intermediate representation: a neutral form of the program that later passes read instead of the source tree | Compiler texts (front end, IR, back end) | `DirectProblemFacts` → `DirectIR`; `DirectDecisionFact` → `DirectIRDecision`; `DirectConstraintFact` → `DirectIRConstraint`; `DirectObjectiveFact` → `DirectIRObjective`; `DirectScopeFact` → `DirectIRScope`; `DirectEntityScopeFact` → `DirectIREntityScope`; `FindDirectFactsInput` → `FindDirectIRInput`; variables `facts` → `ir`, `fact` → `ir_node`; test helpers `QueryFacts` → `QueryIR`, `FactsQuery` → `IRQuery`; `direct_problem.cpp` → `direct_ir.cpp`; `architecture/facts.md` → `architecture/ir.md` | facts, fact, `*Fact`, `Facts` |
| front end | The pass that reads the source tree and builds the IR; the only code that knows `LogicalDecide`'s layout | Compiler texts | `FactReader` → `DirectFrontEnd` | fact reader, adapter (for the second front end) |
| conservative analysis | An analysis that reports only what it can prove, and "not proved" for anything else, so a wrong rewrite cannot follow from it | Static analysis (soundness) | `DirectFactStatus` → `DirectAnalysisStatus` with `PROVED` / `NOT_PROVED`; `FirstUnknownReason` → `FirstNotProvedReason`; `MarkUnknown` → `MarkNotProved`; `require` reasons `constraint_facts_unknown:` → `constraints_not_proved:`, `objective_facts_unknown:` → `objective_not_proved:`. Not the canonicalizer's `DecideTermKind::UNKNOWN` | unknown fails closed, `KNOWN`, `UNKNOWN` |
| aggregate | An aggregate function call over decision expressions (`SUM`, `AVG`, `MIN`, `MAX`, `COUNT`) | DuckDB (`BoundAggregateExpression`), SQL | `DirectReducer` → `DirectAggregate`; `S1Reducer` → `Aggregate` (in its namespace); `QUALIFIED_REDUCER_TAG` → `QUALIFIED_AGGREGATE_TAG` (`__qualified_aggregate__`); `ContainsReducer`, `ReadReducer`, `ClassifyReducerCall`, `IsIntegerValuedReducer`, `UnwrapQualifiedReducer`, `TryParseQualifiedReducerTag`, `EvaluateRhsReducerPerGroup`, `rhs_reducers`, `reducers_needed`, `reducer_result`, `reducer_values`, `ReducerProblem` → the same names with `Aggregate`; `04_testing/qualified_reducer/` → `04_testing/qualified_aggregate/`; `require` reasons and docs that say "reducer". Not DuckDB's own `ExpressionDepthReducer` | reducer, reducers |
| singleton-row bound | A constraint on one variable, folded into that variable's bounds before the model is built | Presolve (Andersen & Andersen 1995; Achterberg et al. 2020) | `AbsorbVariableBounds` → `FoldSingletonRows`; `DecideAbsorbBound` → `DecideFoldSingletonRow`; `DecideBoundIsAbsorbed` → `DecideIsSingletonRowBound`; `AbsorbBoundsInExpression` → `FoldSingletonRowsInExpression`; `AbsorbableBound`, `TryReadAbsorbableBound` → `SingletonRowBound`, `TryReadSingletonRowBound`; `absorbed_boxes` → `singleton_row_boxes`; `user_absorbed_bounds` → `user_singleton_row_bounds`; `ABSORBED_BOUND_TAG` → `SINGLETON_ROW_BOUND_TAG`; phase `optimizer.absorb_bounds` → `optimizer.fold_singleton_rows` | absorbed bound, absorb, absorbable |
| fixed variable | A decision whose value the problem fixes before optimizing | Optimization texts | `ProvePins` → `ProveFixedVariables`; `PinTerms` → `FixedTerms`; `PinDomain` → `FixedDomain`; `ConstantPinDomain` → `ConstantFixedDomain`; `SourcePin` → `SourceFixedValue`; `SourceBooleanPinValue` → `SourceBooleanFixedValue` | pin, pins, pinned |

## Rule framework

The rule interface is a **Strategy**; the registry is a **registry with a Factory Method** per rule; the rule driver
runs the same fixed steps for every rule, a **Template Method**. The vocabulary is Cascades' with one merge: the
pattern step and the condition step are one, because a DECIDE query has a single candidate node to match.

| Name | Standard idea | Source | Covers | Retired |
|---|---|---|---|---|
| rule | A transformation rule: a Strategy that checks its applicability condition and applies its substitute | Volcano, Cascades; GoF Strategy | `DirectSolveRule` (two methods: `CheckCondition`, `Apply`) and `Summary()` | Match, Prove, Rewrite, Explain as rule steps; `DirectRuleMatch`, `S1Match`, `A1Match` |
| applicability condition | What a rule must establish before it fires. Here it includes optimality, which a Cascades condition does not require | Cascades | `DirectRuleProof` → `DirectApplicabilityCondition`; `S1Proof`, `A1Proof` → `Condition` (in their namespaces); `EXPLAIN` key `Direct solve proof` → `Direct solve condition` | proof (as a type or key) |
| substitute | The plan a rule returns in place of the matched node | Cascades | `DirectRelationalProposal` → `DirectSubstitute` | proposal |
| applies / does not apply | Whether a rule's condition holds for this query | Optimizer texts | comments, docs, `policy.md` | hit, miss |
| rule driver | The loop that offers the node to each rule, requires at most one to apply, and installs the substitute | Optimizer texts | `direct_coordinator.cpp` → `direct_rule_driver.cpp`; `TryDirectSolve` → `ApplyDirectSolveRules` | coordinator |
| registry | The one list of rules, each built by its factory method | Fowler (Registry); GoF Factory Method | `direct_registry.cpp` (name kept) | none |
| test seam | A point where a test substitutes its own rules without editing the code under test | Feathers, *Working Effectively with Legacy Code* | `DirectRuleOverride` → `DirectRuleTestSeam`; `DIRECT_RULE_OVERRIDE_KEY` → `DIRECT_RULE_TEST_SEAM_KEY` | override |
| plan builder | An object that assembles a rule's plan step by step | GoF Builder | `S1Rewriter`, `A1Rewriter` → `PlanBuilder` (in their namespaces) | rewriter |
| summary | The one line a rule contributes to the `EXPLAIN` annotation | none (plain word) | `Explain` → `Summary` | rule-level Explain |
| rule-local helpers | Helpers only one rule uses; they move to the plan factory functions when a second rule needs them | none (plain word) | `s1_support.{hpp,cpp}` → `s1_uniform_matroid_helpers.{hpp,cpp}`; `direct_s1::DirectProjectScope` → `direct_s1_uniform_matroid::ProjectScope`, likewise every `Direct*` name in a rule namespace | support, double prefix |

## Plan and output (back end)

| Name | Standard idea | Source | Covers | Retired |
|---|---|---|---|---|
| plan factory functions | Free functions that each return one plan fragment | GoF Factory Method (function form) | `direct_builder.{hpp,cpp}` → `direct_plan_factory.{hpp,cpp}` | builders (not GoF Builders) |
| expression builder | A small fluent helper that builds bound expressions | Fowler (Expression Builder) | `Calc` → `ExpressionBuilder`; `Ex` → `ExprPtr` | `Calc`, `Ex` |
| pipeline breaker | A blocking operator that reads every input row before it emits one; here it validates the inputs the solver path would reject | DuckDB, HyPer (pipeline breakers) | `DirectValidationBarrier` → `DirectValidationBreaker`; `DirectBarrier` → `DirectPipelineBreaker`; `barrier` field → `breaker`; `validation_slots` → `validated_columns`; `streams_rows` → `pipelined`; the internal-error text | barrier, validation barrier, streams |
| late materialization | Read the narrow columns first and fetch the wide ones by row id after the plan has chosen its rows. Distinct from DuckDB's `LATE_MATERIALIZATION` pass, and it runs inside `UNUSED_COLUMNS` | Column-store texts; DuckDB | `DirectSecondRead` → `DirectLateMaterialization`; `second_read` → `late_materialization`; `direct_second_read.{hpp,cpp}` → `direct_late_materialization.{hpp,cpp}`; `ApplyDirectSecondRead` → `ApplyDirectLateMaterialization`; `SecondReadOneBoundary` → `LateMaterializeOneAdapter`; `DirectExposeStoredRowId` (name kept: "row id" is DuckDB's term) | second read |
| result adapter | The operator that presents the substitute under the DECIDE node's bindings (GoF Adapter) and stops parent filters and limits from crossing it (an optimization fence) | GoF Adapter; optimizer texts (optimization fence) | `LogicalDirectSolveResult` → `LogicalDirectResultAdapter`, its `EXPLAIN` operator name `DIRECT_SOLVE_RESULT` → `DIRECT_RESULT_ADAPTER`; `MapDirectResult` → `AdaptDirectResult`; `DirectResultExtension` → `DirectResultAdapterExtension`; `MakeDirectResultExtension` → `MakeDirectResultAdapterExtension`; `direct_result_boundary.{hpp,cpp}` → `direct_result_adapter.{hpp,cpp}`; `output_slots` → `slot_map`; `architecture/result_boundary.md` → `architecture/result_adapter.md` | result boundary, boundary, output slots |
| column pruning | Dropping columns no parent reads | Optimizer texts; DuckDB `RemoveUnusedColumns` | `prunable_outputs`, `prunable_sources`, `prunable_decisions` (names kept: already standard) | none |
| `EXPLAIN` annotation | The keys direct solve adds to the plan's `EXPLAIN` output: mode, rule, condition | DuckDB (`ParamsToString`) | `DirectSolveDecisionRecord` → `DirectExplainAnnotation`; `record` fields → `annotation`; `Render` (name kept) | decision record (means an architecture decision record elsewhere) |

## Problem classes

The slug names the folder (`rules/<id>_<slug>/`), the rule's files and namespace (`<id>_<slug>_rule.cpp`,
`direct_<id>_<slug>`), the Python differential test (`test_direct_<id>_<slug>.py`) and, upper-cased, the `EXPLAIN`
value. Classes without code take the name in docs only until their code exists.

| Id | Textbook name | Slug | `EXPLAIN` value | Source | Retired |
|---|---|---|---|---|---|
| A1 | separable box-constrained problem | `separable_box` | `A1_SEPARABLE_BOX` | Optimization texts | Independent bounded decisions; `A1_INDEPENDENT_BOUNDS`; `A1BoundedRule`; `direct_a1`; `a1_rule.{hpp,cpp}`; `test_direct_a1.py` |
| A2 | single-facility location (1-D) | `single_facility_location` | `A2_SINGLE_FACILITY_LOCATION` | Location theory | One shared mean, median, midpoint, or mode |
| A3 | multilinear optimization over a box | `multilinear_box` | `A3_MULTILINEAR_BOX` | Rosenberg 1975 | Fixed-small multi-affine box |
| A4 | bounded-variable least squares | `bounded_least_squares` | `A4_BOUNDED_LEAST_SQUARES` | Stark and Parker 1995 (BVLS) | Bounded two-parameter least squares |
| A5 | monotone optimization | `monotone` | `A5_MONOTONE` | Tuy 2000 | Objective-aligned monotone corner |
| A6 | rank-one bounded least squares | `rank_one_least_squares` | `A6_RANK_ONE_LEAST_SQUARES` | Descriptive; no single textbook name | One squared affine residual over a box |
| N1 | Euclidean-ball projection and support function | `euclidean_ball` | `N1_EUCLIDEAN_BALL` | Convex analysis | Projection or linear support under one sum-of-squares bound |
| N2 | ellipsoid support function | `ellipsoid_support` | `N2_ELLIPSOID_SUPPORT` | Convex analysis | Linear support over a diagonal ellipsoid |
| S1 | uniform-matroid optimization | `uniform_matroid` | `S1_UNIFORM_MATROID` | Matroid theory | Top-k and cardinality intervals; `S1_CARDINALITY_INTERVAL`; `S1CardinalityRule`; `direct_s1`; `s1_rule.{hpp,cpp}`; `test_direct_three_way.py` |
| S2 | laminar-matroid optimization | `laminar_matroid` | `S2_LAMINAR_MATROID` | Matroid theory | Nested upper quotas |
| S3 | maximum-cardinality knapsack | `max_cardinality_knapsack` | `S3_MAX_CARDINALITY_KNAPSACK` | Knapsack texts | Maximum item count under one budget |
| S4 | cardinality-constrained bottleneck knapsack | `bottleneck_knapsack` | `S4_BOTTLENECK_KNAPSACK` | Bottleneck optimization; no single textbook name | Exact count and budget, maximizing the best selected item |
| R1 | continuous (fractional) knapsack | `continuous_knapsack` | `R1_CONTINUOUS_KNAPSACK` | Dantzig 1957 | Continuous piecewise-linear allocation |
| R2 | discrete separable convex resource allocation | `discrete_resource_allocation` | `R2_DISCRETE_RESOURCE_ALLOCATION` | Ibaraki and Katoh 1988 | Bounded integer marginal units |
| R3 | continuous quadratic knapsack | `quadratic_knapsack` | `R3_QUADRATIC_KNAPSACK` | Brucker 1984 | Strictly convex quadratic allocation |
| R4 | weighted max-min fair allocation | `max_min_fairness` | `R4_MAX_MIN_FAIRNESS` | Bertsekas and Gallager (networking) | Proportional max-min fairness |
| O1 | Monge assignment | `monge_assignment` | `O1_MONGE_ASSIGNMENT` | Burkard, Klinz and Rudolf 1996 | Ordered complete assignment |
| O2 | Monge transportation problem | `monge_transportation` | `O2_MONGE_TRANSPORTATION` | Hoffman 1963 | Balanced one-dimensional Monge transport |
| D1 | block-diagonal decomposition | `block_decomposition` | `D1_BLOCK_DECOMPOSITION` | Decomposition texts | Exact keyed decomposition |

## Tests

| Name | Standard idea | Source | Covers | Retired |
|---|---|---|---|---|
| differential oracle test | The same case run on an independent oracle, the solver path and the direct path; all must agree | Differential testing (McKeeman 1998); test oracles | `test_direct_<id>_<slug>.py` per rule; `_direct_differential.py` (name kept) | three-way table, `test_direct_three_way.py` |
| conformance suite | One suite every rule must pass, fed by a fixture per rule | Conformance testing | `test_direct_rule_contract.py` → `test_direct_rule_conformance.py`; `_direct_rule_fixtures.py` (name kept) | contract suite |
| test matrix | The checks run under every setting combination | Standard | `test/decide/run_direct_matrix.sh` (name kept) | none |
| C++ regressions | Unit tests of the IR and the rule driver | none | `test_decidb_direct_facts.cpp` → `test_decidb_direct_ir.cpp`; `test_decidb_direct_coordinator.cpp` → `test_decidb_direct_rule_driver.cpp` | facts, coordinator in test names |
