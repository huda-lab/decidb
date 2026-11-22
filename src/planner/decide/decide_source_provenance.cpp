#include "duckdb/planner/decide/decide_source_provenance.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_decide_null_test_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"
#include "duckdb/planner/bind_context.hpp"

#include <cmath>
#include <cstring>

namespace duckdb {

//! `text` without the one pair of parentheses that encloses all of it, else `text` itself.
static string WithoutEnclosingParentheses(const string &text) {
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

//! Spell a comparison the way DECIDE documents it. `ExpressionTypeToOperator` renders
//! not-equal as `!=`, but every other user-facing DECIDE surface -- the syntax
//! reference, the binder's own messages -- writes `<>`. A repair
//! quotes this text straight back as SQL the user is meant to recognize, so the one
//! spelling wins here too.
static string DecideComparisonOperator(ExpressionType type) {
	return type == ExpressionType::COMPARE_NOTEQUAL ? "<>" : ExpressionTypeToOperator(type);
}

namespace {
//! What rendering a bound clause reads besides the tree.
struct RenderContext {
	//! The query's text: a located node is cut from it.
	const DecideSourceText &text;
	const vector<KeyedScopeInfo> &keyed_scopes;
	//! The keys, so a reducer prints its `per K` and `by (...)`; null prints neither.
	const vector<DecideKey> *keys = nullptr;
	//! The decision columns that stand for an expression (DecideSourceNames::aux_labels),
	//! which print as that expression; null prints a column's name.
	idx_t decide_index = DConstants::INVALID_INDEX;
	const vector<pair<idx_t, string>> *aux_labels = nullptr;
	//! True quotes every located node as written, as a message and a diagnosis do. False quotes the data-only ones and
	//! reprints the decision structure, which canonicalization and the rewrites reshape, as a plan's layers do.
	bool written = false;
	//! The frames (DecideSourceNames::frames); null prints a frame marker's aggregate.
	const vector<DecideFrameSpec> *frames = nullptr;
	//! True for a row a rewrite built (RenderFunctionRow): a term of zero it added is left out, and a number it wrote
	//! prints as a plain number.
	bool rewrite_row = false;
	//! The body of a BETWEEN half being rendered: it carries the BETWEEN's location, but is half of its value, so it is
	//! not quoted by that span. Null for any other render.
	const Expression *between_half = nullptr;
};
} // namespace

//! The context `names` describes.
static RenderContext ContextOf(const DecideSourceNames &names) {
	RenderContext ctx {names.text, names.keyed_scopes, &names.keys};
	ctx.decide_index = names.decide_index;
	ctx.aux_labels = names.aux_labels;
	ctx.written = names.written;
	ctx.frames = names.frames;
	return ctx;
}

static string RenderSource(const Expression &expr, const RenderContext &ctx, int parent_precedence = 0);

//! The precedence of an operand of a node with no arithmetic of its own (`NOT`, `IS NULL`, `IN`, a CASE): the level of
//! a loose operator (DecideBinding::LOOSE), so an arithmetic operand needs no parentheses there.
static constexpr int OPERAND_PRECEDENCE = 5;

//! A child standing aside for its text while its parent prints (RenderSource): the text as it is, whatever it holds.
class WrittenOperand : public BoundReferenceExpression {
public:
	WrittenOperand(string text_p, LogicalType type)
	    : BoundReferenceExpression(std::move(type), 0U), text(std::move(text_p)) {
	}
	string ToString() const override {
		return text;
	}
	string GetName() const override {
		return text;
	}

private:
	string text;
};

bool DecideIsGuarded(const Expression &expr) {
	return HasDecideTag(expr.GetAlias(), NULL_GUARD_TAG);
}

const Expression &DecideUnguarded(const Expression &expr) {
	if (!DecideIsGuarded(expr)) {
		return expr;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CASE) {
		return *expr.Cast<BoundCaseExpression>().case_checks[0].then_expr;
	}
	D_ASSERT(expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	         expr.type == ExpressionType::OPERATOR_COALESCE);
	return *expr.Cast<BoundOperatorExpression>().children[0];
}

//! The binary arithmetic operator `expr` applies, looking through the casts the binder
//! inserted (which print as nothing); empty for anything else.
static string BinaryOperatorOf(const Expression &expr) {
	const Expression *node = &expr;
	while (node->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &child = *node->Cast<BoundCastExpression>().child;
		if (!(node->GetQueryLocation() == child.GetQueryLocation())) {
			return string(); // a cast the user wrote prints as written, parentheses and all
		}
		node = &child;
	}
	if (node->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return string();
	}
	auto &func = node->Cast<BoundFunctionExpression>();
	return func.children.size() == 2 ? func.function.name : string();
}

//! How tightly a binary arithmetic operator binds; 0 for any other name.
static int ArithmeticPrecedence(const string &name) {
	if (name == "+" || name == "-") {
		return 10;
	}
	if (name == "*" || name == "/" || name == "%") {
		return 20;
	}
	return name == "**" ? 30 : 0;
}

//! Whether `expr` is a zero a rewrite wrote: a numeric constant with no span of the query (the user's own zeros have
//! one), under the casts the binder inserted.
static bool IsRewriteZero(const Expression &expr) {
	const Expression *node = &expr;
	while (node->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		node = node->Cast<BoundCastExpression>().child.get();
	}
	if (node->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT || node->GetQueryLocation().IsValid()) {
		return false;
	}
	auto &value = node->Cast<BoundConstantExpression>().value;
	return !value.IsNull() && value.type().IsNumeric() && value.GetValue<double>() == 0.0;
}

//! A number a rewrite wrote, as a plan prints it: a whole number without its `.0`, any other as DuckDB prints it.
static string RewriteNumberText(const Value &value) {
	const double number = value.GetValue<double>();
	if (std::floor(number) == number && std::fabs(number) < 1e15) {
		return to_string(static_cast<int64_t>(number));
	}
	return value.ToString();
}

//! `-operand`; `--` starts a SQL comment, so a negated negation keeps its parentheses.
static string RenderNegation(const Expression &operand, const RenderContext &ctx) {
	auto text = RenderSource(operand, ctx, 40);
	return text.rfind('-', 0) == 0 ? "-(" + text + ")" : "-" + text;
}

static string RenderFunction(const BoundFunctionExpression &func, const RenderContext &ctx, int parent_precedence) {
	// As the user wrote it: `abs` for `@`, `pow`, `power` and `^` for `**`.
	auto name = DecideWrittenName(func);
	if (func.children.size() == 1 && name == "-") {
		return RenderNegation(*func.children[0], ctx);
	}
	// A zero term a rewrite added says nothing: `0 + v` is `v` and `0 - v` is `-v`.
	if (ctx.rewrite_row && func.children.size() == 2 && (name == "+" || name == "-") &&
	    IsRewriteZero(*func.children[0])) {
		return name == "+" ? RenderSource(*func.children[1], ctx, parent_precedence)
		                   : RenderNegation(*func.children[1], ctx);
	}
	if (func.children.size() == 2 && ArithmeticPrecedence(name) != 0) {
		int precedence = ArithmeticPrecedence(name);
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
	if (func.is_operator) {
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

//! The order of a norm as the user wrote it after the body (DecideNorm::Written), or empty for a plain reducer.
static string NormOrder(const BoundAggregateExpression &agg) {
	DecideNorm norm;
	if (!IsDecideNormMarker(agg, norm)) {
		// Canonicalization replaced the norm with its definition, which keeps the order (RenderAggregate reads the
		// body back out of the definition).
		norm = DecideWrittenNorm(agg);
	}
	return norm.Written();
}

static bool HoldsSkipNullTest(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_NULL_TEST) {
		return true;
	}
	if (expr.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
		return false;
	}
	for (auto &child : expr.Cast<BoundConjunctionExpression>().children) {
		if (HoldsSkipNullTest(*child)) {
			return true;
		}
	}
	return false;
}

//! A reducer's own `when` as written: without the tests SkipNullData added, which the
//! query never wrote; empty when nothing else is left.
static string RenderWrittenWhen(const Expression &filter, const RenderContext &ctx) {
	if (!HoldsSkipNullTest(filter)) {
		return RenderSource(filter, ctx);
	}
	string written;
	if (filter.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : filter.Cast<BoundConjunctionExpression>().children) {
			written += RenderWrittenWhen(*child, ctx);
		}
	}
	return written;
}

//! A reducer's `by` key as it wrote it, else as the key it shares was first written.
static string ByText(const BoundAggregateExpression &agg, const RenderContext &ctx, idx_t group_key) {
	string written;
	if (ctx.text.Cut(agg.GetQueryLocation(), DecideSpanRole::BY, written)) {
		return written;
	}
	return (*ctx.keys)[group_key].text;
}

//! A reducer reprinted: `sum(when c per K: e) by (g)`, for a reducer with no span and, in a plan layer, for one that
//! reads a decision (a plan reprints decision structure).
static string RenderReducer(const BoundAggregateExpression &agg, const string &body, const RenderContext &ctx) {
	string scope;
	if (agg.filter) {
		auto written = RenderWrittenWhen(*agg.filter, ctx);
		if (!written.empty()) {
			scope = "when " + written;
		}
	}
	auto order = NormOrder(agg);
	if (order.empty() && agg.children.empty()) {
		// `count(*)` binds as `count_star()`; it takes no scope inside it.
		const idx_t group_key = DecideReducerGroupKey(agg);
		bool has_by = ctx.keys && group_key != DConstants::INVALID_INDEX;
		return "count(*)" + (has_by ? " by (" + ByText(agg, ctx, group_key) + ")" : string());
	}
	string result = (order.empty() ? StringUtil::Lower(DecideWrittenName(agg)) : string("norm")) + "(";
	idx_t scope_idx;
	if (ctx.keys && DecideReducerPer(agg, scope_idx)) {
		// The key as this reducer wrote it, else as the key was first written.
		string key_text;
		if (!ctx.text.Cut(agg.GetQueryLocation(), DecideSpanRole::PER, key_text)) {
			key_text = DecideKeyText(*ctx.keys, ctx.keyed_scopes[scope_idx].key);
		}
		scope += (scope.empty() ? "per " : " per ") + key_text;
	}
	result += (scope.empty() ? "" : scope + ": ") + body + (order.empty() ? "" : ", " + order) + ")";
	const idx_t group_key = DecideReducerGroupKey(agg);
	if (ctx.keys && group_key != DConstants::INVALID_INDEX) {
		result += " by (" + ByText(agg, ctx, group_key) + ")";
	}
	return result;
}

static string RenderAggregate(const BoundAggregateExpression &agg, const RenderContext &ctx) {
	// A frame marker is `first(e)` or the range's reducer, which no one wrote: it prints as the frame.
	idx_t frame;
	if (ctx.frames && TryParseFrameTag(agg.GetAlias(), frame) && frame < ctx.frames->size()) {
		return (*ctx.frames)[frame].spelling;
	}
	string body = agg.children.empty() ? string() : RenderSource(*agg.children[0], ctx);
	// A norm's definition reads as written, `norm(e, p)`.
	if (auto written_body = DecideWrittenNormBody(agg)) {
		body = RenderSource(*written_body, ctx);
	}
	return RenderReducer(agg, body, ctx);
}

const Expression *DecideWrittenNormBody(const BoundAggregateExpression &reducer) {
	return DecideNormBody(reducer, DecideWrittenNorm(reducer));
}

const Expression *DecideNormBody(const BoundAggregateExpression &reducer, const DecideNorm &norm) {
	if (!norm) {
		return nullptr;
	}
	// ABS(e) for 1 and 'inf', POWER(e, 2) for 2, ABS(SIGN(e)) for a data-only 0: the body is the first argument of
	// the outermost function, and of the one inside it for 0. Binding the aggregate may cast its argument
	// (`sum` reads ABS(SIGN(e)), a TINYINT, as a BIGINT).
	D_ASSERT(reducer.children.size() == 1);
	const Expression *body = reducer.children[0].get();
	for (idx_t depth = norm.IsCount() ? 2 : 1; depth > 0; depth--) {
		while (body->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			body = body->Cast<BoundCastExpression>().child.get();
		}
		D_ASSERT(body->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
		         !body->Cast<BoundFunctionExpression>().children.empty());
		body = body->Cast<BoundFunctionExpression>().children[0].get();
	}
	return body;
}

//! An expression with the registries alone -- no relation names, no auxiliary labels: the form
//! the clause renderers in this file use. `keys` prints each reducer's `per` and `by`.
static string RenderSource(const Expression &expr, const DecideSourceText &text,
                           const vector<KeyedScopeInfo> &keyed_scopes, const vector<DecideKey> *keys = nullptr,
                           bool written = false) {
	RenderContext ctx {text, keyed_scopes, keys};
	ctx.written = written;
	return RenderSource(expr, ctx);
}

//! The text the user wrote for `expr`, when its query location names a span of the query and the render may quote
//! it. A message quotes every located node (ctx.written). A plan quotes the located nodes that read no decision and
//! reprints the rest: canonicalization and the rewrites reshape the decision structure, and a located node's text is
//! the user's for the value it had when it was bound. A reducer rendered without its `by` quotes its call alone.
//! `binding` is how tightly the quoted text binds, as the parser read it (DecideBinding): the text is the user's, so
//! whether it takes parentheses under an operator is the text's to say, not the bound node's.
static bool QuoteSpan(const Expression &expr, const RenderContext &ctx, string &text, int &binding) {
	if (!expr.GetQueryLocation().IsValid()) {
		return false;
	}
	// The two comparisons a BETWEEN binds to carry its location, but each is half of its value, not the whole.
	if (&expr == ctx.between_half) {
		return false;
	}
	if (!ctx.written && BoundExpressionReferencesDecide(expr, ctx.decide_index)) {
		return false;
	}
	return ctx.text.CutExpr(expr.GetQueryLocation(), text, binding);
}

static string RenderSource(const Expression &expr, const RenderContext &ctx, int parent_precedence) {
	if (HasDecideTag(expr.GetAlias(), NULL_GUARD_TAG)) {
		return RenderSource(DecideUnguarded(expr), ctx, parent_precedence);
	}
	string quoted;
	int binding;
	if (QuoteSpan(expr, ctx, quoted, binding)) {
		return binding != 0 && binding < parent_precedence ? "(" + quoted + ")" : quoted;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		if (HasDecideTag(expr.GetAlias(), L0_AUTO_BOUND_TAG)) {
			return "M"; // stage 06 derives it from the column box
		}
		if (HasDecideTag(expr.GetAlias(), L0_TOLERANCE_TAG)) {
			return "decide_l0_tolerance";
		}
		auto &value = expr.Cast<BoundConstantExpression>().value;
		if (ctx.rewrite_row && !value.IsNull() && value.type().id() == LogicalTypeId::DOUBLE) {
			return RewriteNumberText(value);
		}
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CAST:
		// A cast with no span of its own was introduced by DuckDB binding (one the user wrote, or one that took its
		// child's location, is quoted above) and is display noise. The child takes the cast's place, precedence
		// included: `2 * CAST(a + b AS DOUBLE)` prints `2 * (a + b)`.
		return RenderSource(*expr.Cast<BoundCastExpression>().child, ctx, parent_precedence);
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (ctx.aux_labels && colref.binding.table_index == ctx.decide_index) {
			for (auto &labelled : *ctx.aux_labels) {
				if (labelled.first == colref.binding.column_index) {
					// A label is parenthesized for arithmetic (`(b * x)`); as a whole operand
					// -- a reducer's body, a side of a comparison -- it needs none.
					return parent_precedence <= OPERAND_PRECEDENCE ? WithoutEnclosingParentheses(labelled.second)
					                                               : labelled.second;
				}
			}
		}
		auto name = StripDecideTags(colref.GetName());
		if (!name.empty()) {
			return KeywordHelper::WriteOptionallyQuoted(name);
		}
		break;
	}
	case ExpressionClass::BOUND_REF: {
		// A column the physical plan resolved to a chunk position keeps its name as alias; a frame column's name is
		// the frame as written.
		auto name = StripDecideTags(expr.GetAlias());
		if (!name.empty()) {
			return HasDecideTag(expr.GetAlias(), FRAME_COLUMN_TAG) ? name : KeywordHelper::WriteOptionallyQuoted(name);
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
			// AND binds tighter than OR: an OR under an AND keeps its parentheses. The parser flattens an AND under
			// an AND, so a conjunction under an AND is an OR.
			auto &child = *conj.children[i];
			bool nested_or = conj.type == ExpressionType::CONJUNCTION_AND &&
			                 child.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION;
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
		string result = RenderSource(*in.children[0], ctx, OPERAND_PRECEDENCE) + " IN (";
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
	// Any other node prints in its own syntax over its children as written: its ToString would print a cast's child by
	// its alias, which may be nothing but tags. Each child stands aside for its text while it prints, since a child
	// holding a bound subquery cannot be copied. Rendered first, so nothing can throw with a child aside.
	vector<string> written;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { written.push_back(RenderSource(child, ctx, OPERAND_PRECEDENCE)); });
	auto &node = const_cast<Expression &>(expr);
	vector<unique_ptr<Expression>> aside;
	ExpressionIterator::EnumerateChildren(node, [&](unique_ptr<Expression> &child) {
		auto type = child->return_type;
		aside.push_back(std::move(child));
		child = make_uniq<WrittenOperand>(written[aside.size() - 1], std::move(type));
	});
	auto result = node.ToString();
	idx_t restored = 0;
	ExpressionIterator::EnumerateChildren(node,
	                                      [&](unique_ptr<Expression> &child) { child = std::move(aside[restored++]); });
	return result;
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

string RenderDecidePerKey(idx_t key, optional_idx location, const DecideSourceText &text,
                          const vector<DecideKey> *keys) {
	string written;
	if (text.Cut(location, DecideSpanRole::PER, written)) {
		return written;
	}
	// A `per` the optimizer built, or one of a statement with no source text, names the key as it was first written;
	// `per ()` names none.
	if (key == DConstants::INVALID_INDEX) {
		return "()";
	}
	D_ASSERT(keys);
	return DecideKeyText(*keys, key);
}

//! A qualifier (`when c per K`) is written in front of its clause and ends in a colon.
static string FrontQualifier(const string &qualifier) {
	return qualifier.empty() ? qualifier : qualifier + ":";
}

//! The `when c per K` qualifier of a clause's scope, as written in front of it: the condition, a data expression,
//! quoted.
static string ScopeQualifier(const BoundDecideClause &clause, const RenderContext &ctx) {
	auto &scope = clause.scope;
	return AppendQualifier(scope.when ? "when " + RenderSource(*scope.when, ctx) : string(),
	                       scope.HasPer()
	                           ? "per " + RenderDecidePerKey(scope.class_key, clause.per_location, ctx.text, ctx.keys)
	                           : string());
}

//! The context that renders the body of `clause`.
static RenderContext ClauseContext(RenderContext ctx, const BoundDecideClause &clause) {
	ctx.between_half = clause.between == DecideBetweenHalf::NONE ? nullptr : clause.body.get();
	return ctx;
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

//! Whether a side of a clause reads a row's data, on the bound tree before flattening: a column that is no decision, or
//! a correlated subquery. Decisions, constants and an uncorrelated subquery's one value read none.
static bool ReadsRowData(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return expr.Cast<BoundColumnRefExpression>().binding.table_index != decide_index;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		return expr.Cast<BoundSubqueryExpression>().IsCorrelated();
	}
	bool reads = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { reads = reads || ReadsRowData(child, decide_index); });
	return reads;
}

//! Link the two halves of `input BETWEEN lo AND hi` into the clause the user wrote: each points at the other. Both
//! carry the BETWEEN's location, so the clause is quoted whole.
static void LinkBetweenHalves(ConstraintSourceInfo &lower, ConstraintSourceInfo &upper, idx_t lower_id,
                              idx_t upper_id) {
	lower.between_partner = upper_id;
	upper.between_partner = lower_id;
	upper.between_upper = true;
}

vector<ConstraintSourceInfo> InitializeConstraintSourceInfo(vector<BoundDecideClause> &clauses,
                                                            const DecideSourceText &text,
                                                            const vector<KeyedScopeInfo> &keyed_scopes,
                                                            idx_t decide_index, const vector<DecideKey> *keys,
                                                            const vector<DecideFrameSpec> &frames) {
	vector<ConstraintSourceInfo> result;
	RenderContext ctx {text, keyed_scopes, keys};
	ctx.decide_index = decide_index;
	ctx.frames = &frames;
	// The `>=` half of the BETWEEN being visited: its `<=` half is the next comparison.
	idx_t lower_half = DConstants::INVALID_INDEX;
	for (auto &clause : clauses) {
		if (clause.body->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			continue;
		}
		auto &cmp = clause.body->Cast<BoundComparisonExpression>();
		idx_t source_id = result.size();
		clause.origin.source_clause_id = source_id;
		// The last point at which the clause is as written: whether canonicalization has to
		// merge the sides. That is the one rewrite that leaves algebra the user cannot
		// recognize, when BOTH sides bear decisions: `ship <= capacity * open`
		// becomes `ship - capacity * open <= 0`, a clause the query does not contain, against a
		// literal bound that is not in it either. When only one side bears decisions the
		// rewrite is a clean move and the leftover bound folds into something BETTER than what
		// was written: `(SELECT 7) >= x + 2` becomes `x <= 5`, turning an opaque subquery into a
		// number the user can edit.
		ConstraintSourceInfo info;
		// A scope written in front belongs to the clause as written: canonicalization drops the
		// `per K:` of a clause in the aggregate shape, which still holds (its scope checks).
		info.qualifier = FrontQualifier(ScopeQualifier(clause, ctx));
		info.location = cmp.GetQueryLocation();
		if (ReferencesDecideVariable(*cmp.left, decide_index) && ReferencesDecideVariable(*cmp.right, decide_index)) {
			// What an edit quotes stays DeciDB's own rendering.
			info.source_lhs = RenderSource(*cmp.left, ctx);
			info.source_rhs = RenderSource(*cmp.right, ctx);
			info.source_rhs_reads_data = ReadsRowData(*cmp.right, decide_index);
		}
		if (clause.between == DecideBetweenHalf::LOWER) {
			lower_half = source_id;
		} else if (clause.between == DecideBetweenHalf::UPPER) {
			if (lower_half == DConstants::INVALID_INDEX) {
				throw InternalException("DECIDE: the upper half of a BETWEEN came without "
				                        "its lower half; the binder emits them in order");
			}
			LinkBetweenHalves(result[lower_half], info, lower_half, source_id);
			lower_half = DConstants::INVALID_INDEX;
		}
		result.push_back(std::move(info));
	}
	// A DECIDE-variable IN is intentionally an opaque pre-optimizer marker, not
	// a comparison. Give it the same stable source identity now so every emitted
	// indicator/linking row can still point back to the original SQL clause. The
	// constraint binder admits no other operator as a clause.
	for (auto &clause : clauses) {
		if (clause.body->GetExpressionClass() != ExpressionClass::BOUND_OPERATOR) {
			continue;
		}
		auto &in = *clause.body;
		clause.origin.source_clause_id = result.size();
		ConstraintSourceInfo info;
		info.location = in.GetQueryLocation();
		info.canonical_lhs = RenderSource(in, ctx);
		info.rhs_kind = ConstraintSourceRhsKind::NUMERIC_FALLBACK;
		// Its scope in front, as a comparison's: the IN itself shows no scope word.
		info.qualifier = FrontQualifier(ScopeQualifier(clause, ctx));
		result.push_back(std::move(info));
	}
	return result;
}

void FinalizeConstraintSourceInfo(const vector<BoundDecideClause> &clauses, vector<ConstraintSourceInfo> &sources,
                                  const DecideSourceText &text, const vector<KeyedScopeInfo> &keyed_scopes,
                                  idx_t decide_index, const vector<DecideKey> *keys,
                                  const vector<DecideFrameSpec> &frames) {
	RenderContext ctx {text, keyed_scopes, keys};
	ctx.decide_index = decide_index;
	ctx.frames = &frames;
	for (auto &clause : clauses) {
		if (clause.body->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			continue;
		}
		auto &cmp = clause.body->Cast<BoundComparisonExpression>();
		// InitializeConstraintSourceInfo gave every comparison its source clause.
		const idx_t source_id = clause.origin.source_clause_id;
		if (source_id >= sources.size()) {
			throw InternalException("DECIDE: a comparison reached the clause registry without its source clause; "
			                        "InitializeConstraintSourceInfo gives every comparison one");
		}
		auto &info = sources[source_id];
		info.canonical_lhs = RenderSource(*cmp.left, ctx);
		info.canonical_rhs = RenderSource(*cmp.right, ctx);
		info.lhs_location = cmp.left->GetQueryLocation();
		info.canonical_cmp = DecideComparisonOperator(cmp.type);
		if (!clause.query_wide_bound && !DecideHoldsSubqueryValue(*cmp.right)) {
			info.rhs_kind = ConstraintSourceRhsKind::DATA_EXPRESSION;
		} else {
			info.rhs_kind = ConstraintSourceRhsKind::NUMERIC_FALLBACK;
		}
	}
}

string RenderDecideClauseBody(const BoundDecideClause &clause, const DecideSourceNames &names) {
	return RenderSource(*clause.body, ClauseContext(ContextOf(names), clause));
}

string RenderDecideSource(const Expression &expr, const DecideSourceText &text,
                          const vector<KeyedScopeInfo> &keyed_scopes, const vector<DecideKey> *keys) {
	return RenderSource(expr, text, keyed_scopes, keys, true);
}

string RenderDecideSource(const Expression &expr, const DecideSourceNames &names, int parent_precedence) {
	return RenderSource(expr, ContextOf(names), parent_precedence);
}

DecideSourceNames DecidePlanNames(const DecideSourceText &text, const vector<KeyedScopeInfo> &keyed_scopes,
                                  const vector<DecideKey> &keys, idx_t decide_index,
                                  const vector<pair<idx_t, string>> &aux_labels,
                                  const vector<DecideFrameSpec> &frames) {
	DecideSourceNames names {text, keyed_scopes, keys};
	names.decide_index = decide_index;
	names.aux_labels = &aux_labels;
	names.written = false;
	names.frames = &frames;
	return names;
}

//! The source id CollectDecideClauseLayers gives a row a rewrite added for the
//! objective (DecideRowOrigin::objective): no clause's, and not INVALID_INDEX, which is a row no
//! one claims.
static constexpr idx_t OBJECTIVE_ROW_SOURCE_ID = DConstants::INVALID_INDEX - 1;

//! The residue of a clause the optimizer lifted out of the tree (a NORM or IN lowering
//! replaces its comparison with a TRUE placeholder). It carries no information, and
//! printing it as a bare `true` only makes a clause look like it vanished.
static bool IsTruePlaceholder(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &value = expr.Cast<BoundConstantExpression>().value;
	return value.type().id() == LogicalTypeId::BOOLEAN && !value.IsNull() && value.GetValue<bool>();
}

//! An objective tree rendered in the direction it was written: the terms of its additive spine, each with its sign
//! reversed (only the sign: a factor on a term is the user's number and stays). The text is the one the same objective
//! written as a minimization renders to (RenderFunction): a leading negative term takes the unary minus, the others
//! ` + ` and ` - `.
static string RenderWrittenSense(const Expression &expr, const RenderContext &ctx) {
	string result;
	for (auto &atom : ReadCanonicalAtoms(expr, ctx.decide_index)) {
		const bool negative = atom.sign > 0;
		if (result.empty()) {
			auto operand = RenderSource(*atom.term, ctx, negative ? 40 : 10);
			result = !negative ? operand : operand.rfind('-', 0) == 0 ? "-(" + operand + ")" : "-" + operand;
		} else {
			result += (negative ? " - " : " + ") + RenderSource(*atom.term, ctx, negative ? 11 : 10);
		}
	}
	return result;
}

//! One clause as a plan prints it: its `when c per K:` in front, then its body. Empty for a placeholder and for a
//! defined body's row, which restates the body the reducer already prints (the account line says how it is held): the
//! placeholder is the only thing left marking where the lifted clause stood, and a caller ordering clauses by
//! position needs that position. `written_sense` renders an objective in the direction it was written.
static string RenderClause(const BoundDecideClause &clause, const DecideSourceNames &names, bool written_sense) {
	auto &body = *clause.body;
	if (IsTruePlaceholder(body) || HasDecideTag(body.GetAlias(), BODY_DEFINITION_TAG)) {
		return string();
	}
	auto ctx = ClauseContext(ContextOf(names), clause);
	return AppendQualifier(FrontQualifier(ScopeQualifier(clause, ctx)),
	                       written_sense ? RenderWrittenSense(body, ctx) : RenderSource(body, ctx));
}

string DecideObjectiveQualifier(const BoundDecideClause &objective, const DecideSourceNames &names) {
	return FrontQualifier(ScopeQualifier(objective, ContextOf(names)));
}

string RenderDecideObjective(const BoundDecideClause &objective, const DecideSourceNames &names, bool written_sense) {
	return RenderClause(objective, names, written_sense);
}

//! A term's coefficient is left out when it is the literal 1, as a user writes no factor of 1; an L0 link's bound and
//! tolerance are printed whatever they hold (RenderSource).
static bool IsUnitCoefficient(const Expression &coefficient) {
	if (coefficient.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
	    HasDecideTag(coefficient.GetAlias(), L0_AUTO_BOUND_TAG) ||
	    HasDecideTag(coefficient.GetAlias(), L0_WRITTEN_BOUND_TAG) ||
	    HasDecideTag(coefficient.GetAlias(), L0_TOLERANCE_TAG)) {
		return false;
	}
	auto &value = coefficient.Cast<BoundConstantExpression>().value;
	return !value.IsNull() && value.GetValue<double>() == 1.0;
}

//! One row an F node owns, as `c1 * x1 + c2 * x2 >= bound` over the terms and bound stage 05 lowered, without the zero
//! terms the rewrite added. It has no qualifier: the rows an F node owns print under the clause they came from, behind
//! the scope that clause was written with (CollectDecideClauseLayers).
static string RenderFunctionRow(const DecideConstraint &row, const vector<unique_ptr<Expression>> &decide_variables,
                                const RenderContext &plan_ctx) {
	auto ctx = plan_ctx;
	ctx.rewrite_row = true;
	string result;
	for (auto &term : row.lhs_terms) {
		// A frame prints as its selector, `else` included: its READ term, whose coefficient is an indicator column,
		// prints as the selector, and the term that carries its `else` (no decision, a frame column) prints nothing.
		string text;
		if (term.frame_read != DConstants::INVALID_INDEX) {
			text = (*ctx.frames)[term.frame_read].spelling;
		} else if (term.variable_index == DConstants::INVALID_INDEX) {
			continue;
		} else {
			text = RenderSource(*decide_variables[term.variable_index], ctx, 20);
			if (!IsUnitCoefficient(*term.coefficient)) {
				text = RenderSource(*term.coefficient, ctx, 20) + " * " + text;
			}
		}
		if (result.empty()) {
			result = term.sign < 0 ? "-" + text : text;
		} else {
			result += (term.sign < 0 ? " - " : " + ") + text;
		}
	}
	return result + " " + DecideComparisonOperator(row.comparison_type) + " " + RenderSource(*row.rhs_expr, ctx);
}

vector<DecideFunctionRow> CollectDecideFunctionRows(const DecideTree &tree, const DecideSourceNames &names,
                                                    const vector<unique_ptr<Expression>> &decide_variables) {
	vector<DecideFunctionRow> rows;
	auto ctx = ContextOf(names);
	// A definition's row restates the body its reducer already prints, so only ABS envelopes and L0 links are rows.
	for (auto function : tree.function_nodes) {
		auto &node = *tree.nodes[function];
		for (auto *roots : {&node.abs_envelope_roots, &node.l0_link_roots}) {
			for (auto root : *roots) {
				auto &clause = tree.nodes[root]->compiled.clause;
				rows.push_back(
				    {tree.nodes[root]->origin.source_clause_id, RenderFunctionRow(*clause, decide_variables, ctx)});
			}
		}
	}
	return rows;
}

vector<DecideClauseLayers>
CollectDecideClauseLayers(const vector<BoundDecideClause> &clauses, const DecideSourceNames &names,
                          const vector<ConstraintSourceInfo> &sources, vector<string> &unattributed,
                          vector<string> &objective_rows, const vector<LogicalDecide::RewriteAccount> &accounts,
                          const vector<DecideFunctionRow> &function_rows, const string &objective_qualifier) {
	// Every row the solver will see, with the clause it came from: its source clause, or the objective's for a row a
	// rewrite added for it.
	vector<string> rows;
	vector<idx_t> row_ids;
	for (auto &clause : clauses) {
		rows.push_back(RenderClause(clause, names, false));
		row_ids.push_back(clause.origin.source_clause_id == DConstants::INVALID_INDEX && clause.origin.objective
		                      ? OBJECTIVE_ROW_SOURCE_ID
		                      : clause.origin.source_clause_id);
	}
	vector<vector<string>> by_clause(sources.size());
	// Display follows the clause list, not the registry. Source ids are handed out in two
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
	// The rows the F nodes own follow the clause's own, or the objective's, behind the scope it was written with.
	for (auto &row : function_rows) {
		if (row.source_clause_id < by_clause.size()) {
			by_clause[row.source_clause_id].push_back(
			    AppendQualifier(sources[row.source_clause_id].qualifier, row.text));
		} else {
			objective_rows.push_back(AppendQualifier(objective_qualifier, row.text));
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
	vector<bool> printed(sources.size(), false);
	for (auto i : clause_order) {
		if (printed[i]) {
			continue;
		}
		// A BETWEEN is one clause: its two halves print together, the lower one first.
		vector<idx_t> halves {i};
		bool between = sources[i].between_partner != DConstants::INVALID_INDEX;
		if (between) {
			halves.push_back(sources[i].between_partner);
			if (sources[i].between_upper) {
				std::swap(halves[0], halves[1]);
			}
		}
		DecideClauseLayers layers;
		layers.written = QuoteWrittenClause(sources, i, names.text);
		for (auto half : halves) {
			printed[half] = true;
			auto &info = sources[half];
			string canonical = info.canonical_lhs;
			if (!info.canonical_cmp.empty()) {
				canonical += " " + info.canonical_cmp + " " + info.canonical_rhs;
			}
			// The halves are the canonical reading of a BETWEEN, whether or not canonicalization moved anything.
			if (between || info.canonical_differs) {
				layers.canonical.push_back(AppendQualifier(info.qualifier, canonical));
			}
			// The solver form is worth printing only where it says something the layer above
			// did not: a row that reads as that layer is left out, whatever the rewrite put
			// beside it (an ABS leaves the clause's own row next to its envelope rows).
			const string above = AppendQualifier(info.qualifier, canonical);
			for (auto &row : by_clause[half]) {
				if (row != above) {
					layers.rewritten.push_back(std::move(row));
				}
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

string RenderDecideClauseLayers(const vector<DecideClauseLayers> &layers, const vector<string> &unattributed) {
	string result;
	auto append_line = [&](const string &line) {
		if (!result.empty()) {
			result += "\n";
		}
		result += line;
	};
	for (auto &clause : layers) {
		append_line(clause.written);
		for (auto &canonical : clause.canonical) {
			append_line(LAYER_CANONICAL_PREFIX + canonical);
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
	case Kind::HELPER:
		became = "a helper decision";
		break;
	case Kind::HELPER_PER_GROUP:
		became = "a helper decision per group";
		break;
	case Kind::HELPER_PER_VALUE:
		became = "a helper decision per value";
		break;
	case Kind::HELPER_PER_ROW:
		became = "a helper decision per row";
		break;
	case Kind::FRAME_HELPER_PER_TIMELINE:
		became = "a helper decision per timeline";
		break;
	case Kind::FRAME_HELPER_PER_POSITION:
		became = "a helper decision per position";
		break;
	case Kind::NOT_EQUAL:
		became = "a helper decision choosing between < and >";
		break;
	case Kind::BODY_AT_LEAST:
		became = "a helper decision per row, at least its value";
		break;
	case Kind::BODY_AT_MOST:
		became = "a helper decision per row, at most its value";
		break;
	case Kind::BODY_EQUAL:
		became = "a helper decision per row, equal to its value";
		break;
	case Kind::RUN_TIME:
		return account.subject;
	case Kind::VALUE_HELPER_INLINED:
		return account.subject + " is written where it is read, with no helper decision";
	}
	return account.subject + " becomes " + became;
}

string RenderDecideObjectiveLayers(const string &sense_prefix, const string &written, const string &canonical,
                                   bool canonical_differs, const vector<string> &rewritten,
                                   const vector<string> &accounts, const vector<string> &derived_rows) {
	string result;
	auto append_line = [&](const string &line) {
		if (!result.empty()) {
			result += "\n";
		}
		result += line;
	};
	append_line(sense_prefix + written);
	if (canonical_differs) {
		append_line(LAYER_CANONICAL_PREFIX + canonical);
	}
	// What the rewrites made of it, then the objective as the solver receives it where it
	// reads differently, then the rows the rewrites added for it (an ABS envelope, a
	// product's corners), which belong to the objective rather than to any clause.
	for (auto &account : accounts) {
		append_line(LAYER_REWRITTEN_PREFIX + account);
	}
	for (auto &row : rewritten) {
		// A rewrite row is DeciDB's rendering, so it is compared with the canonical rendering, never with the text.
		if (row != canonical) {
			append_line(LAYER_REWRITTEN_PREFIX + row);
		}
	}
	for (auto &row : derived_rows) {
		append_line(LAYER_REWRITTEN_PREFIX + row);
	}
	return result;
}

} // namespace duckdb
