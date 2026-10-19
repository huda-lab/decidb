# Glossary

One name per concept. Each row gives the name direct solve uses, the standard idea it is, where that idea comes from, and
the identifiers it covers. When a name here and a name in the code differ, the code is behind and this page wins.

Conventions that apply to every row:

- Free functions and types in `namespace duckdb` keep the `Direct` prefix, as DuckDB names a feature inside its one
  namespace. Inside a rule's own namespace (`direct_s1_uniform_matroid`, `direct_a1_separable_box`) names carry no
  prefix and no class id: the namespace already says both.
- A problem class keeps its id (A1, S1, ...) as a prefix and adds the textbook name of its **problem**, not of the
  algorithm that solves it. Where operations research and databases disagree, the optimization term wins.
- User-visible text follows the names: `EXPLAIN` keys and values, `require` reasons, the syntax reference and the
  website.

## The feature

| Name | Standard idea | Source | Covers |
|---|---|---|---|
| direct solve | A rule-based rewrite of a DECIDE query into a relational plan that computes the optimum: a problem-specific algorithm, expressed as a plan transformation | Operations research (specialized algorithms); query optimizers (rule-based rewrite) | `decide_direct_solve`, `EXPLAIN` key prefix `Direct solve`, `Direct*` identifiers, `08_direct_solve/` |
| problem class | A problem with a known specialized algorithm; one rule implements one class | Optimization texts | `rules/<id>_<slug>/` |

## Intermediate representation (front end)

| Name | Standard idea | Source | Covers |
|---|---|---|---|
| IR | Intermediate representation: a neutral form of the program that later passes read instead of the source tree | Compiler texts (front end, IR, back end) | `DirectIR`, `DirectIRDecision`, `DirectIRConstraint`, `DirectIRObjective`, `DirectIRScope`, `DirectIREntityScope`, `FindDirectIRInput`; variables `ir`, `ir_node`; test helpers `QueryIR`, `IRQuery`; `direct_ir.cpp`; `architecture/ir.md` |
| front end | The pass that reads the source tree and builds the IR; the only code that knows `LogicalDecide`'s layout | Compiler texts | `DirectFrontEnd` |
| conservative analysis | An analysis that reports only what it can prove, and "not proved" for anything else, so a wrong rewrite cannot follow from it | Static analysis (soundness) | `DirectAnalysisStatus` with `PROVED` / `NOT_PROVED`; `FirstNotProvedReason`; `MarkNotProved`; `require` reasons `constraints_not_proved:` and `objective_not_proved:`. Not the canonicalizer's `DecideTermKind::UNKNOWN` |
| reducer | An aggregate function (`SUM`, `AVG`, `MIN`, `MAX`, `COUNT`) applied over DECIDE variables; DECIDE's name for it, so it is never confused with a SQL aggregate over data | DeciDB term | `DirectReducer`, the S1 rule's `Reducer`, `QUALIFIED_REDUCER_TAG` (`__qualified_reducer__`), `ContainsReducer`, `ReadReducer`, `ClassifyReducerCall`, `EvaluateRhsReducerPerGroup`, `04_testing/qualified_reducer/` |
| singleton-row bound | A constraint on one variable, folded into that variable's bounds before the model is built | Presolve (Andersen & Andersen 1995; Achterberg et al. 2020) | `FoldSingletonRows`, `DecideFoldSingletonRow`, `DecideIsSingletonRowBound`, `FoldSingletonRowsInExpression`, `SingletonRowBound`, `TryReadSingletonRowBound`, `singleton_row_boxes`, `user_singleton_row_bounds`, `SINGLETON_ROW_BOUND_TAG`, phase `optimizer.fold_singleton_rows` |
| fixed variable | A decision whose value the problem fixes before optimizing | Optimization texts | `ProveFixedVariables`, `FixedTerms`, `FixedDomain`, `ConstantFixedDomain`, `SourceFixedValue`, `SourceBooleanFixedValue` |

## Rule framework

The rule interface is a **Strategy**; the registry is a **registry with a Factory Method** per rule; the rule driver
runs the same fixed steps for every rule, a **Template Method**. The vocabulary is Cascades' with one merge: the
pattern step and the condition step are one, because a DECIDE query has a single candidate node to match.

| Name | Standard idea | Source | Covers |
|---|---|---|---|
| rule | A transformation rule: a Strategy that checks its applicability condition and applies its substitute | Volcano, Cascades; GoF Strategy | `DirectSolveRule` (two methods: `CheckCondition`, `Apply`) and `Summary()` |
| applicability condition | What a rule must establish before it fires. Here it includes optimality, which a Cascades condition does not require | Cascades | `DirectApplicabilityCondition`; `Condition` in each rule's namespace; `EXPLAIN` key `Direct solve condition` |
| substitute | The plan a rule returns in place of the matched node | Cascades | `DirectSubstitute` |
| applies / does not apply | Whether a rule's condition holds for this query | Optimizer texts | comments, docs, `policy.md` |
| rule driver | The loop that offers the node to each rule, requires at most one to apply, and installs the substitute | Optimizer texts | `direct_rule_driver.cpp`, `ApplyDirectSolveRules` |
| registry | The one list of rules, each built by its factory method | Fowler (Registry); GoF Factory Method | `direct_registry.cpp` |
| test seam | A point where a test substitutes its own rules without editing the code under test | Feathers, *Working Effectively with Legacy Code* | `DirectRuleTestSeam`, `DIRECT_RULE_TEST_SEAM_KEY` |
| plan builder | An object that assembles a rule's plan step by step | GoF Builder | `PlanBuilder` in each rule's namespace |
| summary | The one line a rule contributes to the `EXPLAIN` annotation | none (plain word) | `Summary` |
| rule-local helpers | Helpers only one rule uses; they move to the plan factory functions when a second rule needs them | none (plain word) | `s1_uniform_matroid_helpers.{hpp,cpp}`, `direct_s1_uniform_matroid::ProjectScope` |

## Plan and output (back end)

| Name | Standard idea | Source | Covers |
|---|---|---|---|
| plan factory functions | Free functions that each return one plan fragment | GoF Factory Method (function form) | `direct_plan_factory.{hpp,cpp}` |
| expression builder | A small fluent helper that builds bound expressions | Fowler (Expression Builder) | `ExpressionBuilder`, `ExprPtr` |
| pipeline breaker | A blocking operator that reads every input row before it emits one; here it validates the inputs the solver path would reject | DuckDB, HyPer (pipeline breakers) | `DirectValidationBreaker`, `DirectPipelineBreaker`, field `breaker`, `validated_columns`, `pipelined` |
| late materialization | Read the narrow columns first and fetch the wide ones by row id after the plan has chosen its rows. Distinct from DuckDB's `LATE_MATERIALIZATION` pass, and it runs inside `UNUSED_COLUMNS` | Column-store texts; DuckDB | `DirectLateMaterialization`, `late_materialization`, `direct_late_materialization.{hpp,cpp}`, `ApplyDirectLateMaterialization`, `LateMaterializeOneAdapter`, `DirectExposeStoredRowId` |
| result adapter | The operator that presents the substitute under the DECIDE node's bindings (GoF Adapter) and stops parent filters and limits from crossing it (an optimization fence) | GoF Adapter; optimizer texts (optimization fence) | `LogicalDirectResultAdapter`, `EXPLAIN` operator `DIRECT_RESULT_ADAPTER`, `AdaptDirectResult`, `DirectResultAdapterExtension`, `MakeDirectResultAdapterExtension`, `direct_result_adapter.{hpp,cpp}`, `slot_map`, `architecture/result_adapter.md` |
| column pruning | Dropping columns no parent reads | Optimizer texts; DuckDB `RemoveUnusedColumns` | `prunable_outputs`, `prunable_sources`, `prunable_decisions` |
| `EXPLAIN` annotation | The keys direct solve adds to the plan's `EXPLAIN` output: mode, rule, condition | DuckDB (`ParamsToString`) | `DirectExplainAnnotation`, field `annotation`, `Render` |

## Problem classes

The slug names the folder (`rules/<id>_<slug>/`), the rule's files and namespace (`<id>_<slug>_rule.cpp`,
`direct_<id>_<slug>`), the Python differential test (`test_direct_<id>_<slug>.py`) and, upper-cased, the `EXPLAIN`
value. Classes without code take the name in docs only until their code exists.

| Id | Textbook name | Slug | `EXPLAIN` value | Source |
|---|---|---|---|---|
| A1 | separable box-constrained problem | `separable_box` | `A1_SEPARABLE_BOX` | Optimization texts |
| A2 | single-facility location (1-D) | `single_facility_location` | `A2_SINGLE_FACILITY_LOCATION` | Location theory |
| A3 | multilinear optimization over a box | `multilinear_box` | `A3_MULTILINEAR_BOX` | Rosenberg 1975 |
| A4 | bounded-variable least squares | `bounded_least_squares` | `A4_BOUNDED_LEAST_SQUARES` | Stark and Parker 1995 (BVLS) |
| A5 | monotone optimization | `monotone` | `A5_MONOTONE` | Tuy 2000 |
| A6 | rank-one bounded least squares | `rank_one_least_squares` | `A6_RANK_ONE_LEAST_SQUARES` | Descriptive; no single textbook name |
| N1 | Euclidean-ball projection and support function | `euclidean_ball` | `N1_EUCLIDEAN_BALL` | Convex analysis |
| N2 | ellipsoid support function | `ellipsoid_support` | `N2_ELLIPSOID_SUPPORT` | Convex analysis |
| S1 | uniform-matroid optimization | `uniform_matroid` | `S1_UNIFORM_MATROID` | Matroid theory |
| S2 | laminar-matroid optimization | `laminar_matroid` | `S2_LAMINAR_MATROID` | Matroid theory |
| S3 | maximum-cardinality knapsack | `max_cardinality_knapsack` | `S3_MAX_CARDINALITY_KNAPSACK` | Knapsack texts |
| S4 | cardinality-constrained bottleneck knapsack | `bottleneck_knapsack` | `S4_BOTTLENECK_KNAPSACK` | Bottleneck optimization; no single textbook name |
| R1 | continuous (fractional) knapsack | `continuous_knapsack` | `R1_CONTINUOUS_KNAPSACK` | Dantzig 1957 |
| R2 | discrete separable convex resource allocation | `discrete_resource_allocation` | `R2_DISCRETE_RESOURCE_ALLOCATION` | Ibaraki and Katoh 1988 |
| R3 | continuous quadratic knapsack | `quadratic_knapsack` | `R3_QUADRATIC_KNAPSACK` | Brucker 1984 |
| R4 | weighted max-min fair allocation | `max_min_fairness` | `R4_MAX_MIN_FAIRNESS` | Bertsekas and Gallager (networking) |
| O1 | Monge assignment | `monge_assignment` | `O1_MONGE_ASSIGNMENT` | Burkard, Klinz and Rudolf 1996 |
| O2 | Monge transportation problem | `monge_transportation` | `O2_MONGE_TRANSPORTATION` | Hoffman 1963 |
| D1 | block-diagonal decomposition | `block_decomposition` | `D1_BLOCK_DECOMPOSITION` | Decomposition texts |

## Tests

| Name | Standard idea | Source | Covers |
|---|---|---|---|
| differential oracle test | The same case run on an independent oracle, the solver path and the direct path; all must agree | Differential testing (McKeeman 1998); test oracles | `test_direct_<id>_<slug>.py` per rule; `_direct_differential.py` |
| conformance suite | One suite every rule must pass, fed by a fixture per rule | Conformance testing | `test_direct_rule_conformance.py`; `_direct_rule_fixtures.py` |
| test matrix | The checks run under every setting combination | Standard | `test/decide/run_direct_matrix.sh` |
| C++ regressions | Unit tests of the IR and the rule driver | none | `test_decidb_direct_ir.cpp`, `test_decidb_direct_rule_driver.cpp` |

## Names that do not follow the glossary

A few names stay as they are because renaming them would break something or because the word means something else there.

- **Serialized names.** The plan serializer's property `"proof"` (id 209, `direct_result_adapter.cpp`; the field behind it
  is `condition`), the extension name `decidb_direct_solve_result_v3` (`DIRECT_RESULT_EXTENSION`), and the parser tag
  string `__qualified_reducer__` (`QUALIFIED_REDUCER_TAG`), which a saved view stores. Renaming any of them would stop
  older plans or database files from reading back.
- **Oracle cache keys.** A Python test's cached oracle answer is keyed on its node id and its function's source, so
  outside the direct-solve suites some test function and class names, parameter ids, and strings inside oracle test
  bodies keep older wording. In `test_direct_user_facing.py` the same holds for
  `test_a1_unused_wide_columns_stay_out_of_the_barrier_and_computed_errors_stay_in` and the four `test_second_read_*`
  tests. Changing any of them means re-solving those tests with Gurobi.
- **Benchmark flags.** `--pins` and the JSON key `pins` in `benchmark/decide/profile_direct_s1_api.*`, because the
  measurement commands in `rules/s1_uniform_matroid/done.md` use them.
- **Other meanings.** DuckDB's aggregate-state `Absorb`; `AbsorbAdditive` (`decide_degree.cpp`, merging a child's degree
  into its parent); `DecideTermKind::UNKNOWN`; "pin" (a test pins `require`) and "boundary" in their everyday senses;
  "near miss" (a testing term); a cache "miss"; "proof" and "prove" in the mathematical sense and in the `Prove*` helpers
  that certify one part of the IR; the variable name `calc` for an `ExpressionBuilder`.
