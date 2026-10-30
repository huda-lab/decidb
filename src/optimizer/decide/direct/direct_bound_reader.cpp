#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"

#include <algorithm>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"

namespace duckdb {

static bool DirectProveValues(const DirectIR &ir, const vector<DecideSplitTerm> &terms,
                              vector<DirectSignedValue> &out) {
	for (auto &term : terms) {
		if (!DirectProveValue(ir, *term.coefficient)) {
			return false;
		}
		DirectSignedValue value;
		value.sign = term.sign;
		value.expr = term.coefficient->Copy();
		out.push_back(std::move(value));
	}
	return true;
}

bool DirectIsSingletonRow(const DirectIR &ir, const DirectIRConstraint &ir_node) {
	return ir_node.singleton_row && ir_node.bound.var_idx < ir.decisions.size();
}

bool DirectContradictsDomain(const DirectIR &ir) {
	// Every bound here is the user's (Add): direct solve runs before RewriteInDomain, so a rule reads an IN as written
	// and never the bounds its lowering adds (AddLowered).
	vector<DecideWrittenBox> written(ir.decisions.size());
	for (auto &ir_node : ir.constraints) {
		if (ir_node.variable_bound && DirectIsSingletonRow(ir, ir_node)) {
			written[ir_node.bound.var_idx].Add(ir_node.bound.sense, ir_node.bound.written_k);
		} else if (ir_node.scope.when && DirectIsSingletonRow(ir, ir_node) && ir_node.bound.LiftsDefaultFloor()) {
			// A floor under a `when` replaces the default on the rows it keeps (AddLowered counts it only when
			// negative); the solve says whether the clauses on the other rows leave a value.
			written[ir_node.bound.var_idx].AddLowered('>', ir_node.bound.written_k);
		}
	}
	for (idx_t decision = 0; decision < written.size(); decision++) {
		if (written[decision].ContradictsDomain(ir.decisions[decision].domain == DirectDomain::BOOL)) {
			return true;
		}
	}
	return false;
}

bool DirectReadBounds(const DirectIR &ir, ClientContext &context, DirectBoundReading &reading) {
	// The solver rejects, before it reads any row, a box that contradicts the variable's built-in domain. It says so on
	// an empty source too, so the query is left to the solver, which reports it, rather than proved here and answered
	// with no rows.
	if (DirectContradictsDomain(ir)) {
		return false;
	}
	idx_t decision_count = ir.decisions.size();
	// The values the bounds the solver folds admit, folded by the function the solver's own folding pass calls.
	vector<DecideBoundBox> admitted(decision_count);
	for (auto &ir_node : ir.constraints) {
		if (ir_node.aggregate) {
			continue;
		}
		auto &bound = ir_node.bound;
		idx_t decision = bound.var_idx;
		// The classifier and the IR read the comparison with the same term splitter: one part of linear terms in the
		// decision.
		D_ASSERT(DirectIsSingletonRow(ir, ir_node) && ir_node.lhs.size() == 1);
		if (!DirectIsSourceOnlyValue(*ir_node.rhs, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		if (ir_node.scope.when &&
		    !DirectIsSourceOnlyPredicate(*ir_node.scope.when, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		if (ir_node.variable_bound) {
			if (bound.sense != '>') {
				DecideFoldSingletonRow(admitted[decision], '<', bound.upper_value);
			}
			if (bound.sense != '<') {
				DecideFoldSingletonRow(admitted[decision], '>', bound.lower_value);
			}
			continue;
		}
		DirectBoundClause clause;
		clause.decision = decision;
		if (!DirectProveValues(ir, ir_node.lhs[0].terms, clause.coefficient)) {
			return false;
		}
		clause.rhs = ir_node.rhs->Copy();
		clause.coefficient_names = DirectNamesOf(ir, clause.coefficient, ir_node.written);
		clause.rhs_text = ir.render(*clause.rhs);
		clause.lower = bound.RowLower();
		clause.upper = bound.RowUpper();
		if (ir_node.scope.when) {
			clause.when = ir_node.scope.when->Copy();
			clause.lifts_floor = bound.LiftsDefaultFloor();
		}
		reading.clauses.push_back(std::move(clause));
	}
	// A floor under a `when` replaces the default floor of 0 only on a decision with no lower bound written, which a
	// BOOL always has: its floor stays 0.
	for (auto &clause : reading.clauses) {
		clause.lifts_floor = clause.lifts_floor && !admitted[clause.decision].lower_written &&
		                     ir.decisions[clause.decision].domain != DirectDomain::BOOL;
	}
	reading.boxes.resize(decision_count);
	for (idx_t decision = 0; decision < decision_count; decision++) {
		// A written lower bound replaces the default of 0, except that a BOOL never leaves {0, 1}. An end of magnitude
		// 1e20 or more (a written -1e30 included) is no bound.
		bool boolean = ir.decisions[decision].domain == DirectDomain::BOOL;
		auto &box = admitted[decision];
		double lower = DecideIsOpenBelow(box.lower) ? NEGATIVE_INFINITY : box.lower;
		double upper = DecideIsOpenAbove(box.upper) ? POSITIVE_INFINITY : box.upper;
		reading.boxes[decision].lower = box.lower_written ? (boolean ? std::max(0.0, lower) : lower) : 0.0;
		reading.boxes[decision].upper = boolean ? std::min(1.0, upper) : upper;
	}
	return true;
}

void DirectClauseBounds(const ExpressionBuilder &calc, const DirectBoundColumns &columns, ExprPtr &lower,
                        ExprPtr &upper) {
	lower = calc.Col(LogicalType::DOUBLE, columns.lower);
	upper = calc.Col(LogicalType::DOUBLE, columns.upper);
	if (columns.has_when) {
		auto active = calc.Col(LogicalType::BOOLEAN, columns.when);
		lower = calc.If(active, lower, calc.Num(NEGATIVE_INFINITY));
		upper = calc.If(active, upper, calc.Num(POSITIVE_INFINITY));
	}
}

ExprPtr DirectStaticFloor(const ExpressionBuilder &calc, double static_lower,
                          const vector<ColumnBinding> &lowering_whens) {
	if (lowering_whens.empty()) {
		return calc.Num(static_lower);
	}
	vector<ExprPtr> whens;
	for (auto &binding : lowering_whens) {
		whens.push_back(calc.Col(LogicalType::BOOLEAN, binding));
	}
	return calc.If(calc.Any(whens), calc.Num(NEGATIVE_INFINITY), calc.Num(static_lower));
}

void DirectRowBox(const ExpressionBuilder &calc, const ExprPtr &static_lower, double static_upper,
                  const vector<ColumnBinding> &clause_lowers, const vector<ColumnBinding> &clause_uppers,
                  ExprPtr &lower, ExprPtr &upper) {
	vector<ExprPtr> lowers {static_lower};
	vector<ExprPtr> uppers {calc.Num(static_upper)};
	for (auto &binding : clause_lowers) {
		lowers.push_back(calc.Col(LogicalType::DOUBLE, binding));
	}
	for (auto &binding : clause_uppers) {
		uppers.push_back(calc.Col(LogicalType::DOUBLE, binding));
	}
	lower = calc.Max(lowers);
	upper = calc.Min(uppers);
}

ExprPtr DirectBoxIsEmpty(const ExpressionBuilder &calc, const ExprPtr &lower, const ExprPtr &upper) {
	return calc.Any({calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, lower, upper),
	                 calc.Cmp(ExpressionType::COMPARE_EQUAL, lower, calc.Num(POSITIVE_INFINITY)),
	                 calc.Cmp(ExpressionType::COMPARE_EQUAL, upper, calc.Num(NEGATIVE_INFINITY))});
}

DirectNumberNames DirectNamesOf(const DirectIR &ir, const vector<DirectSignedValue> &values, const string &clause) {
	DirectNumberNames names;
	for (auto &value : values) {
		if (!value.expr->IsFoldable()) {
			names.inputs.push_back(value.expr->Copy());
			names.texts.push_back(ir.render(*value.expr));
		}
	}
	names.clause = clause;
	return names;
}

bool DirectReadLinearPart(const DirectIR &ir, ClientContext &context, const DirectPart &part, const string &clause,
                          DirectLinearPart &out) {
	out.sign = part.sign;
	out.scaled = part.scale != nullptr;
	out.divides = part.scale_divides;
	if (!DirectPartScale(context, part, out.scale)) {
		return false;
	}
	for (auto &term : part.terms) {
		if (term.kind != DecideTermKind::LINEAR) {
			continue;
		}
		if (!term.coefficient || !DirectProveValue(ir, *term.coefficient)) {
			return false;
		}
		out.terms.push_back({term.var_a, term.sign, term.coefficient->Copy()});
	}
	// The part's factor is a literal, which no column holds: a message names the terms' own values.
	for (idx_t decision = 0; decision < ir.decisions.size(); decision++) {
		vector<DirectSignedValue> like;
		for (auto &term : out.terms) {
			if (term.decision == decision) {
				like.push_back({term.sign, term.coefficient->Copy()});
			}
		}
		out.names.push_back(DirectNamesOf(ir, like, clause));
	}
	return true;
}

} // namespace duckdb
