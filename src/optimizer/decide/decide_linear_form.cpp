#include "duckdb/optimizer/decide/decide_linear_form.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/execution/expression_executor.hpp"
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
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
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

//! The solver path reads what survives lowering: constant, linear, product and square terms. An unknown term is
//! raised as the error the split recorded for it.
[[noreturn]] static void ThrowUnknownTerm(const DecideSplitTerm &term) {
	if (term.user_error) {
		throw InvalidInputException(term.reason);
	}
	throw InternalException(term.reason);
}

//! Takes what a coefficient is computed from out of its term (DecideTerm::inputs): the values recorded when DeciDB
//! computed it, else the coefficient itself, as written.
static vector<unique_ptr<Expression>> TakeInputs(vector<unique_ptr<Expression>> &inputs,
                                                 const Expression &coefficient) {
	vector<unique_ptr<Expression>> taken;
	if (inputs.empty()) {
		taken.push_back(coefficient.Copy());
	}
	for (auto &input : inputs) {
		taken.push_back(std::move(input));
	}
	inputs.clear();
	return taken;
}

//! Whether `expr` is a reducer: an aggregate, a frame's marker excepted (a tuple term read at another row).
static bool IsReducer(const Expression &expr) {
	idx_t frame;
	return expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && !TryGetDecideFrame(expr, frame);
}

//! Collect DECIDE variable references from a bound expression, tracking sign
//! through subtraction operators. Used for multi-variable per-row constraints.
struct ExprVarRef {
	idx_t var_idx;
	int sign; // +1 or -1
};

//===--------------------------------------------------------------------===//
// DecideLinearFormBuilder
//===--------------------------------------------------------------------===//

//! Flattens the canonical constraint tree and the objective into the prepared
//! linear form. Everything it needs is a type or a structure: the decision
//! variable list, their scopes, and the tags earlier passes stamped on the tree.
//! It never reads a data row.
class DecideLinearFormBuilder {
public:
	DecideLinearFormBuilder(ClientContext &context, LogicalDecide &op)
	    : context(context), op(op), decide_index(op.decide_index),
	      canonicalizer(context, op.decide_index, op.variable_scopes, op.keyed_scopes, op.keys, op.is_boolean_var),
	      splitter(context, op.decide_index, op.decide_variables) {
		// A frame's values are columns after the input's (DecideFrameColumn).
		op.prepared.frame_column_base = op.children.empty() ? 0 : op.children[0]->types.size();
	}

	void Build() {
		if (op.decide_constraints) {
			ForEachScopedConstraintLeaf(
			    *op.decide_constraints,
			    [&](const Expression &leaf, const DecideEnclosingScope &scope) { AnalyzeConstraint(leaf, scope); });
		}
		if (op.decide_objective) {
			AnalyzeObjective(op.decide_objective);
		}
		// The composed MIN/MAX terms are emitted by RewriteComposedMinMax as bare
		// inner expressions. Flatten them here too, so stage 08 reads prepared terms
		// for every construct rather than for all but one.
		for (auto &spec : op.composed_minmax_constraints) {
			for (auto &term : spec.terms) {
				ExtractLinearTerms(*term.inner_expr, term.inner_terms);
			}
		}
		for (auto &term : op.composed_minmax_objective_terms) {
			ExtractLinearTerms(*term.inner_expr, term.inner_terms);
		}
		// The reducers the value rewrite lifted into helper decisions, the same way; a range frame's helper also reads
		// its `else`, at the window's anchor.
		const BoundConstantExpression one(Value::INTEGER(1));
		for (auto &link : op.reducer_value_links) {
			ExtractLinearTerms(*link.inner_expr, link.inner_terms);
			if (link.frame != DConstants::INVALID_INDEX) {
				AppendFallbackTerms(link.frame, one, 1, link.fallback_terms);
			}
		}

		// Group like terms, once, over every list that becomes a linear solver row.
		for (auto &constraint : op.prepared.constraints) {
			CollectLikeTerms(constraint->lhs_terms);
		}
		if (op.prepared.objective) {
			DropNullObjectiveTerms(*op.prepared.objective);
			CollectLikeTerms(op.prepared.objective->terms);
		}
		for (auto &spec : op.composed_minmax_constraints) {
			for (auto &term : spec.terms) {
				CollectLikeTerms(term.inner_terms);
			}
		}
		for (auto &term : op.composed_minmax_objective_terms) {
			CollectLikeTerms(term.inner_terms);
		}
		for (auto &link : op.reducer_value_links) {
			CollectLikeTerms(link.inner_terms);
			CollectLikeTerms(link.fallback_terms);
		}

		// Every frame left in an expression execution evaluates is over data -- a frame over
		// decisions became its terms above -- or the NULL test of a reducer's member, and
		// becomes the column it reads.
		LowerFramesInPreparedModel();

		// A recorded bound's repair rows come from its clause, so the repair model would
		// open its column with nothing enforcing it were its clause not flattened above.
		if (op.user_singleton_row_bounds.size() != op.prepared.singleton_row_constraints.size()) {
			throw InternalException("DECIDE: %llu singleton-row bounds take their repair rows from their clause, but "
			                        "%llu such clauses were flattened",
			                        op.user_singleton_row_bounds.size(), op.prepared.singleton_row_constraints.size());
		}
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
		splitter.SplitLinear(expr, split);
		AppendLinearTerms(split, out);
	}

	//! Terms of an expression that must be linear, as prepared terms; a frame over decisions as the terms it reads.
	void AppendLinearTerms(vector<DecideSplitTerm> &split, vector<DecideTerm> &out) {
		for (auto &term : split) {
			if (term.kind == DecideTermKind::UNKNOWN) {
				ThrowUnknownTerm(term);
			}
			if (term.kind == DecideTermKind::FRAME) {
				AppendFrameTerms(term, out);
				continue;
			}
			if (term.kind != DecideTermKind::CONSTANT && term.kind != DecideTermKind::LINEAR) {
				throw InternalException("DECIDE linear expression split into a non-linear term");
			}
			out.push_back(DecideTerm {term.var_a, std::move(term.coefficient), term.sign});
		}
	}

	void CollectDecideVarRefs(const Expression &expr, int sign, vector<ExprVarRef> &refs) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			idx_t var_idx = FindDecideVariable(expr);
			if (var_idx != DConstants::INVALID_INDEX) {
				refs.push_back({var_idx, sign});
			}
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = expr.Cast<BoundFunctionExpression>();
			if (func.function.name == "-" && func.children.size() == 2) {
				CollectDecideVarRefs(*func.children[0], sign, refs);
				CollectDecideVarRefs(*func.children[1], -sign, refs);
				return;
			}
			if (func.function.name == "+" && func.children.size() == 2) {
				CollectDecideVarRefs(*func.children[0], sign, refs);
				CollectDecideVarRefs(*func.children[1], sign, refs);
				return;
			}
			if (func.function.name == "*" && func.children.size() == 2) {
				// Multiplication: descend into both children to find decide variables.
				// Sign propagates unchanged — * doesn't flip algebraic sign, it changes
				// the coefficient magnitude, which this walk does not measure.
				CollectDecideVarRefs(*func.children[0], sign, refs);
				CollectDecideVarRefs(*func.children[1], sign, refs);
				return;
			}
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			CollectDecideVarRefs(*cast.child, sign, refs);
			return;
		}
		// Constants, data columns, etc.: no DECIDE vars
	}

	//! Whether `expr` holds a reducer -- a frame's marker is none.
	static bool BoundExpressionContainsAggregate(const Expression &expr) {
		idx_t frame;
		if (TryGetDecideFrame(expr, frame)) {
			return false;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			return true;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			return BoundExpressionContainsAggregate(*cast.child);
		}
		bool found = false;
		EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
			if (!found && child && BoundExpressionContainsAggregate(*child)) {
				found = true;
			}
		});
		return found;
	}

	//===------------------------------------------------------------------===//
	// Frame selectors (language/guide.md §5.6)
	//===------------------------------------------------------------------===//

	//! The frame column holding `value` read the way `kind` says for frame `frame`
	//! (DecideFrameColumn), registered once per distinct value; its index. A VALUE column
	//! also reads the frame's `else`.
	idx_t FrameColumnSlot(DecideFrameColumn::Kind kind, idx_t frame, const Expression *value) {
		auto &columns = op.prepared.frame_columns;
		for (idx_t i = 0; i < columns.size(); i++) {
			auto &column = columns[i];
			if (column.kind == kind && column.frame == frame &&
			    (column.value ? value && column.value->Equals(*value) : !value)) {
				return i;
			}
		}
		DecideFrameColumn column;
		column.kind = kind;
		column.frame = frame;
		column.value = value ? value->Copy() : nullptr;
		column.type = value ? value->return_type : LogicalType::BOOLEAN;
		auto &fallback = op.frames[frame].fallback;
		const bool reads_fallback = kind == DecideFrameColumn::Kind::VALUE || kind == DecideFrameColumn::Kind::REDUCED;
		if (reads_fallback && fallback) {
			// The binder gave payload and fallback one type (BindDecideFrame): a point's value, a range's reducer
			// argument.
			auto &payload_type = kind == DecideFrameColumn::Kind::REDUCED
			                         ? value->Cast<BoundAggregateExpression>().children[0]->return_type
			                         : column.type;
			if (fallback->return_type != payload_type) {
				throw InternalException("DECIDE: %s reads a payload of type %s and an else of type %s",
				                        op.frames[frame].text, payload_type.ToString(),
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
		auto name = op.frames[frame].text;
		AddDecideTag(name, FRAME_COLUMN_TAG);
		return make_uniq<BoundReferenceExpression>(std::move(name), op.prepared.frame_columns[slot].type,
		                                           op.prepared.frame_column_base + slot);
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
		if (frame >= op.frames.size() || !term.coefficient) {
			throw InternalException("DECIDE: a frame marker names no frame of the plan");
		}
		// A range over decisions became a helper decision before any rewrite (RewriteFrameRanges).
		if (op.frames[frame].range) {
			throw InternalException("DECIDE: a range frame over decisions reached term extraction: %s",
			                        op.frames[frame].text);
		}
		auto &payload_terms = op.prepared.frame_payload_terms;
		payload_terms.resize(op.frames.size());
		// Every read of the frame extracts the same terms: the payload is recorded once.
		const bool record = payload_terms[frame].empty();
		for (auto &inner : term.inner) {
			auto slot =
			    AppendFrameColumnTerm(frame, DecideFrameColumn::Kind::READ, inner, *term.coefficient, term.sign, out);
			if (inner.var_a != DConstants::INVALID_INDEX) {
				out.back().frame_read = frame;
			}
			if (record) {
				payload_terms[frame].push_back(DecideFramePayloadTerm {inner.var_a, slot, inner.sign});
			}
		}
		AppendFallbackTerms(frame, *term.coefficient, term.sign, out);
	}

	//! `factor * c * v` for the term `c * v` of frame `frame`'s payload or `else`, its coefficient `c` read through
	//! the frame column of `kind`; the column's index.
	idx_t AppendFrameColumnTerm(idx_t frame, DecideFrameColumn::Kind kind, DecideSplitTerm &inner,
	                            const Expression &factor, int sign, vector<DecideTerm> &out) {
		if (inner.kind == DecideTermKind::UNKNOWN) {
			ThrowUnknownTerm(inner);
		}
		if ((inner.kind != DecideTermKind::CONSTANT && inner.kind != DecideTermKind::LINEAR) || !inner.coefficient) {
			throw InternalException("DECIDE: %s reads a term that is not linear", op.frames[frame].text);
		}
		auto slot = FrameColumnSlot(kind, frame, inner.coefficient.get());
		auto column = FrameColumnRef(kind, frame, inner.coefficient.get());
		out.push_back(DecideTerm(inner.var_a, ScaleFrameColumn(factor, std::move(column)), sign * inner.sign));
		return slot;
	}

	//! A frame's `else` as its terms, each read at the anchor -- its coefficient a FALLBACK column, 0 where no position
	//! is missing -- each times `factor`, and its decisions recorded (DecidePreparedModel::frame_fallback_decisions).
	//! Nothing for a frame with none.
	void AppendFallbackTerms(idx_t frame, const Expression &factor, int sign, vector<DecideTerm> &out) {
		auto &fallback_decisions = op.prepared.frame_fallback_decisions;
		fallback_decisions.resize(op.frames.size());
		auto &spec = op.frames[frame];
		if (!spec.fallback) {
			return;
		}
		vector<DecideSplitTerm> fallback;
		splitter.SplitLinear(*spec.fallback, fallback);
		for (auto &inner : fallback) {
			AppendFrameColumnTerm(frame, DecideFrameColumn::Kind::FALLBACK, inner, factor, sign, out);
			auto &into = fallback_decisions[frame];
			if (inner.var_a != DConstants::INVALID_INDEX &&
			    std::find(into.begin(), into.end(), inner.var_a) == into.end()) {
				into.push_back(inner.var_a);
			}
		}
	}

	//! What a signature keys each frame's reached `else` by (DecidePreparedModel::frame_fallback_columns): the
	//! FALLBACK column of each of its terms, which the frame's own reading may already have registered
	//! (FrameColumnSlot keeps each once).
	void RecordFallbackColumns() {
		auto &columns = op.prepared.frame_fallback_columns;
		columns.assign(op.frames.size(), vector<idx_t>());
		op.prepared.frame_fallback_decisions.resize(op.frames.size());
		const BoundConstantExpression one(Value::INTEGER(1));
		for (idx_t f = 0; f < op.frames.size(); f++) {
			vector<DecideTerm> terms;
			AppendFallbackTerms(f, one, 1, terms);
			for (auto &term : terms) {
				auto &ref = term.coefficient->Cast<BoundReferenceExpression>();
				columns[f].push_back(ref.index - op.prepared.frame_column_base);
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
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
		    TryParseFrameAggregateTag(expr->GetAlias(), aggregate)) {
			// A reducer over data reading a frame (LogicalDecide::frame_aggregates): its frames first; once every
			// frame has its columns, a column of its own after them (lower_aggregates).
			auto &reducer = expr->Cast<BoundAggregateExpression>();
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
				                        op.frames[frame].text);
			}
			auto &marker = expr->Cast<BoundAggregateExpression>();
			// A point reads its payload at one row; a range reduces its payload's values, its `else` read where a
			// position is missing.
			auto value = op.frames[frame].range
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
			LowerFrames(term.inputs);
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
		for (auto &term : constraint.bilinear_terms) {
			LowerFrames(term.coefficient);
			LowerFrames(term.filter);
			LowerFrames(term.inputs);
		}
		for (auto &group : constraint.quadratic_groups) {
			LowerFrames(group.inner_terms);
			LowerFrames(group.scale);
			LowerFrames(group.filter);
		}
	}

	//! Every expression execution evaluates, over the columns frames read (LowerFrames).
	//! The column of frame aggregate `index` (made when new, after every frame column), as the reducer `marker`'s
	//! parent reads it: of its type, and named by the reducer as written.
	unique_ptr<Expression> FrameAggregateRef(idx_t index, const Expression &marker) {
		auto &columns = op.prepared.frame_columns;
		auto slot = frame_aggregate_slots.find(index);
		if (slot == frame_aggregate_slots.end()) {
			auto &record = op.frame_aggregates[index];
			DecideFrameColumn column;
			column.kind = DecideFrameColumn::Kind::AGGREGATE;
			column.value = marker.Copy();
			column.type = marker.return_type;
			column.key = record.key;
			column.class_key = record.class_key;
			column.text = record.text;
			column.norm_order = record.norm_order;
			columns.push_back(std::move(column));
			slot = frame_aggregate_slots.emplace(index, columns.size() - 1).first;
		}
		auto name = op.frame_aggregates[index].text;
		AddDecideTag(name, BOUND_REDUCER_TAG);
		AddDecideTag(name, FRAME_COLUMN_TAG);
		return make_uniq<BoundReferenceExpression>(std::move(name), marker.return_type,
		                                           op.prepared.frame_column_base + slot->second);
	}

	void LowerFramesInPreparedModel() {
		if (op.frames.empty()) {
			return;
		}
		// Twice: the frames first, then each reducer over data reading one, whose column follows every frame's.
		lower_aggregates = false;
		LowerFramesOnce();
		if (!op.frame_aggregates.empty()) {
			lower_aggregates = true;
			LowerFramesOnce();
		}
		op.prepared.frame_payload_terms.resize(op.frames.size());
		RecordFallbackColumns();
	}

	void LowerFramesOnce() {
		for (auto *constraints : {&op.prepared.constraints, &op.prepared.singleton_row_constraints}) {
			for (auto &constraint : *constraints) {
				LowerFrames(*constraint);
			}
		}
		if (op.prepared.objective) {
			auto &objective = *op.prepared.objective;
			LowerFrames(objective.when_condition);
			LowerFrames(objective.terms);
			LowerFrames(objective.squared_terms);
			for (auto &term : objective.bilinear_terms) {
				LowerFrames(term.coefficient);
				LowerFrames(term.filter);
				LowerFrames(term.inputs);
			}
		}
		for (auto &spec : op.composed_minmax_constraints) {
			for (auto &term : spec.terms) {
				LowerFrames(term.inner_terms);
				LowerFrames(term.filter);
				LowerFrames(term.scale);
			}
			LowerFrames(spec.rhs_expr);
			LowerFrames(spec.scope.when);
		}
		for (auto &term : op.composed_minmax_objective_terms) {
			LowerFrames(term.inner_terms);
			LowerFrames(term.filter);
			LowerFrames(term.scale);
		}
		for (auto &link : op.reducer_value_links) {
			LowerFrames(link.inner_terms);
			LowerFrames(link.fallback_terms);
			LowerFrames(link.filter);
			LowerFrames(link.when);
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
	//! - `qualifier_scope_idx` selects a de-duplication mask (`sum(D: ...)`), which is
	//!   again a statement about which rows contribute.
	//!
	//! Constants (`INVALID_INDEX`) are deliberately left alone: they are a fixed
	//! offset folded into the RHS, not a repeated column, and they are not what any
	//! consumer iterating `variable_indices` can trip over.
	static bool TermsAreLike(const DecideTerm &a, const DecideTerm &b) {
		if (a.variable_index == DConstants::INVALID_INDEX || a.variable_index != b.variable_index) {
			return false;
		}
		// `avg_group_key` too: an inlined `sum(per K: avg(e) by (G))` divides by each G
		// group's size, another AVG of the same decision by the whole relation's.
		// `frame_read` too: `x - at(previous: x)` names one decision at two rows.
		if (a.reduction != b.reduction || a.avg_scale != b.avg_scale ||
		    a.qualifier_scope_idx != b.qualifier_scope_idx || a.avg_group_key != b.avg_group_key ||
		    a.frame_read != b.frame_read) {
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
	//! squared term is not a sum of its coefficients, so its NULL is not read as 0.
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
		for (auto &term : objective.bilinear_terms) {
			zero_if_null(term.coefficient);
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
			auto &merged = out[target];
			auto inputs = TakeInputs(merged.inputs, *merged.coefficient);
			for (auto &input : TakeInputs(term.inputs, *term.coefficient)) {
				inputs.push_back(std::move(input));
			}
			merged.coefficient = DecideMergeLikeCoefficients(context, merged.sign, std::move(merged.coefficient),
			                                                 term.sign, std::move(term.coefficient));
			merged.inputs = std::move(inputs);
		}
		terms = std::move(out);
	}

	//===------------------------------------------------------------------===//
	// Reducer metadata and peeled scales
	//===------------------------------------------------------------------===//

	//! Keyed scope a reducer is qualified by (`sum(D: ...)`), read back from the tag the
	//! binder stamped on the aggregate; INVALID_INDEX when the reducer is unqualified.
	static idx_t QualifierScopeOf(const BoundAggregateExpression &agg) {
		idx_t scope_idx = DConstants::INVALID_INDEX;
		TryParseQualifiedReducerTag(agg.alias, scope_idx);
		return scope_idx;
	}

	static void ApplyAggregateMetadata(vector<DecideTerm> &terms, idx_t begin, const BoundAggregateExpression &agg) {
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);
		for (idx_t i = begin; i < terms.size(); i++) {
			terms[i].avg_scale = is_avg;
			terms[i].qualifier_scope_idx = qualifier_scope;
			terms[i].reduction = LinearTermReduction::SUM;
		}
	}

	//! Attach a reducer's own `when` to every term extracted from its body since the given
	//! marks: linear terms, bilinear terms and quadratic groups alike. It is done here, once,
	//! after extraction, rather than threaded through every extraction branch: a branch that
	//! forgot it -- the decision-free cast arm once did -- left one term unfiltered, and the
	//! executor then read every kept row as a member of an easy MIN/MAX, so the rows the
	//! reducer dropped reached the model with the body's constant as their whole left side.
	static void AttachReducerFilter(DecideConstraint &constraint, idx_t linear_before, idx_t bilinear_before,
	                                idx_t quadratic_before, const Expression *filter) {
		if (!filter) {
			return;
		}
		for (idx_t i = linear_before; i < constraint.lhs_terms.size(); i++) {
			constraint.lhs_terms[i].filter = filter->Copy();
		}
		for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
			constraint.bilinear_terms[i].filter = filter->Copy();
		}
		for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
			constraint.quadratic_groups[i].filter = filter->Copy();
		}
	}

	//! Name an expression the way the user wrote it: strip the casts the binder added.
	static string ScaleUserName(const Expression &expr) {
		const Expression *cur = StripCastsForIdentity(expr);
		auto name = StripDecideTags(cur->GetName());
		return name.empty() ? cur->ToString() : name;
	}

	//! Multiply a coefficient by a factor that stayed outside a reducer, keeping the
	//! operand types the original `*` / `/` node was bound for and then binding the
	//! rebuilt node through `FunctionBinder`. Casting first preserves the division
	//! semantics the user's expression was bound with; binding after is what removes
	//! the need to reuse another node's `FunctionData`. A missing coefficient is 1.
	//!
	//! The product is a number DeciDB computes: `inputs` (DecideTerm::inputs) records the factor and what the
	//! coefficient is computed from, in the order the operands stand.
	void ScaleCoefficient(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                      unique_ptr<Expression> &coef, vector<unique_ptr<Expression>> &inputs) const {
		if (!coef) {
			coef = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		}
		auto written = TakeInputs(inputs, *coef);
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
			inputs.push_back(scale.Copy());
		}
		for (auto &input : written) {
			inputs.push_back(std::move(input));
		}
		if (divides) {
			inputs.push_back(scale.Copy());
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
	                           DecideConstraint &constraint, idx_t linear_before, idx_t bilinear_before,
	                           idx_t quadratic_before) {
		for (idx_t i = linear_before; i < constraint.lhs_terms.size(); i++) {
			auto &term = constraint.lhs_terms[i];
			ScaleCoefficient(scale_func, scale, divides, term.coefficient, term.inputs);
		}
		for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
			// A null coefficient means 1.0; the scale becomes the whole coefficient.
			auto &bt = constraint.bilinear_terms[i];
			ScaleCoefficient(scale_func, scale, divides, bt.coefficient, bt.inputs);
		}
		if (quadratic_before == constraint.quadratic_groups.size()) {
			return;
		}
		// Fold a literal immediately. A query-wide subquery scale cannot be evaluated
		// until the relational input has run, so retain it on the quadratic group.
		if (!scale.IsFoldable()) {
			for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
				constraint.quadratic_groups[i].scale = scale.Copy();
				constraint.quadratic_groups[i].scale_divides = divides;
			}
			return;
		}
		double factor =
		    ExpressionExecutor::EvaluateScalar(context, scale).DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		if (divides && factor == 0.0) {
			throw InvalidInputException("DECIDE constraint: division by zero in a squared term.");
		}
		for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
			constraint.quadratic_groups[i].sign *= divides ? 1.0 / factor : factor;
		}
	}

	//! Objective twin of ApplyScaleToExtracted: multiply a factor that stayed outside a
	//! reducer into everything the reducer produced. `quadratic_sign` is a number
	//! rather than an expression, so a squared term needs the factor's value here.
	void ApplyScaleToObjective(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                           DecideObjective &obj, idx_t linear_before, idx_t bilinear_before, idx_t squared_before) {
		for (idx_t i = linear_before; i < obj.terms.size(); i++) {
			ScaleCoefficient(scale_func, scale, divides, obj.terms[i].coefficient, obj.terms[i].inputs);
		}
		for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
			auto &bt = obj.bilinear_terms[i];
			ScaleCoefficient(scale_func, scale, divides, bt.coefficient, bt.inputs);
		}
		if (squared_before == obj.squared_terms.size()) {
			return;
		}
		double factor;
		if (!TryEvaluateFoldableDouble(context, scale, factor, FOLD_PLAIN)) {
			throw InvalidInputException(
			    "DECIDE objective: a squared term cannot be multiplied by '%s', whose value is "
			    "not known until the query runs. Use a constant factor, or move it inside the "
			    "aggregate as SUM(%s * POWER(...)).",
			    scale.GetName(), scale.GetName());
		}
		if (divides && factor == 0.0) {
			throw InvalidInputException("DECIDE objective: division by zero in a squared term.");
		}
		obj.quadratic_sign *= divides ? 1.0 / factor : factor;
	}

	//! Every decision `expr` reads is query-wide: one column each, whichever row reads it.
	bool ReadsOnlyQueryWideDecisions(const Expression &expr) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			idx_t var_idx = FindDecideVariable(expr);
			return var_idx == DConstants::INVALID_INDEX ||
			       (var_idx < op.variable_scopes.size() && op.variable_scopes[var_idx].IsGlobal());
		}
		bool all = true;
		EnumerateChildrenConst(
		    expr, [&](unique_ptr<Expression> &child) { all = all && (!child || ReadsOnlyQueryWideDecisions(*child)); });
		return all;
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
					                        ScaleUserName(*scale));
				}
				idx_t linear_before = constraint.lhs_terms.size();
				idx_t bilinear_before = constraint.bilinear_terms.size();
				idx_t quadratic_before = constraint.quadratic_groups.size();
				ExtractReducerConstraintTerms(*atom.scale.aggregate, constraint, atom.sign);
				ApplyScaleToExtracted(*atom.scale.function, *scale, atom.scale.divides, constraint, linear_before,
				                      bilinear_before, quadratic_before);
				continue;
			}
			// A tuple term beside the reducers: the canonicalizer admitted it as one value per group
			// (ClassifyScoped), and execution counts it once in each group's row (CountOnce). A decision inside a
			// reducer body (`SUM(cost * cap)`) is the reducer's, read below with the coefficient of each row.
			if (!IsReducer(expr)) {
				ExtractConstraintTerms(expr, constraint, atom.sign);
				continue;
			}
			ExtractReducerConstraintTerms(expr.Cast<BoundAggregateExpression>(), constraint, atom.sign);
		}
	}

	void ExtractReducerConstraintTerms(const BoundAggregateExpression &agg, DecideConstraint &constraint, int sign) {
		auto agg_name = StringUtil::Lower(agg.function.name);
		if (agg_name != "sum") {
			throw InternalException("DECIDE optimizer did not rewrite aggregate '%s' to SUM before execution",
			                        agg.function.name);
		}
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);

		idx_t linear_before = constraint.lhs_terms.size();
		idx_t bilinear_before = constraint.bilinear_terms.size();
		idx_t quadratic_before = constraint.quadratic_groups.size();
		ExtractConstraintTerms(*agg.children[0], constraint, sign);
		AttachReducerFilter(constraint, linear_before, bilinear_before, quadratic_before, agg.filter.get());
		ApplyAggregateMetadata(constraint.lhs_terms, linear_before, agg);
		for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
			constraint.bilinear_terms[i].avg_scale = is_avg;
			constraint.bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
		}
		for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
			constraint.quadratic_groups[i].avg_scale = is_avg;
			constraint.quadratic_groups[i].qualifier_scope_idx = qualifier_scope;
		}


	}

	//! The per-row terms of a scoped easy MIN/MAX, `[s *] max(when c: e)`: the terms of `e`,
	//! each filtered by `c` and scaled by `s`.
	void ExtractEasyExtremumTerms(const Expression &lhs, DecideConstraint &constraint) {
		ScaledAggregateMatch scale_match;
		const BoundAggregateExpression *agg = nullptr;
		if (TryMatchScaledAggregate(lhs, decide_index, scale_match)) {
			agg = scale_match.aggregate;
		} else if (lhs.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			agg = &lhs.Cast<BoundAggregateExpression>();
		}
		if (!agg || agg->children.size() != 1) {
			throw InternalException("DECIDE scoped MIN/MAX reached term extraction without its reducer: %s",
			                        lhs.ToString());
		}
		D_ASSERT(constraint.lhs_terms.empty() && constraint.bilinear_terms.empty() &&
		         constraint.quadratic_groups.empty());
		ExtractConstraintTerms(*agg->children[0], constraint, 1);
		AttachReducerFilter(constraint, 0, 0, 0, agg->filter.get());
		if (scale_match.aggregate) {
			ApplyScaleToExtracted(*scale_match.function, *scale_match.scale, scale_match.divides, constraint, 0, 0, 0);
		}
	}

	//! A reducer body or a per-row left side, split by the shared splitter into the constraint's linear terms,
	//! bilinear terms, and one quadratic group per square.
	void ExtractConstraintTerms(const Expression &expr, DecideConstraint &constr, int sign) {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, sign, split);
		for (auto &term : split) {
			switch (term.kind) {
			case DecideTermKind::CONSTANT:
			case DecideTermKind::LINEAR:
				constr.lhs_terms.push_back(DecideTerm {term.var_a, std::move(term.coefficient), term.sign});
				break;
			case DecideTermKind::PRODUCT: {
				BilinearConstraintTerm bt;
				bt.var_a = term.var_a;
				bt.var_b = term.var_b;
				bt.coefficient = std::move(term.coefficient);
				bt.sign = term.sign;
				constr.bilinear_terms.push_back(std::move(bt));
				constr.has_bilinear = true;
				break;
			}
			case DecideTermKind::SQUARE: {
				DecideConstraint::QuadraticGroup qg;
				qg.sign = static_cast<double>(term.sign) * term.square_scale;
				AppendLinearTerms(term.inner, qg.inner_terms);
				constr.quadratic_groups.push_back(std::move(qg));
				constr.has_quadratic = true;
				break;
			}
			case DecideTermKind::FRAME:
				AppendFrameTerms(term, constr.lhs_terms);
				break;
			case DecideTermKind::ABS:
				throw InternalException("DECIDE ABS reached term extraction; RewriteAbs lowers it first");
			case DecideTermKind::UNKNOWN:
				ThrowUnknownTerm(term);
			}
		}
	}

	//! One clause of the constraint tree, under the `when` and `per K:` that enclose it.
	void AnalyzeConstraint(const Expression &expr, const DecideEnclosingScope &scope) {
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::BOUND_COMPARISON: {
			auto &comp = expr.Cast<BoundComparisonExpression>();

			// Skip comparisons the optimizer already folded into the column box.
			// Emitting a DecideConstraint here would add num_rows redundant model
			// rows. The comparison is still in the tree so EXPLAIN renders it; the
			// tag is the decision, and it was made by FoldSingletonRows. One whose
			// repair rows come from its clause is flattened aside, for the repair model
			// alone (singleton_row_constraints).
			const bool folded = HasDecideTag(comp.alias, SINGLETON_ROW_BOUND_TAG);
			if (folded && !HasDecideTag(comp.alias, SINGLETON_ROW_CLAUSE_ROWS_TAG)) {
				break;
			}

			auto constraint = ReadDecideClauseFacts(context, op, comp, scope);
			Expression *lhs = UnwrapDecideCasts(*comp.left, decide_index);

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

				// K1 guard. DecideCanonicalizer puts every decision-bearing term on
				// the left, so a decision variable reaching the RHS here means the
				// invariant was broken upstream -- by a new optimizer rewrite that
				// mutates a constraint in place instead of going through
				// LogicalDecide::AddConstraint, most likely. This used to be a second
				// implementation of the partition (the canonicalization refactor); it was
				// verified unreachable across the golden corpus and the full suite
				// before being replaced by the check, so a wrong answer here would
				// otherwise be silent.
				vector<ExprVarRef> rhs_refs;
				CollectDecideVarRefs(*comp.right, +1, rhs_refs);
				if (!rhs_refs.empty()) {
					throw InternalException(
					    "DECIDE constraint is not canonical: decision variable on the right-hand "
					    "side of '%s'. Constraints must be canonicalized by DecideCanonicalizer "
					    "before reaching term extraction.",
					    comp.right->ToString());
				}

				if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					// Simple single-variable constraint (e.g., x <= 5)
					idx_t var_idx = FindDecideVariable(*lhs);
					if (var_idx != DConstants::INVALID_INDEX) {
						constraint->lhs_terms.push_back(DecideTerm {
						    var_idx, make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))});
					}
				} else {
					// Multi-variable per-row constraint with complex LHS
					// (e.g., z_0 + z_1 = 1, or x + (-3)*z_0 + (-5)*z_1 = 0,
					//  or POWER(x - target, 2) <= K quadratic constraint)
					ExtractConstraintTerms(*lhs, *constraint, 1);
				}
			}

			(folded ? op.prepared.singleton_row_constraints : op.prepared.constraints).push_back(std::move(constraint));
			break;
		}

		default:
			break;
		}
	}

	//===------------------------------------------------------------------===//
	// Objective
	//===------------------------------------------------------------------===//

	//! A SUM argument, split by the shared splitter into the objective's linear, bilinear and squared terms. The
	//! objective supports exactly one quadratic group (one Q matrix with one sign); a second is refused.
	void ExtractLinearAndBilinearTerms(const Expression &expr, DecideObjective &obj, int sign,
	                                   const Expression *filter = nullptr) {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, sign, split);
		for (auto &term : split) {
			switch (term.kind) {
			case DecideTermKind::CONSTANT:
			case DecideTermKind::LINEAR: {
				DecideTerm linear {term.var_a, std::move(term.coefficient), term.sign};
				if (filter) {
					linear.filter = filter->Copy();
				}
				obj.terms.push_back(std::move(linear));
				break;
			}
			case DecideTermKind::FRAME: {
				idx_t before = obj.terms.size();
				AppendFrameTerms(term, obj.terms);
				for (idx_t i = before; i < obj.terms.size() && filter; i++) {
					obj.terms[i].filter = filter->Copy();
				}
				break;
			}
			case DecideTermKind::PRODUCT: {
				DecideObjective::BilinearTerm bt;
				bt.var_a = term.var_a;
				bt.var_b = term.var_b;
				bt.coefficient = std::move(term.coefficient);
				bt.sign = term.sign;
				if (filter) {
					bt.filter = filter->Copy();
				}
				obj.bilinear_terms.push_back(std::move(bt));
				obj.has_bilinear = true;
				break;
			}
			case DecideTermKind::SQUARE: {
				if (obj.has_quadratic) {
					throw InvalidInputException(
					    "DECIDE objective contains multiple quadratic (POWER / (expr)*(expr)) "
					    "groups. Only a single quadratic group plus linear terms is supported; "
					    "combine them mathematically or rewrite the objective.");
				}
				obj.has_quadratic = true;
				obj.quadratic_sign = term.square_scale * static_cast<double>(term.sign);
				idx_t before = obj.squared_terms.size();
				AppendLinearTerms(term.inner, obj.squared_terms);
				if (filter) {
					for (idx_t i = before; i < obj.squared_terms.size(); i++) {
						obj.squared_terms[i].filter = filter->Copy();
					}
				}
				break;
			}
			case DecideTermKind::ABS:
				throw InternalException("DECIDE ABS reached term extraction; RewriteAbs lowers it first");
			case DecideTermKind::UNKNOWN:
				ThrowUnknownTerm(term);
			}
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
				// A decision on both sides of the `*` is a product of two decisions
				// (bilinear), not a scaled reducer; treating it as a coefficient reads a
				// decision column as data and crashes in evaluation.
				if (FindDecideVariable(*obj_scale) != DConstants::INVALID_INDEX) {
					throw InvalidInputException("DECIDE objective: '%s' is a decision, so it cannot multiply an "
					                            "aggregate. Only constants and query-wide values can scale "
					                            "SUM/AVG/MIN/MAX.",
					                            ScaleUserName(*obj_scale));
				}
				idx_t linear_before = obj.terms.size();
				idx_t bilinear_before = obj.bilinear_terms.size();
				idx_t squared_before = obj.squared_terms.size();
				ExtractReducerObjectiveTerms(*atom.scale.aggregate, obj, atom.sign);
				ApplyScaleToObjective(*atom.scale.function, *obj_scale, atom.scale.divides, obj, linear_before,
				                      bilinear_before, squared_before);
				continue;
			}
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST &&
			    FindDecideVariable(expr) == DConstants::INVALID_INDEX) {
				ExtractLinearAndBilinearTerms(expr, obj, atom.sign);
				continue;
			}
			// The objective reads a decision outside its reducers once, on the first kept row
			// (a scope check made sure it has one instance there), a query-wide one included.
			// A product of decisions has no once-only reading in the model -- except a product
			// of two query-wide decisions (`s * u`), one `NONE` term execution counts once
			// (CountOnce in physical_decide.cpp).
			if (!BoundExpressionContainsAggregate(expr) && FindDecideVariable(expr) != DConstants::INVALID_INDEX) {
				idx_t bilinear_before = obj.bilinear_terms.size();
				idx_t squared_before = obj.squared_terms.size();
				ExtractLinearAndBilinearTerms(expr, obj, atom.sign, nullptr);
				if (obj.squared_terms.size() != squared_before ||
				    (obj.bilinear_terms.size() != bilinear_before && !ReadsOnlyQueryWideDecisions(expr))) {
					throw InvalidInputException(
					    "DECIDE objective: '%s' multiplies decisions outside a reducer, which is not supported yet",
					    RenderDecideSource(expr, op.source_fragments, op.keyed_scopes, &op.keys));
				}
				continue;
			}
			if (!IsReducer(expr)) {
				throw InvalidInputException(
				    "DECIDE objective contains a non-aggregate term: %s.\n"
				    "The objective must be a SUM/MIN/MAX/AVG of an expression in decision variables.\n"
				    "If you wrapped an aggregate inside another function (e.g. POWER(AVG(x), 2)), "
				    "use the supported shape SUM(POWER(x, 2)) instead.",
				    expr.ToString());
			}
			ExtractReducerObjectiveTerms(expr.Cast<BoundAggregateExpression>(), obj, atom.sign);
		}
	}

	void ExtractReducerObjectiveTerms(const BoundAggregateExpression &agg, DecideObjective &obj, int sign) {
		auto agg_name = StringUtil::Lower(agg.function.name);
		if (agg_name != "sum") {
			throw InvalidInputException(
			    "DECIDE optimizer should rewrite objective aggregate '%s' to SUM before execution",
			    agg.function.name);
		}
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);
		idx_t avg_group_key = is_avg ? DecideReducerGroupKey(agg) : DConstants::INVALID_INDEX;

		idx_t before = obj.terms.size();
		idx_t bilinear_before = obj.bilinear_terms.size();
		idx_t squared_before = obj.squared_terms.size();
		ExtractLinearAndBilinearTerms(*agg.children[0], obj, sign, agg.filter.get());
		for (idx_t i = before; i < obj.terms.size(); i++) {
			obj.terms[i].avg_scale = is_avg;
			obj.terms[i].avg_group_key = avg_group_key;
			obj.terms[i].qualifier_scope_idx = qualifier_scope;
			obj.terms[i].reduction = LinearTermReduction::SUM;
		}
		for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
			obj.bilinear_terms[i].avg_scale = is_avg;
			obj.bilinear_terms[i].avg_group_key = avg_group_key;
			obj.bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
			obj.bilinear_terms[i].reduction = LinearTermReduction::SUM;
		}
		for (idx_t i = squared_before; i < obj.squared_terms.size(); i++) {
			obj.squared_terms[i].avg_scale = is_avg;
			obj.squared_terms[i].avg_group_key = avg_group_key;
			obj.squared_terms[i].qualifier_scope_idx = qualifier_scope;
		}
	}

	void AnalyzeObjective(const unique_ptr<Expression> &expr_ptr) {
		auto *expr = UnwrapDecideCasts(*expr_ptr, decide_index);

		// DeciDB: Check for WHEN wrapper on objective
		unique_ptr<Expression> when_cond;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			if (IsWhenFilterWrapper(conj) && conj.children.size() == 2) {
				when_cond = conj.children[1]->Copy();
				// Unwrap to get the actual objective expression
				expr = UnwrapDecideCasts(*conj.children[0], decide_index);
			}
		}

		auto &objective = op.prepared.objective;
		if (IsReducer(*expr)) {
			auto &agg = expr->Cast<BoundAggregateExpression>();

			objective = make_uniq<DecideObjective>();

			// Walk the SUM argument. ExtractLinearAndBilinearTerms recognises
			// quadratic patterns (POWER/(expr)*(expr)/negated/K*POWER) at any
			// position in `+`/`-` trees and routes them into squared_terms, so
			// the same walker handles pure QP, pure linear+bilinear, and the
			// mixed forms (e.g. SUM(POWER(x-t, 2) + penalty*x)) uniformly.
			idx_t before = objective->terms.size();
			idx_t bilinear_before = objective->bilinear_terms.size();
			idx_t squared_before = objective->squared_terms.size();
			ExtractLinearAndBilinearTerms(*agg.children[0], *objective, 1, agg.filter.get());
			bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
			idx_t qualifier_scope = QualifierScopeOf(agg);
			idx_t avg_group_key = is_avg ? DecideReducerGroupKey(agg) : DConstants::INVALID_INDEX;
			for (idx_t i = before; i < objective->terms.size(); i++) {
				objective->terms[i].avg_scale = is_avg;
				objective->terms[i].avg_group_key = avg_group_key;
				objective->terms[i].qualifier_scope_idx = qualifier_scope;
				objective->terms[i].reduction = LinearTermReduction::SUM;
			}
			for (idx_t i = bilinear_before; i < objective->bilinear_terms.size(); i++) {
				objective->bilinear_terms[i].avg_scale = is_avg;
				objective->bilinear_terms[i].avg_group_key = avg_group_key;
				objective->bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
				objective->bilinear_terms[i].reduction = LinearTermReduction::SUM;
			}
			for (idx_t i = squared_before; i < objective->squared_terms.size(); i++) {
				objective->squared_terms[i].avg_scale = is_avg;
				objective->squared_terms[i].avg_group_key = avg_group_key;
				objective->squared_terms[i].qualifier_scope_idx = qualifier_scope;
			}

			objective->when_condition = std::move(when_cond);
		} else if (BoundExpressionContainsAggregate(*expr) || FindDecideVariable(*expr) != DConstants::INVALID_INDEX) {
			// The second arm covers an objective with no aggregate at all: decisions
			// outside every reducer (e.g. `minimize max_shortfall`), read once.
			objective = make_uniq<DecideObjective>();
			ExtractAggregateObjectiveTerms(*expr, *objective);
			objective->when_condition = std::move(when_cond);
		}
	}

private:
	ClientContext &context;
	LogicalDecide &op;
	idx_t decide_index;
	DecideTermSplitter splitter;
	DecideCanonicalizer canonicalizer;
	//! LowerFramesInPreparedModel's second pass: each reducer over data reading a frame becomes its column.
	bool lower_aggregates = false;
	//! Frame aggregate (LogicalDecide::frame_aggregates) -> its column (into DecidePreparedModel::frame_columns).
	unordered_map<idx_t, idx_t> frame_aggregate_slots;
};

void BuildDecidePreparedModel(ClientContext &context, LogicalDecide &decide) {
	DecideLinearFormBuilder builder(context, decide);
	builder.Build();
}

} // namespace duckdb
