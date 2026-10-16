#include "duckdb/planner/decide/decide_source_provenance.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/bind_context.hpp"

#include <cstring>

namespace duckdb {

//! Spell a comparison the way DECIDE documents it. `ExpressionTypeToOperator` renders
//! not-equal as `!=`, but every other user-facing DECIDE surface -- the syntax
//! reference, `ne_clause_labels`, the binder's own messages -- writes `<>`. A repair
//! quotes this text straight back as SQL the user is meant to recognize, so the one
//! spelling wins here too.
static string DecideComparisonOperator(ExpressionType type) {
	return type == ExpressionType::COMPARE_NOTEQUAL ? "<>" : ExpressionTypeToOperator(type);
}

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

namespace {
//! What rendering a bound clause reads besides the tree.
struct RenderContext {
	const vector<string> &fragments;
	const vector<EntityScopeInfo> &entity_scopes;
	//! The `by` keys, so a reducer prints its `by (...)`; null prints none.
	const vector<DecideGroupKey> *group_keys = nullptr;
	//! A relation's alias by table index, so a column prints as `D.depotID`; null
	//! prints its name alone.
	const unordered_map<idx_t, string> *relations = nullptr;
	//! Inside a clause written with the scope spellings (SCOPED_CLAUSE_TAG): a reducer
	//! prints its `when`, `per` and `by` as that clause spells them.
	bool scoped = false;
};
} // namespace

static string RenderSource(const Expression &expr, const RenderContext &ctx, int parent_precedence = 0);

static string RenderFunction(const BoundFunctionExpression &func, const RenderContext &ctx, int parent_precedence) {
	auto name = func.function.name;
	if (func.children.size() == 1 && name == "-") {
		return "-" + RenderSource(*func.children[0], ctx, 40);
	}
	if (func.children.size() == 2 &&
	    (name == "+" || name == "-" || name == "*" || name == "/" || name == "%" || name == "**")) {
		int precedence = (name == "+" || name == "-") ? 10 : ((name == "**") ? 30 : 20);
		string result = RenderSource(*func.children[0], ctx, precedence) + " " + name + " " +
		                RenderSource(*func.children[1], ctx,
		                             precedence + ((name == "-" || name == "/" || name == "%") ? 1 : 0));
		return precedence < parent_precedence ? "(" + result + ")" : result;
	}
	if (func.is_operator && func.children.size() == 1) {
		return name + "(" + RenderSource(*func.children[0], ctx) + ")";
	}
	if (func.is_operator && func.children.size() == 2) {
		return "(" + RenderSource(*func.children[0], ctx) + " " + name + " " + RenderSource(*func.children[1], ctx) +
		       ")";
	}
	string result = StringUtil::Upper(name) + "(";
	for (idx_t i = 0; i < func.children.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += RenderSource(*func.children[i], ctx);
	}
	result += ")";
	return result;
}

//! A reducer in a clause written with the scope spellings, as that clause spells it:
//! `sum(when c per K: e) by (g)`. A relation qualifier (`SUM(D: e)`) keeps its colon form.
static string RenderScopedAggregate(const BoundAggregateExpression &agg, const string &body,
                                    const RenderContext &ctx) {
	string scope;
	if (agg.filter) {
		scope = "when " + RenderSource(*agg.filter, ctx);
	}
	string result = StringUtil::Lower(agg.function.name) + "(";
	idx_t scope_idx;
	if (TryParseQualifiedReducerTag(agg.GetAlias(), scope_idx) && scope_idx < ctx.entity_scopes.size()) {
		auto &key = ctx.entity_scopes[scope_idx];
		if (key.exact_key) {
			scope += (scope.empty() ? "per " : " per ") + key.table_alias;
		} else {
			result += key.table_alias + ": ";
		}
	}
	result += (scope.empty() ? "" : scope + ": ") + body + ")";
	idx_t group_key;
	if (ctx.group_keys && TryParseGroupKeyTag(agg.GetAlias(), group_key) && group_key < ctx.group_keys->size()) {
		result += " by (" + (*ctx.group_keys)[group_key].text + ")";
	}
	return result;
}

static string RenderAggregate(const BoundAggregateExpression &agg, const RenderContext &ctx) {
	string body;
	for (idx_t i = 0; i < agg.children.size(); i++) {
		if (i > 0) {
			body += ", ";
		}
		body += RenderSource(*agg.children[i], ctx);
	}
	if (ctx.scoped) {
		return RenderScopedAggregate(agg, body, ctx);
	}
	idx_t scope_idx;
	if (TryParseQualifiedReducerTag(agg.GetAlias(), scope_idx) && scope_idx < ctx.entity_scopes.size()) {
		body = ctx.entity_scopes[scope_idx].table_alias + ": " + body;
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
		result += " WHEN " + RenderSource(*agg.filter, ctx);
	}
	return result;
}

//! The old three-argument form every other renderer in this file uses.
static string RenderSource(const Expression &expr, const vector<string> &fragments,
                           const vector<EntityScopeInfo> &entity_scopes,
                           const vector<DecideGroupKey> *group_keys = nullptr) {
	RenderContext ctx {fragments, entity_scopes, group_keys};
	return RenderSource(expr, ctx);
}

template <class CALLBACK>
static void VisitSourceInOperators(Expression &expr, CALLBACK &&callback) {
	ForEachConstraintLeaf(expr, [&](Expression &node) {
		if (node.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
		    node.type == ExpressionType::COMPARE_IN) {
			callback(node);
		}
	});
}

static string RenderSource(const Expression &expr, const RenderContext &ctx, int parent_precedence) {
	idx_t fragment_id;
	if (TryParseSourceFragmentTag(expr.GetAlias(), fragment_id) && fragment_id < ctx.fragments.size()) {
		return ctx.fragments[fragment_id];
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CAST:
		// Untagged casts were introduced by DuckDB binding and are display noise.
		return RenderSource(*expr.Cast<BoundCastExpression>().child, ctx);
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (ctx.relations) {
			auto relation = ctx.relations->find(colref.binding.table_index);
			if (relation != ctx.relations->end()) {
				return relation->second + "." + KeywordHelper::WriteOptionallyQuoted(StripDecideTags(colref.GetName()));
			}
		}
		break;
	}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &cmp = expr.Cast<BoundComparisonExpression>();
		RenderContext sides = ctx;
		sides.scoped = ctx.scoped || HasDecideTag(cmp.GetAlias(), SCOPED_CLAUSE_TAG);
		return RenderSource(*cmp.left, sides) + " " + DecideComparisonOperator(cmp.type) + " " +
		       RenderSource(*cmp.right, sides);
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		string op = conj.type == ExpressionType::CONJUNCTION_OR ? " OR " : " AND ";
		string result;
		for (idx_t i = 0; i < conj.children.size(); i++) {
			if (i > 0) {
				result += op;
			}
			result += RenderSource(*conj.children[i], ctx);
		}
		return result;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		// A DECIDE-variable IN is an opaque marker rather than a comparison, so without
		// a case here it fell through to ToString() -- the one renderer that spells out
		// the casts the binder inserted. Every other clause in the same plan is
		// cast-free, so this one read as though the user had typed CAST(10 AS BIGINT).
		if (expr.type != ExpressionType::COMPARE_IN) {
			break;
		}
		auto &in = expr.Cast<BoundOperatorExpression>();
		if (in.children.size() < 2) {
			break;
		}
		string result = RenderSource(*in.children[0], ctx) + " IN (";
		for (idx_t i = 1; i < in.children.size(); i++) {
			if (i > 1) {
				result += ", ";
			}
			result += RenderSource(*in.children[i], ctx);
		}
		return result + ")";
	}
	case ExpressionClass::BOUND_FUNCTION:
		return RenderFunction(expr.Cast<BoundFunctionExpression>(), ctx, parent_precedence);
	case ExpressionClass::BOUND_AGGREGATE:
		return RenderAggregate(expr.Cast<BoundAggregateExpression>(), ctx);
	default:
		break;
	}
	return expr.ToString();
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

//! `per K` of the scope spellings: the key as written, else its columns; `per ()` for none.
static string PerClassQualifier(const BoundConjunctionExpression &conj, const vector<string> &fragments,
                                const vector<EntityScopeInfo> &entity_scopes) {
	idx_t fragment_id;
	if (TryParseSourceFragmentTag(conj.GetAlias(), fragment_id) && fragment_id < fragments.size()) {
		return "per " + fragments[fragment_id];
	}
	string key;
	for (idx_t i = 1; i < conj.children.size(); i++) {
		key += (i > 1 ? ", " : "") + RenderSource(*conj.children[i], fragments, entity_scopes);
	}
	return "per " + (key.empty() ? string("()") : key);
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

//! The qualifier of a WHEN wrapper: the scope spellings' `when c` in front, else the old
//! postfix `WHEN c`.
static string WhenQualifier(const BoundConjunctionExpression &conj, const vector<string> &fragments,
                            const vector<EntityScopeInfo> &entity_scopes) {
	bool scoped = HasDecideTag(conj.GetAlias(), SCOPED_CLAUSE_TAG);
	return (scoped ? "when " : "WHEN ") + RenderSource(*conj.children[1], fragments, entity_scopes);
}

//! A scope qualifier (`when c per K`) is written in front of its clause; its lower-case
//! keyword and the colon that ends it are what AttachDecideQualifier reads to put it
//! there.
static string FrontQualifier(const string &qualifier) {
	return qualifier.empty() ? qualifier : qualifier + ":";
}

template <class CALLBACK>
static void VisitSourceComparisons(Expression &expr, const vector<string> &fragments,
	                               const vector<EntityScopeInfo> &entity_scopes, bool render_qualifiers,
	                               string qualifier,
	                               CALLBACK &&callback) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (IsPerClassWrapper(conj) && !conj.children.empty()) {
			if (render_qualifiers) {
				qualifier = AppendQualifier(std::move(qualifier), PerClassQualifier(conj, fragments, entity_scopes));
			}
			VisitSourceComparisons(*conj.children[0], fragments, entity_scopes, render_qualifiers,
			                       std::move(qualifier), callback);
			return;
		}
		if (IsPerConstraintWrapper(conj) && !conj.children.empty()) {
			if (render_qualifiers) {
				qualifier = AppendQualifier(std::move(qualifier), PerQualifier(conj, fragments, entity_scopes));
			}
			VisitSourceComparisons(*conj.children[0], fragments, entity_scopes, render_qualifiers,
			                       std::move(qualifier), callback);
			return;
		}
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			if (render_qualifiers) {
				qualifier = AppendQualifier(WhenQualifier(conj, fragments, entity_scopes), qualifier);
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
		auto &cmp = expr.Cast<BoundComparisonExpression>();
		callback(cmp, HasDecideTag(cmp.GetAlias(), SCOPED_CLAUSE_TAG) ? FrontQualifier(qualifier) : qualifier);
	}
}

//! Whether an expression references a decision variable -- a column ref bound to the
//! DECIDE table index. Used to spot the one thing that forces canonicalization to move
//! a term across the comparison: a BOUND that contains a decision.
static bool ReferencesDecideVariable(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    expr.Cast<BoundColumnRefExpression>().binding.table_index == decide_index) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ReferencesDecideVariable(child, decide_index);
		}
	});
	return found;
}

//! One side of a comparison, its reducers printed as the comparison's clause spells them.
static string RenderSide(const BoundComparisonExpression &cmp, const Expression &side,
                         const vector<string> &fragments, const vector<EntityScopeInfo> &entity_scopes,
                         const vector<DecideGroupKey> *group_keys) {
	RenderContext ctx {fragments, entity_scopes, group_keys};
	ctx.scoped = HasDecideTag(cmp.GetAlias(), SCOPED_CLAUSE_TAG);
	return RenderSource(side, ctx);
}

vector<ConstraintSourceInfo> InitializeConstraintSourceInfo(Expression &constraints,
	                                                        const vector<string> &fragments,
	                                                        const vector<EntityScopeInfo> &entity_scopes,
	                                                        idx_t decide_index,
	                                                        const vector<DecideGroupKey> *group_keys) {
	vector<ConstraintSourceInfo> result;
	VisitSourceComparisons(constraints, fragments, entity_scopes, /*render_qualifiers=*/true, string(),
	                       [&](BoundComparisonExpression &cmp, const string &qualifier) {
		                       idx_t source_id = result.size();
		                       auto alias = cmp.GetAlias();
		                       AddDecideTag(alias, MakeSourceClauseTag(source_id));
		                       cmp.SetAlias(std::move(alias));
		                       // Rendered HERE, on the bound-but-not-yet-canonical tree, because
		                       // this is the last point at which the written spelling still
		                       // exists -- but only for the one rewrite that leaves algebra the
		                       // user cannot recognize, which is when BOTH sides bear decisions.
		                       //
		                       // That is the case the canonicalizer cannot resolve by moving a
		                       // side: it has to MERGE them, and `ship <= capacity * open`
		                       // becomes `ship - capacity * open <= 0` -- a clause the query
		                       // does not contain, against a literal bound that is not in it
		                       // either.
		                       //
		                       // When only one side bears decisions the rewrite is a clean move
		                       // and the leftover bound folds into something BETTER than what
		                       // was written: `(SELECT 7) >= x + 2` becomes `x <= 5`, turning an
		                       // opaque subquery into a number the user can edit. Quoting the
		                       // written form there would take that away, so this stays quiet.
		                       //
		                       // EXPLAIN asks a wider question than a repair does -- it wants
		                       // the written spelling of EVERY clause, so a plan can show what
		                       // became of it -- so the render happens unconditionally and the
		                       // narrower diagnostic signal is assigned from it.
		                       ConstraintSourceInfo info;
		                       // A scope written in front belongs to the clause as written:
		                       // canonicalization drops the `per K:` of a clause in the
		                       // aggregate shape, which still holds (its scope checks).
		                       if (HasDecideTag(cmp.GetAlias(), SCOPED_CLAUSE_TAG)) {
			                       info.qualifier = qualifier;
		                       }
		                       info.written_lhs = RenderSide(cmp, *cmp.left, fragments, entity_scopes, group_keys);
		                       info.written_rhs = RenderSide(cmp, *cmp.right, fragments, entity_scopes, group_keys);
		                       info.written_cmp = DecideComparisonOperator(cmp.type);
		                       if (ReferencesDecideVariable(*cmp.left, decide_index) &&
		                           ReferencesDecideVariable(*cmp.right, decide_index)) {
			                       info.source_lhs = info.written_lhs;
			                       info.source_rhs = info.written_rhs;
		                       }
		                       result.push_back(std::move(info));
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
		info.canonical_lhs = RenderSource(in, fragments, entity_scopes);
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
	                              const vector<EntityScopeInfo> &entity_scopes,
	                              const vector<DecideGroupKey> *group_keys) {
	auto &mutable_constraints = const_cast<Expression &>(constraints);
	VisitSourceComparisons(mutable_constraints, fragments, entity_scopes, true, string(),
	                       [&](BoundComparisonExpression &cmp, const string &qualifier) {
		                       idx_t source_id;
		                       if (!TryParseSourceClauseTag(cmp.GetAlias(), source_id) || source_id >= sources.size()) {
			                       return;
		                       }
		                       auto &info = sources[source_id];
		                       info.canonical_lhs = RenderSide(cmp, *cmp.left, fragments, entity_scopes, group_keys);
		                       info.canonical_rhs = RenderSide(cmp, *cmp.right, fragments, entity_scopes, group_keys);
		                       info.canonical_cmp = DecideComparisonOperator(cmp.type);
		                       if (!HasDecideTag(cmp.GetAlias(), SCOPED_CLAUSE_TAG)) {
			                       info.qualifier = qualifier;
		                       }
		                       // The written spelling is only worth carrying when it differs
		                       // from the canonical one. Two cases retire it here. When both
		                       // sides render identically nothing moved, so the canonical text
		                       // already IS the user's. When the sides were merely SWAPPED
		                       // (`100000 <= SUM(x)` canonicalizing to `SUM(x) >= 100000`) the
		                       // canonical form is a faithful, better-oriented reading of the
		                       // same clause, and the repair's offset belongs on the bound it
		                       // now names -- quoting the written order would put the offset on
		                       // the wrong side.
		                       bool unchanged = info.source_lhs == info.canonical_lhs &&
		                                        info.source_rhs == info.canonical_rhs;
		                       bool swapped = info.source_lhs == info.canonical_rhs &&
		                                      info.source_rhs == info.canonical_lhs;
		                       if (unchanged || swapped) {
			                       info.source_lhs.clear();
			                       info.source_rhs.clear();
		                       }
		                       if (!IsQueryWideBoundTag(cmp.GetAlias()) &&
		                           !ContainsSubquerySource(*cmp.right)) {
			                       info.rhs_kind = ConstraintSourceRhsKind::DATA_EXPRESSION;
		                       } else {
			                       info.rhs_kind = ConstraintSourceRhsKind::NUMERIC_FALLBACK;
		                       }
	                       });
}

string RenderDecideSource(const Expression &expr, const vector<string> &fragments,
                          const vector<EntityScopeInfo> &entity_scopes, const vector<DecideGroupKey> *group_keys) {
	return RenderSource(expr, fragments, entity_scopes, group_keys);
}

string RenderDecideSource(const Expression &expr, const DecideSourceNames &names, bool scoped) {
	RenderContext ctx {names.fragments, names.entity_scopes, &names.group_keys, &names.relations, scoped};
	return RenderSource(expr, ctx);
}

unordered_map<idx_t, string> DecideRelationNames(BindContext &bind_context, idx_t decide_index) {
	unordered_map<idx_t, string> relations;
	for (auto &binding : bind_context.GetBindingsList()) {
		if (binding->index != decide_index) {
			relations.emplace(binding->index, KeywordHelper::WriteOptionallyQuoted(binding->GetAlias()));
		}
	}
	if (relations.size() < 2) {
		relations.clear();
	}
	return relations;
}

string RenderDecideReducer(const BoundAggregateExpression &reducer, const DecideSourceNames &names) {
	RenderContext ctx {names.fragments, names.entity_scopes, nullptr, &names.relations, true};
	return RenderSource(reducer, ctx);
}

//! Push one rendered clause, keeping `out_source_ids` the same length as `out`.
static void PushClause(vector<string> &out, vector<idx_t> *out_source_ids, string rendered, idx_t source_id) {
	out.push_back(std::move(rendered));
	if (out_source_ids) {
		out_source_ids->push_back(source_id);
	}
}

//! The residue of a clause the optimizer lifted out of the tree (composed MIN/MAX
//! replaces its comparison with a TRUE placeholder). It carries no information, and
//! printing it as a bare `true` only makes a clause look like it vanished.
static bool IsTruePlaceholder(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &value = expr.Cast<BoundConstantExpression>().value;
	return value.type().id() == LogicalTypeId::BOOLEAN && !value.IsNull() && value.GetValue<bool>();
}

//! The scope spellings' qualifier for the two-level objective `outer(per k: inner(e)
//! by (k))`, which binds onto the old nested objective `OUTER(INNER(e)) PER k`: rendered
//! as written when the wrapper still holds the nested reducer, else null.
static unique_ptr<string> RenderTwoLevelObjective(const BoundConjunctionExpression &per,
                                                  const vector<string> &fragments,
                                                  const vector<EntityScopeInfo> &entity_scopes) {
	const Expression *body = per.children[0].get();
	string when;
	if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	    IsWhenConstraintWrapper(body->Cast<BoundConjunctionExpression>())) {
		auto &wrapper = body->Cast<BoundConjunctionExpression>();
		when = "when " + RenderSource(*wrapper.children[1], fragments, entity_scopes) + ": ";
		body = wrapper.children[0].get();
	}
	if (body->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return nullptr;
	}
	auto &outer = body->Cast<BoundAggregateExpression>();
	if (outer.children.size() != 1 || outer.children[0]->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return nullptr;
	}
	auto &inner = outer.children[0]->Cast<BoundAggregateExpression>();
	string key;
	for (idx_t i = 1; i < per.children.size(); i++) {
		key += (i > 1 ? ", " : "") + RenderSource(*per.children[i], fragments, entity_scopes);
	}
	string inner_body = inner.children.empty() ? string() : RenderSource(*inner.children[0], fragments, entity_scopes);
	return make_uniq<string>(when + StringUtil::Lower(outer.function.name) + "(per " + key + ": " +
	                         StringUtil::Lower(inner.function.name) + "(" + inner_body + ") by (" + key + "))");
}

//! `CollectDecideExpressionStrings` with the scope spellings' pending front qualifier.
static void CollectExpressionStrings(const Expression &expr, const vector<string> &fragments,
                                     const vector<EntityScopeInfo> &entity_scopes, vector<string> &out,
                                     const vector<ConstraintSourceInfo> *sources, vector<idx_t> *out_source_ids,
                                     const vector<DecideGroupKey> *group_keys, const string &front, bool scoped) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// `per K:` of the scope spellings: in front, like the `when` it encloses.
		if (IsPerClassWrapper(conj) && !conj.children.empty()) {
			auto per = PerClassQualifier(conj, fragments, entity_scopes);
			CollectExpressionStrings(*conj.children[0], fragments, entity_scopes, out, sources, out_source_ids,
			                         group_keys, AppendQualifier(front, per), true);
			return;
		}
		if (IsPerConstraintWrapper(conj) && HasDecideTag(conj.GetAlias(), SCOPED_CLAUSE_TAG)) {
			auto two_level = RenderTwoLevelObjective(conj, fragments, entity_scopes);
			if (two_level) {
				PushClause(out, out_source_ids, *two_level, DConstants::INVALID_INDEX);
				return;
			}
		}
		// PER wrapper: child[0] is the constraint, children[1..N] are the PER key columns.
		if (IsPerConstraintWrapper(conj) && conj.children.size() >= 2) {
			string suffix = " " + PerQualifier(conj, fragments, entity_scopes);
			vector<string> inner;
			vector<idx_t> inner_ids;
			CollectExpressionStrings(*conj.children[0], fragments, entity_scopes, inner, sources, &inner_ids,
			                         group_keys, string(), scoped);
			for (idx_t i = 0; i < inner.size(); i++) {
				PushClause(out, out_source_ids, inner[i] + suffix, inner_ids[i]);
			}
			return;
		}
		// WHEN wrapper: child[0] is the constraint, child[1] is the condition.
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			if (HasDecideTag(conj.GetAlias(), SCOPED_CLAUSE_TAG)) {
				CollectExpressionStrings(*conj.children[0], fragments, entity_scopes, out, sources, out_source_ids,
				                         group_keys,
				                         AppendQualifier(WhenQualifier(conj, fragments, entity_scopes), front), true);
				return;
			}
			string suffix = " WHEN " + RenderSource(*conj.children[1], fragments, entity_scopes);
			vector<string> inner;
			vector<idx_t> inner_ids;
			CollectExpressionStrings(*conj.children[0], fragments, entity_scopes, inner, sources, &inner_ids,
			                         group_keys, string(), scoped);
			for (idx_t i = 0; i < inner.size(); i++) {
				PushClause(out, out_source_ids, inner[i] + suffix, inner_ids[i]);
			}
			return;
		}
		// A plain AND is the constraint list itself, so each child is its own clause.
		for (auto &child : conj.children) {
			CollectExpressionStrings(*child, fragments, entity_scopes, out, sources, out_source_ids, group_keys,
			                         front, scoped);
		}
		return;
	}
	// A clause canonicalization rewrote reads back as it was written, for the same
	// reason a diagnosis quotes the written form: `ship - capacity * open <= 0` is not
	// a clause anyone can find in their query. Only set when the two forms differ.
	idx_t leaf_source_id = DConstants::INVALID_INDEX;
	TryParseSourceClauseTag(expr.GetAlias(), leaf_source_id);
	if (IsTruePlaceholder(expr)) {
		// Emitted as an empty string rather than dropped: the placeholder is the only
		// thing left marking where the extracted clause stood, and a caller ordering
		// clauses by tree position needs that position. Callers drop the empty string.
		PushClause(out, out_source_ids, string(), leaf_source_id);
		return;
	}
	auto front_qualifier = FrontQualifier(front);
	if (sources && expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		if (leaf_source_id != DConstants::INVALID_INDEX && leaf_source_id < sources->size()) {
			auto &info = (*sources)[leaf_source_id];
			if (!info.source_lhs.empty()) {
				auto &cmp = expr.Cast<BoundComparisonExpression>();
				PushClause(out, out_source_ids,
				           AttachDecideQualifier(info.source_lhs + " " + DecideComparisonOperator(cmp.type) + " " +
				                                     info.source_rhs,
				                                 front_qualifier),
				           leaf_source_id);
				return;
			}
		}
	}
	RenderContext ctx {fragments, entity_scopes, group_keys};
	ctx.scoped = scoped;
	PushClause(out, out_source_ids, AttachDecideQualifier(RenderSource(expr, ctx), front_qualifier), leaf_source_id);
}

void CollectDecideExpressionStrings(const Expression &expr, const vector<string> &fragments,
                                    const vector<EntityScopeInfo> &entity_scopes, vector<string> &out,
                                    const vector<ConstraintSourceInfo> *sources, vector<idx_t> *out_source_ids,
                                    const vector<DecideGroupKey> *group_keys, bool scoped) {
	CollectExpressionStrings(expr, fragments, entity_scopes, out, sources, out_source_ids, group_keys, string(),
	                         scoped);
}

string RenderDecideObjective(const Expression &expr, const vector<string> &fragments,
                             const vector<EntityScopeInfo> &entity_scopes, const vector<DecideGroupKey> *group_keys,
                             bool scoped) {
	vector<string> parts;
	CollectDecideExpressionStrings(expr, fragments, entity_scopes, parts, nullptr, nullptr, group_keys, scoped);
	string result;
	for (auto &part : parts) {
		if (part.empty()) {
			continue;
		}
		if (!result.empty()) {
			result += "\n";
		}
		result += part;
	}
	return result;
}

//! Reconstruct one term of a composed MIN/MAX clause as the user's algebra.
static string RenderComposedTerm(const LogicalDecide::ComposedMinMaxTerm &term,
                                 const vector<string> &fragments,
                                 const vector<EntityScopeInfo> &entity_scopes) {
	string body = term.inner_expr ? RenderSource(*term.inner_expr, fragments, entity_scopes) : string();
	idx_t scope_idx = term.qualifier_scope_idx;
	if (scope_idx != DConstants::INVALID_INDEX && scope_idx < entity_scopes.size()) {
		body = entity_scopes[scope_idx].table_alias + ": " + body;
	}
	string result = StringUtil::Upper(term.agg_name) + "(" + body + ")";
	if (term.filter) {
		result += " WHEN " + RenderSource(*term.filter, fragments, entity_scopes);
	}
	if (term.scale) {
		string scale = RenderSource(*term.scale, fragments, entity_scopes);
		result = term.scale_divides ? result + " / " + scale : scale + " * " + result;
	}
	return result;
}

//! What a composed MIN/MAX clause became. It has no rows in the tree -- the optimizer
//! replaced its comparison with a TRUE placeholder and moved the terms to
//! `composed_minmax_constraints` -- so the account is the one thing a reader cannot see
//! anywhere else: which reducers turned into auxiliary variables.
static void RenderComposedRewrite(const LogicalDecide::ComposedMinMaxConstraint &spec,
                                  const vector<string> &fragments,
                                  const vector<EntityScopeInfo> &entity_scopes, vector<string> &out) {
	for (auto &term : spec.terms) {
		if (term.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
			continue;
		}
		string line = RenderComposedTerm(term, fragments, entity_scopes) + " becomes an auxiliary variable";
		if (!term.is_easy) {
			line += " pinned by indicator rows";
		}
		out.push_back(std::move(line));
	}
}


vector<DecideClauseLayers> CollectDecideClauseLayers(
    optional_ptr<const Expression> constraints,
    const vector<LogicalDecide::ComposedMinMaxConstraint> &composed_minmax_constraints,
    const vector<string> &fragments, const vector<EntityScopeInfo> &entity_scopes,
    const vector<ConstraintSourceInfo> &sources, vector<string> &unattributed,
    const vector<DecideGroupKey> *group_keys) {
	// Every row the solver will see, tagged with the clause it came from. `sources` is
	// deliberately not passed: this walk wants the tree AS IT IS, not the written
	// substitution the flat renderer performs.
	vector<string> rows;
	vector<idx_t> row_ids;
	if (constraints) {
		CollectDecideExpressionStrings(*constraints, fragments, entity_scopes, rows, nullptr, &row_ids, group_keys);
	}
	vector<vector<string>> by_clause(sources.size());
	// Display follows the tree, not the registry. Source ids are handed out in two
	// passes -- every comparison first, then the DECIDE-variable INs -- so registry
	// order would print an `IN` written first as though it were written last.
	vector<idx_t> clause_order;
	vector<bool> ordered(sources.size(), false);
	for (idx_t i = 0; i < rows.size(); i++) {
		if (row_ids[i] >= by_clause.size()) {
			if (!rows[i].empty()) {
				unattributed.push_back(std::move(rows[i]));
			}
			continue;
		}
		if (!ordered[row_ids[i]]) {
			ordered[row_ids[i]] = true;
			clause_order.push_back(row_ids[i]);
		}
		if (!rows[i].empty()) {
			by_clause[row_ids[i]].push_back(std::move(rows[i]));
		}
	}
	// A clause the tree no longer mentions anywhere still belongs in the plan.
	for (idx_t i = 0; i < sources.size(); i++) {
		if (!ordered[i]) {
			clause_order.push_back(i);
		}
	}
	for (auto &spec : composed_minmax_constraints) {
		if (spec.source_clause_id < by_clause.size()) {
			RenderComposedRewrite(spec, fragments, entity_scopes, by_clause[spec.source_clause_id]);
		}
	}

	vector<DecideClauseLayers> result;
	for (auto i : clause_order) {
		auto &info = sources[i];
		DecideClauseLayers layers;
		string canonical = info.canonical_lhs;
		if (!info.canonical_cmp.empty()) {
			canonical += " " + info.canonical_cmp + " " + info.canonical_rhs;
		}
		canonical = AttachDecideQualifier(canonical, info.qualifier);
		// A clause with no written spelling is either a DECIDE-variable IN -- an opaque
		// marker the binder registers rather than a comparison -- or a plan serialized
		// before the written layer existed. Both read as canonical-only.
		if (info.written_lhs.empty() && info.written_cmp.empty()) {
			layers.written = canonical;
		} else {
			layers.written = AttachDecideQualifier(
			    info.written_lhs + " " + info.written_cmp + " " + info.written_rhs, info.qualifier);
			if (canonical != layers.written) {
				layers.canonical = canonical;
			}
		}
		// The solver form is worth printing only when it says something the layer above
		// did not: a different single row, or more than one row.
		auto &emitted = by_clause[i];
		const string &above = layers.canonical.empty() ? layers.written : layers.canonical;
		if (!(emitted.size() == 1 && emitted[0] == above)) {
			layers.rewritten = std::move(emitted);
		}
		result.push_back(std::move(layers));
	}
	return result;
}

//! Indented continuation lines. `RIGHT_ARROW` introduces a row the solver receives;
//! `IDENTICAL_TO` introduces the canonical reading of the clause above it.
static constexpr const char *LAYER_REWRITTEN_PREFIX = "  \xE2\x86\xB3 ";
static constexpr const char *LAYER_CANONICAL_PREFIX = "  \xE2\x89\xA1 ";

string RenderDecideClauseLayers(const vector<DecideClauseLayers> &layers,
                                const vector<string> &unattributed) {
	string result;
	auto append_line = [&](const string &line) {
		if (!result.empty()) {
			result += "\n";
		}
		result += line;
	};
	for (auto &clause : layers) {
		append_line(clause.written);
		if (!clause.canonical.empty()) {
			append_line(LAYER_CANONICAL_PREFIX + clause.canonical);
		}
		for (auto &row : clause.rewritten) {
			append_line(LAYER_REWRITTEN_PREFIX + row);
		}
	}
	// Rows the optimizer emitted without carrying an origin. They belong to the model,
	// so they are shown rather than dropped, but nothing can say which clause produced
	// them and pretending otherwise would be worse than admitting it.
	if (!unattributed.empty()) {
		append_line("added by formulation:");
		for (auto &row : unattributed) {
			append_line(LAYER_REWRITTEN_PREFIX + row);
		}
	}
	return result;
}

string RenderDecideObjectiveLayers(const string &sense_prefix, const string &written,
                                   const string &canonical, const vector<string> &rewritten) {
	string result;
	auto append_line = [&](const string &line) {
		if (!result.empty()) {
			result += "\n";
		}
		result += line;
	};
	string top = written;
	if (top.empty()) {
		// No written snapshot (a plan from before the objective layers existed): fall
		// back to the post-optimizer render, which is what EXPLAIN always showed.
		for (auto &row : rewritten) {
			append_line(sense_prefix + row);
		}
		return result;
	}
	append_line(sense_prefix + top);
	const string *above = &top;
	if (!canonical.empty()) {
		append_line(LAYER_CANONICAL_PREFIX + canonical);
		above = &canonical;
	}
	if (!(rewritten.size() == 1 && rewritten[0] == *above)) {
		for (auto &row : rewritten) {
			append_line(LAYER_REWRITTEN_PREFIX + row);
		}
	}
	return result;
}

} // namespace duckdb
