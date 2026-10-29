#pragma once

#include <functional>

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
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
	//! Index into DirectIR::keyed_scopes for a KEYED decision.
	idx_t keyed_scope = DConstants::INVALID_INDEX;
};

//! A relation whose tuples carry one identity: a table-scoped declaration (`T.x`) or a reducer qualifier
//! (`SUM(D: e)`).
struct DirectIRKeyedScope {
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

//! One element of a grouping key: the expression rows group on (`DecideKeyElement::Grouping()`, the collated one) and
//! the expression a message quotes (`DecideKeyElement::expression`).
struct DirectKeyElement {
	unique_ptr<Expression> grouping;
	unique_ptr<Expression> label;
	//! How a message names it (`D.region`).
	string name;
	//! A row whose element is NULL is in no group (D5), unless the element is a column of a relation, where NULL is a
	//! value of its own.
	bool no_group = true;
};

//! Whether two keys group the same rows the same way: the same expressions in the same order. The binder keeps one
//! entry for the same columns (FindOrCreateDecideKey), so two keys that agree there handle a NULL alike.
inline bool DirectSameKey(const vector<DirectKeyElement> &a, const vector<DirectKeyElement> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (idx_t i = 0; i < a.size(); i++) {
		if (!a[i].grouping->Equals(*b[i].grouping)) {
			return false;
		}
	}
	return true;
}

//! One additive part of a constraint's left side or of the objective: `sign * scale * reducer(terms)`.
struct DirectPart {
	DirectAnalysisStatus status = DirectAnalysisStatus::PROVED;
	int sign = 1;
	//! A query-wide factor canonicalization left on the reducer; null for none.
	unique_ptr<Expression> scale;
	bool scale_divides = false;
	DirectReducer reducer = DirectReducer::NONE;
	//! The reducer's `by` key (`sum(x) by (g)`): the groups it reduces, one value each. Empty without one. Every
	//! reducer of an aggregate clause shares it, and a clause's rows are one constraint per group.
	vector<DirectKeyElement> by;
	//! The reducer's own `when` (`sum(when c: x)`).
	unique_ptr<Expression> filter;
	//! Keyed scope the reducer is qualified by (`SUM(D: e)`), or INVALID_INDEX.
	idx_t qualifier = DConstants::INVALID_INDEX;
	//! The body's terms. Empty when `inner` holds a nested reducer (the two-level objective
	//! `outer(per k: inner(e) by (k))`, which the front end declines today).
	vector<DecideSplitTerm> terms;
	unique_ptr<DirectPart> inner;
};

//! Which rows a constraint or objective applies to: a row whose `when` is false or NULL does not join.
struct DirectIRScope {
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
	//! Set when the bound-side rewrite left the bound to the rule (DecideReducedBound): the rule computes its
	//! aggregates in its own group aggregate, with `rhs` as the rewrite saw it. Null when `rhs` is one value per group
	//! already.
	unique_ptr<DecideGroupBound> group_bound;
	//! True when the comparison is a bound on one decision, read into `bound` by the solver path's own classifier
	//! (DecideReadSingletonRow): the values it admits, on each row it keeps.
	bool singleton_row = false;
	DecideSingletonRow bound;
	//! True when the solver path folds this clause into its decision's box: a bound known before the query runs, with
	//! no `when`. Such a bound replaces the variable's default lower bound of 0, where every other bound only
	//! intersects with it (D41), except a constant floor under a `when`, which replaces it on the rows the
	//! `when` keeps (DecideSingletonRow::LiftsDefaultFloor).
	bool variable_bound = false;
	DirectIRScope scope;
	//! The clause is written `per K: C` (D6): one constraint per class of K, each bounding the whole total. `classes`
	//! is K; a row whose K has a NULL in a NO_GROUP element is in no class, and with no class at all the clause has no
	//! instance. Empty for `per ()`.
	bool per_class = false;
	vector<DirectKeyElement> classes;
	idx_t degree = 0;

	//! The reduced left side when it is one plain `SUM(...)`: unsigned, unscaled, unqualified, not nested; null
	//! otherwise. Its own `when`, if any, is the returned part's `filter`.
	const DirectPart *PlainSum() const;

	//! The rows the constraint's sum covers: the plain SUM's own `when`, the clause's `when`, or, with both, the rows
	//! both keep. Null for none. `ok` is false when the constraint is not a plain SUM.
	unique_ptr<Expression> Membership(bool &ok) const;
};

struct DirectIRObjective {
	DirectAnalysisStatus status = DirectAnalysisStatus::PROVED;
	DecideSense sense = DecideSense::FEASIBILITY;
	//! Decision-free additive constant canonicalization folded out of the objective.
	double offset = 0;
	//! The objective as a message names it, sense in front (DecideWrittenObjective), as the solver path names it.
	string written;
	vector<DirectPart> parts;
	DirectIRScope scope;
};

enum class DirectRowCheckKind : uint8_t {
	//! The input raises `invalid`, a per-row message that is NULL on a valid row.
	VALUE_IS_VALID,
	//! The input raises when a class of `key` shows more than one `value`, or, with no `value`, holds more than one row
	//! of a row-scoped decision. The classes are the groups of the rows that pass `rows` and then `filter` (NULL drops
	//! a row), among rows whose key has no NULL in a NO_GROUP element. The message names the least failing group and
	//! quotes the least and greatest value (DecideDeterminesText), as PhysicalDecide::RunScopeChecks does.
	DETERMINES
};

//! A run-time scope check the solver makes before it reads a value, which the plan makes on the source rows. Every
//! expression reads source columns only.
struct DirectRowCheck {
	DirectRowCheckKind kind = DirectRowCheckKind::VALUE_IS_VALID;
	//! VALUE_IS_VALID: the message a row raises, NULL on a valid row.
	unique_ptr<Expression> invalid;
	//! DETERMINES: the rows considered (a clause's `when`, D4) and the reducer's own `when`; null for every row.
	unique_ptr<Expression> rows;
	unique_ptr<Expression> filter;
	//! DETERMINES: the classes; empty for one class.
	vector<DirectKeyElement> key;
	//! DETERMINES: the data value each class shows once; null for a decision, which a class holds in one row.
	unique_ptr<Expression> value;
	DecideDeterminesText text;
};

struct DirectIR {
	static DirectIR Read(ClientContext &context, LogicalDecide &decide);

	//! Prints an expression of the source as the user wrote it, for a message that names a value (RenderDecideSource).
	//! It reads the DECIDE node the IR was read from, which outlives the IR.
	std::function<string(const Expression &)> render;
	idx_t decide_index;
	vector<DirectIRDecision> decisions;
	vector<DirectIRKeyedScope> keyed_scopes;
	vector<ColumnBinding> source_bindings;
	//! Number of entries in the source-clause registry.
	idx_t source_clause_count = 0;
	vector<DirectIRConstraint> constraints;
	DirectIRObjective objective;
	//! The scope checks the plan makes on the source rows, which the rule driver adds above every rule's plan.
	vector<DirectRowCheck> row_checks;
	//! A bound the bound-side rewrite left to the rule cannot be computed by it (its sum has a `when` of its own): the
	//! driver builds the windows (InstallReducedBoundWindows) and reads the IR again.
	bool bound_windows_needed = false;
	//! NOT_PROVED when the DECIDE operator owes the rows a scope check the plan cannot make: a determination of an
	//! keyed decision.
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
//! the objective offset is finite and the sense is one the rules know. The decisions, the keyed scopes and the source
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
