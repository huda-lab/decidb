#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>
#include <cmath>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_decide_null_guard_expression.hpp"
#include "duckdb/planner/expression/bound_decide_null_test_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

namespace {

//! Whether any term holds algebra no rule reads. A part's own terms may be an ABS; what an ABS or a factor holds is
//! linear algebra only, so an ABS or a product there is unknown.
bool HasUnknownTerm(const vector<DecideSplitTerm> &terms, bool nested = false) {
	for (auto &term : terms) {
		bool readable = nested ? !term.opaque && term.degree <= 1 : !term.opaque || term.IsAbs();
		if (!readable || HasUnknownTerm(term.inner, true)) {
			return true;
		}
		for (auto &factor : term.factors) {
			if (HasUnknownTerm(factor.form, true)) {
				return true;
			}
		}
	}
	return false;
}

//! Reads the clauses and the objective of one DECIDE node as parts, with the solver path's term splitter.
class DirectClauseReader {
public:
	DirectClauseReader(ClientContext &context_p, const LogicalDecide &decide_p)
	    : context(context_p), decide(decide_p), splitter(context_p, decide_p) {
		// The clauses are what the user wrote. After OptimizeDecide the operator holds a solver formulation instead.
		D_ASSERT(!decide.optimized && !decide.num_auxiliary_vars);
	}

private:
	ClientContext &context;
	const LogicalDecide &decide;
	DecideTermSplitter splitter;

	//===--------------------------------------------------------------------===//
	// Parts: one additive term of a left side or the objective
	//===--------------------------------------------------------------------===//

	//! `filter` without the tests DecideCanonicalizer::SkipNullData added, which move to `tests`; null when nothing
	//! else is left.
	static unique_ptr<Expression> TakeSkipNullTests(unique_ptr<Expression> filter,
	                                                vector<unique_ptr<Expression>> &tests) {
		if (filter->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_NULL_TEST) {
			tests.push_back(std::move(filter->Cast<BoundDecideNullTestExpression>().test));
			return nullptr;
		}
		if (filter->GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			return filter;
		}
		auto &conjunction = filter->Cast<BoundConjunctionExpression>();
		vector<unique_ptr<Expression>> kept;
		for (auto &child : conjunction.children) {
			auto rest = TakeSkipNullTests(std::move(child), tests);
			if (rest) {
				kept.push_back(std::move(rest));
			}
		}
		if (kept.empty()) {
			return nullptr;
		}
		if (kept.size() == 1) {
			return std::move(kept[0]);
		}
		conjunction.children = std::move(kept);
		return filter;
	}

	//! Each coefficient of `term` read as 0 on a row `valid` fails: a SUM that skips a row adds nothing for it. A term
	//! of degree 2 is guarded in the forms it multiplies and in its own coefficient (a square's weight), which a rule
	//! that needs a constant reads through (DecideUnguarded).
	void GuardCoefficients(const ExpressionBuilder &calc, const ExprPtr &valid, DecideSplitTerm &term) const {
		auto zero = [&](const LogicalType &type) {
			return calc.Constant(Value::INTEGER(0).DefaultCastAs(type));
		};
		// The guard is the engine's: a render prints the coefficient it guards.
		auto guard = [&](const Expression &coefficient) {
			return make_uniq<BoundDecideNullGuardExpression>(
			    calc.Out(calc.If(valid, calc.Wrap(coefficient.Copy()), zero(coefficient.return_type))));
		};
		for (auto &factor : term.factors) {
			for (auto &part : factor.form) {
				GuardCoefficients(calc, valid, part);
			}
		}
		if (term.coefficient) {
			term.coefficient = guard(*term.coefficient);
		} else if (term.factors.empty()) {
			// A term with no coefficient is 1 where the row is read, guarded like one that has a coefficient.
			term.coefficient = guard(*calc.Num(1));
		}
		// An ABS's argument is guarded term by term as a part's terms are: a term of degree 2 there has no coefficient
		// of its own, only the forms it multiplies, and HasUnknownTerm declines it afterwards.
		for (auto &inner : term.inner) {
			GuardCoefficients(calc, valid, inner);
		}
	}

	bool ReadReducer(const BoundDecideReducerExpression &aggregate, DirectPart &part) const {
		D_ASSERT(aggregate.children.size() == 1);
		// Canonicalization replaced every order of a norm but 0 with its definition. Order 0 needs indicator variables,
		// so no rule models it.
		if (aggregate.kind == DecideReducerKind::NORM) {
			return false;
		}
		// A reducer's own `per` over a body with variants (D71): its instances are the key's classes split by the
		// body's values, which the operator numbers and no rule's plan reads.
		if (aggregate.per != DConstants::INVALID_INDEX &&
		    decide.bound.keyed_scopes[aggregate.per].signature != DConstants::INVALID_INDEX) {
			return false;
		}
		part.reduced = true;
		part.reducer = aggregate.kind;
		if (!DirectReadKey(decide, aggregate.by, part.by)) {
			return false;
		}
		if (aggregate.filter) {
			part.filter = aggregate.filter->Copy();
		}
		part.qualifier = aggregate.per;
		splitter.Split(*aggregate.children[0], 1, part.terms);
		// A SUM skips a row whose data is NULL by reading its terms as 0 there, which every rule reads as it reads any
		// coefficient; the written `when` stays the reducer's.
		if (part.filter && part.reducer == DecideReducerKind::SUM) {
			vector<unique_ptr<Expression>> tests;
			part.filter = TakeSkipNullTests(std::move(part.filter), tests);
			if (!tests.empty()) {
				ExpressionBuilder calc(context);
				ExprPtr valid = calc.Wrap(std::move(tests[0]));
				for (idx_t i = 1; i < tests.size(); i++) {
					valid = calc.Wrap(make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
					                                                        calc.Out(valid), std::move(tests[i])));
				}
				for (auto &term : part.terms) {
					GuardCoefficients(calc, valid, term);
				}
			}
		}
		return true;
	}

	bool ReadPart(const CanonicalAtom &atom, DirectPart &part) const {
		part.sign = atom.sign;
		const Expression *term = atom.term;
		if (atom.scaled) {
			part.scale = atom.scale.scale->Copy();
			part.scale_divides = atom.scale.divides;
			term = atom.scale.aggregate;
		}
		auto root = UnwrapDecideCasts(*term, decide.decide_index);
		if (root->GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER) {
			splitter.Split(*root, 1, part.terms);
		} else if (!ReadReducer(root->Cast<BoundDecideReducerExpression>(), part)) {
			return false;
		}
		return !HasUnknownTerm(part.terms);
	}

	bool ReadParts(const Expression &side, vector<DirectPart> &parts) const {
		for (auto &atom : ReadCanonicalAtoms(side, decide.decide_index)) {
			parts.emplace_back();
			if (!ReadPart(atom, parts.back())) {
				return false;
			}
		}
		return true;
	}

	//! One row-level part holding every term of a per-row side.
	bool ReadRowPart(const Expression &side, vector<DirectPart> &parts) const {
		parts.emplace_back();
		splitter.Split(side, 1, parts.back().terms);
		return !HasUnknownTerm(parts.back().terms);
	}

	//===--------------------------------------------------------------------===//
	// Constraints
	//===--------------------------------------------------------------------===//

public:
	bool ReadClause(DirectClause &read) const {
		auto &clause = read.clause;
		auto &leaf = *clause.body;
		// A clause the query text holds no place for has no name in a message. No rule models the helper decision a
		// reducer's value becomes.
		if (clause.origin.source_clause_id == DConstants::INVALID_INDEX ||
		    clause.shape == CanonicalConstraintClass::VALUE) {
			return false;
		}
		read.written = DecideClauseRefOf(decide.bound.constraint_sources, decide.bound.source_text,
		                                 clause.origin.source_clause_id);
		// `per K: C` is one constraint per class of K and variant of C (D6, D70). An aggregate clause bounds the whole
		// total of each `by` group, and every instance of a group differs only in its bound, which the group's
		// tightest bound covers, as for a clause with no `per`. A clause per row would hold in a variant alone, which
		// no rule models.
		if (clause.scope.HasPer() &&
		    (!read.Aggregate() || !DirectReadKey(decide, clause.scope.class_key, read.classes))) {
			return false;
		}
		// The binder admits only comparisons and IN as a constraint leaf, and no rule models a membership.
		if (leaf.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return false;
		}
		auto &comparison = leaf.Cast<BoundComparisonExpression>();
		// Direct rules run before the MIN/MAX rewrite marks an easy extremum, so only a per-row bound reads here.
		read.singleton_row = DecideReadSingletonRow(context, decide, comparison, clause.minmax_easy, read.bound);
		// Folding never reads a clause under a `when`: `when c: x <= 0` does not bound `x` everywhere.
		read.variable_bound = read.singleton_row && read.bound.folds && !clause.scope.when;
		return read.Aggregate() ? ReadParts(*comparison.left, read.lhs) : ReadRowPart(*comparison.left, read.lhs);
	}

	//===--------------------------------------------------------------------===//
	// Objective
	//===--------------------------------------------------------------------===//

	//! Whether `expr` holds a reducer the canonicalizer marked for the value rewrite: one holding a reducer (D81).
	static bool HoldsValueRewrite(const Expression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER &&
		    expr.Cast<BoundDecideReducerExpression>().holds_reducer) {
			return true;
		}
		bool holds = false;
		ExpressionIterator::EnumerateChildren(
		    expr, [&](const Expression &child) { holds = holds || HoldsValueRewrite(child); });
		return holds;
	}

	bool ReadObjective(DirectObjective &objective) const {
		objective.written =
		    DecideClauseRefOf(decide.bound.constraint_sources, decide.bound.source_text, DConstants::INVALID_INDEX);
		if (!decide.bound.objective.body) {
			return true;
		}
		const Expression *body = UnwrapDecideCasts(*decide.bound.objective.body, decide.decide_index);
		// No rule models the helper decision a nested reducer becomes (D81). An objective with variants is produced
		// once (D72): the operator refuses a second variant of it, which no rule's plan checks.
		if (HoldsValueRewrite(*body) || decide.bound.objective.scope.signature != DConstants::INVALID_INDEX ||
		    !std::isfinite(decide.bound.objective_constant_offset)) {
			return false;
		}
		return ReadParts(*body, objective.parts);
	}
};

} // namespace

DirectDomain DirectDomainOf(const LogicalDecide &decide, idx_t decision) {
	if (decide.bound.is_boolean_var[decision]) {
		return DirectDomain::BOOL;
	}
	auto &type = decide.bound.variables[decision]->return_type;
	D_ASSERT(type == LogicalType::DOUBLE || type.IsIntegral());
	return type == LogicalType::DOUBLE ? DirectDomain::REAL : DirectDomain::INT;
}

bool DirectReadClauses(ClientContext &context, const LogicalDecide &decide, vector<DirectClause> &clauses) {
	DirectClauseReader reader(context, decide);
	// The parser requires a SUCH THAT clause.
	D_ASSERT(!decide.bound.clauses.empty());
	clauses.reserve(decide.bound.clauses.size());
	for (auto &clause : decide.bound.clauses) {
		clauses.emplace_back(clause);
		if (!reader.ReadClause(clauses.back())) {
			return false;
		}
	}
	return true;
}

bool DirectReadObjective(ClientContext &context, const LogicalDecide &decide, DirectObjective &objective) {
	return DirectClauseReader(context, decide).ReadObjective(objective);
}

bool DirectReadKey(const LogicalDecide &decide, idx_t key, vector<DirectKeyElement> &elements) {
	auto source_bindings = decide.SourceBindings();
	for (auto &element : DecideKeyElements(decide.bound.keys, key)) {
		if (!DirectReferencesOnlySource(element.Grouping(), source_bindings) ||
		    !DirectReferencesOnlySource(*element.expression, source_bindings)) {
			return false;
		}
		DirectKeyElement read;
		read.grouping = element.Grouping().Copy();
		read.label = element.expression->Copy();
		read.name = element.name;
		read.no_group = element.null_mode == DecideKeyNullMode::NO_GROUP;
		elements.push_back(std::move(read));
	}
	return true;
}

string DirectRender(const LogicalDecide &decide, const Expression &expr) {
	return RenderDecideSource(expr, decide.bound.source_text, decide.bound.keyed_scopes, &decide.bound.keys);
}

optional_idx DirectPlace(const LogicalDecide &decide, const Expression &expr) {
	// What a NULL guard protects stands in the query, as the render quotes it.
	return DecideSpanLocation(decide.bound.source_text, DecideUnguarded(expr).GetQueryLocation());
}

const Expression &DirectClause::Rhs() const {
	return *clause.body->Cast<BoundComparisonExpression>().right;
}

const DirectPart *DirectClause::PlainSum() const {
	if (!Aggregate() || lhs.size() != 1) {
		return nullptr;
	}
	auto &part = lhs[0];
	if (part.sign != 1 || part.scale || !part.reduced || part.reducer != DecideReducerKind::SUM ||
	    part.qualifier != DConstants::INVALID_INDEX) {
		return nullptr;
	}
	return &part;
}

unique_ptr<Expression> DirectClause::Membership(bool &ok) const {
	auto sum = PlainSum();
	auto when = When();
	ok = sum != nullptr;
	if (!ok || (!sum->filter && !when)) {
		return nullptr;
	}
	if (!sum->filter || !when) {
		return (sum->filter ? sum->filter.get() : when)->Copy();
	}
	// A row is in the sum when both keep it. The solver reads the sum's own `when` only on the rows the clause's `when`
	// keeps; a rule reads only a membership that cannot raise (DirectIsSourceOnlyPredicate), and over those the
	// conjunction keeps the same rows.
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, when->Copy(), sum->filter->Copy());
}

} // namespace duckdb
