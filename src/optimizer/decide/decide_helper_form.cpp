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
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
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
	//! The linear rows and the linear objective that read it, and whether each lies within one of its groups.
	idx_t rows = 0;
	bool rows_within = true;
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
	DecideCanonicalizer canonicalizer(context, decide.decide_index, decide.bound);
	// Whether `clause` states of `body` at most one row for each group of `helper`: the objective is one row, and a
	// clause is a row per class and per value of what it reads (its variant signature, D70). Its classes and every key
	// it reads by must be held by the helper's key, so a group is never read from outside it. A decision read per row
	// makes a row per input row, and data its class key does not hold a row per value: either reads a group once per
	// row, and written in the group's members would be stated once per row.
	auto rows_within = [&](const BoundDecideClause *clause, const Expression &body,
	                       const DecideReducerValueDefinition &helper) {
		if (!clause) {
			return false;
		}
		if (clause == &decide.bound.objective) {
			return true;
		}
		auto &group = DecideKeyElements(decide.bound.keys, helper.path_key);
		auto held = [&](idx_t key) {
			for (auto &element : DecideKeyElements(decide.bound.keys, key)) {
				if (std::none_of(group.begin(), group.end(), [&](const DecideKeyElement &of_group) {
					    return of_group.Grouping().Equals(element.Grouping());
				    })) {
					return false;
				}
			}
			return true;
		};
		if (!held(clause->scope.class_key)) {
			return false;
		}
		vector<DecideSignatureField> fields;
		canonicalizer.CollectSignatureFields(body, clause->scope.class_key, fields);
		for (auto &field : fields) {
			switch (field.kind) {
			case DecideSignatureFieldKind::VALUE:
				return false;
			case DecideSignatureFieldKind::DECISION: {
				auto &scope = decide.bound.variable_scopes[field.decision];
				if (scope.IsGlobal()) {
					break;
				}
				if (!scope.IsKeyed() || !held(decide.bound.keyed_scopes[scope.keyed_scope_idx].key)) {
					return false;
				}
				break;
			}
			case DecideSignatureFieldKind::BY_KEY:
				if (!held(field.key)) {
					return false;
				}
				break;
			case DecideSignatureFieldKind::FRAME:
				return false;
			}
		}
		return true;
	};
	// Reads every helper `expr` holds: into `outer`'s member when `outer` is set, into a row of `clause` otherwise.
	// `linear` is whether the row is a plain linear one, and `rows` how many rows state `expr`.
	auto read = [&](unique_ptr<Expression> &expr, bool linear, idx_t outer, const BoundDecideClause *clause,
	                idx_t rows = 1) {
		const Expression &body = *expr;
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
				             reader.rows_within =
				                 reader.rows_within &&
				                 rows_within(clause, body, definitions[helper]->Cast<DecideReducerValueDefinition>());
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
		read(body.expr, linear_row(*body.expr, body.clause.get()), member ? body.index : DConstants::INVALID_INDEX,
		     body.clause.get());
	});
	for (auto &definition : definitions) {
		if (definition->kind == DecideHelperKind::ABS) {
			// A one-sided ABS states its operand in two plain envelope rows; an exact one states its sign rows from the
			// operand's columns.
			auto &abs = definition->Cast<DecideAbsDefinition>();
			if (abs.body) {
				read(abs.body, !abs.exact && linear_row(*abs.body, nullptr), DConstants::INVALID_INDEX, nullptr, 2);
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
	// What a SUM or AVG reads as a member is read once, in its group's row. Two reducers so nested are the deck's nest
	// (pp. 115, 119), and the outer one is written into its readers too, whatever they are. A range beside them is not
	// of the nest: one that reads a reducer has a window per row, and one a SUM reads leaves the SUM as its readers
	// make it.
	vector<bool> nest(definitions.size(), false);
	for (auto &entry : helper_of) {
		const idx_t i = entry.second;
		const idx_t outer = readers[i].outer;
		if (outer == DConstants::INVALID_INDEX || is_frame(outer) || IsDecideExtremum(link(outer).reducer)) {
			continue;
		}
		nest[i] = true;
		nest[outer] = nest[outer] || !is_frame(i);
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
			// What reads each group once, or from inside it: a MIN or MAX, as a member; the one row that reads a window
			// of a position (a range of a whole timeline is one instance every row of the timeline reads); the one
			// clause whose rows lie within the group.
			const bool windows = is_frame(i) && decide.bound.frames[definition.Frame()].Relative();
			const bool read_once = reader.outer != DConstants::INVALID_INDEX
			                           ? IsDecideExtremum(link(reader.outer).reducer)
			                           : reader.rows == 1 && (is_frame(i) ? windows : reader.rows_within);
			definition.form = nest[i] || read_once ? DecideHelperForm::WRITTEN_IN : DecideHelperForm::VARIABLE;
		}
	}
	// A plan says what each helper became: one written in is no helper decision.
	using Kind = LogicalDecide::RewriteAccount::Kind;
	for (auto &entry : helper_of) {
		auto &definition = link(entry.second);
		if (definition.form != DecideHelperForm::WRITTEN_IN) {
			continue;
		}
		for (auto &account : decide.rewrite_accounts) {
			if (account.subject == definition.label) {
				account.kind = Kind::VALUE_HELPER_INLINED;
			}
		}
	}
}

} // namespace duckdb
