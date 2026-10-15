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

//! NOT_PROVED means the front end cannot certify this IR node; a rule must decline it.
enum class DirectAnalysisStatus : uint8_t { PROVED, NOT_PROVED };

//===--------------------------------------------------------------------===//
// Intermediate representation
//===--------------------------------------------------------------------===//
//
// What a rule may know about a DECIDE problem, read once from the bound, canonical
// tree before any DECIDE rewrite (see context/descriptions/08_direct_solve/architecture/
// ir.md). The front end is the only code that knows LogicalDecide's layout and the
// bound-tree spelling of DECIDE constructs; rules read meaning. Data-valued expressions
// (coefficients, bounds, WHEN predicates) stay DuckDB expressions over the source.
// The IR owns everything it holds. Anything the front end cannot model is NOT_PROVED with
// a reason, per constraint and per objective part.

enum class DirectDomain : uint8_t { BOOL, INT, REAL };

struct DirectIRDecision {
	//! The name the user declared, as the solver quotes it.
	string name;
	DirectDomain domain;
	//! The SQL type of the decision's output column (INTEGER for BOOL, BIGINT for INT, DOUBLE for REAL).
	LogicalType output_type;
	DecideVarScope scope;
	//! Index into DirectIR::entity_scopes for an ENTITY decision.
	idx_t entity_scope = DConstants::INVALID_INDEX;
};

//! A relation whose tuples carry one identity: a table-scoped declaration (`T.x`) or a reducer qualifier
//! (`SUM(D: e)`).
struct DirectIREntityScope {
	vector<idx_t> relations;
	//! Source slots of the identity key columns.
	vector<idx_t> key_slots;
	DirectAnalysisStatus status = DirectAnalysisStatus::PROVED;
};

enum class DirectReducer : uint8_t {
	//! A row-level part: per-row constraint algebra, or a query-wide decision beside reducers.
	NONE,
	SUM,
	AVG,
	MIN,
	MAX
};

//! One additive part of a constraint's left side or of the objective: `sign * scale * reducer(terms)`.
struct DirectPart {
	DirectAnalysisStatus status = DirectAnalysisStatus::PROVED;
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
	//! The body's terms. Empty when `inner` holds a nested reducer (`OUTER(INNER(e)) PER k` objectives).
	vector<DecideSplitTerm> terms;
	unique_ptr<DirectPart> inner;
};

//! Which rows a constraint or objective applies to. PER keys exclude NULL rows; WHEN false or NULL rows do not
//! join.
struct DirectIRScope {
	vector<idx_t> per_key_slots;
	unique_ptr<Expression> when;
};

struct DirectIRConstraint {
	DirectAnalysisStatus status = DirectAnalysisStatus::PROVED;
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
	//! True when the solver path folds this clause into its decision's box (see DecideBoundIsAbsorbed): an
	//! unconditional comparison of one bare decision against a finite constant. Such a bound replaces the variable's
	//! default lower bound of 0, where every other bound only intersects with it.
	bool variable_bound = false;
	DirectIRScope scope;
	idx_t degree = 0;

	//! The reduced left side when it is one plain `SUM(...)`: unsigned, unscaled, unqualified, not nested; null
	//! otherwise. Its aggregate-local WHEN is allowed only when `allow_filter`.
	const DirectPart *PlainSum(bool allow_filter) const;
};

struct DirectIRObjective {
	DirectAnalysisStatus status = DirectAnalysisStatus::PROVED;
	string reason;
	DecideSense sense = DecideSense::FEASIBILITY;
	//! Decision-free additive constant canonicalization folded out of the objective.
	double offset = 0;
	vector<DirectPart> parts;
	DirectIRScope scope;
};

struct DirectIR {
	static DirectIR Read(ClientContext &context, LogicalDecide &decide);

	idx_t decide_index;
	DirectAnalysisStatus decisions_status = DirectAnalysisStatus::NOT_PROVED;
	vector<DirectIRDecision> decisions;
	vector<DirectIREntityScope> entity_scopes;
	DirectAnalysisStatus source_status = DirectAnalysisStatus::NOT_PROVED;
	vector<ColumnBinding> source_bindings;
	//! Number of entries in the source-clause registry.
	idx_t source_clause_count = 0;
	//! NOT_PROVED only when the constraint tree's wrappers themselves cannot be read; one unmodelled clause leaves it
	//! PROVED and marks that constraint.
	DirectAnalysisStatus constraints_status = DirectAnalysisStatus::NOT_PROVED;
	string constraints_reason;
	vector<DirectIRConstraint> constraints;
	DirectIRObjective objective;

	//! The first not-proved constraint or objective part, as a `require` reason; empty when every IR node is proved.
	string FirstNotProvedReason() const;
};

//! Only CheckCondition can construct an applicability condition. Apply receives this
//! object, never an unverified shape match.
struct DirectApplicabilityCondition {
	virtual ~DirectApplicabilityCondition() = default;
};

//! A complete relational child plus an explicit map back to DECIDE outputs: the source columns in order, then one
//! output per decision variable. The rule driver derives the external bindings and which source columns may be
//! skipped; the rule states where each output lives and whether evaluating each decision can be skipped.
struct DirectSubstitute {
	unique_ptr<LogicalOperator> child;
	//! The slot map: the child slot that carries each DECIDE output, in output order.
	vector<idx_t> slot_map;
	//! Per decision variable: true when no parent reading it leaves nothing unchecked, so its evaluation may be
	//! skipped. Validation the rule needs must then hang off a validated column.
	vector<uint8_t> prunable_decisions;
	//! Child slots kept live even when no parent reads any output. A rule whose plan must read every input row
	//! before it releases one (the solver's obligation) declares the column that forces that read here, typically
	//! the breaker from DirectValidationBreaker. Empty when the plan has nothing to keep live.
	vector<idx_t> validated_columns;
	//! True when the plan streams rows through without holding the input, so a column a parent reads costs no memory
	//! to carry. The rule driver then never reads a stored column a second time for it. Leave false for a plan that
	//! holds every row (a breaker, a rank).
	bool pipelined = false;
};

//! The lifecycle the rule driver runs: CheckCondition, then Apply. CheckCondition checks the query's shape and then
//! proves what the plan relies on; when the condition does not hold it leaves the DECIDE node untouched and reports
//! `reason`. Summary is the one line the rule adds to the EXPLAIN annotation. Rules are disjoint: each requires IR
//! the others forbid, so at most one applies to a query, and the rule driver raises an internal error when two do.
class DirectSolveRule {
public:
	virtual ~DirectSolveRule() = default;
	virtual const char *Name() const = 0;
	virtual unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context,
	                                                                string &reason) const = 0;
	virtual string Summary() const = 0;
	//! `source` is the DECIDE input with its types resolved.
	virtual DirectSubstitute Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                               const DirectApplicabilityCondition &condition) const = 0;
};

//! Every rule the rule driver tries, in registration order (direct_registry.cpp). A new problem class adds one line
//! there and changes nothing in policy, fallback, or output mapping.
vector<unique_ptr<DirectSolveRule>> RegisteredDirectRules();

//! Replaces the registered rules for one connection. Rule driver tests install it to drive the whole path with stub
//! rules; nothing else does.
static constexpr const char *DIRECT_RULE_TEST_SEAM_KEY = "decidb_direct_rule_test_seam";
struct DirectRuleTestSeam : public ClientContextState {
	std::function<vector<unique_ptr<DirectSolveRule>>()> make_rules;
};

} // namespace duckdb
