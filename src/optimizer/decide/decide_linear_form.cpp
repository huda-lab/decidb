#include "duckdb/optimizer/decide/decide_linear_form.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Structural helpers
//===--------------------------------------------------------------------===//

// ExpressionIterator::EnumerateChildren has no const overload; this wrapper
// isolates the const_cast so no call site needs to mention it.
static void EnumerateChildrenConst(const Expression &expr,
                                   const std::function<void(unique_ptr<Expression> &)> &callback) {
	ExpressionIterator::EnumerateChildren(const_cast<Expression &>(expr), callback);
}

//! The solver path reads what survives lowering: terms of degree 0, 1 and 2. The shape check refused every other
//! node over a decision, and RewriteAbs gave every ABS over a decision a helper the splitter reads (an F node), so an
//! opaque term here was built by a rewrite.
[[noreturn]] static void ThrowOpaqueTerm(const DecideSplitTerm &term) {
	throw InternalException("DECIDE term '%s' reached term extraction unopened", term.opaque->ToString());
}

//! `value` as a frame column holds it, for execution to evaluate: a reducer is the aggregate it names.
static unique_ptr<Expression> FrameColumnValue(const Expression &value) {
	return value.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER
	           ? value.Cast<BoundDecideReducerExpression>().ToAggregate()
	           : value.Copy();
}

//! Whether `expr` is a reducer: an aggregate, a frame's marker excepted (a tuple term read at another row).
static bool IsReducer(const Expression &expr) {
	idx_t frame;
	return expr.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER && !TryGetDecideFrame(expr, frame);
}

//===--------------------------------------------------------------------===//
// DecideLinearFormBuilder
//===--------------------------------------------------------------------===//

//! Flattens the canonical clauses and the objective into the tree's
//! terms, at physical planning. Everything it needs is a type or a structure: the
//! decision variable list, their scopes, and what earlier passes recorded on
//! the clauses. It never reads a data row.
class DecideLinearFormBuilder {
public:
	DecideLinearFormBuilder(ClientContext &context, LogicalDecide &op, DecideTreeConstruction &tree)
	    : context(context), op(op), tree(tree), decide_index(op.decide_index),
	      canonicalizer(context, op.decide_index, op.bound.variable_scopes, op.bound.keyed_scopes, op.bound.keys,
	                    op.bound.is_boolean_var, op.bound.computed_values),
	      splitter(context, op) {
		// A frame's values are columns after the input's (DecideFrameColumn).
		frame_reads.column_base = op.children.empty() ? 0 : op.children[0]->types.size();
	}

	void Build() {
		for (auto &clause : op.bound.clauses) {
			AnalyzeConstraint(clause);
		}
		// Tree-owned nonlinear definition rows are built beneath their F nodes, while their placeholders preserve the
		// clause-list position. Lower the retained rows through the same compiler as ordinary clauses.
		for (auto function : tree.Tree().function_nodes) {
			auto &node = *tree.Tree().nodes[function];
			if (node.function_kind == DecideFunctionKind::DEFINITION && node.definition_row.body) {
				AnalyzeConstraint(node.definition_row);
			}
		}
		// The L0 indicator's formulation rows remain outside the written constraint list. Prepare them in the same
		// position they had when RewriteNorm appended them, while their tree F node owns their execution scopes.
		for (auto &indicator : op.l0_indicator_functions) {
			for (auto &link : indicator.links) {
				AnalyzeConstraint(link);
			}
		}
		// An ABS's two lower-envelope rows (MakeAbsRow) are built beneath its F node, from the optimizer's record.
		for (auto &function : op.abs_functions) {
			for (auto index : tree.Tree().function_nodes) {
				auto &node = *tree.Tree().nodes[index];
				if (node.function_kind != DecideFunctionKind::ABS || node.value_var != function.aux_idx) {
					continue;
				}
				for (auto &row : node.abs_rows) {
					AnalyzeConstraint(row);
				}
				break;
			}
		}
		if (op.bound.objective.body) {
			AnalyzeObjective(op.bound.objective);
		}
		// The reducers the value rewrite lifted into helper decisions are lowered too; a range frame's helper also
		// reads its `else`, at the window's anchor.
		const BoundConstantExpression one(Value::INTEGER(1));
		link_terms.resize(op.reducer_value_links.size());
		link_fallbacks.resize(op.reducer_value_links.size());
		for (idx_t l = 0; l < op.reducer_value_links.size(); l++) {
			auto &link = op.reducer_value_links[l];
			ExtractLinearTerms(*link.inner_expr, link_terms[l]);
			if (link.frame != DConstants::INVALID_INDEX) {
				AppendFallbackTerms(link.frame, one, 1, link_fallbacks[l]);
			}
		}

		// Group like terms, once, over every list that becomes a linear solver row.
		for (auto &clause : lowered_clauses) {
			CollectLikeTerms(clause.second->lhs_terms);
			SettleWeightSigns(clause.second->product_groups);
		}
		if (lowered_objective) {
			DropNullObjectiveTerms(*lowered_objective);
			CollectLikeTerms(lowered_objective->terms);
			SettleWeightSigns(lowered_objective->product_groups);
		}
		for (idx_t l = 0; l < link_terms.size(); l++) {
			CollectLikeTerms(link_terms[l]);
			CollectLikeTerms(link_fallbacks[l]);
		}

		// Every frame left in an expression execution evaluates is over data -- a frame over
		// decisions became its terms above -- or the NULL test of a reducer's member, and
		// becomes the column it reads.
		LowerFramesInPreparedModel();

		// A recorded bound's repair rows come from its clause, so the repair model would
		// open its column with nothing enforcing it were its clause not flattened above.
		idx_t singleton_rows = 0;
		for (auto &clause : lowered_clauses) {
			singleton_rows += clause.first.singleton_row;
		}
		if (op.user_singleton_row_bounds.size() != singleton_rows) {
			throw InternalException("DECIDE: %llu singleton-row bounds take their repair rows from their clause, but "
			                        "%llu such clauses were flattened",
			                        op.user_singleton_row_bounds.size(), singleton_rows);
		}
		// Each clause's records in the root the tree made of its comparison, the objective's in the objective's root.
		for (auto &clause : lowered_clauses) {
			tree.TakeClause(*clause.first.leaf, std::move(clause.second), clause.first.singleton_row);
		}
		if (lowered_objective) {
			tree.TakeObjective(std::move(lowered_objective));
		}
		for (idx_t l = 0; l < link_terms.size(); l++) {
			tree.TakeLinkTerms(l, std::move(link_terms[l]), std::move(link_fallbacks[l]));
		}
		tree.TakeFrameReads(std::move(frame_reads));
	}

private:
	//===------------------------------------------------------------------===//
	// Variable lookup and degree
	//===------------------------------------------------------------------===//

	idx_t FindDecideVariable(const Expression &expr) const {
		return splitter.FindVariable(expr);
	}

	void ExtractLinearTerms(const Expression &expr, vector<DecideTerm> &out) {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, 1, split);
		AppendLinearTerms(split, out);
	}

	//! Terms of an expression that must be linear, as lowered terms; a frame over decisions as the terms it reads.
	void AppendLinearTerms(vector<DecideSplitTerm> &split, vector<DecideTerm> &out) {
		for (auto &term : split) {
			if (term.IsFrame()) {
				AppendFrameTerms(term, out);
				continue;
			}
			if (term.opaque) {
				ThrowOpaqueTerm(term);
			}
			if (term.degree > 1) {
				throw InternalException("DECIDE linear expression split into a non-linear term");
			}
			out.push_back(DecideTerm {term.var_a, std::move(term.coefficient), term.sign});
		}
	}

	//! A term of degree 2 as the group stages 06 and 08 read: its coefficient is the group's weight, and its one
	//! squared factor, or its two factors, are the group's forms.
	DecideProductGroup ReadProductGroup(DecideSplitTerm &term) {
		D_ASSERT(term.IsSquare() || term.IsProduct());
		DecideProductGroup group;
		group.weight = std::move(term.coefficient);
		group.sign = term.sign;
		group.square = term.IsSquare();
		AppendLinearTerms(term.factors[0].form, group.left);
		if (!group.square) {
			AppendLinearTerms(term.factors[1].form, group.right);
		}
		return group;
	}

	//===------------------------------------------------------------------===//
	// Frame selectors (language/guide.md §5.6)
	//===------------------------------------------------------------------===//

	//! The frame column holding `value` read the way `kind` says for frame `frame`
	//! (DecideFrameColumn), registered once per distinct value; its index. A VALUE column
	//! also reads the frame's `else`.
	idx_t FrameColumnSlot(DecideFrameColumn::Kind kind, idx_t frame, const Expression *value) {
		auto &columns = frame_reads.columns;
		auto held = value ? FrameColumnValue(*value) : nullptr;
		for (idx_t i = 0; i < columns.size(); i++) {
			auto &column = columns[i];
			if (column.kind == kind && column.frame == frame && Expression::Equals(column.value, held)) {
				return i;
			}
		}
		DecideFrameColumn column;
		column.kind = kind;
		column.frame = frame;
		column.value = std::move(held);
		column.type = value ? value->return_type : LogicalType::BOOLEAN;
		auto &fallback = op.bound.frames[frame].fallback;
		const bool reads_fallback = kind == DecideFrameColumn::Kind::VALUE || kind == DecideFrameColumn::Kind::REDUCED;
		if (reads_fallback && fallback) {
			// The binder gave payload and fallback one type (BindDecideFrame): a point's value, a range's reducer
			// argument.
			auto &payload_type = kind == DecideFrameColumn::Kind::REDUCED
			                         ? value->Cast<BoundDecideReducerExpression>().children[0]->return_type
			                         : column.type;
			if (fallback->return_type != payload_type) {
				throw InternalException("DECIDE: %s reads a payload of type %s and an else of type %s",
				                        op.bound.frames[frame].text, payload_type.ToString(),
				                        fallback->return_type.ToString());
			}
			column.fallback = fallback->Copy();
		}
		columns.push_back(std::move(column));
		return columns.size() - 1;
	}

	//! The chunk column holding `value` read the way `kind` says for frame `frame`.
	unique_ptr<Expression> FrameColumnRef(DecideFrameColumn::Kind kind, idx_t frame, const Expression *value) {
		auto slot = FrameColumnSlot(kind, frame, value);
		auto name = op.bound.frames[frame].spelling;
		AddDecideTag(name, FRAME_COLUMN_TAG);
		return make_uniq<BoundReferenceExpression>(std::move(name), frame_reads.columns[slot].type,
		                                           frame_reads.column_base + slot);
	}

	//! `factor * column`, the factor a frame term stands under (1 for a bare frame).
	unique_ptr<Expression> ScaleFrameColumn(const Expression &factor, unique_ptr<Expression> column) const {
		if (factor.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			auto &value = factor.Cast<BoundConstantExpression>().value;
			if (!value.IsNull() && value.type().IsNumeric() &&
			    value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() == 1.0) {
				return column;
			}
		}
		vector<unique_ptr<Expression>> children;
		children.push_back(factor.Copy());
		children.push_back(std::move(column));
		return DecideRebindOperator(context, "*", std::move(children));
	}

	//! A frame over decisions, `coefficient * at(F: payload)`, as its terms: each term of its payload read at the row
	//! the frame selects -- the decision's instance there, its coefficient a READ column -- and each term of its
	//! `else` read at the row itself, its coefficient a FALLBACK column; each times the frame's factor.
	void AppendFrameTerms(DecideSplitTerm &term, vector<DecideTerm> &out) {
		auto frame = term.frame;
		if (frame >= op.bound.frames.size() || !term.coefficient) {
			throw InternalException("DECIDE: a frame marker names no frame of the plan");
		}
		// A range over decisions became a helper decision before any rewrite (RewriteFrameRanges).
		if (op.bound.frames[frame].range) {
			throw InternalException("DECIDE: a range frame over decisions reached term extraction: %s",
			                        op.bound.frames[frame].text);
		}
		for (auto &inner : term.inner) {
			AppendFrameColumnTerm(frame, DecideFrameColumn::Kind::READ, inner, *term.coefficient, term.sign, out);
			if (inner.var_a != DConstants::INVALID_INDEX) {
				out.back().frame_read = frame;
			}
		}
		AppendFallbackTerms(frame, *term.coefficient, term.sign, out);
	}

	//! `factor * c * v` for the term `c * v` of frame `frame`'s payload or `else`, its coefficient `c` read through
	//! the frame column of `kind`; the column's index.
	idx_t AppendFrameColumnTerm(idx_t frame, DecideFrameColumn::Kind kind, DecideSplitTerm &inner,
	                            const Expression &factor, int sign, vector<DecideTerm> &out) {
		if (inner.opaque) {
			ThrowOpaqueTerm(inner);
		}
		if (inner.degree > 1) {
			throw InternalException("DECIDE: %s reads a term that is not linear", op.bound.frames[frame].text);
		}
		// A null coefficient is 1 (DecideSplitTerm): the column reads it as written.
		if (!inner.coefficient) {
			inner.coefficient = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		}
		auto slot = FrameColumnSlot(kind, frame, inner.coefficient.get());
		auto column = FrameColumnRef(kind, frame, inner.coefficient.get());
		out.push_back(DecideTerm(inner.var_a, ScaleFrameColumn(factor, std::move(column)), sign * inner.sign));
		return slot;
	}

	//! A frame's `else` as its terms, each read at the anchor -- its coefficient a FALLBACK column, 0 where no position
	//! is missing -- each times `factor`, and its decisions recorded (DecideFrameReads::fallback_decisions).
	//! Nothing for a frame with none.
	void AppendFallbackTerms(idx_t frame, const Expression &factor, int sign, vector<DecideTerm> &out) {
		auto &fallback_decisions = frame_reads.fallback_decisions;
		fallback_decisions.resize(op.bound.frames.size());
		auto &spec = op.bound.frames[frame];
		if (!spec.fallback) {
			return;
		}
		vector<DecideSplitTerm> fallback;
		splitter.Split(*spec.fallback, 1, fallback);
		for (auto &inner : fallback) {
			AppendFrameColumnTerm(frame, DecideFrameColumn::Kind::FALLBACK, inner, factor, sign, out);
			auto &into = fallback_decisions[frame];
			if (inner.var_a != DConstants::INVALID_INDEX &&
			    std::find(into.begin(), into.end(), inner.var_a) == into.end()) {
				into.push_back(inner.var_a);
			}
		}
	}

	//! What a signature keys each frame's reached `else` by (DecideFrameReads::fallback_columns): the
	//! FALLBACK column of each of its terms, which the frame's own reading may already have registered
	//! (FrameColumnSlot keeps each once).
	void RecordFallbackColumns() {
		auto &columns = frame_reads.fallback_columns;
		columns.assign(op.bound.frames.size(), vector<idx_t>());
		frame_reads.fallback_decisions.resize(op.bound.frames.size());
		const BoundConstantExpression one(Value::INTEGER(1));
		for (idx_t f = 0; f < op.bound.frames.size(); f++) {
			vector<DecideTerm> terms;
			AppendFallbackTerms(f, one, 1, terms);
			for (auto &term : terms) {
				auto &ref = term.coefficient->Cast<BoundReferenceExpression>();
				columns[f].push_back(ref.index - frame_reads.column_base);
			}
		}
	}

	//! Replaces each frame in `expr` by the column it reads: a frame over data by its VALUE (its payload at the
	//! selected row, or its `else` at the row itself, NULL where neither stands), and a frame over decisions under a
	//! reducer's NULL test (DecideCanonicalizer::SkipNullData) by its DEFINED column. Every other frame over decisions
	//! became its terms.
	void LowerFrames(unique_ptr<Expression> &expr) {
		if (!expr) {
			return;
		}
		idx_t frame, aggregate;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER &&
		    TryParseFrameAggregateTag(expr->GetAlias(), aggregate)) {
			// A reducer over data reading a frame (LogicalDecide::frame_aggregates): its frames first; once every
			// frame has its columns, a column of its own after them (lower_aggregates).
			auto &reducer = expr->Cast<BoundDecideReducerExpression>();
			for (auto &child : reducer.children) {
				LowerFrames(child);
			}
			LowerFrames(reducer.filter);
			if (lower_aggregates) {
				expr = FrameAggregateRef(aggregate, *expr);
			}
			return;
		}
		if (expr->GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
			auto &test = expr->Cast<BoundOperatorExpression>();
			if (test.children.size() == 1 && TryGetDecideFrame(*test.children[0], frame) &&
			    FindDecideVariable(*test.children[0]) != DConstants::INVALID_INDEX) {
				test.children[0] = FrameColumnRef(DecideFrameColumn::Kind::DEFINED, frame, nullptr);
				return;
			}
		}
		if (TryGetDecideFrame(*expr, frame)) {
			if (FindDecideVariable(*expr) != DConstants::INVALID_INDEX) {
				throw InternalException("DECIDE: a frame over decisions reached evaluation unextracted: %s",
				                        op.bound.frames[frame].text);
			}
			auto &marker = expr->Cast<BoundDecideReducerExpression>();
			// A point reads its payload at one row; a range reduces its payload's values, its `else` read where a
			// position is missing.
			auto value = op.bound.frames[frame].range
			                 ? FrameColumnRef(DecideFrameColumn::Kind::REDUCED, frame, &marker)
			                 : FrameColumnRef(DecideFrameColumn::Kind::VALUE, frame, marker.children[0].get());
			// The marker's parent was bound to the marker's type.
			expr = BoundCastExpression::AddCastToType(context, std::move(value), marker.return_type);
			return;
		}
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { LowerFrames(child); });
	}

	void LowerFrames(vector<unique_ptr<Expression>> &exprs) {
		for (auto &expr : exprs) {
			LowerFrames(expr);
		}
	}

	void LowerFrames(vector<DecideTerm> &terms) {
		for (auto &term : terms) {
			LowerFrames(term.coefficient);
			LowerFrames(term.filter);
		}
	}

	void LowerFrames(DecideConstraint &constraint) {
		LowerFrames(constraint.scope.when);
		LowerFrames(constraint.lhs_terms);
		LowerFrames(constraint.rhs_expr);
		for (auto *end : {&constraint.admitted_lower, &constraint.admitted_upper, &constraint.admitted_a,
		                  &constraint.admitted_r, &constraint.admitted_member}) {
			LowerFrames(*end);
		}
		LowerFrames(constraint.product_groups);
	}

	void LowerFrames(vector<DecideProductGroup> &groups) {
		for (auto &group : groups) {
			LowerFrames(group.weight);
			LowerFrames(group.filter);
			LowerFrames(group.left);
			LowerFrames(group.right);
		}
	}

	//! Every expression execution evaluates, over the columns frames read (LowerFrames).
	//! The column of frame aggregate `index` (made when new, after every frame column), as the reducer `marker`'s
	//! parent reads it: of its type, and named by the reducer as written.
	unique_ptr<Expression> FrameAggregateRef(idx_t index, const Expression &marker) {
		auto &columns = frame_reads.columns;
		auto slot = frame_aggregate_slots.find(index);
		if (slot == frame_aggregate_slots.end()) {
			auto &record = op.frame_aggregates[index];
			DecideFrameColumn column;
			column.kind = DecideFrameColumn::Kind::AGGREGATE;
			column.value = FrameColumnValue(marker);
			column.type = marker.return_type;
			column.key = record.key;
			column.class_key = record.class_key;
			column.text = record.text;
			columns.push_back(std::move(column));
			slot = frame_aggregate_slots.emplace(index, columns.size() - 1).first;
		}
		auto name = op.frame_aggregates[index].text;
		AddDecideTag(name, FRAME_COLUMN_TAG);
		auto column = make_uniq<BoundReferenceExpression>(std::move(name), marker.return_type,
		                                                  frame_reads.column_base + slot->second);
		// It equals the reducer in value, so a message quotes it as the reducer was written (the location rule).
		DecideReplaces(*column, marker);
		return std::move(column);
	}

	void LowerFramesInPreparedModel() {
		if (op.bound.frames.empty()) {
			return;
		}
		// Twice: the frames first, then each reducer over data reading one, whose column follows every frame's.
		lower_aggregates = false;
		LowerFramesOnce();
		if (!op.frame_aggregates.empty()) {
			lower_aggregates = true;
			LowerFramesOnce();
		}
		RecordFallbackColumns();
	}

	void LowerFramesOnce() {
		for (auto &clause : lowered_clauses) {
			LowerFrames(*clause.second);
		}
		if (lowered_objective) {
			LowerFrames(lowered_objective->when_condition);
			LowerFrames(lowered_objective->terms);
			LowerFrames(lowered_objective->product_groups);
		}
		for (idx_t l = 0; l < link_terms.size(); l++) {
			LowerFrames(link_terms[l]);
			LowerFrames(link_fallbacks[l]);
			LowerFrames(op.reducer_value_links[l].filter);
			LowerFrames(op.reducer_value_links[l].when);
		}
	}

	//===------------------------------------------------------------------===//
	// Like-term collection
	//===------------------------------------------------------------------===//

	//! Whether two terms describe the same contribution and may be summed into one.
	//!
	//! Naming the same variable is not enough. A term also carries *which rows it
	//! applies to* and *which reducer produced it*, and two terms that disagree on
	//! any of that are different contributions that happen to share a column:
	//!
	//! - `reduction` separates a reducer term from a row-invariant one. `SUM(x)` and
	//!   a query-wide `x` in the same clause are summed differently downstream.
	//! - `filter` is the reducer's own `when`. `sum(when a: x)` and `sum(when b: x)`
	//!   name one column over two row sets; merging them would apply one mask to both.
	//! - `avg_scale` divides by the group's row count, so an AVG term and a SUM term
	//!   are not summable before that scaling happens.
	//! - `qualifier_scope_idx` selects a de-duplication mask (`sum(per D: ...)`), and
	//!   `qualifier_reducer` distinguishes occurrences that reuse that scope.
	//!
	//! Constants (`INVALID_INDEX`) are deliberately left alone: they are a fixed
	//! offset folded into the RHS, not a repeated column, and they are not what any
	//! consumer iterating `variable_indices` can trip over.
	static bool TermsAreLike(const DecideTerm &a, const DecideTerm &b) {
		if (a.variable_index == DConstants::INVALID_INDEX) {
			return false;
		}
		// `avg_group_key` too: an inlined `sum(per K: avg(e) by (G))` divides by each G
		// group's size, another AVG of the same decision by the whole relation's.
		// `frame_read` too: `x - at(previous: x)` names one decision at two rows.
		if (a.reduction != b.reduction || a.avg_scale != b.avg_scale ||
		    a.qualifier_scope_idx != b.qualifier_scope_idx || a.qualifier_reducer != b.qualifier_reducer ||
		    a.avg_group_key != b.avg_group_key || a.frame_read != b.frame_read) {
			return false;
		}
		if ((a.filter == nullptr) != (b.filter == nullptr)) {
			return false;
		}
		return !a.filter || DecideExpressionsEqual(*a.filter, *b.filter);
	}

	//! A NULL term of the objective is dropped (B1): its coefficient reads 0. This is
	//! what keeps a NULL from reaching the objective's solver row, wherever it comes
	//! from -- a factor on a reducer known only at run time (`(SELECT k) * SUM(p*x)`) or
	//! a term read once outside every reducer (`c * u`); a NULL inside a reducer is
	//! already out of its filter (DecideCanonicalizer::SkipNullData). It runs before like
	//! terms merge, which would let one NULL term drop another on the same decision. A
	//! product group's weight is guarded the same way; a form the group multiplies is not a
	//! sum of terms, so a NULL inside it is not read as 0.
	void DropNullObjectiveTerms(DecideObjective &objective) const {
		auto zero_if_null = [&](unique_ptr<Expression> &coefficient) {
			if (!coefficient || coefficient->IsFoldable()) {
				return;
			}
			auto type = coefficient->return_type;
			auto guarded = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, type);
			guarded->children.push_back(std::move(coefficient));
			guarded->children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(0).DefaultCastAs(type)));
			guarded->SetAlias(NULL_GUARD_TAG);
			coefficient = std::move(guarded);
		};
		for (auto &term : objective.terms) {
			zero_if_null(term.coefficient);
		}
		for (auto &group : objective.product_groups) {
			zero_if_null(group.weight);
		}
	}

	//! Sum like terms into one, in place, preserving first-occurrence order.
	//!
	//! `2*ship + 3*ship` used to reach the solver as two terms naming one column. The
	//! model builder folded the duplicate when writing the matrix row, so the emitted
	//! model was always right -- but every other consumer had to remember that an
	//! index can repeat, and one of them did not: the implied-bound derivation read a
	//! single term's coefficient instead of the sum (fixed 2026-08-15 by a defensive
	//! accumulate, which stays). Collecting here removes the trap at its source.
	//!
	//! A NULL coefficient merges to NULL, so a NULL objective term is made 0 first
	//! (DropNullObjectiveTerms); in a constraint the NULL is the instance's and skips it.
	//!
	//! A term contributes `sign * coefficient`, so a group merges as
	//! `sign_first * (coef_first ± coef_next ± ...)`, taking `-` exactly when a term's
	//! sign differs from the group's. The result is bound through `FunctionBinder`
	//! like every other rebuilt coefficient.
	void CollectLikeTerms(vector<DecideTerm> &terms) const {
		if (terms.size() < 2) {
			return;
		}
		vector<DecideTerm> out;
		out.reserve(terms.size());
		// Bucket by variable so the scan for a match stays short on a wide clause.
		unordered_map<idx_t, vector<idx_t>> candidates;
		for (auto &term : terms) {
			idx_t target = DConstants::INVALID_INDEX;
			auto bucket = candidates.find(term.variable_index);
			if (bucket != candidates.end()) {
				for (auto slot : bucket->second) {
					if (TermsAreLike(out[slot], term)) {
						target = slot;
						break;
					}
				}
			}
			if (target == DConstants::INVALID_INDEX) {
				candidates[term.variable_index].push_back(out.size());
				out.push_back(std::move(term));
				continue;
			}
			// `(a + b) * x` replaces the sum `a * x + b * x`. DeciDB made it, so it has no location of its own.
			auto &merged = out[target];
			merged.coefficient = DecideMergeLikeCoefficients(context, merged.sign, std::move(merged.coefficient),
			                                                 term.sign, std::move(term.coefficient));
		}
		terms = std::move(out);
	}

	//===------------------------------------------------------------------===//
	// Reducer metadata and peeled scales
	//===------------------------------------------------------------------===//

	//! Keyed scope a reducer is qualified by (`sum(per D: ...)`); INVALID_INDEX when the reducer is unqualified.
	static idx_t QualifierScopeOf(const BoundDecideReducerExpression &agg) {
		idx_t scope_idx = DConstants::INVALID_INDEX;
		DecideReducerPer(agg, scope_idx);
		return scope_idx;
	}

	//! A reducer's count scale, qualifier and reduction on every term or product group extracted from its body since
	//! `begin`.
	template <class TERM>
	static void ApplyAggregateMetadata(vector<TERM> &items, idx_t begin, const BoundDecideReducerExpression &agg) {
		bool is_avg = DecideReducerCountScale(agg);
		idx_t qualifier_scope = QualifierScopeOf(agg);
		for (idx_t i = begin; i < items.size(); i++) {
			items[i].avg_scale = is_avg;
			items[i].qualifier_scope_idx = qualifier_scope;
			items[i].reduction = LinearTermReduction::SUM;
		}
	}

	//! An objective AVG's `by` key on every term or product group extracted from its body since `begin`: it divides
	//! each row by its group's count, not the objective's (DecideTerm::avg_group_key).
	template <class TERM>
	static void ApplyAvgGroupKey(vector<TERM> &items, idx_t begin, const BoundDecideReducerExpression &agg) {
		const idx_t avg_group_key =
		    DecideReducerCountScale(agg) ? DecideReducerGroupKey(agg) : DConstants::INVALID_INDEX;
		for (idx_t i = begin; i < items.size(); i++) {
			items[i].avg_group_key = avg_group_key;
		}
	}

	//! Attach a reducer's own `when` to every term extracted from its body since the given
	//! marks: linear terms and product groups alike. It is done here, once,
	//! after extraction, rather than threaded through every extraction branch: a branch that
	//! forgot it -- the decision-free cast arm once did -- left one term unfiltered, and the
	//! executor then read every kept row as a member of an easy MIN/MAX, so the rows the
	//! reducer dropped reached the model with the body's constant as their whole left side.
	static void AttachReducerFilter(DecideConstraint &constraint, idx_t linear_before, idx_t group_before,
	                                const Expression *filter) {
		if (!filter) {
			return;
		}
		for (idx_t i = linear_before; i < constraint.lhs_terms.size(); i++) {
			constraint.lhs_terms[i].filter = filter->Copy();
		}
		for (idx_t i = group_before; i < constraint.product_groups.size(); i++) {
			constraint.product_groups[i].filter = filter->Copy();
		}
	}

	//! What the plan knows of each group's weight sign, read once its reducer's factor is in (IsNonconvexProductGroup).
	void SettleWeightSigns(vector<DecideProductGroup> &groups) const {
		for (auto &group : groups) {
			group.weight_sign = group.sign * ScaleSignAtPlanTime(context, group.weight.get(), false);
		}
	}

	//! Multiply a coefficient by a factor that stayed outside a reducer, keeping the
	//! operand types the original `*` / `/` node was bound for and then binding the
	//! rebuilt node through `FunctionBinder`. Casting first preserves the division
	//! semantics the user's expression was bound with; binding after is what removes
	//! the need to reuse another node's `FunctionData`. A missing coefficient is 1.
	void ScaleCoefficient(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                      unique_ptr<Expression> &coef) const {
		if (!coef) {
			coef = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		}
		const auto &coef_type = scale_func.function.arguments[divides ? 0 : 1];
		const auto &scale_type = scale_func.function.arguments[divides ? 1 : 0];
		vector<unique_ptr<Expression>> children;
		// `scale * coef` keeps the factor on the left, matching the canonical
		// spelling; `coef / scale` has to keep the operand order division needs.
		if (divides) {
			children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), coef_type));
			children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), scale_type));
		} else {
			children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), scale_type));
			children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), coef_type));
		}
		coef = DecideRebindOperator(context, divides ? "/" : "*", std::move(children));
	}

	//! Multiply everything the aggregate under a peeled scale just produced by that
	//! scale. Folding the factor into the reducer's body is what the canonicalizer
	//! refuses to do, because at the parsed level the aggregate may still be MIN/MAX
	//! and `MAX(-2x)` is `-2*MIN(x)`, not `-2*MAX(x)`. Here it is safe and exact: the
	//! optimizer has already rewritten every MIN/MAX to SUM (asserted below), and a
	//! sum distributes over any factor regardless of sign.
	void ApplyScaleToExtracted(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                           vector<DecideTerm> &terms, vector<DecideProductGroup> &groups, idx_t linear_before,
	                           idx_t group_before) {
		for (idx_t i = linear_before; i < terms.size(); i++) {
			ScaleCoefficient(scale_func, scale, divides, terms[i].coefficient);
		}
		for (idx_t i = group_before; i < groups.size(); i++) {
			ScaleCoefficient(scale_func, scale, divides, groups[i].weight);
		}
	}

	//! Whether decision `var_idx` is query-wide: declared `per ()`, or a helper decision holding a reducer with no `by`
	//! (`power(sum(x) - 3, 2)` reads the helper of `sum(x)`), one group of every row. No binder scope has the empty
	//! key, so a keyed scope with it is a helper's. A decision that has several instances over the kept rows is refused
	//! when the objective is read (DecideObjectiveTwice).
	bool IsQueryWideDecision(idx_t var_idx) const {
		auto &scope = op.bound.variable_scopes[var_idx];
		return scope.IsGlobal() ||
		       (scope.IsKeyed() && op.bound.keyed_scopes[scope.keyed_scope_idx].key == DConstants::INVALID_INDEX);
	}

	//! Every decision `expr` reads is query-wide: one column each, whichever row reads it.
	bool ReadsOnlyQueryWideDecisions(const Expression &expr) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			idx_t var_idx = FindDecideVariable(expr);
			// A data factor of a product outside a reducer is peeled as the atom's scale (ReadCanonicalAtoms), so the
			// columns left here are decisions.
			if (var_idx == DConstants::INVALID_INDEX) {
				throw InternalException("DECIDE objective term '%s' holds a data column beside a product of decisions",
				                        expr.ToString());
			}
			return IsQueryWideDecision(var_idx);
		}
		bool all = true;
		EnumerateChildrenConst(
		    expr, [&](unique_ptr<Expression> &child) { all = all && ReadsOnlyQueryWideDecisions(*child); });
		return all;
	}

	//! Names, on every term and product group `agg`'s body produced since the marks, the tree node it ranges over
	//! (DecideTerm::child) and the reducer whose `per` qualifies it; a group's forms too.
	void Stamp(const BoundDecideReducerExpression &agg, vector<DecideTerm> &terms, vector<DecideProductGroup> &groups,
	           idx_t linear, idx_t group_before) const {
		const auto node = tree.ReducerChild(agg);
		const auto qualifier_reducer =
		    QualifierScopeOf(agg) == DConstants::INVALID_INDEX ? DConstants::INVALID_INDEX : tree.ReducerNode(agg);
		auto stamp = [&](DecideTerm &term) {
			term.child = node;
			if (qualifier_reducer != DConstants::INVALID_INDEX) {
				term.qualifier_reducer = qualifier_reducer;
			}
		};
		for (idx_t i = linear; i < terms.size(); i++) {
			stamp(terms[i]);
		}
		for (idx_t i = group_before; i < groups.size(); i++) {
			auto &group = groups[i];
			group.child = node;
			if (qualifier_reducer != DConstants::INVALID_INDEX) {
				group.qualifier_reducer = qualifier_reducer;
			}
			for (auto *form : {&group.left, &group.right}) {
				for (auto &term : *form) {
					stamp(term);
				}
			}
		}
	}

	//===------------------------------------------------------------------===//
	// Constraints
	//===------------------------------------------------------------------===//

	//! The left side of an aggregate constraint, one canonical atom at a time: a reducer, a reducer with the
	//! factor canonicalization left on it, or a tuple term beside the reducers.
	void ExtractAggregateConstraintTerms(const Expression &lhs, DecideConstraint &constraint) {
		for (auto &atom : ReadCanonicalAtoms(lhs, decide_index)) {
			auto &expr = *atom.term;
			if (atom.scaled) {
				auto scale = atom.scale.scale;
				// Defensive invariant: both user-written and optimizer-generated
				// constraints pass canonical validation. Keep this guard so an
				// in-place optimizer mutation cannot turn a decision into a coefficient
				// and crash during evaluation.
				if (FindDecideVariable(*scale) != DConstants::INVALID_INDEX) {
					throw InternalException("DECIDE constraint: '%s' is a decision, so it cannot multiply an "
					                        "aggregate. Only data can scale SUM/AVG/MIN/MAX.",
					                        scale->ToString());
				}
				idx_t linear_before = constraint.lhs_terms.size();
				idx_t group_before = constraint.product_groups.size();
				ExtractReducerConstraintTerms(*atom.scale.aggregate, constraint, atom.sign);
				ApplyScaleToExtracted(*atom.scale.function, *scale, atom.scale.divides, constraint.lhs_terms,
				                      constraint.product_groups, linear_before, group_before);
				continue;
			}
			// A tuple term beside the reducers: the canonicalizer admitted it as one value per group
			// (ClassifyScoped), and execution counts it once in each group's row (CountOnce). A decision inside a
			// reducer body (`SUM(cost * cap)`) is the reducer's, read below with the coefficient of each row.
			if (!IsReducer(expr)) {
				ExtractConstraintTerms(expr, constraint, atom.sign);
				continue;
			}
			ExtractReducerConstraintTerms(expr.Cast<BoundDecideReducerExpression>(), constraint, atom.sign);
		}
	}

	void ExtractReducerConstraintTerms(const BoundDecideReducerExpression &agg, DecideConstraint &constraint,
	                                   int sign) {
		auto agg_name = StringUtil::Lower(agg.function.name);
		if (agg_name != "sum") {
			throw InternalException("DECIDE optimizer did not rewrite aggregate '%s' to SUM before execution",
			                        agg.function.name);
		}
		idx_t linear_before = constraint.lhs_terms.size();
		idx_t group_before = constraint.product_groups.size();
		ExtractConstraintTerms(*agg.children[0], constraint, sign);
		AttachReducerFilter(constraint, linear_before, group_before, agg.filter.get());
		ApplyAggregateMetadata(constraint.lhs_terms, linear_before, agg);
		ApplyAggregateMetadata(constraint.product_groups, group_before, agg);
		Stamp(agg, constraint.lhs_terms, constraint.product_groups, linear_before, group_before);
	}

	//! The per-row terms of a scoped easy MIN/MAX, `[-] [s *] max(when c: e)`: the terms of `e`,
	//! each filtered by `c`, scaled by `s` and signed.
	void ExtractEasyExtremumTerms(const Expression &lhs, DecideConstraint &constraint) {
		auto atoms = ReadCanonicalAtoms(lhs, decide_index);
		const BoundDecideReducerExpression *agg = nullptr;
		if (atoms.size() == 1 && atoms[0].scaled) {
			agg = atoms[0].scale.aggregate;
		} else if (atoms.size() == 1) {
			auto &term = *UnwrapDecideCasts(*atoms[0].term, decide_index);
			agg = term.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER
			          ? &term.Cast<BoundDecideReducerExpression>()
			          : nullptr;
		}
		if (!agg || agg->children.size() != 1) {
			throw InternalException("DECIDE scoped MIN/MAX reached term extraction without its reducer: %s",
			                        lhs.ToString());
		}
		auto &atom = atoms[0];
		D_ASSERT(constraint.lhs_terms.empty() && constraint.product_groups.empty());
		ExtractConstraintTerms(*agg->children[0], constraint, atom.sign);
		AttachReducerFilter(constraint, 0, 0, agg->filter.get());
		Stamp(*agg, constraint.lhs_terms, constraint.product_groups, 0, 0);
		if (atom.scaled) {
			ApplyScaleToExtracted(*atom.scale.function, *atom.scale.scale, atom.scale.divides, constraint.lhs_terms,
			                      constraint.product_groups, 0, 0);
		}
	}

	//! A reducer body or a per-row left side, split by the shared splitter into the constraint's linear terms and one
	//! product group per term of degree 2; a frame over decisions as the terms it reads.
	void ExtractConstraintTerms(const Expression &expr, DecideConstraint &constr, int sign) {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, sign, split);
		for (auto &term : split) {
			if (term.IsFrame()) {
				AppendFrameTerms(term, constr.lhs_terms);
				continue;
			}
			if (term.opaque) {
				ThrowOpaqueTerm(term);
			}
			if (term.degree <= 1) {
				constr.lhs_terms.push_back(DecideTerm {term.var_a, std::move(term.coefficient), term.sign});
				continue;
			}
			constr.product_groups.push_back(ReadProductGroup(term));
		}
	}

	//! One clause, or one row a function owns.
	void AnalyzeConstraint(const BoundDecideClause &clause) {
		auto &expr = *clause.body;
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::BOUND_COMPARISON: {
			auto &written_comp = expr.Cast<BoundComparisonExpression>();
			BoundDecideClause canonical_envelope;
			const BoundDecideClause *canonical = &clause;
			idx_t abs_aux = DConstants::INVALID_INDEX;
			idx_t l0_value = DConstants::INVALID_INDEX;
			if (TryParseAbsFunctionEnvelopeTag(written_comp.GetAlias(), abs_aux) ||
			    TryParseDecideIndexTag(written_comp.GetAlias(), L0_INDICATOR_FUNCTION_TAG_PREFIX, l0_value) ||
			    HasDecideTag(written_comp.GetAlias(), BODY_DEFINITION_TAG)) {
				// Function-owned links are generated in their natural form and can put decision terms on both sides.
				// Canonicalize their linear form like an ordinary constraint, but keep `expr` as the clause identity so
				// it still joins the root built from the optimizer metadata.
				canonical_envelope = canonicalizer.CanonicalizeFunctionRow(clause);
				canonical = &canonical_envelope;
			}
			const auto *comp = &canonical->body->Cast<BoundComparisonExpression>();

			// Skip comparisons the optimizer already folded into the column box.
			// Emitting a DecideConstraint here would add num_rows redundant model
			// rows. The comparison is still in the tree so EXPLAIN renders it; the
			// tag is the decision, and it was made by FoldSingletonRows. One whose
			// repair rows come from its clause is flattened aside, for the repair model
			// alone (singleton_row_constraints).
			const bool folded = clause.folded_bound;
			if (folded && !clause.folded_clause_rows) {
				break;
			}

			auto constraint = ReadDecideClauseFacts(context, op, *canonical);
			Expression *lhs = UnwrapDecideCasts(*comp->left, decide_index);

			if (constraint->was_minmax_easy) {
				// The easy direction of a MIN/MAX (`max(when c: e) by (g) <= cap`)
				// holds when every member of its reducer does: one row per member, the
				// reducer's `when` choosing the members, while the bound is reduced over
				// each `by` group of every kept row. RewriteMinMax kept the reducer so its
				// `when` and `by` reach this point; a factor on it distributes over the rows.
				constraint->lhs_is_aggregate = false;
				ExtractEasyExtremumTerms(*lhs, *constraint);
			} else if (constraint->lhs_is_aggregate) {
				// Aggregate constraint. Handles both legacy single aggregates and
				// additive aggregate expressions whose reducers carry their own `when`. The
				// classification comes from the canonical boundary rather than aggregate
				// presence alone, so a data-only RHS reducer cannot change row semantics.
				constraint->lhs_is_aggregate = true;
				ExtractAggregateConstraintTerms(*lhs, *constraint);
			} else {
				// Per-row constraint (e.g., x <= 5, or multi-variable: d >= x - c)
				constraint->lhs_is_aggregate = false;

				if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					// Simple single-variable constraint (e.g., x <= 5)
					constraint->lhs_terms.push_back(
					    DecideTerm {FindDecideVariable(*lhs),
					                make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))});
				} else {
					// Multi-variable per-row constraint with complex LHS
					// (e.g., z_0 + z_1 = 1, or x + (-3)*z_0 + (-5)*z_1 = 0,
					//  or POWER(x - target, 2) <= K quadratic constraint)
					ExtractConstraintTerms(*lhs, *constraint, 1);
				}
			}

			lowered_clauses.emplace_back(LoweredClause {&expr, folded}, std::move(constraint));
			break;
		}

		default:
			break;
		}
	}

	//===------------------------------------------------------------------===//
	// Objective
	//===------------------------------------------------------------------===//

	//! A SUM argument, split by the shared splitter into the objective's linear terms and one product group per term of
	//! degree 2; a frame over decisions as the terms it reads.
	void ExtractObjectiveBodyTerms(const Expression &expr, DecideObjective &obj, int sign,
	                               const Expression *filter = nullptr) {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, sign, split);
		for (auto &term : split) {
			if (term.IsFrame()) {
				idx_t before = obj.terms.size();
				AppendFrameTerms(term, obj.terms);
				for (idx_t i = before; i < obj.terms.size() && filter; i++) {
					obj.terms[i].filter = filter->Copy();
				}
				continue;
			}
			if (term.opaque) {
				ThrowOpaqueTerm(term);
			}
			if (term.degree > 1) {
				obj.product_groups.push_back(ReadProductGroup(term));
				if (filter) {
					obj.product_groups.back().filter = filter->Copy();
				}
				continue;
			}
			DecideTerm linear {term.var_a, std::move(term.coefficient), term.sign};
			if (filter) {
				linear.filter = filter->Copy();
			}
			obj.terms.push_back(std::move(linear));
		}
	}

	//! The objective, one canonical atom at a time, as for an aggregate constraint's left side.
	void ExtractAggregateObjectiveTerms(const Expression &objective_expr, DecideObjective &obj) {
		for (auto &atom : ReadCanonicalAtoms(objective_expr, decide_index)) {
			auto &expr = *atom.term;
			// A factor left outside a reducer (`2 * SUM(x*p)`): extract the reducer, then
			// multiply the factor into everything it produced.
			if (atom.scaled) {
				auto obj_scale = atom.scale.scale;
				idx_t linear_before = obj.terms.size();
				idx_t group_before = obj.product_groups.size();
				ExtractReducerObjectiveTerms(*atom.scale.aggregate, obj, atom.sign);
				ApplyScaleToExtracted(*atom.scale.function, *obj_scale, atom.scale.divides, obj.terms,
				                      obj.product_groups, linear_before, group_before);
				continue;
			}
			// The objective reads a decision outside its reducers once, on the first kept row
			// (a scope check made sure it has one instance there), a query-wide one included.
			// A product or a square of decisions has no once-only reading in the model -- except
			// one of query-wide decisions (`s * u`, `(s - 3) * (s - 3)`), one `NONE` group execution
			// counts once (CountOnce in decide_value_extraction.cpp). A frame's marker is a tuple term read at
			// another row, not a reducer, and splits here too.
			if (!IsReducer(expr)) {
				// A reducer left in a term that is not itself one (`sum(x) * max(x)`) was multiplied by another
				// decision-bearing factor, which no rewrite held in a helper decision: the term is a product of
				// decisions outside a reducer, and its reducers cannot be read as terms.
				const bool holds_reducer = DecideCanonicalizer::ContainsReducer(expr);
				idx_t group_before = obj.product_groups.size();
				if (!holds_reducer) {
					ExtractObjectiveBodyTerms(expr, obj, atom.sign, nullptr);
				}
				if (holds_reducer ||
				    (obj.product_groups.size() != group_before && !ReadsOnlyQueryWideDecisions(expr))) {
					throw NotImplementedException(
					    expr, "'%s' multiplies decisions outside a reducer, which is not supported",
					    RenderDecideSource(expr, op.bound.source_text, op.bound.keyed_scopes, &op.bound.keys));
				}
				continue;
			}
			ExtractReducerObjectiveTerms(expr.Cast<BoundDecideReducerExpression>(), obj, atom.sign);
		}
	}

	void ExtractReducerObjectiveTerms(const BoundDecideReducerExpression &agg, DecideObjective &obj, int sign) {
		auto agg_name = StringUtil::Lower(agg.function.name);
		if (agg_name != "sum") {
			throw InternalException("DECIDE optimizer should rewrite objective aggregate '%s' to SUM before execution",
			                        agg.function.name);
		}
		idx_t before = obj.terms.size();
		idx_t group_before = obj.product_groups.size();
		ExtractObjectiveBodyTerms(*agg.children[0], obj, sign, agg.filter.get());
		ApplyAggregateMetadata(obj.terms, before, agg);
		ApplyAggregateMetadata(obj.product_groups, group_before, agg);
		ApplyAvgGroupKey(obj.terms, before, agg);
		ApplyAvgGroupKey(obj.product_groups, group_before, agg);
		Stamp(agg, obj.terms, obj.product_groups, before, group_before);
	}

	void AnalyzeObjective(const BoundDecideClause &clause) {
		auto *expr = UnwrapDecideCasts(*clause.body, decide_index);
		auto when_cond = clause.scope.when ? clause.scope.when->Copy() : nullptr;

		if (IsReducer(*expr)) {
			auto &agg = expr->Cast<BoundDecideReducerExpression>();

			lowered_objective = make_uniq<DecideObjective>();
			auto &objective = lowered_objective;

			// Walk the SUM argument: its linear terms and its product groups, a mixed body
			// (SUM(POWER(x-t, 2) + penalty*x)) included.
			ExtractObjectiveBodyTerms(*agg.children[0], *objective, 1, agg.filter.get());
			ApplyAggregateMetadata(objective->terms, 0, agg);
			ApplyAggregateMetadata(objective->product_groups, 0, agg);
			ApplyAvgGroupKey(objective->terms, 0, agg);
			ApplyAvgGroupKey(objective->product_groups, 0, agg);
			Stamp(agg, objective->terms, objective->product_groups, 0, 0);

			objective->when_condition = std::move(when_cond);
		} else if (DecideCanonicalizer::ContainsReducer(*expr) ||
		           FindDecideVariable(*expr) != DConstants::INVALID_INDEX) {
			// The second arm covers an objective with no aggregate at all: decisions
			// outside every reducer (e.g. `minimize max_shortfall`), read once.
			lowered_objective = make_uniq<DecideObjective>();
			ExtractAggregateObjectiveTerms(*expr, *lowered_objective);
			lowered_objective->when_condition = std::move(when_cond);
		}
	}

private:
	ClientContext &context;
	LogicalDecide &op;
	//! The tree under construction, which names the G each reducer ranges over and takes each clause's records.
	DecideTreeConstruction &tree;
	//! A clause's comparison, and whether it is a folded bound's, which the repair model alone reads.
	struct LoweredClause {
		const Expression *leaf;
		bool singleton_row;
	};
	//! The clauses in leaf order and the objective, lowered, until Build hands them to the tree.
	vector<pair<LoweredClause, unique_ptr<DecideConstraint>>> lowered_clauses;
	unique_ptr<DecideObjective> lowered_objective;
	//! [link]: a helper's member terms and its frame's `else` terms.
	vector<vector<DecideTerm>> link_terms;
	vector<vector<DecideTerm>> link_fallbacks;
	//! The frame columns the lowering reads (DecideTree::frame_reads).
	DecideFrameReads frame_reads;
	idx_t decide_index;
	DecideTermSplitter splitter;
	DecideCanonicalizer canonicalizer;
	//! LowerFramesInPreparedModel's second pass: each reducer over data reading a frame becomes its column.
	bool lower_aggregates = false;
	//! Frame aggregate (LogicalDecide::frame_aggregates) -> its column (into DecideFrameReads::columns).
	unordered_map<idx_t, idx_t> frame_aggregate_slots;
};

void LowerDecideTree(ClientContext &context, LogicalDecide &decide, DecideTreeConstruction &tree) {
	DecideLinearFormBuilder builder(context, decide, tree);
	builder.Build();
}

} // namespace duckdb
