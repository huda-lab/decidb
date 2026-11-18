#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_tree_terms.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"
#include "duckdb/execution/operator/decide/decide_debug.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/solver/solver_registry.hpp"
#include <unordered_map>

namespace duckdb {

//! Global sink state accumulated by PhysicalDecide::Sink/Combine; defined in
//! physical_decide.cpp. Finalize's private step methods take it by reference.
class DecideGlobalSinkState;

class DecideDescent;

//! The time-limit report's closeness figure: `gap`, a fraction, as a percentage with two significant digits.
string FormatDecideGapPercent(double gap);

//===--------------------------------------------------------------------===//
// PhysicalDecide Operator
//===--------------------------------------------------------------------===//

//! PhysicalDecide represents a blocking operator that solves an ILP over its
//! entire input before producing output.
class PhysicalDecide : public PhysicalOperator {
public:
	//! Semantic E/G/O graph, captured before the optimizer lowers frames.
	unique_ptr<DecideTree> tree;
	//! `SET decide_debug`, read once when the operator is planned.
	DecideDebug debug;
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::DECIDE;
	//! The tree's back end, which records the variants it numbers (RecordVariants).
	friend class DecideModelEmitter;

public:
	PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality, unique_ptr<PhysicalOperator> child,
	               idx_t decide_index, vector<unique_ptr<Expression>> decide_variables,
	               unique_ptr<Expression> decide_constraints, DecideSense written_sense,
	               unique_ptr<Expression> decide_objective);

	// The table index for the new columns
	idx_t decide_index;

	// The variables to be decided (e.g., x, y)
	vector<unique_ptr<Expression>> decide_variables;

	// The bound constraints expression
	unique_ptr<Expression> decide_constraints;

	//! Stable source display registry copied from LogicalDecide.
	vector<ConstraintSourceInfo> constraint_sources;

	//! The objective as written and as canonicalized, copied from LogicalDecide so the
	//! physical plan renders the same layers the logical one does.
	string written_objective;
	string canonical_objective;
	bool objective_canonical_differs = false;

	//! The DECIDE tail as the user wrote it, copied from LogicalDecide.
	DecideSourceText source_text;

	//! The direction the objective was written in, copied from LogicalDecide. Only a message, EXPLAIN and a shown value
	//! read it: `decide_objective` is a minimization.
	DecideSense written_sense;

	//! The bound objective function expression, minimized.
	unique_ptr<Expression> decide_objective;

	//! The constant canonicalization peeled off the objective (LogicalDecide::objective_constant_offset), so the
	//! objective value a solve reports is the minimized objective's (the negation of a MAXIMIZE query's).
	double objective_constant_offset = 0.0;

	// Number of auxiliary variables (e.g. from ABS linearization) at the end of decide_variables
	idx_t num_auxiliary_vars = 0;

	// Per-variable flag: true if the variable was declared IS BOOLEAN (copied from
	// LogicalDecide). A BOOLEAN variable is lowered to an INTEGER with a 0/1 domain
	// (synthesized `x >= 0` / `x <= 1` constraints), so its runtime type is INTEGER;
	// this flag is the only surviving signal that the 0/1 box is the variable's
	// intrinsic domain, not a user-editable bound. Indexed by decide_variables position.
	vector<bool> is_boolean_var;

	// The source clause (index into constraint_sources) of each not-equal (<>) constraint
	vector<idx_t> ne_clause_sources;

	// Links from MIN/MAX indicator variables: (agg_name "min"/"max", indicator_idx)
	vector<string> minmax_clause_labels;

	// F6: auxiliary variable index -> user's original source expression string
	// (ABS(...) / MIN/MAX(...) / product / <>), for naming an escaping aux column
	// in the unbounded diagnosis. Sparse; only auxiliary variables appear.
	vector<pair<idx_t, string>> aux_var_expressions;

	//! What each rewrite made of a clause, for EXPLAIN (LogicalDecide::rewrite_accounts).
	vector<LogicalDecide::RewriteAccount> rewrite_accounts;
	//! What the data made of each clause (Kind::RUN_TIME): the variants of its classes (D70, D71), the instances it
	//! skipped for having no value (deck pp. 50, 56). Written by Finalize, read by ParamsToString, which the profiler
	//! calls again once the result is read -- so EXPLAIN ANALYZE prints it, and a plain EXPLAIN, which runs nothing,
	//! does not. Run-time facts of the one execution, hence mutable.
	mutable vector<LogicalDecide::RewriteAccount> run_time_accounts;

	// --- Singleton-row variable bounds (decided by DecideOptimizer::FoldSingletonRows) ---
	//
	// The decision column box, already resolved upstream. Finalize copies these into
	// SolverInput; comparisons folded into them carry SINGLETON_ROW_BOUND_TAG so term
	// extraction skips them. Execution consumes this, it does not re-derive it.
	vector<DecideBoundBox> singleton_row_boxes;
	// Every folded user bound, kept so the infeasible diagnosis can open its column box and
	// quote it as written; its clause reaches the repair model through
	// its folded root's clause (DecideNode::folded). A BETWEEN contributes two entries.
	vector<LogicalDecide::UserBoundSpec> user_singleton_row_bounds;
	vector<LogicalDecide::UserBoundSpec> member_singleton_row_bounds;

	// --- MIN/MAX objective metadata (set by DecideOptimizer::RewriteMinMaxObjective) ---

	// A MIN/MAX objective: original aggregate type before rewrite to SUM
	ObjectiveAggregateType flat_objective_agg = ObjectiveAggregateType::NONE;
	// Pre-computed: true if easy formulation (MAXIMIZE+MIN or MINIMIZE+MAX)
	bool flat_objective_is_easy = false;

	// --- Key scopes ---

	//! Every key scope: of a `per K:` decision, a reducer's `per K:` or colon-form
	//! qualifier, and the value rewrite's helper decisions
	vector<KeyedScopeInfo> keyed_scopes;

	//! Per-variable scope assignment (row / keyed / query-wide);
	//! keyed_scope_idx indexes keyed_scopes
	vector<DecideVarScopeInfo> variable_scopes;

	//! Every key of the clause (LogicalDecide::keys), its expressions resolved against the
	//! input chunk.
	vector<DecideKey> keys;

	//! The frame selectors (LogicalDecide::frames), their expressions resolved against the
	//! input chunk.
	vector<DecideFrameSpec> frames;

	//! The run-time checks of the values the bounds read, which a plan under the node has made by now.
	vector<DecideScopeCheck> scope_checks;
	//! The variant signatures (LogicalDecide::signatures), their expressions resolved against the input chunk, and the
	//! objective's, INVALID_INDEX for none.
	vector<DecideSignature> signatures;
	idx_t objective_signature = DConstants::INVALID_INDEX;

	//! Source column name per physical child-output column (positionally aligned
	//! with gstate.data columns), resolved post-pruning in plan_decide.cpp. Each name
	//! is SQL, quoted where it needs it, so a remedy that prints one can be pasted
	//! back. Used by the unbounded diagnosis to label escaping categorical groups
	//! (affected_rows).
	vector<string> input_column_names;

	//! Positionally aligned with `input_column_names`: true when the DECIDE clause
	//! itself references the column (a `when`, a key, a constraint, the objective), false when
	//! it only rides along in the outer SELECT. The unbounded characterization uses it
	//! to pick a representative when several columns describe the same escaping rows:
	//! a column the user put in the clause explains the escape, one that merely
	//! correlates with it is a coincidence.
	vector<bool> input_column_in_clause;

	//! The `DIAGNOSE` prefix the user wrote, carried down from LogicalDecide::diagnose.
	//! It is the ONLY thing that arms the diagnosis engines. When true a failed solve
	//! stashes its findings for the DIAGNOSE operator above to return as rows instead of
	//! raising; when false — every unprefixed query — a failed solve reports its status
	//! and stops, and none of the diagnostic prep (unbounded-ray extraction, model
	//! retention, the elastic re-solves) is done or paid for.
	bool diagnose = false;

	//! The registered NAME of the backend this query was planned for, chosen once by
	//! stage 05 (LogicalDecide::solver_backend_name) and copied here at physical
	//! planning. Layer 8 READS it — the primary solve and every diagnostic re-solve run
	//! on this one backend — and never re-derives it: the rewrites upstream already
	//! committed to what this backend takes natively, so a second, differently-answered
	//! selection would run a model on a solver it was not built for. It is resolved back
	//! to a backend only where a solve is about to happen (SolverRegistry::Find).
	string solver_backend_name;

	//! Stage 05's formulation decision, carried down verbatim: which constructs this
	//! query hands to the backend to state itself, and which arrive already lowered into
	//! plain rows. Layer 8 READS this and translates. It must NOT ask a backend what it
	//! supports — choosing a formulation is stage 05's job, and asking again here is how
	//! the plan and the solve come to disagree about what was lowered.
	SolverConstructSupport use_native_constructs;

	//! Stage 05's formulation POLICY, beside the capability table above: whether a
	//! declared construct is used wherever it is declared, or only where the lowering
	//! has no valid Big-M. False is the shipping answer — the lowering is the smaller
	//! model wherever it works, so native is the fallback (see NativeConstructPolicy).
	//! True only under the test-only `DECIDB_NATIVE_CONSTRUCTS=force`.
	//!
	//! Whether a given SITE has a valid Big-M depends on evaluated data, so stage 08
	//! answers that question. It does not re-decide the policy: it applies this one to
	//! a fact only it can see, the same way the `<>` range collapse does.
	bool force_native_constructs = false;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSource() const override {
		return true;
	}

	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}

public:
	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

private:
	//! What a message renders the query's values with: a value prints as the user wrote it.
	DecideSourceNames Names() const;
	//! The clause of `source_clause_id` as the user wrote it, the objective for none (DecideWrittenClause).
	string WrittenClause(idx_t source_clause_id) const;

	//! The backend named on the plan, resolved to a registry entry. This is the ONLY
	//! place layer 8 turns `solver_backend_name` back into a backend, and it happens
	//! where a solve is about to run — the primary one and every diagnostic re-solve.
	//! Layer 8 never re-selects: it looks up the name stage 05 recorded.
	SolverBackend PlannedSolverBackend() const;

	//! Records an EXPLAIN ANALYZE line (run_time_accounts) for a scoped expression that generated `variants`
	//! instances -- `unit`: constraints, or a reducer's values -- from `classes` classes.
	void RecordVariants(const DecideSignature &signature, const char *unit, idx_t variants, idx_t classes) const;
	void RecordRunTimeAccount(
	    idx_t source_clause_id, string subject,
	    LogicalDecide::RewriteAccount::Kind kind = LogicalDecide::RewriteAccount::Kind::RUN_TIME) const;
	//! What the data left of each clause (deck pp. 50, 56): the instances it skipped for having no value, an EXPLAIN
	//! ANALYZE line under it, and a clause left with none, which constrains nothing, a warning on stderr.
	void ReportSkippedInstances(const DecideGlobalSinkState &gstate) const;
	//! An objective that is 0 whatever the decisions -- every term with no value or a zero factor on every row -- is
	//! a warning on stderr and an EXPLAIN ANALYZE line: any feasible answer is optimal.
	void ReportAnEmptyObjective(const SolverInput &input) const;
	//! What stage 06 made of each helper the plan names (D89): written where it is read, with no column, or its groups
	//! of one member written in -- an EXPLAIN ANALYZE line under each clause whose plan line names the helper. A helper
	//! with no column at all replaces its plan line (ParamsToString), which says it becomes one.
	void ReportInlinedHelpers(const SolverInput &input) const;

	//! Everything EvaluateTree reads off the data that a later step still needs. It carries
	//! objective terms *and* composed-MIN/MAX constraint clauses, so it is named for the
	//! step that produces it rather than for one of its consumers. Nothing here depends
	//! on a variable bound, which is what lets the formulation pass run twice against
	//! two different boxes from the same evaluated clauses.
	struct EvaluatedClauses {
		//! Filter state for the objective's linear terms and its product groups.
		vector<TermFilterState> linear_filters, group_filters;

		//! One composed MIN/MAX *constraint*, evaluated. Source identity and repair metadata came from its E node.
		struct ComposedConstraint {
			vector<ComposedMinMaxTermData> terms;
			double rhs = 0.0;
			//! Comparison owned by the composed clause's E node.
			ExpressionType comparison = ExpressionType::INVALID;
			//! Source and removal groups owned by the composed clause's E node.
			idx_t source_clause_id = DConstants::INVALID_INDEX;
			idx_t removal_group_id = DConstants::INVALID_INDEX;
			//! Its repair group, numbered after every clause of `constraints` and
			//! `singleton_row_constraints`, so its slack is shared with no other clause.
			idx_t repair_group_id = DConstants::INVALID_INDEX;
			//! The classes its one row stands for (SolverInput::class_sets), under
			//! DIAGNOSE; INVALID_INDEX for none.
			idx_t class_set = DConstants::INVALID_INDEX;
			//! A NULL factor made its one instance NULL, or its `per K:` has no class among
			//! the kept rows: no row is emitted.
			bool skipped = false;
		};
		vector<ComposedConstraint> composed_constraints;
		//! The composed MIN/MAX *objective*'s terms, evaluated.
		vector<ComposedMinMaxTermData> composed_objective_terms;
	};

	//! The evaluation (language/guide.md §6.4): the tree's descent over the input (`gstate.descent`), each node's
	//! programs over its entries, and the model emitter, filling gstate's evaluation fields and the arguments after
	//! `num_rows`, with the values its groups are named by in `key_values`.
	void EvaluateTree(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
	                  vector<SolverInput::KeyValues> &key_values, vector<KeyedMapping> &keyed_mappings,
	                  EvaluatedClauses &evaluated) const;

	//! Then each AVG term is divided by the rows it reads, or by those of its own group (D81).
	static void ScaleObjectiveAvg(DecideGlobalSinkState &gstate, idx_t num_rows, EvaluatedClauses &evaluated);

	//! The tail of a composed clause's evaluation once its bound and terms are in `clause`: its one
	//! instance is skipped where a factor is NULL (`null_factor`) or a term reads no row (deck pp. 50, 56), repaired in
	//! a group after every other clause, and held where its `when` keeps a row, counted unless skipped. The rows its
	//! `when` keeps (`kept_rows`) are read only under DIAGNOSE or once it is skipped; under DIAGNOSE `class_set` gives
	//! the classes its one row stands for among them (D28). Appends the clause to `evaluated`.
	void FinishComposedClause(DecideGlobalSinkState &gstate, idx_t num_rows, const DecideNode &node, bool null_factor,
	                          const std::function<vector<bool>()> &kept_rows,
	                          const std::function<idx_t(const vector<bool> &)> &class_set,
	                          EvaluatedClauses::ComposedConstraint clause, EvaluatedClauses &evaluated) const;

	//! BuildSolverInput: build `SolverInput` out of what EvaluateTree evaluated — variable
	//! types and the column box, implied-bound propagation, the objective transfer, and
	//! the flat column space. Runs once per query: it moves the evaluated data off
	//! `gstate`, and the box it settles is the one the formulation pass below is then
	//! handed.
	SolverInput BuildSolverInput(DecideGlobalSinkState &gstate, idx_t num_rows, vector<KeyedMapping> keyed_mappings,
	                             vector<SolverInput::KeyValues> key_values, EvaluatedClauses &evaluated,
	                             VarIndexer &out_var_indexer) const;

	//! FormulateModel: turn the evaluated clauses into solver rows against `box`
	//! — the auto-`M` refill, every linearization pass, and the lowering of whatever the
	//! chosen backend cannot state natively.
	//!
	//! A pure function of its arguments: no `ClientContext`, no data scan, no expression
	//! evaluation. That is what lets it run a second time, on a copy of `input` against a
	//! widened box, to derive the constants an elastic (infeasibility-repair) model needs
	//! — see `FormulateElasticModel`. Keep it that way: anything here that reads the data
	//! instead of `input` belongs in EvaluateTree.
	void FormulateModel(SolverInput &input, const FormulationBox &box, VarIndexer &var_indexer,
	                    const EvaluatedClauses &evaluated) const;

	//! The model input and column space as BuildSolverInput left them — before a single Big-M or
	//! McCormick constant was derived — plus the scoped bounds the column box enforces
	//! (`evaluated_singleton_row_constraints`), whose rows only a repair model carries. Retained
	//! only under `DIAGNOSE`, so the INFEASIBLE terminal can formulate a *second* model
	//! against the widened box a repair searches in, instead of re-using constants that
	//! were derived against the narrower box the solved model declares.
	struct ElasticFormulation {
		SolverInput input;
		VarIndexer var_indexer;
	};

	//! FinalizeSolveResult, solve and readback: build_var_labels, the RouteSolveResult switch and its
	//! four terminal cases, and the shared success epilogue.
	SinkFinalizeType FinalizeSolveResult(ClientContext &context, DecideGlobalSinkState &gstate,
	                                     SolverInput &solver_input, VarIndexer &var_indexer,
	                                     const EvaluatedClauses &evaluated, ElasticFormulation *elastic, bool bench,
	                                     Profiler &model_timer, Profiler &solver_timer) const;

	//! Formulate the elastic model: widen every loosenable column direction, re-derive
	//! every constant against that box, and build the solver model from it. `elastic` is
	//! left untouched — the copy it works on is local.
	SolverModel FormulateElasticModel(const ElasticFormulation &elastic, const EvaluatedClauses &evaluated,
	                                  VarIndexer &out_var_indexer, vector<string> &out_global_labels) const;
};

} // namespace duckdb
