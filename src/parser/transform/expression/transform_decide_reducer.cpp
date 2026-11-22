#include "duckdb/parser/expression/decide_reducer_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

DecideScope Transformer::TransformDecideScope(optional_ptr<duckdb_libpgquery::PGNode> node) {
	DecideScope result;
	if (!node) {
		return result;
	}
	auto &scope = PGCast<duckdb_libpgquery::PGDecideScope>(*node);
	if (scope.when) {
		result.when = TransformExpression(scope.when);
	}
	switch (scope.per) {
	case duckdb_libpgquery::PG_DECIDE_PER_ROW:
		result.per = DecidePer::ROW;
		break;
	case duckdb_libpgquery::PG_DECIDE_PER_ALL:
		result.per = DecidePer::ALL;
		break;
	case duckdb_libpgquery::PG_DECIDE_PER_KEY:
		result.per = DecidePer::KEY;
		TransformExpressionList(*scope.key, result.key);
		break;
	default:
		break;
	}
	result.location = NumericCast<idx_t>(scope.location);
	return result;
}

DecideClause Transformer::TransformDecideItem(duckdb_libpgquery::PGNode &node) {
	auto &item = PGCast<duckdb_libpgquery::PGDecideItem>(node);
	DecideClause result;
	result.scope = TransformDecideScope(item.scope);
	result.body = TransformExpression(item.body);
	return result;
}

unique_ptr<ParsedExpression> Transformer::TransformDecideReducer(duckdb_libpgquery::PGDecideReducer &root) {
	auto result = make_uniq<DecideReducerExpression>();
	result->call = TransformExpression(root.call);
	result->scope = TransformDecideScope(root.scope);
	result->has_by = root.has_by;
	if (root.by) {
		TransformExpressionList(*root.by, result->by);
	}
	SetQueryLocation(*result, root.location);
	return std::move(result);
}

} // namespace duckdb
