#pragma once

#include <functional>

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;
class LogicalDecide;
class LogicalOperator;
class Optimizer;
struct DirectSolveDecisionRecord;

//! UNKNOWN means the adapter cannot certify this fact; a rule must decline it.
enum class DirectFactStatus : uint8_t { KNOWN, UNKNOWN };

struct DirectDecisionFact {
	LogicalType output_type;
	DecideVarScope scope;
	bool boolean_domain;
};

struct DirectObjectiveTerm {
	const Expression *expression;
	int sign;
};

struct DirectConstraintFactor {
	const Expression *expression;
	DirectFactStatus source_status;
	idx_t source_clause_id;
	//! Exact constraint-instance selectors; empty/null means a global clause.
	//! PER keys exclude NULL rows. WHEN false/NULL rows do not join a clause.
	vector<const Expression *> per_keys;
	const Expression *when_condition = nullptr;
};

//! Read-only, complete facts from the bound DECIDE tree. Expression pointers live
//! only for the duration of Match/Prove, before the original node is moved.
struct DirectProblemFacts {
	static DirectProblemFacts Read(LogicalDecide &decide);

	idx_t decide_index;
	DirectFactStatus decisions_status = DirectFactStatus::UNKNOWN;
	vector<DirectDecisionFact> decisions;
	idx_t auxiliary_variables;
	bool has_entity_scopes;
	bool has_entity_keys;
	DecideSense sense;
	double objective_offset;
	DirectFactStatus objective_status = DirectFactStatus::UNKNOWN;
	vector<DirectObjectiveTerm> objective_terms;
	DirectFactStatus constraints_status = DirectFactStatus::UNKNOWN;
	vector<DirectConstraintFactor> constraint_factors;
	DirectFactStatus source_status = DirectFactStatus::UNKNOWN;
	vector<ColumnBinding> source_bindings;
	idx_t source_clause_count;
};

struct DirectRuleMatch {
	virtual ~DirectRuleMatch() = default;
};

//! Only Prove can construct a rule proof. The relational builder receives this
//! object, never an unverified shape match.
struct DirectRuleProof {
	virtual ~DirectRuleProof() = default;
};

//! What Cost may see about the input. Only an estimate of the source's size, never its data, so no estimate can
//! decide whether a rule applies.
struct DirectCostContext {
	//! DuckDB's estimate for the DECIDE input before join ordering. Reading it leaves the plan's own estimates as
	//! they were.
	idx_t estimated_source_rows = 0;
};

//! A complete relational child plus an explicit map back to DECIDE outputs: the source columns in order, then one
//! output per decision variable. The coordinator derives the external bindings and which source columns may be
//! skipped; the rule states where each output lives and whether evaluating each decision can be skipped.
struct DirectRelationalProposal {
	unique_ptr<LogicalOperator> child;
	//! Child slot of every DECIDE output.
	vector<idx_t> output_slots;
	//! Per decision variable: true when no parent reading it leaves nothing unchecked, so its evaluation may be
	//! skipped. Validation the rule needs must then hang off a validation slot.
	vector<uint8_t> prunable_decisions;
	//! Child slots kept live even when no parent reads any output. A rule whose plan must read every input row
	//! before it releases one (the solver's obligation) declares the column that forces that read here, typically
	//! the barrier from DirectValidationBarrier. Empty when the plan has nothing to keep live.
	vector<idx_t> validation_slots;
};

//! The lifecycle the coordinator drives: Match, Prove, Cost, Explain, Rewrite. A miss in Match or Prove leaves the
//! DECIDE node untouched and reports `reason`.
class DirectSolveRule {
public:
	virtual ~DirectSolveRule() = default;
	virtual const char *Name() const = 0;
	virtual unique_ptr<DirectRuleMatch> Match(const DirectProblemFacts &facts, string &reason) const = 0;
	virtual unique_ptr<DirectRuleProof> Prove(const DirectProblemFacts &facts, const DirectRuleMatch &match,
	                                          ClientContext &context, string &reason) const = 0;
	//! Called only for proved candidates; must be finite. The cheapest proved rule is chosen, and among equal costs
	//! the first registered. Estimates must never certify a proof.
	virtual double Cost(const DirectRuleProof &proof, const DirectCostContext &context) const = 0;
	virtual void Explain(const DirectRuleProof &proof, DirectSolveDecisionRecord &record) const = 0;
	//! `source` is the DECIDE input with its types resolved.
	virtual DirectRelationalProposal Rewrite(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                                         const DirectRuleProof &proof) const = 0;
};

//! Every rule the coordinator tries, in registration order (direct_registry.cpp). A new problem class adds one line
//! there and changes nothing in policy, fallback, or output mapping.
vector<unique_ptr<DirectSolveRule>> RegisteredDirectRules();

//! Replaces the registered rules for one connection. Coordinator tests install it to drive the whole path with stub
//! rules; nothing else does.
static constexpr const char *DIRECT_RULE_OVERRIDE_KEY = "decidb_direct_rule_override";
struct DirectRuleOverride : public ClientContextState {
	std::function<vector<unique_ptr<DirectSolveRule>>()> make_rules;
};

} // namespace duckdb
