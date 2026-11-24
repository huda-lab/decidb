//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_helper_form.cpp
//
// Stage 05 — the form of each reducer helper (context/docs/pipeline/05_optimizer/done.md § The form of a reducer helper). A reducer the value rewrite holds
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
	//! The places the plan names the helper's column, and those of them that take a linear expression in its place: a
	//! linear row of a clause, the linear objective, a member of one other helper. Every other place states the
	//! column, whatever it is.
	idx_t named = 0;
	idx_t taken = 0;
	//! The linear rows and the linear objective that read it, and whether each lies within one of its groups.
	idx_t rows = 0;
	bool rows_within = true;
	//! The one other helper whose member reads it.
	idx_t outer = DConstants::INVALID_INDEX;

	//! Every reader takes a linear expression in its place: rows and the objective, or one other helper, not both.
	bool TakeMembers() const {
		return taken == named && !(rows > 0 && outer != DConstants::INVALID_INDEX);
	}
};

//! The form of each reducer helper of one plan: the readers of each are read, then the form is chosen from them.
class HelperForms {
public:
	HelperForms(ClientContext &context, LogicalDecide &decide)
	    : context(context), decide(decide),
	      definitions(decide.helper_definitions), booleans {decide.decide_index, decide.bound.is_boolean_var},
	      canonicalizer(context, decide.decide_index, decide.bound), readers(definitions.size()) {
		for (idx_t i = 0; i < definitions.size(); i++) {
			if (definitions[i]->kind == DecideHelperKind::REDUCER_VALUE) {
				helpers.push_back(i);
				helper_of[decide.bound.variables[definitions[i]->helper]->Cast<BoundColumnRefExpression>().binding] = i;
			}
		}
	}

	void Choose() {
		if (helpers.empty()) {
			return;
		}
		CountNames();
		ReadReaders();
		const auto nest = Nest();
		for (auto i : helpers) {
			Link(i).form = FormOf(i, nest);
		}
		TellAccounts();
	}

private:
	DecideReducerValueDefinition &Link(idx_t i) const {
		return definitions[i]->Cast<DecideReducerValueDefinition>();
	}
	bool IsFrame(idx_t i) const {
		return Link(i).Frame() != DConstants::INVALID_INDEX;
	}
	bool IsExtremum(idx_t i) const {
		return IsDecideExtremum(Link(i).reducer);
	}
	//! The reducer helper `expr` is the column of, or INVALID_INDEX.
	idx_t HelperAt(const Expression &expr) const {
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return DConstants::INVALID_INDEX;
		}
		auto entry = helper_of.find(expr.Cast<BoundColumnRefExpression>().binding);
		return entry == helper_of.end() ? idx_t(DConstants::INVALID_INDEX) : entry->second;
	}
	bool HoldsHelper(const Expression &expr) const {
		bool found = HelperAt(expr) != DConstants::INVALID_INDEX;
		ExpressionIterator::EnumerateChildren(expr,
		                                      [&](const Expression &child) { found = found || HoldsHelper(child); });
		return found;
	}
	//! A kept ABS or definition stands for its own definition, whose body is read in its place.
	static bool ReadThroughDefinition(const Expression &expr) {
		const auto kept = DecideDefinitionOf(expr);
		return kept.Is(DecideHelperKind::ABS) || kept.Is(DecideHelperKind::DEFINITION);
	}

	//! Every place the plan names each helper's column (HelperReaders::named), over every expression it holds.
	void CountNames() {
		std::function<void(const Expression &)> count = [&](const Expression &expr) {
			if (ReadThroughDefinition(expr)) {
				return;
			}
			const idx_t helper = HelperAt(expr);
			if (helper != DConstants::INVALID_INDEX) {
				readers[helper].named++;
			}
			ExpressionIterator::EnumerateChildren(expr, count);
		};
		decide.EnumerateExpressions([&](unique_ptr<Expression> *expr) {
			if (*expr) {
				count(**expr);
			}
		});
		// Its declaration is one of them, and reads nothing.
		for (auto i : helpers) {
			readers[i].named--;
		}
		// A product names its factors by decision.
		vector<idx_t> definition_of(decide.bound.variables.size(), DConstants::INVALID_INDEX);
		for (auto i : helpers) {
			definition_of[definitions[i]->helper] = i;
		}
		for (auto &definition : definitions) {
			if (definition->kind != DecideHelperKind::BOOLEAN_PRODUCT) {
				continue;
			}
			auto &product = definition->Cast<DecideBooleanProductDefinition>();
			for (auto factor : {product.bool_factor, product.other_factor}) {
				if (factor < definition_of.size() && definition_of[factor] != DConstants::INVALID_INDEX) {
					readers[definition_of[factor]].named++;
				}
			}
		}
	}

	//! Whether every element of `key` is one of `group`'s.
	bool HeldBy(const vector<DecideKeyElement> &group, idx_t key) const {
		for (auto &element : DecideKeyElements(decide.bound.keys, key)) {
			if (std::none_of(group.begin(), group.end(), [&](const DecideKeyElement &of_group) {
				    return of_group.Grouping().Equals(element.Grouping());
			    })) {
				return false;
			}
		}
		return true;
	}
	//! Whether what one field of a clause's variant signature reads lies within `group`. A decision read per row makes
	//! a row per input row, and data the class key does not hold a row per value: either reads a group once per row.
	bool FieldWithin(const vector<DecideKeyElement> &group, const DecideSignatureField &field) const {
		switch (field.kind) {
		case DecideSignatureFieldKind::DECISION: {
			auto &scope = decide.bound.variable_scopes[field.decision];
			return scope.IsGlobal() ||
			       (scope.IsKeyed() && HeldBy(group, decide.bound.keyed_scopes[scope.keyed_scope_idx].key));
		}
		case DecideSignatureFieldKind::BY_KEY:
			return HeldBy(group, field.key);
		default:
			return false;
		}
	}
	//! Whether `clause` states of `body` at most one row for each group of `helper`: the objective is one row, and a
	//! clause is a row per class and per value of what it reads (its variant signature, D70). Its classes and every key
	//! it reads by must be held by the helper's key, so a group is never read from outside it: written in, the group's
	//! members would be stated once per row.
	bool RowsWithin(const BoundDecideClause *clause, const Expression &body,
	                const DecideReducerValueDefinition &helper) {
		if (!clause) {
			return false;
		}
		if (clause == &decide.bound.objective) {
			return true;
		}
		auto &group = DecideKeyElements(decide.bound.keys, helper.path_key);
		if (!HeldBy(group, clause->scope.class_key)) {
			return false;
		}
		vector<DecideSignatureField> fields;
		canonicalizer.CollectSignatureFields(body, clause->scope.class_key, fields);
		return std::all_of(fields.begin(), fields.end(),
		                   [&](const DecideSignatureField &field) { return FieldWithin(group, field); });
	}
	//! A plain linear row: no `<>`, no product or square beside the helper, and no bound on one decision, which
	//! execution may fold into that decision's box.
	bool LinearRow(const Expression &body, const BoundDecideClause *clause) const {
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
	}

	//! Reads every helper `expr` holds where a reader takes a linear expression in its place: into `outer`'s member
	//! when `outer` is set, into a row of `clause` otherwise. `linear` is whether the row is a plain linear one, and
	//! `rows` how many rows state `expr`.
	void Read(unique_ptr<Expression> &expr, bool linear, idx_t outer, const BoundDecideClause *clause, idx_t rows = 1) {
		const Expression &body = *expr;
		const PolarityHolds holds = [&](const Expression &held) {
			return HoldsHelper(held);
		};
		WalkPolarity(context, booleans, expr, 1, holds,
		             [&](unique_ptr<Expression> &slot, int, const PolarityPath &path) {
			             if (ReadThroughDefinition(*slot)) {
				             return true;
			             }
			             const idx_t helper = HelperAt(*slot);
			             if (helper == DConstants::INVALID_INDEX) {
				             return false;
			             }
			             auto &reader = readers[helper];
			             if (!linear || path.beside_decision || path.beside_held || path.inside_function) {
				             return true;
			             }
			             if (outer == DConstants::INVALID_INDEX) {
				             reader.taken++;
				             reader.rows += rows;
				             reader.rows_within = reader.rows_within && RowsWithin(clause, body, Link(helper));
			             } else if (reader.outer == DConstants::INVALID_INDEX || reader.outer == outer) {
				             reader.taken++;
				             reader.outer = outer;
			             }
			             return true;
		             });
	}
	void ReadReaders() {
		decide.EnumerateBodies([&](DecideOwnedBody &body) {
			const bool member = body.owner == DecideOwnedBody::Owner::HELPER &&
			                    definitions[body.index]->kind == DecideHelperKind::REDUCER_VALUE;
			Read(body.expr, LinearRow(*body.expr, body.clause.get()), member ? body.index : DConstants::INVALID_INDEX,
			     body.clause.get());
		});
		for (auto &definition : definitions) {
			// A one-sided ABS states its operand in two plain envelope rows; an exact one states its sign rows from the
			// operand's columns.
			if (definition->kind == DecideHelperKind::ABS && definition->Cast<DecideAbsDefinition>().body) {
				auto &abs = definition->Cast<DecideAbsDefinition>();
				Read(abs.body, !abs.exact && LinearRow(*abs.body, nullptr), DConstants::INVALID_INDEX, nullptr, 2);
			}
		}
	}

	//! What a SUM or AVG reads as a member is read once, in its group's row. Two reducers so nested are the deck's nest
	//! (pp. 115, 119), and the outer one is written into its readers too, whatever they are. A range beside them is not
	//! of the nest: one that reads a reducer has a window per row, and one a SUM reads leaves the SUM as its readers
	//! make it.
	vector<bool> Nest() const {
		vector<bool> nest(definitions.size(), false);
		for (auto i : helpers) {
			const idx_t outer = readers[i].outer;
			if (outer == DConstants::INVALID_INDEX || IsFrame(outer) || IsExtremum(outer)) {
				continue;
			}
			nest[i] = true;
			nest[outer] = nest[outer] || !IsFrame(i);
		}
		return nest;
	}
	DecideHelperForm FormOf(idx_t i, const vector<bool> &nest) const {
		auto &reader = readers[i];
		const bool member_of_extremum = reader.outer != DConstants::INVALID_INDEX && IsExtremum(reader.outer);
		// A MIN or MAX pinned on both sides states each member in two rows.
		if (!reader.TakeMembers() ||
		    (member_of_extremum && Link(reader.outer).NeedsEnvelope() && Link(reader.outer).NeedsClosing())) {
			return DecideHelperForm::VARIABLE;
		}
		if (IsExtremum(i)) {
			return DecideHelperForm::ONE_MEMBER_WRITTEN_IN;
		}
		// What reads each group once, or from inside it: a MIN or MAX, as a member; the one row that reads a window of
		// a position (a range of a whole timeline is one instance every row of the timeline reads); the one clause
		// whose rows lie within the group.
		const bool windows = IsFrame(i) && decide.bound.frames[Link(i).Frame()].Relative();
		const bool one_row = reader.outer == DConstants::INVALID_INDEX && reader.rows == 1;
		if (nest[i] || member_of_extremum || (one_row && (IsFrame(i) ? windows : reader.rows_within))) {
			return DecideHelperForm::WRITTEN_IN;
		}
		// One clause reads it, in rows the plan cannot count per group: the data says which groups one row reads.
		return one_row ? DecideHelperForm::ONE_ROW_WRITTEN_IN : DecideHelperForm::VARIABLE;
	}
	//! A plan says what each helper became: one written in is no helper decision.
	void TellAccounts() {
		for (auto &account : decide.rewrite_accounts) {
			for (auto i : helpers) {
				if (account.helper == Link(i).helper && Link(i).form == DecideHelperForm::WRITTEN_IN) {
					account.kind = LogicalDecide::RewriteAccount::Kind::VALUE_HELPER_INLINED;
				}
			}
		}
	}

	ClientContext &context;
	LogicalDecide &decide;
	vector<unique_ptr<DecideHelperDefinition>> &definitions;
	const BooleanDecisions booleans;
	DecideCanonicalizer canonicalizer;
	//! The reducer helpers, as indices into `definitions`, and each by its decision's column.
	vector<idx_t> helpers;
	column_binding_map_t<idx_t> helper_of;
	vector<HelperReaders> readers;
};

} // namespace

void DecideOptimizer::ChooseHelperForms(LogicalDecide &decide) {
	HelperForms(optimizer.context, decide).Choose();
}

} // namespace duckdb
