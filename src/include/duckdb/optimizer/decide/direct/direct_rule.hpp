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
// tree before any DECIDE rewrite (see context/docs/direct_solve/architecture/
// ir.md). The front end is the only code that knows LogicalDecide's layout and the
// bound-tree spelling of DECIDE constructs; rules read meaning. Data-valued expressions
// (coefficients, bounds, `when` conditions) stay DuckDB expressions over the source.
// The IR owns everything it holds. Anything the front end cannot model is NOT_PROVED,
// per constraint and per objective part.

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
	int sign = 1;
	//! A query-wide factor canonicalization left on the reducer; null for none.
	unique_ptr<Expression> scale;
	bool scale_divides = false;
	DirectReducer reducer = DirectReducer::NONE;
	//! The reducer's own `when` (`sum(when c: x)`).
	unique_ptr<Expression> filter;
	//! Entity scope the reducer is qualified by (`SUM(D: e)`), or INVALID_INDEX.
	idx_t qualifier = DConstants::INVALID_INDEX;
	//! The body's terms. Empty when `inner` holds a nested reducer (the two-level objective
	//! `outer(per k: inner(e) by (k))`, which the front end declines today).
	vector<DecideSplitTerm> terms;
	unique_ptr<DirectPart> inner;
};

//! Which rows a constraint or objective applies to: a row whose `when` is false or NULL does not join. `per_key_slots`
//! are group keys, read by the per-group code S1 and R1 keep, which leaves a NULL-key row out of every group (the
//! removed PER). Nothing fills them until the front end reads a reducer's `by` key, where a NULL key is a group of its
//! own (D5).
struct DirectIRScope {
	vector<idx_t> per_key_slots;
	unique_ptr<Expression> when;
};

struct DirectIRConstraint {
	DirectAnalysisStatus status = DirectAnalysisStatus::PROVED;
	//! Index into the source-clause registry: the user's clause this constraint came from, shared by the constraints
	//! one clause expands into.
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	//! True for a reduced constraint, false for one constraint per row.
	bool aggregate = false;
	//! COMPARE_LESSTHANOREQUALTO ... COMPARE_NOTEQUAL, or COMPARE_IN for a membership `x IN (v, ...)`.
	ExpressionType comparison = ExpressionType::INVALID;
	vector<DirectPart> lhs;
	unique_ptr<Expression> rhs;
	//! True when the solver path folds this clause into its decision's box (see DecideIsSingletonRowBound): an
	//! unconditional comparison of one bare decision against a finite constant. Such a bound replaces the variable's
	//! default lower bound of 0, where every other bound only intersects with it.
	bool variable_bound = false;
	DirectIRScope scope;
	idx_t degree = 0;

	//! The reduced left side when it is one plain `SUM(...)`: unsigned, unscaled, unqualified, not nested; null
	//! otherwise. Its own `when`, if any, is the returned part's `filter`.
	const DirectPart *PlainSum() const;

	//! The rows the constraint's sum covers: the plain SUM's own `when` or the clause's `when`. Null for none. `ok` is
	//! false when the constraint is not a plain SUM, or has both (the solver reads both; the rules decline it).
	const Expression *Membership(bool &ok) const;
};

struct DirectIRObjective {
	DirectAnalysisStatus status = DirectAnalysisStatus::PROVED;
	DecideSense sense = DecideSense::FEASIBILITY;
	//! Decision-free additive constant canonicalization folded out of the objective.
	double offset = 0;
	vector<DirectPart> parts;
	DirectIRScope scope;
};

//! A run-time scope check the solver makes before it reads a value: the input raises `message` when some row passes
//! `rows` (every row, when null) and none of those rows also passes `filter`. NULL drops a row. A clause's `when` (D4)
//! is `filter` with no `rows`; a reducer's own `when` without a `by` key (D8) is `filter` under its clause's `when`.
//! A value check (VALUE_IS_VALID) has `invalid` instead: the message a row raises, NULL on a valid row.
struct DirectRowCheck {
	unique_ptr<Expression> rows;
	unique_ptr<Expression> filter;
	string message;
	unique_ptr<Expression> invalid;
};

struct DirectIR {
	static DirectIR Read(ClientContext &context, LogicalDecide &decide);

	idx_t decide_index;
	vector<DirectIRDecision> decisions;
	vector<DirectIREntityScope> entity_scopes;
	vector<ColumnBinding> source_bindings;
	//! Number of entries in the source-clause registry.
	idx_t source_clause_count = 0;
	vector<DirectIRConstraint> constraints;
	DirectIRObjective objective;
	//! The scope checks the plan makes on the source rows, which the rule driver adds above every rule's plan.
	vector<DirectRowCheck> row_checks;
	//! NOT_PROVED when the DECIDE operator owes the rows a scope check that is not a row check: one per `by` group or
	//! per class of a key.
	DirectAnalysisStatus status = DirectAnalysisStatus::PROVED;
};

//! Only CheckCondition can construct an applicability condition. Apply receives this
//! object, never an unverified query shape.
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
	//! True when the plan is pipelined: it passes rows through without holding the input, so a column a parent reads
	//! costs no memory to carry. The rule driver then never reads a stored column a second time for it. Leave false for
	//! a plan that holds every row (a breaker, a rank).
	bool pipelined = false;
};

//! Whether the front end read the whole IR: the constraints and the objective are all proved (each constraint too),
//! the objective offset is finite and the sense is one the rules know. The decisions, the entity scopes and the source
//! are always read in full. The rule driver offers a rule an IR only when this holds, so a rule checks only what
//! concerns its own class.
bool DirectIRIsReadable(const DirectIR &ir);

//! The lifecycle the rule driver runs: CheckCondition, then Apply. CheckCondition checks the query's shape and then
//! establishes what the plan relies on; when the condition does not hold it returns null and leaves the DECIDE node
//! untouched. Summary is the one line the rule adds to the EXPLAIN annotation. Rules are disjoint: each requires
//! IR the others forbid, so at most one applies to a query, and the rule driver raises an internal error when two do.
class DirectSolveRule {
public:
	virtual ~DirectSolveRule() = default;
	virtual const char *Name() const = 0;
	virtual unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir,
	                                                                ClientContext &context) const = 0;
	virtual string Summary() const = 0;
	//! `source` is the DECIDE input with its types resolved. `source_fragments` is the DECIDE node's source-fragment
	//! registry, for the messages that quote what the user wrote (DirectNullMessageOf).
	virtual DirectSubstitute Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                               const DirectApplicabilityCondition &condition,
	                               const vector<string> &source_fragments) const = 0;
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
