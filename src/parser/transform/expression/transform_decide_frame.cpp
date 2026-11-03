#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/decide_frame_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

static DecideFrameAnchor TransformDecideFrameAnchor(duckdb_libpgquery::PGDecideFrameAnchor anchor) {
	switch (anchor) {
	case duckdb_libpgquery::PG_DECIDE_FRAME_FIRST:
		return DecideFrameAnchor::FIRST;
	case duckdb_libpgquery::PG_DECIDE_FRAME_LAST:
		return DecideFrameAnchor::LAST;
	case duckdb_libpgquery::PG_DECIDE_FRAME_PREVIOUS:
		return DecideFrameAnchor::PREVIOUS;
	case duckdb_libpgquery::PG_DECIDE_FRAME_NEXT:
		return DecideFrameAnchor::NEXT;
	default:
		throw InternalException("unknown DECIDE frame anchor");
	}
}

//! DeciDB: a frame selector, at(F [else v]: e) over (S ...) or agg(from F1 to F2 ...: e)
//! over (S ...). A range's reducer arrives as a call over its argument list, read here
//! as a name and one argument: through TransformFuncCall, a name it rewrites
//! (`construct_array`, `if`) would come back as another kind of expression.
unique_ptr<ParsedExpression> Transformer::TransformDecideFrame(duckdb_libpgquery::PGDecideFrame &root) {
	auto result = make_uniq<DecideFrameExpression>();
	result->from = TransformDecideFrameAnchor(root.from_anchor);
	result->from_k = root.from_k;
	if (root.is_range) {
		result->to = TransformDecideFrameAnchor(root.to_anchor);
		result->to_k = root.to_k;
		result->every = root.every;
		result->all = root.all;
		auto &call = PGCast<duckdb_libpgquery::PGFuncCall>(*root.body);
		vector<string> name;
		for (auto cell = call.funcname->head; cell != nullptr; cell = cell->next) {
			name.push_back(PGPointerCast<duckdb_libpgquery::PGValue>(cell->data.ptr_value)->val.str);
		}
		result->reducer = StringUtil::Lower(StringUtil::Join(name, "."));
		const idx_t arguments = call.args ? NumericCast<idx_t>(call.args->length) : 0;
		auto argument = arguments == 1 ? PGPointerCast<duckdb_libpgquery::PGNode>(call.args->head->data.ptr_value)
		                               : nullptr;
		auto range = StringUtil::Format("%s(from %s to %s: e)", result->reducer,
		                                DecideFrameExpression::PositionToString(result->from, result->from_k),
		                                DecideFrameExpression::PositionToString(result->to, result->to_k));
		if (arguments != 1) {
			throw ParserException(optional_idx(NumericCast<idx_t>(root.location)),
			                      "DECIDE: %s reduces one expression, found %d", range, arguments);
		}
		if (argument->type == duckdb_libpgquery::T_PGNamedArgExpr) {
			throw ParserException(optional_idx(NumericCast<idx_t>(root.location)),
			                      "DECIDE: %s takes its expression without a name", range);
		}
		result->payload = TransformExpression(*argument);
	} else {
		result->payload = TransformExpression(root.body);
	}
	result->fallback = TransformExpression(root.fallback);
	result->order = TransformExpression(root.order);
	result->descending = root.descending;
	result->cyclic = root.cyclic;
	if (root.within) {
		TransformExpressionList(*root.within, result->within);
	}
	SetQueryLocation(*result, root.location);
	// k and d are positive whole numbers (P8); the grammar reads them as unsigned literals
	for (auto k : {result->from_k, result->IsRange() ? result->to_k : int64_t(1), result->every}) {
		if (k <= 0) {
			throw ParserException(*result, "DECIDE: in %s, %d must be a positive whole number", result->ToString(), k);
		}
	}
	return std::move(result);
}

} // namespace duckdb
