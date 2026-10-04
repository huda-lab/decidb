#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/decide/decide_declaration.hpp"
#include "duckdb/parser/decide/decide_frame_spec.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

// DecidB: the DECIDE clause's own parse nodes. The transformer keeps the parser's
// structure -- which prefix belongs to which body, which BY to which reducer -- and
// encodes it in the parsed tree's DECIDE tags (common/enums/decide.hpp). Nothing here
// resolves a name or decides a shape.

void Transformer::TransformDecideScope(duckdb_libpgquery::PGDecideScope *scope, DecideScopeKind &kind,
                                       vector<unique_ptr<ParsedExpression>> &key, bool row_spelling) {
	key.clear();
	if (!scope) {
		kind = DecideScopeKind::ROW;
		return;
	}
	switch (scope->kind) {
	case duckdb_libpgquery::PG_DECIDE_SCOPE_ROW:
		kind = DecideScopeKind::ROW;
		return;
	case duckdb_libpgquery::PG_DECIDE_SCOPE_GLOBAL:
		kind = DecideScopeKind::GLOBAL;
		return;
	case duckdb_libpgquery::PG_DECIDE_SCOPE_KEY:
		kind = DecideScopeKind::KEY;
		for (auto cell = scope->keys->head; cell != nullptr; cell = cell->next) {
			auto node = PGPointerCast<duckdb_libpgquery::PGNode>(cell->data.ptr_value);
			key.push_back(TransformExpression(node));
		}
		// `PER ROW` is the spec's explicit spelling of the default (one instance per
		// row, spec §6.1). ROW is a keyword that also reads as a plain identifier, so
		// it reaches here as a one-element key; a column literally named `row` is
		// keyed as `t.row`. Only a generation key (`PER`) has that spelling: a frame's
		// `WITHIN row` and a reducer's `BY (row)` name the column.
		if (row_spelling && key.size() == 1 && key[0]->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = key[0]->Cast<ColumnRefExpression>();
			if (colref.column_names.size() == 1 && StringUtil::CIEquals(colref.column_names[0], "row")) {
				key.clear();
				kind = DecideScopeKind::ROW;
			}
		}
		return;
	}
}

static DecideDomain TransformDecideDomain(duckdb_libpgquery::PGDecideDomain domain) {
	switch (domain) {
	case duckdb_libpgquery::PG_DECIDE_DOMAIN_INT:
		return DecideDomain::INT;
	case duckdb_libpgquery::PG_DECIDE_DOMAIN_REAL:
		return DecideDomain::REAL;
	case duckdb_libpgquery::PG_DECIDE_DOMAIN_BOOL:
		return DecideDomain::BOOL;
	case duckdb_libpgquery::PG_DECIDE_DOMAIN_SEMIREAL:
		return DecideDomain::SEMIREAL;
	case duckdb_libpgquery::PG_DECIDE_DOMAIN_SEMIINT:
		return DecideDomain::SEMIINT;
	case duckdb_libpgquery::PG_DECIDE_DOMAIN_TEXT:
		return DecideDomain::TEXT;
	}
	throw InternalException("Unknown DECIDE domain");
}

DecideDeclaration Transformer::TransformDecideDeclarator(duckdb_libpgquery::PGDecideDeclarator &root) {
	DecideDeclaration result;
	result.name = root.name;
	TransformDecideScope(root.scope, result.scope_kind, result.scope_key);
	result.domain = TransformDecideDomain(root.domain);
	if (root.text_values) {
		for (auto cell = root.text_values->head; cell != nullptr; cell = cell->next) {
			auto value = PGPointerCast<duckdb_libpgquery::PGValue>(cell->data.ptr_value);
			result.text_values.emplace_back(value->val.str);
		}
	}
	if (root.lower_bound) {
		result.lower_bound = TransformExpression(root.lower_bound);
	}
	if (root.upper_bound) {
		result.upper_bound = TransformExpression(root.upper_bound);
	}
	return result;
}

void Transformer::TransformDecideClause(duckdb_libpgquery::PGDecideClause &clause, SelectNode &result) {
	for (auto cell = clause.variables->head; cell != nullptr; cell = cell->next) {
		auto node = PGPointerCast<duckdb_libpgquery::PGNode>(cell->data.ptr_value);
		if (node->type != duckdb_libpgquery::T_PGDecideDeclarator) {
			throw InternalException("DECIDE declaration is not a declarator node");
		}
		result.decide_variables.push_back(
		    TransformDecideDeclarator(PGCast<duckdb_libpgquery::PGDecideDeclarator>(*node)));
	}
	result.decide_constraints = TransformExpression(clause.constraints);
	if (clause.objectives) {
		for (auto cell = clause.objectives->head; cell != nullptr; cell = cell->next) {
			auto &objective = *PGPointerCast<duckdb_libpgquery::PGDecideObjective>(cell->data.ptr_value);
			auto sense = objective.sense == duckdb_libpgquery::PG_OBJ_MAXIMIZE ? DecideSense::MAXIMIZE
			                                                                     : DecideSense::MINIMIZE;
			result.decide_objectives.emplace_back(sense, TransformExpression(objective.expr));
		}
	}
}

//! Builds a tagged wrapper: `tag(child, keys...)`.
static unique_ptr<ParsedExpression> MakeDecideWrapper(const char *tag, unique_ptr<ParsedExpression> child,
                                                      vector<unique_ptr<ParsedExpression>> keys) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(child));
	for (auto &key : keys) {
		children.push_back(std::move(key));
	}
	auto result = make_uniq<FunctionExpression>(tag, std::move(children));
	result->is_operator = true;
	return std::move(result);
}

unique_ptr<ParsedExpression> Transformer::TransformDecideScopeWrapper(const char *tag,
                                                                      duckdb_libpgquery::PGNode *body,
                                                                      duckdb_libpgquery::PGNode *scope_node) {
	auto child = TransformExpression(body);
	DecideScopeKind kind;
	vector<unique_ptr<ParsedExpression>> key;
	TransformDecideScope(scope_node ? PGPointerCast<duckdb_libpgquery::PGDecideScope>(scope_node).get() : nullptr,
	                     kind, key);
	if (kind == DecideScopeKind::ROW) {
		// `PER ROW` is the default and needs no wrapper.
		return child;
	}
	// GLOBAL is a wrapper with no key children.
	return MakeDecideWrapper(tag, std::move(child), std::move(key));
}

unique_ptr<ParsedExpression> Transformer::TransformDecideReducerBy(duckdb_libpgquery::PGNode *reducer,
                                                                   duckdb_libpgquery::PGNode *keys_node) {
	auto child = TransformExpression(reducer);
	vector<unique_ptr<ParsedExpression>> keys;
	if (keys_node) {
		if (keys_node->type != duckdb_libpgquery::T_PGList) {
			throw InternalException("DECIDE BY keys are not a list");
		}
		auto list = PGPointerCast<duckdb_libpgquery::PGList>(keys_node);
		for (auto cell = list->head; cell != nullptr; cell = cell->next) {
			keys.push_back(TransformExpression(PGPointerCast<duckdb_libpgquery::PGNode>(cell->data.ptr_value)));
		}
	}
	if (keys.empty()) {
		// `BY ()` is the global group, the same as no BY at all.
		return child;
	}
	return MakeDecideWrapper(REDUCER_BY_TAG, std::move(child), std::move(keys));
}

static DecideFrameSelector TransformFrameSelector(const duckdb_libpgquery::PGDecideFrameSelector &selector) {
	DecideFrameSelector result;
	switch (selector.kind) {
	case duckdb_libpgquery::PG_DECIDE_FRAME_FIRST:
		result.kind = DecideFrameSelectorKind::FIRST;
		break;
	case duckdb_libpgquery::PG_DECIDE_FRAME_LAST:
		result.kind = DecideFrameSelectorKind::LAST;
		break;
	case duckdb_libpgquery::PG_DECIDE_FRAME_PREVIOUS:
		result.kind = DecideFrameSelectorKind::PREVIOUS;
		break;
	case duckdb_libpgquery::PG_DECIDE_FRAME_NEXT:
		result.kind = DecideFrameSelectorKind::NEXT;
		break;
	}
	result.distance = selector.distance <= 0 ? 1 : static_cast<idx_t>(selector.distance);
	return result;
}

unique_ptr<ParsedExpression> Transformer::TransformDecideFrame(duckdb_libpgquery::PGDecideFrame &root) {
	DecideFrameSpec spec;
	spec.is_range = root.is_range;
	spec.aggregate = root.agg ? StringUtil::Lower(root.agg) : string();
	spec.from_selector = TransformFrameSelector(root.from_sel);
	spec.to_selector = TransformFrameSelector(root.to_sel);
	spec.every = root.every <= 0 ? 1 : static_cast<idx_t>(root.every);
	switch (root.policy) {
	case duckdb_libpgquery::PG_DECIDE_FRAME_ELSE_NULL:
		spec.policy = DecideFramePolicy::ELSE_NULL;
		break;
	case duckdb_libpgquery::PG_DECIDE_FRAME_ELSE_VALUE:
		spec.policy = DecideFramePolicy::ELSE_VALUE;
		break;
	case duckdb_libpgquery::PG_DECIDE_FRAME_ALL:
		spec.policy = DecideFramePolicy::ALL;
		break;
	}
	spec.descending = root.descending;
	spec.cyclic = root.cyclic;
	spec.has_within = root.within != nullptr;

	// `ELSE NULL` is the deck's spelling of the default policy (a missing position
	// reads nothing), not a fill value of NULL.
	unique_ptr<ParsedExpression> else_value;
	if (root.else_value) {
		else_value = TransformExpression(root.else_value);
		if (else_value->GetExpressionClass() == ExpressionClass::CONSTANT &&
		    else_value->Cast<ConstantExpression>().value.IsNull()) {
			else_value.reset();
			spec.policy = DecideFramePolicy::ELSE_NULL;
		}
	}

	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(TransformExpression(root.expr));
	children.push_back(make_uniq<ConstantExpression>(Value(spec.Encode())));
	children.push_back(TransformExpression(root.order_key));
	if (else_value) {
		children.push_back(std::move(else_value));
	} else {
		children.push_back(make_uniq<ConstantExpression>(Value()));
	}
	DecideScopeKind within_kind;
	vector<unique_ptr<ParsedExpression>> within;
	TransformDecideScope(root.within, within_kind, within, /*row_spelling=*/false);
	for (auto &column : within) {
		children.push_back(std::move(column));
	}
	auto result = make_uniq<FunctionExpression>(FRAME_TAG, std::move(children));
	result->is_operator = true;
	SetQueryLocation(*result, root.location);
	return std::move(result);
}

} // namespace duckdb
