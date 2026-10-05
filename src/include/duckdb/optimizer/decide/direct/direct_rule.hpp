#pragma once

#include <functional>

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;
class LogicalDecide;
class LogicalOperator;
class Optimizer;
struct DirectSolveDecisionRecord;

//! UNKNOWN means the adapter cannot certify this fact; a rule must decline it.
enum class DirectFactStatus : uint8_t { KNOWN, UNKNOWN };

//===--------------------------------------------------------------------===//
// Semantic facts
//===--------------------------------------------------------------------===//
//
// What a rule may know about a DECIDE problem, read once from the bound, canonical
// tree before any DECIDE rewrite (00_design/architecture.md#semantic-facts). The
// adapter is the only code that knows LogicalDecide's layout and the bound-tree
// spelling of DECIDE constructs; rules read meaning. Data-valued expressions
// (coefficients, bounds, WHEN predicates) stay DuckDB expressions over the source.
// Facts own everything they hold. Anything the adapter cannot model is UNKNOWN with
// a reason, per constraint and per objective part.

enum class DirectDomain : uint8_t { BOOL, INT, REAL };

struct DirectDecisionFact {
	DirectDomain domain;
	//! The SQL type of the decision's output column (INTEGER for BOOL, BIGINT for INT, DOUBLE for REAL).
	LogicalType output_type;
	DecideVarScope scope;
	//! Index into DirectProblemFacts::entity_scopes for an ENTITY decision.
	idx_t entity_scope = DConstants::INVALID_INDEX;
};

//! A relation whose tuples carry one identity: a table-scoped declaration (`T.x`) or a reducer qualifier
//! (`SUM(D: e)`).
struct DirectEntityScopeFact {
	vector<idx_t> relations;
	//! Source slots of the identity key columns.
	vector<idx_t> key_slots;
	DirectFactStatus status = DirectFactStatus::KNOWN;
};

enum class DirectReducer : uint8_t {
	//! A row-level part: per-row constraint algebra, or a query-wide decision beside reducers.
	NONE,
	SUM,
	AVG,
	MIN,
	MAX,
	//! `norm(e, 0)`: the number of rows whose |e| reaches `l0_tolerance`.
	COUNT_NONZERO
};

//! One additive part of a constraint's left side or of the objective: `sign * scale * reducer(terms)`.
struct DirectPart {
	DirectFactStatus status = DirectFactStatus::KNOWN;
	string reason;
	int sign = 1;
	//! A query-wide factor canonicalization left on the reducer; null for none.
	unique_ptr<Expression> scale;
	bool scale_divides = false;
	DirectReducer reducer = DirectReducer::NONE;
	//! Aggregate-local WHEN.
	unique_ptr<Expression> filter;
	//! Entity scope the reducer is qualified by (`SUM(D: e)`), or INVALID_INDEX.
	idx_t qualifier = DConstants::INVALID_INDEX;
	double l0_tolerance = 0;
	//! COUNT_NONZERO: the user's bound on |e|, or 0 when it is inferred from the data.
	double l0_bound = 0;
	//! The body's terms. Empty when `inner` holds a nested reducer (`OUTER(INNER(e)) PER k` objectives).
	vector<DecideSplitTerm> terms;
	unique_ptr<DirectPart> inner;
};

enum class DirectProvenance : uint8_t { CONSTANT, QUERY_WIDE, ROW_VARYING };

//! Which rows a constraint or objective applies to. PER keys exclude NULL rows; WHEN false or NULL rows do not
//! join.
struct DirectScopeFact {
	vector<idx_t> per_key_slots;
	unique_ptr<Expression> when;
};

struct DirectConstraintFact {
	DirectFactStatus status = DirectFactStatus::KNOWN;
	string reason;
	//! Index into the source-clause registry: the user's clause this constraint came from, shared by the constraints
	//! one clause expands into.
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	//! The clause as the user wrote it, for messages that name it.
	string clause_text;
	//! True for a reduced constraint, false for one constraint per row.
	bool aggregate = false;
	//! COMPARE_LESSTHANOREQUALTO ... COMPARE_NOTEQUAL, or COMPARE_IN for a membership `x IN (v, ...)`.
	ExpressionType comparison = ExpressionType::INVALID;
	vector<DirectPart> lhs;
	unique_ptr<Expression> rhs;
	DirectProvenance rhs_provenance = DirectProvenance::ROW_VARYING;
	//! COMPARE_IN: the allowed values.
	vector<unique_ptr<Expression>> members;
	DirectScopeFact scope;
	idx_t degree = 0;

	//! The reduced left side when it is one plain `SUM(...)`: unsigned, unscaled, unqualified, not nested; null
	//! otherwise. Its aggregate-local WHEN is allowed only when `allow_filter`.
	const DirectPart *PlainSum(bool allow_filter) const;
};

struct DirectObjectiveFact {
	DirectFactStatus status = DirectFactStatus::KNOWN;
	string reason;
	DecideSense sense = DecideSense::FEASIBILITY;
	//! Decision-free additive constant canonicalization folded out of the objective.
	double offset = 0;
	vector<DirectPart> parts;
	DirectScopeFact scope;
};

struct DirectProblemFacts {
	static DirectProblemFacts Read(ClientContext &context, LogicalDecide &decide);

	idx_t decide_index;
	DirectFactStatus decisions_status = DirectFactStatus::UNKNOWN;
	vector<DirectDecisionFact> decisions;
	vector<DirectEntityScopeFact> entity_scopes;
	DirectFactStatus source_status = DirectFactStatus::UNKNOWN;
	vector<ColumnBinding> source_bindings;
	//! Number of entries in the source-clause registry.
	idx_t source_clause_count = 0;
	//! UNKNOWN only when the constraint tree's wrappers themselves cannot be read; one unmodelled clause leaves it
	//! KNOWN and marks that constraint.
	DirectFactStatus constraints_status = DirectFactStatus::UNKNOWN;
	string constraints_reason;
	vector<DirectConstraintFact> constraints;
	DirectObjectiveFact objective;

	//! The first unknown constraint or objective part, as a `require` reason; empty when every fact is known.
	string FirstUnknownReason() const;
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
