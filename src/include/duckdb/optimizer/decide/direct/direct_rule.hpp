#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/decide.hpp"
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
	//! External identity certified before Rewrite moves the DECIDE input.
	vector<ColumnBinding> output_bindings;
};

//! A complete relational child plus an explicit map back to DECIDE outputs.
//! Required slots include validation dependencies that parents may not read.
struct DirectRelationalProposal {
	unique_ptr<LogicalOperator> child;
	vector<idx_t> output_slots;
	vector<idx_t> required_slots;
	//! Exact per-output permission to skip evaluation when no parent reads it.
	//! Unknown or potentially observable source expressions must remain live.
	vector<uint8_t> prunable_outputs;
};

class DirectSolveRule {
public:
	virtual ~DirectSolveRule() = default;
	virtual const char *Name() const = 0;
	virtual unique_ptr<DirectRuleMatch> Match(const DirectProblemFacts &facts, string &reason) const = 0;
	virtual unique_ptr<DirectRuleProof> Prove(const DirectProblemFacts &facts, const DirectRuleMatch &match,
	                                          ClientContext &context, string &reason) const = 0;
	//! Called only for proved candidates. Estimates must never certify a proof.
	virtual double Cost(const DirectRuleProof &proof) const = 0;
	virtual void Explain(const DirectRuleProof &proof, DirectSolveDecisionRecord &record) const = 0;
	virtual DirectRelationalProposal Rewrite(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                                         const DirectRuleProof &proof) const = 0;
};

//! Registry used by the coordinator and contract tests. New problem classes
//! register here without changing policy, fallback, or output mapping.
vector<unique_ptr<DirectSolveRule>> RegisteredDirectRules();

} // namespace duckdb
