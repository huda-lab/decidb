//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_helper_form.cpp
//
// Stage 05 — the form of each reducer helper (context/opa_rewrite/design.md § 4). A reducer the value rewrite holds
// in a helper decision is a column, or is written into what reads it. Which is a fact joined over the helper's
// readers, so it is set here, on the plan, once every rewrite has run; execution repeats the form per group.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/decide/decide_tree_nodes.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

namespace {

//! Where the plan reads one reducer helper.
struct HelperReaders {
	//! A reader states the helper's column: a product, a square, a frame, an exact ABS, a `<>`, a bound that may fold
	//! into the column's box.
	bool held = false;
	//! The linear rows and the linear objective that read it.
	idx_t rows = 0;
	//! The one other helper whose member reads it.
	idx_t outer = DConstants::INVALID_INDEX;

	//! Every reader takes a linear expression in its place: rows and the objective, or one other helper, not both.
	bool TakeMembers() const {
		return !held && !(rows > 0 && outer != DConstants::INVALID_INDEX);
	}
};

} // namespace

void DecideOptimizer::ChooseHelperForms(LogicalDecide &decide) {
	auto &definitions = decide.helper_definitions;
	// The reducer helper each decision is, by the decision's column.
	column_binding_map_t<idx_t> helper_of;
	vector<idx_t> definition_of(decide.bound.variables.size(), DConstants::INVALID_INDEX);
	for (idx_t i = 0; i < definitions.size(); i++) {
		if (definitions[i]->kind != DecideHelperKind::REDUCER_VALUE) {
			continue;
		}
		definition_of[definitions[i]->helper] = i;
		helper_of[decide.bound.variables[definitions[i]->helper]->Cast<BoundColumnRefExpression>().binding] = i;
	}
	if (helper_of.empty()) {
		return;
	}
	vector<HelperReaders> readers(definitions.size());
	auto helper_at = [&](const Expression &expr) {
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return idx_t(DConstants::INVALID_INDEX);
		}
		auto entry = helper_of.find(expr.Cast<BoundColumnRefExpression>().binding);
		return entry == helper_of.end() ? idx_t(DConstants::INVALID_INDEX) : entry->second;
	};
	std::function<bool(const Expression &)> holds_helper = [&](const Expression &expr) {
		bool found = helper_at(expr) != DConstants::INVALID_INDEX;
		ExpressionIterator::EnumerateChildren(expr,
		                                      [&](const Expression &child) { found = found || holds_helper(child); });
		return found;
	};
	const PolarityHolds holds = holds_helper;
	auto &context = optimizer.context;
	const BooleanDecisions booleans {decide.decide_index, decide.bound.is_boolean_var};
	// Reads every helper `expr` holds: into `outer`'s member when `outer` is set, into a row otherwise. `linear` is
	// whether the row is a plain linear one.
	auto read = [&](unique_ptr<Expression> &expr, bool linear, idx_t outer, idx_t rows = 1) {
		WalkPolarity(context, booleans, expr, 1, holds,
		             [&](unique_ptr<Expression> &slot, int, const PolarityPath &path) {
			             // A kept ABS or definition reads through its own definition, below.
			             const auto kept = DecideDefinitionOf(*slot);
			             if (kept.Is(DecideHelperKind::ABS) || kept.Is(DecideHelperKind::DEFINITION)) {
				             return true;
			             }
			             const idx_t helper = helper_at(*slot);
			             if (helper == DConstants::INVALID_INDEX) {
				             return false;
			             }
			             auto &reader = readers[helper];
			             if (!linear || path.beside_decision || path.beside_held || path.inside_function) {
				             reader.held = true;
			             } else if (outer == DConstants::INVALID_INDEX) {
				             reader.rows += rows;
			             } else if (reader.outer != DConstants::INVALID_INDEX && reader.outer != outer) {
				             reader.held = true;
			             } else {
				             reader.outer = outer;
			             }
			             return true;
		             });
	};
	// A plain linear row: no `<>`, no product or square beside the helper, and no bound on one decision, which
	// execution may fold into that decision's box.
	auto linear_row = [&](const Expression &body, const BoundDecideClause *clause) {
		if (body.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return DecideDegreeOf(context, body, decide.decide_index).IsLinear();
		}
		auto &comparison = body.Cast<BoundComparisonExpression>();
		if (comparison.type == ExpressionType::COMPARE_NOTEQUAL ||
		    !DecideDegreeOf(context, *comparison.left, decide.decide_index).IsLinear() ||
		    !DecideDegreeOf(context, *comparison.right, decide.decide_index).IsLinear()) {
			return false;
		}
		DecideSingletonRow bound;
		return !(clause && clause->shape != CanonicalConstraintClass::AGGREGATE &&
		         DecideReadSingletonRow(context, decide, comparison, clause->minmax_easy, bound));
	};
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		const bool member = body.owner == DecideOwnedBody::Owner::HELPER &&
		                    definitions[body.index]->kind == DecideHelperKind::REDUCER_VALUE;
		read(body.expr, linear_row(*body.expr, body.clause.get()), member ? body.index : DConstants::INVALID_INDEX);
	});
	for (auto &definition : definitions) {
		if (definition->kind == DecideHelperKind::ABS) {
			// A one-sided ABS states its operand in two plain envelope rows; an exact one states its sign rows from the
			// operand's columns.
			auto &abs = definition->Cast<DecideAbsDefinition>();
			if (abs.body) {
				read(abs.body, !abs.exact && linear_row(*abs.body, nullptr), DConstants::INVALID_INDEX, 2);
			}
		} else if (definition->kind == DecideHelperKind::BOOLEAN_PRODUCT) {
			auto &product = definition->Cast<DecideBooleanProductDefinition>();
			for (auto factor : {product.bool_factor, product.other_factor}) {
				if (factor < definition_of.size() && definition_of[factor] != DConstants::INVALID_INDEX) {
					readers[definition_of[factor]].held = true;
				}
			}
		}
	}
	auto link = [&](idx_t i) -> DecideReducerValueDefinition & {
		return definitions[i]->Cast<DecideReducerValueDefinition>();
	};
	auto is_frame = [&](idx_t i) {
		return link(i).Frame() != DConstants::INVALID_INDEX;
	};
	// A MIN or MAX pinned on both sides states each member in two rows.
	for (auto &entry : helper_of) {
		auto &reader = readers[entry.second];
		if (reader.outer != DConstants::INVALID_INDEX && IsDecideExtremum(link(reader.outer).reducer) &&
		    link(reader.outer).NeedsEnvelope() && link(reader.outer).NeedsClosing()) {
			reader.held = true;
		}
	}
	// The sums of a nest: one a SUM or AVG reads, and that SUM or AVG (deck pp. 115, 119).
	vector<bool> nest(definitions.size(), false);
	for (auto &entry : helper_of) {
		const idx_t i = entry.second;
		const idx_t outer = readers[i].outer;
		if (outer == DConstants::INVALID_INDEX || is_frame(i) || is_frame(outer) ||
		    IsDecideExtremum(link(outer).reducer)) {
			continue;
		}
		nest[outer] = true;
		nest[i] = true;
	}
	for (auto &entry : helper_of) {
		const idx_t i = entry.second;
		auto &reader = readers[i];
		auto &definition = link(i);
		if (!reader.TakeMembers()) {
			definition.form = DecideHelperForm::VARIABLE;
		} else if (IsDecideExtremum(definition.reducer)) {
			definition.form = DecideHelperForm::ONE_MEMBER_WRITTEN_IN;
		} else {
			// One reading row per instance by construction: a member of a MIN or MAX, or a window of a position that
			// its one row reads. A range of a whole timeline is one instance every row of the timeline reads.
			const bool windows = is_frame(i) && decide.bound.frames[definition.Frame()].Relative();
			const bool read_once = reader.outer != DConstants::INVALID_INDEX
			                           ? IsDecideExtremum(link(reader.outer).reducer)
			                           : reader.rows == 1 && windows;
			definition.form = nest[i] || read_once ? DecideHelperForm::WRITTEN_IN : DecideHelperForm::VARIABLE;
		}
	}
}

} // namespace duckdb
