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
	//! The decision columns that stand for an expression (DecideSourceNames::aux_labels),
	//! which print as that expression; null prints a column's name.
	idx_t decide_index = DConstants::INVALID_INDEX;
	const vector<pair<idx_t, string>> *aux_labels = nullptr;
	//! The one reducer whose `by` is left out, for a message that names its group apart
	//! (RenderDecideReducer); the reducers inside it keep theirs.
	const BoundAggregateExpression *without_by = nullptr;
};
} // namespace

//! The context `names` describes.
static RenderContext ContextOf(const DecideSourceNames &names) {
	RenderContext ctx {names.fragments, names.entity_scopes, &names.group_keys, &names.relations};
	ctx.decide_index = names.decide_index;
	ctx.aux_labels = names.aux_labels;
	return ctx;
}

static string RenderSource(const Expression &expr, const RenderContext &ctx, int parent_precedence = 0);

//! A column name as SQL spells it: quoted where it would otherwise not parse -- a
//! keyword of DuckDB's, or of the DECIDE clause (`per`, `row`), or a name with spaces.
static string QuoteDecideIdentifier(const string &name) {
	auto lower = StringUtil::Lower(name);
	if (lower == "per" || lower == "row") {
		return KeywordHelper::WriteQuoted(name, '"');
	}
	return KeywordHelper::WriteOptionallyQuoted(name);
}

//! The binary arithmetic operator `expr` applies, looking through the casts the binder
//! inserted (which print as nothing); empty for anything else.
static string BinaryOperatorOf(const Expression &expr) {
	const Expression *node = &expr;
	while (node->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		idx_t fragment_id;
		if (TryParseSourceFragmentTag(node->GetAlias(), fragment_id)) {
			return string(); // a cast the user wrote prints as written, parentheses and all
		}
		node = node->Cast<BoundCastExpression>().child.get();
	}
	if (node->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return string();
	}
	auto &func = node->Cast<BoundFunctionExpression>();
	return func.children.size() == 2 ? func.function.name : string();
}

static string RenderFunction(const BoundFunctionExpression &func, const RenderContext &ctx, int parent_precedence) {
	auto name = func.function.name;
	if (func.children.size() == 1 && name == "-") {
		// `--` starts a SQL comment, so a negated negation keeps its parentheses.
		auto operand = RenderSource(*func.children[0], ctx, 40);
		return operand.rfind('-', 0) == 0 ? "-(" + operand + ")" : "-" + operand;
	}
	if (func.children.size() == 2 &&
	    (name == "+" || name == "-" || name == "*" || name == "/" || name == "%" || name == "**")) {
		int precedence = (name == "+" || name == "-") ? 10 : ((name == "**") ? 30 : 20);
		// Every operator here groups left to right, so a right operand at the same level
		// keeps its parentheses -- `a - (b + c)`, `a * (b % c)`, `a ** (b ** c)` -- except
		// where regrouping cannot change the value: `a + (b + c)`, `a + (b - c)`,
		// `a * (b * c)`.
		auto right_op = BinaryOperatorOf(*func.children[1]);
		bool regroups = (name == "+") || (name == "*" && right_op == "*");
		string result = RenderSource(*func.children[0], ctx, precedence) + " " + name + " " +
		                RenderSource(*func.children[1], ctx, precedence + (regroups ? 0 : 1));
		return precedence < parent_precedence ? "(" + result + ")" : result;
	}
	if (func.is_operator && func.children.size() == 1) {
		return name + "(" + RenderSource(*func.children[0], ctx) + ")";
	}
	if (func.is_operator && func.children.size() == 2) {
		return "(" + RenderSource(*func.children[0], ctx) + " " + name + " " + RenderSource(*func.children[1], ctx) +
		       ")";
	}
	string result = name + "(";
	for (idx_t i = 0; i < func.children.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += RenderSource(*func.children[i], ctx);
	}
	result += ")";
	return result;
}

//! The reducer a hard MIN/MAX's carrier stands for: the optimizer rebuilds `max(e)` as a
//! `sum` tagged `__minmax_clause_<i>_max__` (EmitHardMinMaxClause), which stage 06 reads
//! as the extremum; a plan prints the reducer, not the carrier. Empty for any other.
static string MinMaxCarrierName(const BoundAggregateExpression &agg) {
	string payload;
	if (!ExtractDecideTagPayload(agg.GetAlias(), MINMAX_CLAUSE_TAG_PREFIX, payload)) {
		return string();
	}
	auto sep = payload.find('_');
	return sep == string::npos ? string() : payload.substr(sep + 1);
}

//! The order a `norm` marker carries (`1`, `2`, `inf`, `0_auto`, `0_<m>`) as the user
//! wrote it after the body -- `'inf'` (a string literal: `inf` alone names a column),
//! `0`, or `0, <m>` -- or empty for a plain reducer.
static string NormOrder(const BoundAggregateExpression &agg) {
	auto marker_pos = agg.GetAlias().find(NORM_MARKER_TAG_PREFIX);
	if (marker_pos == string::npos) {
		return string();
	}
	auto begin = marker_pos + strlen(NORM_MARKER_TAG_PREFIX);
	auto end = agg.GetAlias().find("__", begin);
	if (end == string::npos) {
		return string();
	}
	auto payload = agg.GetAlias().substr(begin, end - begin);
	if (payload == "0_auto") {
		return "0";
	}
	if (payload.rfind("0_", 0) == 0) {
		return "0, " + payload.substr(2);
	}
	if (payload == "inf") {
		return "'inf'";
	}
	return payload;
}

//! A reducer as the query writes it: `sum(when c per K: e) by (g)`. The colon form
//! (`SUM(D: e)`) prints as written.
static string RenderReducer(const BoundAggregateExpression &agg, const string &body, const RenderContext &ctx) {
	string scope;
	if (agg.filter) {
		scope = "when " + RenderSource(*agg.filter, ctx);
	}
	auto order = NormOrder(agg);
	auto carrier = MinMaxCarrierName(agg);
	if (carrier.empty() && order.empty() && agg.children.empty() &&
	    StringUtil::CIEquals(agg.function.name, "count_star")) {
		// `count(*)` binds as `count_star()`; it takes no scope inside it.
		idx_t group_key;
		bool has_by = ctx.group_keys && &agg != ctx.without_by && TryParseGroupKeyTag(agg.GetAlias(), group_key) &&
		              group_key < ctx.group_keys->size();
		return (scope.empty() ? "count(*)" : "sum(" + scope + ": 1)") +
		       (has_by ? " by (" + (*ctx.group_keys)[group_key].text + ")" : string());
	}
	string result = (!carrier.empty() ? carrier : order.empty() ? StringUtil::Lower(agg.function.name) : string("norm")) +
	                "(";
	idx_t scope_idx;
	if (TryParseQualifiedReducerTag(agg.GetAlias(), scope_idx) && scope_idx < ctx.entity_scopes.size()) {
		auto &key = ctx.entity_scopes[scope_idx];
		if (HasDecideTag(agg.GetAlias(), COLON_FORM_TAG)) {
			result += key.table_alias + ": ";
		} else {
			scope += (scope.empty() ? "per " : " per ") + key.table_alias;
		}
	}
	result += (scope.empty() ? "" : scope + ": ") + body + (order.empty() ? "" : ", " + order) + ")";
	idx_t group_key;
	if (ctx.group_keys && &agg != ctx.without_by && TryParseGroupKeyTag(agg.GetAlias(), group_key) &&
	    group_key < ctx.group_keys->size()) {
		result += " by (" + (*ctx.group_keys)[group_key].text + ")";
	}
	return result;
}

//! `(e)` as `e` when the parentheses enclose the whole string: a labelled auxiliary such
//! as `(open * ship)` or `(ship <> 0)` reads inside `sum(...)` without a second pair.
static string WithoutEnclosingParentheses(string text) {
	if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
		return text;
	}
	int depth = 0;
	for (idx_t i = 0; i + 1 < text.size(); i++) {
		depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
		if (depth == 0) {
			return text; // the opening parenthesis closes before the end
		}
	}
	return text.substr(1, text.size() - 2);
}

static string RenderAggregate(const BoundAggregateExpression &agg, const RenderContext &ctx) {
	string body;
	for (idx_t i = 0; i < agg.children.size(); i++) {
		if (i > 0) {
			body += ", ";
		}
		body += RenderSource(*agg.children[i], ctx);
	}

	return RenderReducer(agg, body, ctx);
}

//! An expression with the registries alone -- no relation names, no auxiliary labels: the form
//! the clause renderers in this file use. `group_keys` prints each reducer's `by`.
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
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT && HasDecideTag(expr.GetAlias(), L0_AUTO_BOUND_TAG)) {
		return "M"; // stage 06 derives it from the column box
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CAST:
		// Untagged casts were introduced by DuckDB binding and are display noise. The
		// child takes the cast's place, precedence included: `2 * CAST(a + b AS DOUBLE)`
		// prints `2 * (a + b)`.
		return RenderSource(*expr.Cast<BoundCastExpression>().child, ctx, parent_precedence);
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (ctx.aux_labels && colref.binding.table_index == ctx.decide_index) {
			for (auto &labelled : *ctx.aux_labels) {
				if (labelled.first == colref.binding.column_index) {
					// A label is parenthesized for arithmetic (`(b * x)`); as a whole operand
					// -- a reducer's body, a side of a comparison -- it needs none.
					return parent_precedence == 0 ? WithoutEnclosingParentheses(labelled.second) : labelled.second;
				}
			}
		}
		auto name = StripDecideTags(colref.GetName());
		if (ctx.relations) {
			auto relation = ctx.relations->find(colref.binding.table_index);
			if (relation != ctx.relations->end()) {
				return relation->second + "." + QuoteDecideIdentifier(name);
			}
		}
		if (!name.empty()) {
			return QuoteDecideIdentifier(name);
		}
		break;
	}
	case ExpressionClass::BOUND_REF: {
		// A column the physical plan resolved to a chunk position keeps its name as alias.
		auto name = StripDecideTags(expr.GetAlias());
		if (!name.empty()) {
			return QuoteDecideIdentifier(name);
		}
		break;
	}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &cmp = expr.Cast<BoundComparisonExpression>();
		return RenderSource(*cmp.left, ctx) + " " + DecideComparisonOperator(cmp.type) + " " +
		       RenderSource(*cmp.right, ctx);
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		string op = conj.type == ExpressionType::CONJUNCTION_OR ? " OR " : " AND ";
		string result;
		for (idx_t i = 0; i < conj.children.size(); i++) {
			if (i > 0) {
				result += op;
			}
			// AND binds tighter than OR: an OR under an AND keeps its parentheses.
			auto &child = *conj.children[i];
			bool nested_or = conj.type == ExpressionType::CONJUNCTION_AND &&
			                 child.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
			                 child.type == ExpressionType::CONJUNCTION_OR;
			auto rendered = RenderSource(child, ctx);
			result += nested_or ? "(" + rendered + ")" : rendered;
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

string RenderDecidePerKey(const BoundConjunctionExpression &conj, const vector<string> &fragments) {
	idx_t fragment_id;
	if (TryParseSourceFragmentTag(conj.GetAlias(), fragment_id) && fragment_id < fragments.size()) {
		return fragments[fragment_id];
	}
	string key;
	for (idx_t i = 1; i < conj.children.size(); i++) {
		key += (i > 1 ? ", " : "") + conj.children[i]->GetName();
	}
	return key.empty() ? string("()") : key;
}

//! `per K`, in front of its clause.
static string PerClassQualifier(const BoundConjunctionExpression &conj, const vector<string> &fragments) {
	return "per " + RenderDecidePerKey(conj, fragments);
}

//! The qualifier of a `when` wrapper: `when c`, in front of its clause.
static string WhenQualifier(const BoundConjunctionExpression &conj, const vector<string> &fragments,
                            const vector<EntityScopeInfo> &entity_scopes) {
	return "when " + RenderSource(*conj.children[1], fragments, entity_scopes);
}

//! A qualifier (`when c per K`) is written in front of its clause and ends in a colon.
static string FrontQualifier(const string &qualifier) {
	return qualifier.empty() ? qualifier : qualifier + ":";
}

//! Every comparison of a constraint tree with the `when c per K:` qualifier of the
//! wrappers it sits under.
template <class CALLBACK>
static void VisitSourceComparisons(Expression &expr, const vector<string> &fragments,
	                               const vector<EntityScopeInfo> &entity_scopes, string qualifier,
	                               CALLBACK &&callback) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (IsPerClassWrapper(conj) && !conj.children.empty()) {
			VisitSourceComparisons(*conj.children[0], fragments, entity_scopes,
			                       AppendQualifier(std::move(qualifier), PerClassQualifier(conj, fragments)), callback);
			return;
		}
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			VisitSourceComparisons(*conj.children[0], fragments, entity_scopes,
			                       AppendQualifier(WhenQualifier(conj, fragments, entity_scopes), qualifier), callback);
			return;
		}
		for (auto &child : conj.children) {
			VisitSourceComparisons(*child, fragments, entity_scopes, qualifier, callback);
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		callback(expr.Cast<BoundComparisonExpression>(), FrontQualifier(qualifier));
	}
}

//! The qualifier of an IN marker, which VisitSourceComparisons passes over: the front
//! qualifier of the wrappers it sits under.
static string ScopedInQualifier(Expression &constraints, const Expression &in, const vector<string> &fragments,
                                const vector<EntityScopeInfo> &entity_scopes) {
	string found;
	std::function<void(Expression &, string)> visit = [&](Expression &expr, string qualifier) {
		if (&expr == &in) {
			found = FrontQualifier(qualifier);
			return;
		}
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
			return;
		}
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (IsPerClassWrapper(conj) && !conj.children.empty()) {
			visit(*conj.children[0], AppendQualifier(std::move(qualifier), PerClassQualifier(conj, fragments)));
			return;
		}
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			visit(*conj.children[0], AppendQualifier(WhenQualifier(conj, fragments, entity_scopes), qualifier));
			return;
		}
		for (auto &child : conj.children) {
			visit(*child, qualifier);
		}
	};
	visit(constraints, string());
	return found;
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

vector<ConstraintSourceInfo> InitializeConstraintSourceInfo(Expression &constraints,
	                                                        const vector<string> &fragments,
	                                                        const vector<EntityScopeInfo> &entity_scopes,
	                                                        idx_t decide_index,
	                                                        const vector<DecideGroupKey> *group_keys) {
	vector<ConstraintSourceInfo> result;
	VisitSourceComparisons(constraints, fragments, entity_scopes, string(),
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
		                       // aggregate shape, which still holds (its classes).
		                       info.written_lhs = RenderSource(*cmp.left, fragments, entity_scopes, group_keys);
		                       info.written_rhs = RenderSource(*cmp.right, fragments, entity_scopes, group_keys);
		                       info.written_cmp = DecideComparisonOperator(cmp.type);
		                       info.qualifier = qualifier;
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
		// Its scope in front, as a comparison's: the IN itself shows no scope word.
		info.qualifier = ScopedInQualifier(constraints, in, fragments, entity_scopes);
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
	VisitSourceComparisons(mutable_constraints, fragments, entity_scopes, string(),
	                       [&](BoundComparisonExpression &cmp, const string &) {
		                       idx_t source_id;
		                       if (!TryParseSourceClauseTag(cmp.GetAlias(), source_id) || source_id >= sources.size()) {
			                       return;
		                       }
		                       auto &info = sources[source_id];
		                       info.canonical_lhs = RenderSource(*cmp.left, fragments, entity_scopes, group_keys);
		                       info.canonical_rhs = RenderSource(*cmp.right, fragments, entity_scopes, group_keys);
		                       info.canonical_cmp = DecideComparisonOperator(cmp.type);
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

string RenderDecideSource(const Expression &expr, const DecideSourceNames &names) {
	return RenderSource(expr, ContextOf(names));
}

DecideSourceNames DecidePlanNames(const vector<string> &fragments, const vector<EntityScopeInfo> &entity_scopes,
                                  const vector<DecideGroupKey> &group_keys, idx_t decide_index,
                                  const vector<pair<idx_t, string>> &aux_labels) {
	static const unordered_map<idx_t, string> no_relations;
	DecideSourceNames names {fragments, entity_scopes, group_keys, no_relations};
	names.decide_index = decide_index;
	names.aux_labels = &aux_labels;
	return names;
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
	auto ctx = ContextOf(names);
	ctx.without_by = &reducer;
	return RenderSource(reducer, ctx);
}

//! The source id CollectExpressionStrings reports for a row a rewrite added for the
//! objective (OBJECTIVE_ROW_TAG): no clause's, and not INVALID_INDEX, which is a row no
//! one claims.
static constexpr idx_t OBJECTIVE_ROW_SOURCE_ID = DConstants::INVALID_INDEX - 1;

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

//! `CollectDecideExpressionStrings` with the pending `when c per K:` front qualifier.
static void CollectExpressionStrings(const Expression &expr, const DecideSourceNames &names, vector<string> &out,
                                     const vector<ConstraintSourceInfo> *sources, vector<idx_t> *out_source_ids,
                                     const string &front) {
	auto &fragments = names.fragments;
	auto &entity_scopes = names.entity_scopes;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// A clause's `per K:` prints in front, like the `when` it encloses.
		if (IsPerClassWrapper(conj) && !conj.children.empty()) {
			auto per = PerClassQualifier(conj, fragments);
			CollectExpressionStrings(*conj.children[0], names, out, sources, out_source_ids, AppendQualifier(front, per));
			return;
		}
		// `when c:`: child[0] is the constraint, child[1] the condition; in front.
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			CollectExpressionStrings(*conj.children[0], names, out, sources, out_source_ids,
			                         AppendQualifier(WhenQualifier(conj, fragments, entity_scopes), front));
			return;
		}
		// A plain AND is the constraint list itself, so each child is its own clause.
		for (auto &child : conj.children) {
			CollectExpressionStrings(*child, names, out, sources, out_source_ids, front);
		}
		return;
	}
	// A clause canonicalization rewrote reads back as it was written, for the same
	// reason a diagnosis quotes the written form: `ship - capacity * open <= 0` is not
	// a clause anyone can find in their query. Only set when the two forms differ.
	idx_t leaf_source_id = DConstants::INVALID_INDEX;
	if (!TryParseSourceClauseTag(expr.GetAlias(), leaf_source_id) && HasDecideTag(expr.GetAlias(), OBJECTIVE_ROW_TAG)) {
		leaf_source_id = OBJECTIVE_ROW_SOURCE_ID;
	}
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
	PushClause(out, out_source_ids, AttachDecideQualifier(RenderSource(expr, ContextOf(names)), front_qualifier),
	           leaf_source_id);
}

void CollectDecideExpressionStrings(const Expression &expr, const DecideSourceNames &names, vector<string> &out,
                                    const vector<ConstraintSourceInfo> *sources, vector<idx_t> *out_source_ids) {
	CollectExpressionStrings(expr, names, out, sources, out_source_ids, string());
}

string RenderDecideObjective(const Expression &expr, const DecideSourceNames &names) {
	vector<string> parts;
	CollectDecideExpressionStrings(expr, names, parts, nullptr, nullptr);
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

vector<DecideClauseLayers> CollectDecideClauseLayers(optional_ptr<const Expression> constraints,
                                                      const DecideSourceNames &names,
                                                      const vector<ConstraintSourceInfo> &sources,
                                                      vector<string> &unattributed, vector<string> &objective_rows,
                                                      const vector<LogicalDecide::RewriteAccount> &accounts) {
	// Every row the solver will see, tagged with the clause it came from. `sources` is
	// deliberately not passed: this walk wants the tree AS IT IS, not the written
	// substitution the flat renderer performs.
	vector<string> rows;
	vector<idx_t> row_ids;
	if (constraints) {
		CollectDecideExpressionStrings(*constraints, names, rows, nullptr, &row_ids);
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
				(row_ids[i] == OBJECTIVE_ROW_SOURCE_ID ? objective_rows : unattributed).push_back(std::move(rows[i]));
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
	// What each rewrite made of the clause comes first, then the rows it left behind.
	vector<vector<string>> told(sources.size());
	for (auto &account : accounts) {
		if (account.source_clause_id < told.size()) {
			told[account.source_clause_id].push_back(RenderDecideRewriteAccount(account));
		}
	}
	for (idx_t i = 0; i < told.size(); i++) {
		if (!told[i].empty()) {
			told[i].insert(told[i].end(), std::make_move_iterator(by_clause[i].begin()),
			               std::make_move_iterator(by_clause[i].end()));
			by_clause[i] = std::move(told[i]);
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
		// A clause with no written spelling is either a DECIDE-variable IN -- an opaque
		// marker the binder registers rather than a comparison -- or a plan serialized
		// before the written layer existed. Both read as canonical-only.
		string written = info.written_lhs.empty() && info.written_cmp.empty()
		                     ? canonical
		                     : info.written_lhs + " " + info.written_cmp + " " + info.written_rhs;
		layers.written = AttachDecideQualifier(written, info.qualifier);
		if (canonical != written) {
			layers.canonical = AttachDecideQualifier(canonical, info.qualifier);
		}
		// The solver form is worth printing only where it says something the layer above
		// did not: a row that reads as that layer is left out, whatever the rewrite put
		// beside it (an ABS leaves the clause's own row next to its envelope rows).
		const string above = AttachDecideQualifier(layers.canonical.empty() ? written : canonical, info.qualifier);
		for (auto &row : by_clause[i]) {
			if (row != above) {
				layers.rewritten.push_back(std::move(row));
			}
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

string RenderDecideRewriteAccount(const LogicalDecide::RewriteAccount &account) {
	using Kind = LogicalDecide::RewriteAccount::Kind;
	const char *became = "";
	switch (account.kind) {
	case Kind::AVG_SUM:
		became = "a sum divided by its row count";
		break;
	case Kind::NORM_SUM_ABS:
		became = "a sum of absolute values";
		break;
	case Kind::NORM_SUM_SQUARES:
		became = "a sum of squares";
		break;
	case Kind::NORM_MAX_ABS:
		became = "a maximum of absolute values";
		break;
	case Kind::EXTREMUM_EACH_MEMBER:
		became = "a bound on each member";
		break;
	case Kind::ABS_AUXILIARY:
	case Kind::PRODUCT_AND:
		became = "an auxiliary variable";
		break;
	case Kind::ABS_AUXILIARY_SIGN_INDICATOR:
		became = "an auxiliary variable with a sign indicator";
		break;
	case Kind::ABS_AUXILIARY_NATIVE:
		became = "an auxiliary variable stated to the solver as an absolute value";
		break;
	case Kind::PRODUCT_MCCORMICK:
		became = "an auxiliary variable linearized by McCormick rows";
		break;
	case Kind::IN_INDICATORS:
		became = "one indicator per value";
		break;
	case Kind::L0_INDICATORS:
		became = "one indicator per row";
		break;
	case Kind::VALUE_HELPER:
		became = "a helper decision";
		break;
	case Kind::VALUE_HELPER_PER_GROUP:
		became = "a helper decision per group";
		break;
	case Kind::FRAME_HELPER_PER_TIMELINE:
		became = "a helper decision per timeline";
		break;
	case Kind::FRAME_HELPER_PER_POSITION:
		became = "a helper decision per position";
		break;
	case Kind::EXTREMUM:
		became = "an extremum column";
		break;
	case Kind::EXTREMUM_PINNED:
		became = "an extremum column pinned by indicator rows";
		break;
	case Kind::NOT_EQUAL:
		became = "two conditional rows with an indicator";
		break;
	case Kind::VARIANTS:
		return account.subject;
	}
	return account.subject + " becomes " + became;
}

string RenderDecideObjectiveLayers(const string &sense_prefix, const string &written, const string &canonical,
                                   const vector<string> &rewritten, const vector<string> &accounts,
                                   const vector<string> &derived_rows) {
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
	// What the rewrites made of it, then the objective as the solver receives it where it
	// reads differently, then the rows the rewrites added for it (an ABS envelope, a
	// product's corners), which belong to the objective rather than to any clause.
	for (auto &account : accounts) {
		append_line(LAYER_REWRITTEN_PREFIX + account);
	}
	for (auto &row : rewritten) {
		if (row != *above) {
			append_line(LAYER_REWRITTEN_PREFIX + row);
		}
	}
	for (auto &row : derived_rows) {
		append_line(LAYER_REWRITTEN_PREFIX + row);
	}
	return result;
}

} // namespace duckdb
