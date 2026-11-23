#pragma once

#include <functional>

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class DirectSourceReader;

class ClientContext;
class LogicalDecide;
class LogicalOperator;
class Optimizer;

//===--------------------------------------------------------------------===//
// What a rule reads
//===--------------------------------------------------------------------===//
//
// A rule reads the bound DECIDE operator before any DECIDE rewrite (context/docs/direct_solve/architecture/ir.md): the
// decisions, the keys and each clause's scope are its fields. Two readings are made on demand by the rule that wants
// them, with the solver path's own term splitter and bound classifier: a clause's left side and the objective as
// additive parts (DirectReadClauses, DirectReadObjective). Data-valued expressions (coefficients, bounds, `when`
// conditions) stay DuckDB expressions over the source. A reader returns false for a clause no rule's plan reads.

enum class DirectDomain : uint8_t { BOOL, INT, REAL };

//! The domain of the declared decision `decision`.
DirectDomain DirectDomainOf(const LogicalDecide &decide, idx_t decision);

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

	DirectKeyElement Copy() const {
		DirectKeyElement copy;
		copy.grouping = grouping->Copy();
		copy.label = label ? label->Copy() : nullptr;
		copy.name = name;
		copy.no_group = no_group;
		return copy;
	}
};

//! Whether every expression of `a` is one of `b`'s: the groups of `b` are no coarser than `a`'s. A key is a set of
//! expressions, so its order says nothing (FindOrCreateDecideKey), and the empty key is within every key.
inline bool DirectKeyWithin(const vector<DirectKeyElement> &a, const vector<DirectKeyElement> &b) {
	for (auto &element : a) {
		bool found = false;
		for (idx_t i = 0; i < b.size() && !found; i++) {
			found = element.grouping->Equals(*b[i].grouping);
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

//! Whether two keys group the same rows the same way: the same expressions, in any order. The binder keeps one entry
//! for the same columns (FindOrCreateDecideKey), so two keys that agree there handle a NULL alike.
inline bool DirectSameKey(const vector<DirectKeyElement> &a, const vector<DirectKeyElement> &b) {
	return DirectKeyWithin(a, b) && DirectKeyWithin(b, a);
}

//! One additive part of a constraint's left side or of the objective: `sign * scale * reducer(terms)`.
struct DirectPart {
	int sign = 1;
	//! A query-wide factor canonicalization left on the reducer; null for none.
	unique_ptr<Expression> scale;
	bool scale_divides = false;
	//! Whether the part is a reducer's. Else it is row-level: per-row constraint algebra, or a query-wide decision
	//! beside reducers.
	bool reduced = false;
	//! A reducer's part: its kind.
	DecideReducerKind reducer = DecideReducerKind::SUM;
	//! The reducer's `by` key (`sum(x) by (g)`): the groups it reduces, one value each. Empty without one. Every
	//! reducer of an aggregate clause shares it, and a clause's rows are one constraint per group.
	vector<DirectKeyElement> by;
	//! The reducer's own `when` (`sum(when c: x)`).
	unique_ptr<Expression> filter;
	//! Keyed scope the reducer is qualified by (`sum(per D: e)`, BoundDecide::keyed_scopes), or INVALID_INDEX.
	idx_t qualifier = DConstants::INVALID_INDEX;
	//! The body's terms.
	vector<DecideSplitTerm> terms;

	//! Whether the part is a plain `SUM(...)` of the objective: the SUM reducer, not qualified by a keyed scope. Its
	//! sign, factor and filter are the rule's to read. An objective part has no `by` key: DirectReadObjective refuses
	//! an objective with one, which is produced once (D72).
	bool IsPlainSum() const {
		return reduced && reducer == DecideReducerKind::SUM && qualifier == DConstants::INVALID_INDEX;
	}
};

//! One SUCH THAT clause as a rule reads it: the bound clause, with the reading of its left side and of the bound it
//! puts on one decision.
struct DirectClause {
	explicit DirectClause(const BoundDecideClause &clause_p) : clause(clause_p) {
	}

	const BoundDecideClause &clause;
	//! The clause as a message refers to it, as the solver path does: a number the plan computes past a DOUBLE points
	//! at its clause (DecideValueRole::TOO_LARGE).
	DecideClauseRef written;
	//! The left side: the parts of a reduced constraint, or one row-level part.
	vector<DirectPart> lhs;
	//! True when the comparison is a bound on one decision, read into `bound` by the solver path's own classifier
	//! (DecideReadSingletonRow): the values it admits, on each row it keeps.
	bool singleton_row = false;
	DecideSingletonRow bound;
	//! True when the solver path folds this clause into its decision's box: a bound known before the query runs, with
	//! no `when`. Such a bound replaces the variable's default lower bound of 0, where every other bound only
	//! intersects with it (D41), except a constant floor under a `when`, which replaces it on the rows the
	//! `when` keeps (DecideSingletonRow::LiftsDefaultFloor).
	bool variable_bound = false;
	//! The key of a clause written `per K: C` (D6): one constraint per class of K, each bounding the whole total. A row
	//! whose K has a NULL in a NO_GROUP element is in no class, and with no class at all the clause has no instance.
	//! Empty for `per ()`.
	vector<DirectKeyElement> classes;

	//! True for a reduced constraint, false for one constraint per row.
	bool Aggregate() const {
		return clause.shape == CanonicalConstraintClass::AGGREGATE;
	}
	//! COMPARE_LESSTHANOREQUALTO ... COMPARE_NOTEQUAL, or COMPARE_IN for a membership `x IN (v, ...)`.
	ExpressionType Comparison() const {
		return clause.body->type;
	}
	//! The right side of a comparison.
	const Expression &Rhs() const;
	//! The clause's `when`: a row where it is false or NULL does not join. Null for none.
	const Expression *When() const {
		return clause.scope.when.get();
	}
	//! Whether the clause is written `per K: C`.
	bool PerClass() const {
		return clause.scope.HasPer();
	}

	//! The reduced left side when it is one plain `SUM(...)`: unsigned, unscaled, unqualified, not nested; null
	//! otherwise. Its own `when`, if any, is the returned part's `filter`.
	const DirectPart *PlainSum() const;

	//! The rows the constraint's sum covers: the plain SUM's own `when`, the clause's `when`, or, with both, the rows
	//! both keep. Null for none. `ok` is false when the constraint is not a plain SUM.
	unique_ptr<Expression> Membership(bool &ok) const;
};

//! Reads every clause of the query. False when one is a clause no rule's plan reads: a clause the query text holds no
//! place for, a comparison on a reducer's value, a term that is neither linear nor an ABS, an L0 norm, a key that
//! reads more than the source, a `per` on a constraint per row, or a reducer's own `per` whose classes split by the
//! variants of its body (KeyedScopeInfo::signature).
bool DirectReadClauses(ClientContext &context, const LogicalDecide &decide, vector<DirectClause> &clauses);

//! The objective as a rule reads it. The parts are a minimization: of the objective negated when the query was
//! written MAXIMIZE, as BoundDecide::objective_constant_offset is.
struct DirectObjective {
	//! The objective as a message refers to it, as the solver path does.
	DecideClauseRef written;
	vector<DirectPart> parts;
};

//! Reads the objective; no parts for a query with none. False when a part is one no rule's plan reads (as for a
//! clause), a reducer holds a reducer (D81), the objective has variants (D72), or its constant offset is not finite.
bool DirectReadObjective(ClientContext &context, const LogicalDecide &decide, DirectObjective &objective);

//! The key `key` (BoundDecide::keys) as a plan reads it; no element for the empty key. False when it reads more than
//! the source.
bool DirectReadKey(const LogicalDecide &decide, idx_t key, vector<DirectKeyElement> &elements);

//! An expression of the source as the user wrote it, for a message that names a value (RenderDecideSource).
string DirectRender(const LogicalDecide &decide, const Expression &expr);
//! Where an expression of the source stands in the query, as the error that names it points (invalid for one the
//! query text holds no place for).
optional_idx DirectPlace(const LogicalDecide &decide, const Expression &expr);

//! What the plan below the DECIDE node declares, read by the rule that wants it (O1).
struct DirectSourceFacts {
	explicit DirectSourceFacts(const LogicalDecide &decide);

	//! The two sides of a source that is a complete cross product: every row of one side meets every row of the other.
	//! Only DECIDE's own child counts: a projection, filter or join between them leaves both empty.
	vector<ColumnBinding> left;
	vector<ColumnBinding> right;
	//! The source bindings of a directly scanned table's columns that the table declares NOT NULL.
	vector<ColumnBinding> not_null;
	//! The keys of the directly scanned tables, each as the source bindings of its columns: a UNIQUE constraint (the
	//! PRIMARY KEY included) whose columns the scan reads and the table declares all NOT NULL. Two rows of one table
	//! never agree on a key as DuckDB's index compares them; that comparison ignores a column's collation, so a rule
	//! that groups by a key must also check that its grouping applies none.
	vector<vector<ColumnBinding>> keys;

private:
	//! The walk goes down through filters and cross products, which pass their children's bindings and values
	//! through; any other operator (a projection, a join) ends it, so a binding it makes is never recorded.
	void ReadTableFacts(const LogicalOperator &op);
};

//! Only CheckCondition can construct an applicability condition. Apply receives this
//! object, never an unverified query shape.
struct DirectApplicabilityCondition {
	virtual ~DirectApplicabilityCondition() = default;
	//! How many times the rule's plan reads the source. The rule driver adds one read per row check and builds the one
	//! reader of the source (DirectSourceReader) for that total.
	idx_t source_reads = 1;
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
	//! before it releases one (the solver's obligation) and does so inside the plan declares the column that forces
	//! that read here: the hidden rank of a window, or the check a pipelined plan raises on its first row. A rule that
	//! reads the rows once to validate them gates its other reads on the verdict instead
	//! (DirectSourceReader::Gate), which no pass prunes. Empty when the plan has nothing to keep live.
	vector<idx_t> validated_columns;
	//! True when the plan is pipelined: it passes rows through without holding the input, so a column a parent reads
	//! costs no memory to carry. The rule driver then never reads a stored column a second time for it. Leave false for
	//! a plan that holds every row (a rank).
	bool pipelined = false;
};

//! The lifecycle the rule driver runs: CheckCondition, then Apply. CheckCondition checks the query's shape and then
//! establishes what the plan relies on; when the condition does not hold it returns null. Summary is the one line the
//! rule adds to the EXPLAIN annotation. Rules are disjoint: each requires a shape the others forbid, so at most one
//! applies to a query. A debug build asserts it; a release build takes the first.
class DirectSolveRule {
public:
	virtual ~DirectSolveRule() = default;
	virtual const char *Name() const = 0;
	virtual unique_ptr<DirectApplicabilityCondition> CheckCondition(const LogicalDecide &decide,
	                                                                ClientContext &context) const = 0;
	virtual string Summary() const = 0;
	//! `reader` reads the DECIDE input, gated by the row checks when there are any; the rule reads the source
	//! `condition.source_reads` times through it and finishes its plan with it (DirectSourceReader::Finish).
	virtual DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                               const DirectApplicabilityCondition &condition) const = 0;
};

//! A plan over one read the reader gave (ReadSource), written over the source's own bindings: `build` makes it over
//! the read, and the source's bindings in it are re-pointed at the read's columns (ColumnBindingReplacer). The caller
//! finishes the plan with the reader.
unique_ptr<LogicalOperator>
DirectBuildOverRead(DirectSourceReader &reader, DirectSourceReader::Read read,
                    const std::function<unique_ptr<LogicalOperator>(unique_ptr<LogicalOperator>)> &build);

//! Applies a rule that reads the source once and writes its plan over the source's own bindings: `build` makes the
//! substitute over the one read. A recomputed source is read as itself and keeps its bindings; the read of a
//! materialized one has new bindings, at which the plan above it is re-pointed (ColumnBindingReplacer). The reader then
//! finishes the plan.
DirectSubstitute DirectBuildOverOneRead(DirectSourceReader &reader,
                                        const std::function<DirectSubstitute(unique_ptr<LogicalOperator>)> &build);

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
