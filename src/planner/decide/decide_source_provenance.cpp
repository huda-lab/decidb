#include "duckdb/planner/decide/decide_source_provenance.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <cstring>

namespace duckdb {

void TagDecideSourceFragments(ParsedExpression &expr, vector<string> &fragments) {
	ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) {
		TagDecideSourceFragments(child, fragments);
	});
	if (expr.GetExpressionClass() != ExpressionClass::CAST && expr.GetExpressionClass() != ExpressionClass::SUBQUERY) {
		return;
	}
	idx_t existing;
	if (TryParseSourceFragmentTag(expr.GetAlias(), existing)) {
		return;
	}
	idx_t fragment_id = fragments.size();
	fragments.push_back(expr.ToString());
	auto alias = expr.GetAlias();
	AddDecideTag(alias, MakeSourceFragmentTag(fragment_id));
	expr.SetAlias(std::move(alias));
}

void PreserveDecideSourceFragment(const ParsedExpression &source, Expression &bound) {
	idx_t fragment_id;
	if (!TryParseSourceFragmentTag(source.GetAlias(), fragment_id)) {
		return;
	}
	auto alias = bound.GetAlias();
	if (!TryParseSourceFragmentTag(alias, fragment_id)) {
		AddDecideTag(alias, MakeSourceFragmentTag(fragment_id));
		bound.SetAlias(std::move(alias));
	}
}

static string RenderSource(const Expression &expr, const vector<string> &fragments,
	                         const vector<EntityScopeInfo> &entity_scopes, int parent_precedence = 0);

static string RenderFunction(const BoundFunctionExpression &func, const vector<string> &fragments,
	                           const vector<EntityScopeInfo> &entity_scopes, int parent_precedence) {
	auto name = func.function.name;
	if (func.children.size() == 1 && name == "-") {
		return "-" + RenderSource(*func.children[0], fragments, entity_scopes, 40);
	}
	if (func.children.size() == 2 &&
	    (name == "+" || name == "-" || name == "*" || name == "/" || name == "%" || name == "**")) {
		int precedence = (name == "+" || name == "-") ? 10 : ((name == "**") ? 30 : 20);
		string result = RenderSource(*func.children[0], fragments, entity_scopes, precedence) + " " + name + " " +
		                RenderSource(*func.children[1], fragments, entity_scopes,
		                             precedence + ((name == "-" || name == "/" || name == "%") ? 1 : 0));
		return precedence < parent_precedence ? "(" + result + ")" : result;
	}
	if (func.is_operator && func.children.size() == 1) {
		return name + "(" + RenderSource(*func.children[0], fragments, entity_scopes) + ")";
	}
	if (func.is_operator && func.children.size() == 2) {
		return "(" + RenderSource(*func.children[0], fragments, entity_scopes) + " " + name + " " +
		       RenderSource(*func.children[1], fragments, entity_scopes) + ")";
	}
	string result = StringUtil::Upper(name) + "(";
	for (idx_t i = 0; i < func.children.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += RenderSource(*func.children[i], fragments, entity_scopes);
	}
	result += ")";
	return result;
}

static string RenderAggregate(const BoundAggregateExpression &agg, const vector<string> &fragments,
                            const vector<EntityScopeInfo> &entity_scopes) {
	string body;
	for (idx_t i = 0; i < agg.children.size(); i++) {
		if (i > 0) {
			body += ", ";
		}
		body += RenderSource(*agg.children[i], fragments, entity_scopes);
	}
	idx_t scope_idx;
	if (TryParseQualifiedReducerTag(agg.GetAlias(), scope_idx) && scope_idx < entity_scopes.size()) {
		body = entity_scopes[scope_idx].table_alias + ": " + body;
	}
	string result;
	string norm_payload;
	auto marker_pos = agg.GetAlias().find(NORM_MARKER_TAG_PREFIX);
	if (marker_pos != string::npos) {
		auto begin = marker_pos + strlen(NORM_MARKER_TAG_PREFIX);
		auto end = agg.GetAlias().find("__", begin);
		if (end != string::npos) {
			norm_payload = agg.GetAlias().substr(begin, end - begin);
		}
	}
	if (!norm_payload.empty()) {
		if (norm_payload == "0_auto") {
			result = "NORM(" + body + ", 0)";
		} else if (norm_payload.rfind("0_", 0) == 0) {
			result = "NORM(" + body + ", 0, " + norm_payload.substr(2) + ")";
		} else {
			result = "NORM(" + body + ", " + norm_payload + ")";
		}
	} else {
		result = StringUtil::Upper(agg.function.name) + "(" + body + ")";
	}
	if (agg.filter) {
		result += " WHEN " + RenderSource(*agg.filter, fragments, entity_scopes);
	}
	return result;
}

template <class CALLBACK>
static void VisitSourceInOperators(Expression &expr, CALLBACK &&callback) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if ((IsPerConstraintTag(conj.GetAlias()) || HasDecideTag(conj.GetAlias(), WHEN_CONSTRAINT_TAG)) &&
		    !conj.children.empty()) {
			VisitSourceInOperators(*conj.children[0], callback);
			return;
		}
		for (auto &child : conj.children) {
			VisitSourceInOperators(*child, callback);
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR && expr.type == ExpressionType::COMPARE_IN) {
		callback(expr);
	}
}

static string RenderSource(const Expression &expr, const vector<string> &fragments,
	                         const vector<EntityScopeInfo> &entity_scopes, int parent_precedence) {
	idx_t fragment_id;
	if (TryParseSourceFragmentTag(expr.GetAlias(), fragment_id) && fragment_id < fragments.size()) {
		return fragments[fragment_id];
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CAST:
		// Untagged casts were introduced by DuckDB binding and are display noise.
		return RenderSource(*expr.Cast<BoundCastExpression>().child, fragments, entity_scopes);
	case ExpressionClass::BOUND_COMPARISON: {
		auto &cmp = expr.Cast<BoundComparisonExpression>();
		return RenderSource(*cmp.left, fragments, entity_scopes) + " " + ExpressionTypeToOperator(cmp.type) + " " +
		       RenderSource(*cmp.right, fragments, entity_scopes);
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		string op = conj.type == ExpressionType::CONJUNCTION_OR ? " OR " : " AND ";
		string result;
		for (idx_t i = 0; i < conj.children.size(); i++) {
			if (i > 0) {
				result += op;
			}
			result += RenderSource(*conj.children[i], fragments, entity_scopes);
		}
		return result;
	}
	case ExpressionClass::BOUND_FUNCTION:
		return RenderFunction(expr.Cast<BoundFunctionExpression>(), fragments, entity_scopes, parent_precedence);
	case ExpressionClass::BOUND_AGGREGATE:
		return RenderAggregate(expr.Cast<BoundAggregateExpression>(), fragments, entity_scopes);
	default:
		return expr.ToString();
	}
}

static string AppendQualifier(string prefix, const string &suffix) {
	if (suffix.empty()) {
		return prefix;
	}
	if (!prefix.empty()) {
		prefix += " ";
	}
	prefix += suffix;
	return prefix;
}

static string PerQualifier(const BoundConjunctionExpression &conj, const vector<string> &fragments,
	                       const vector<EntityScopeInfo> &entity_scopes) {
	string result = "PER ";
	bool parenthesize = conj.children.size() > 2;
	if (parenthesize) {
		result += "(";
	}
	for (idx_t i = 1; i < conj.children.size(); i++) {
		if (i > 1) {
			result += ", ";
		}
		result += RenderSource(*conj.children[i], fragments, entity_scopes);
	}
	if (parenthesize) {
		result += ")";
	}
	return result;
}

template <class CALLBACK>
static void VisitSourceComparisons(Expression &expr, const vector<string> &fragments,
	                               const vector<EntityScopeInfo> &entity_scopes, bool render_qualifiers,
	                               string qualifier,
	                               CALLBACK &&callback) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (IsPerConstraintTag(conj.GetAlias()) && !conj.children.empty()) {
			if (render_qualifiers) {
				qualifier = AppendQualifier(std::move(qualifier), PerQualifier(conj, fragments, entity_scopes));
			}
			VisitSourceComparisons(*conj.children[0], fragments, entity_scopes, render_qualifiers,
			                       std::move(qualifier), callback);
			return;
		}
		if (HasDecideTag(conj.GetAlias(), WHEN_CONSTRAINT_TAG) && conj.children.size() == 2) {
			if (render_qualifiers) {
				qualifier = AppendQualifier("WHEN " + RenderSource(*conj.children[1], fragments, entity_scopes),
				                            qualifier);
			}
			VisitSourceComparisons(*conj.children[0], fragments, entity_scopes, render_qualifiers,
			                       std::move(qualifier), callback);
			return;
		}
		for (auto &child : conj.children) {
			VisitSourceComparisons(*child, fragments, entity_scopes, render_qualifiers, qualifier, callback);
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		callback(expr.Cast<BoundComparisonExpression>(), qualifier);
	}
}

vector<ConstraintSourceInfo> InitializeConstraintSourceInfo(Expression &constraints,
	                                                        const vector<string> &fragments,
	                                                        const vector<EntityScopeInfo> &entity_scopes) {
	vector<ConstraintSourceInfo> result;
	VisitSourceComparisons(constraints, fragments, entity_scopes, false, string(),
	                       [&](BoundComparisonExpression &cmp, const string &) {
		                       idx_t source_id = result.size();
		                       auto alias = cmp.GetAlias();
		                       AddDecideTag(alias, MakeSourceClauseTag(source_id));
		                       cmp.SetAlias(std::move(alias));
	                       result.emplace_back();
	                       });
	// A DECIDE-variable IN is intentionally an opaque pre-optimizer marker, not
	// a comparison. Give it the same stable source identity now so every emitted
	// indicator/linking row can still point back to the original SQL clause.
	VisitSourceInOperators(constraints, [&](Expression &in) {
		idx_t source_id = result.size();
		auto alias = in.GetAlias();
		AddDecideTag(alias, MakeSourceClauseTag(source_id));
		in.SetAlias(std::move(alias));
		ConstraintSourceInfo info;
		info.canonical_lhs = in.ToString();
		info.rhs_kind = ConstraintSourceRhsKind::NUMERIC_FALLBACK;
		result.push_back(std::move(info));
	});
	return result;
}

static bool ContainsSubquerySource(const Expression &expr) {
	if (IsQueryWideValueTag(expr.GetAlias()) || IsRowVaryingSubqueryTag(expr.GetAlias())) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsSubquerySource(child);
		}
	});
	return found;
}

void FinalizeConstraintSourceInfo(const Expression &constraints, vector<ConstraintSourceInfo> &sources,
	                              const vector<string> &fragments,
	                              const vector<EntityScopeInfo> &entity_scopes) {
	auto &mutable_constraints = const_cast<Expression &>(constraints);
	VisitSourceComparisons(mutable_constraints, fragments, entity_scopes, true, string(),
	                       [&](BoundComparisonExpression &cmp, const string &qualifier) {
		                       idx_t source_id;
		                       if (!TryParseSourceClauseTag(cmp.GetAlias(), source_id) || source_id >= sources.size()) {
			                       return;
		                       }
		                       auto &info = sources[source_id];
		                       info.canonical_lhs = RenderSource(*cmp.left, fragments, entity_scopes);
		                       info.canonical_rhs = RenderSource(*cmp.right, fragments, entity_scopes);
		                       info.qualifier = qualifier;
		                       if (!IsQueryWideBoundTag(cmp.GetAlias()) &&
		                           !ContainsSubquerySource(*cmp.right)) {
			                       info.rhs_kind = ConstraintSourceRhsKind::DATA_EXPRESSION;
		                       } else {
			                       info.rhs_kind = ConstraintSourceRhsKind::NUMERIC_FALLBACK;
		                       }
	                       });
}

string RenderDecideSource(const Expression &expr, const vector<string> &fragments,
                          const vector<EntityScopeInfo> &entity_scopes) {
	return RenderSource(expr, fragments, entity_scopes);
}

void CollectDecideExpressionStrings(const Expression &expr, const vector<string> &fragments,
                                    const vector<EntityScopeInfo> &entity_scopes, vector<string> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// PER wrapper: child[0] is the constraint, children[1..N] are the PER key columns.
		if (IsPerConstraintTag(conj.GetAlias()) && conj.children.size() >= 2) {
			string suffix = " " + PerQualifier(conj, fragments, entity_scopes);
			vector<string> inner;
			CollectDecideExpressionStrings(*conj.children[0], fragments, entity_scopes, inner);
			for (auto &s : inner) {
				out.push_back(s + suffix);
			}
			return;
		}
		// WHEN wrapper: child[0] is the constraint, child[1] is the condition.
		if (HasDecideTag(conj.GetAlias(), WHEN_CONSTRAINT_TAG) && conj.children.size() == 2) {
			string suffix = " WHEN " + RenderSource(*conj.children[1], fragments, entity_scopes);
			vector<string> inner;
			CollectDecideExpressionStrings(*conj.children[0], fragments, entity_scopes, inner);
			for (auto &s : inner) {
				out.push_back(s + suffix);
			}
			return;
		}
		// A plain AND is the constraint list itself, so each child is its own clause.
		for (auto &child : conj.children) {
			CollectDecideExpressionStrings(*child, fragments, entity_scopes, out);
		}
		return;
	}
	out.push_back(RenderSource(expr, fragments, entity_scopes));
}

} // namespace duckdb
