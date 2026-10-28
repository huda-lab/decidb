#include "duckdb/parser/decide/decide_removed_spellings.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enums/subquery_type.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/decide/decide_parsed_markers.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// Quoting the user's text
//===--------------------------------------------------------------------===//

//! `count(*)` is `count_star()` to the parser: put it back as written.
void RestoreCountStar(unique_ptr<ParsedExpression> &expr) {
	if (expr->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr->Cast<FunctionExpression>();
		if (!func.is_operator && func.children.empty() && StringUtil::CIEquals(func.function_name, "count_star")) {
			vector<unique_ptr<ParsedExpression>> star;
			star.push_back(make_uniq<StarExpression>());
			expr = make_uniq<FunctionExpression>("count", std::move(star));
			return;
		}
	}
	ParsedExpressionIterator::EnumerateChildren(*expr,
	                                            [&](unique_ptr<ParsedExpression> &child) { RestoreCountStar(child); });
}

//! `text` without the one pair of parentheses the parser prints around a comparison or an
//! arithmetic term.
string WithoutOuterParentheses(const string &text) {
	if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
		return text;
	}
	int depth = 0;
	for (idx_t i = 0; i + 1 < text.size(); i++) {
		depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
		if (depth == 0) {
			return text;
		}
	}
	return text.substr(1, text.size() - 2);
}

//! An expression as the user wrote it, for quoting back in a message.
string Written(const ParsedExpression &expr) {
	auto copy = expr.Copy();
	RestoreCountStar(copy);
	return WithoutOuterParentheses(copy->ToString());
}

//! A condition about to be joined to another with `and`: an OR keeps its parentheses.
string ConditionText(const ParsedExpression &condition) {
	auto text = Written(condition);
	if (condition.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
		return "(" + text + ")";
	}
	return text;
}

string JoinConditions(const vector<string> &conditions) {
	if (conditions.size() == 1) {
		return WithoutOuterParentheses(conditions[0]);
	}
	return StringUtil::Join(conditions, " and ");
}

//! `when c [and d] [per K]: `, or nothing.
string FrontScope(const vector<string> &conditions, const string &per) {
	string scope;
	if (!conditions.empty()) {
		scope = "when " + JoinConditions(conditions);
	}
	if (!per.empty()) {
		scope += (scope.empty() ? "" : " ") + string("per ") + per;
	}
	return scope.empty() ? scope : scope + ": ";
}

//! A PER key as written, `()` when it names no column.
string KeyText(const FunctionExpression &marker) {
	return marker.children.size() > 1 ? DecideKeyToString(marker) : "()";
}

[[noreturn]] void Refuse(const ParsedExpression &at, const string &message) {
	throw ParserException(at, "DECIDE: " + message);
}

//===--------------------------------------------------------------------===//
// A reducer and the scopes around it
//===--------------------------------------------------------------------===//

//! A reducer as the removed forms write it -- behind WHEN markers, a `by`, the colon form,
//! with scopes in front of its argument -- read once, so it is spelled once.
struct Reducer {
	const FunctionExpression *call = nullptr;
	//! The `when`s written in front of its argument, as condition texts.
	vector<string> conditions;
	//! The removed WHENs after it, innermost first.
	vector<string> removed;
	//! The `per` key: the colon form's relations, or a scope's key, or `row`.
	string per;
	//! ` by (g)`, or nothing.
	string by;
	//! Whether the reducer carries anything of the new spellings (a scope, a `by`).
	bool scoped = false;
};

//! Read `target` as a reducer: false if it is not one.
bool ReadReducer(const ParsedExpression &target, Reducer &out) {
	const ParsedExpression *at = &target;
	vector<string> removed;
	while (IsDecideMarker(*at, WHEN_CONSTRAINT_TAG)) {
		auto &marker = at->Cast<FunctionExpression>();
		if (marker.children.size() < 2) {
			return false;
		}
		removed.push_back(ConditionText(*marker.children[1]));
		at = marker.children[0].get();
	}
	if (IsDecideMarker(*at, REDUCER_BY_TAG)) {
		auto &by = at->Cast<FunctionExpression>();
		out.by = " by (" + DecideKeyToString(by) + ")";
		out.scoped = true;
		at = by.children[0].get();
	}
	if (IsDecideMarker(*at, QUALIFIED_REDUCER_TAG)) {
		auto &qualified = at->Cast<FunctionExpression>();
		out.per = DecideKeyToString(qualified);
		at = qualified.children[0].get();
	}
	if (!TakesReducerBy(*at)) {
		return false;
	}
	out.call = &at->Cast<FunctionExpression>();
	if (IsScopedReducerCall(*at) && out.call->children.size() == 1) {
		const ParsedExpression *argument = out.call->children[0].get();
		while (IsDecideScopeMarker(*argument)) {
			auto &scope = argument->Cast<FunctionExpression>();
			out.scoped = true;
			if (scope.function_name == SCOPE_WHEN_TAG && scope.children.size() > 1) {
				out.conditions.push_back(ConditionText(*scope.children[1]));
			} else if (out.per.empty()) {
				out.per = scope.function_name == SCOPE_PER_ROW_TAG ? "row" : KeyText(scope);
			}
			argument = scope.children[0].get();
		}
	}
	// The markers were read outside in; the innermost removed WHEN applied first.
	out.removed.assign(removed.rbegin(), removed.rend());
	return true;
}

//! The argument of a reducer under the scopes written in front of it.
const ParsedExpression &ReducerBody(const Reducer &reducer) {
	return ReducerArgumentBody(*reducer.call->children[0]);
}

//! `reducer` in the scope spellings, or nothing when it has none: `norm` takes no `when`
//! inside it.
string ReducerSpelling(const Reducer &reducer) {
	auto name = StringUtil::Lower(reducer.call->function_name);
	auto all_conditions = reducer.conditions;
	all_conditions.insert(all_conditions.end(), reducer.removed.begin(), reducer.removed.end());
	auto scope = FrontScope(all_conditions, reducer.per);
	if (name == "count_star") {
		return (scope.empty() ? "count(*)" : "sum(" + scope + "1)") + reducer.by;
	}
	if (name == "count" && reducer.call->children.size() == 1 && !all_conditions.empty()) {
		auto conditions = all_conditions;
		conditions.push_back(ConditionText(*reducer.call->children[0]) + " is not null");
		return "sum(" + FrontScope(conditions, reducer.per) + "1)" + reducer.by;
	}
	if (!IsScopedReducerCall(*reducer.call)) {
		return scope.empty() ? Written(*reducer.call) + reducer.by : string();
	}
	return name + "(" + scope + Written(ReducerBody(reducer)) + ")" + reducer.by;
}

//! The reducer a removed WHEN followed, as the user wrote it, for "the WHEN after X": its own
//! scopes and `by` in the lower-case spelling, any earlier removed WHEN left out.
string ReducerAsWritten(const ParsedExpression &target) {
	const ParsedExpression *reducer_text = &target;
	while (IsDecideMarker(*reducer_text, WHEN_CONSTRAINT_TAG)) {
		reducer_text = reducer_text->Cast<FunctionExpression>().children[0].get();
	}
	Reducer reducer;
	if (!ReadReducer(*reducer_text, reducer) || !reducer.scoped) {
		return Written(*reducer_text);
	}
	return ReducerSpelling(reducer);
}

//===--------------------------------------------------------------------===//
// Where a removed WHEN's condition ends
//===--------------------------------------------------------------------===//

bool ContainsReducerCall(const ParsedExpression &expr) {
	if (TakesReducerBy(expr)) {
		return true;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || ContainsReducerCall(child); });
	return found;
}

//! One node on the way from a clause down to a removed marker.
struct PathStep {
	const ParsedExpression *node;
	//! Which child the way continues into, and how many children the node has.
	idx_t child;
	idx_t children;
};

idx_t ChildCount(const ParsedExpression &expr) {
	idx_t count = 0;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &) { count++; });
	return count;
}

unique_ptr<ParsedExpression> *ChildSlot(ParsedExpression &expr, idx_t index) {
	unique_ptr<ParsedExpression> *slot = nullptr;
	idx_t at = 0;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<ParsedExpression> &child) {
		if (at++ == index) {
			slot = &child;
		}
	});
	return slot;
}

bool IsRemovedMarker(const ParsedExpression &expr) {
	return IsDecideMarker(expr, WHEN_CONSTRAINT_TAG) || IsDecideMarker(expr, PER_CONSTRAINT_TAG);
}

//! Any DECIDE marker: a scope, a `by`, the colon form or a removed form. None is an operator
//! the user wrote.
bool IsAnyDecideMarker(const ParsedExpression &expr) {
	return IsRemovedMarker(expr) || IsDecideScopeMarker(expr) || IsDecideMarker(expr, REDUCER_BY_TAG) ||
	       IsDecideMarker(expr, QUALIFIED_REDUCER_TAG);
}

//! The first removed marker at or below `expr`, outermost first, with the way to it.
//! Subquery bodies are not entered: a subquery is refused on its own (TransformSelectNode).
bool FindRemovedMarker(const ParsedExpression &expr, vector<PathStep> &path) {
	path.push_back({&expr, 0, ChildCount(expr)});
	if (IsRemovedMarker(expr)) {
		return true;
	}
	bool found = false;
	idx_t index = 0;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!found) {
			path.back().child = index;
			found = FindRemovedMarker(child, path);
		}
		index++;
	});
	if (!found) {
		path.pop_back();
	}
	return found;
}

//! A node whose value is a condition.
bool IsConditionNode(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COMPARISON:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::CONJUNCTION:
		return true;
	case ExpressionClass::OPERATOR: {
		auto type = expr.GetExpressionType();
		return type == ExpressionType::OPERATOR_NOT || type == ExpressionType::OPERATOR_IS_NULL ||
		       type == ExpressionType::OPERATOR_IS_NOT_NULL || type == ExpressionType::COMPARE_IN ||
		       type == ExpressionType::COMPARE_NOT_IN;
	}
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		return func.is_operator && StringUtil::Contains(func.function_name, "~");
	}
	case ExpressionClass::SUBQUERY:
		return expr.Cast<SubqueryExpression>().subquery_type == SubqueryType::ANY;
	default:
		return false;
	}
}

//! An operator written after its first operand (`a > b`, `a IN (...)`, `a::INT`), which a
//! condition read up to `a` runs on into.
bool ContinuesFirstOperand(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COMPARISON:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::CONJUNCTION:
	case ExpressionClass::CAST:
		return true;
	case ExpressionClass::OPERATOR: {
		auto type = expr.GetExpressionType();
		return type == ExpressionType::OPERATOR_IS_NULL || type == ExpressionType::OPERATOR_IS_NOT_NULL ||
		       type == ExpressionType::COMPARE_IN || type == ExpressionType::COMPARE_NOT_IN;
	}
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		return func.is_operator && !IsAnyDecideMarker(expr) && func.children.size() >= 2;
	}
	case ExpressionClass::SUBQUERY:
		return expr.Cast<SubqueryExpression>().subquery_type == SubqueryType::ANY;
	default:
		return false;
	}
}

//! An operator whose text ends with its last operand (`a + b`, `NOT b`): what follows it
//! follows that operand. A call ends with its closing parenthesis instead.
bool EndsWithLastOperand(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COMPARISON:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::CONJUNCTION:
		return true;
	case ExpressionClass::OPERATOR:
		return expr.GetExpressionType() == ExpressionType::OPERATOR_NOT;
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		return func.is_operator && !IsAnyDecideMarker(expr);
	}
	default:
		return false;
	}
}

//! The condition a removed WHEN marker at the end of `path` was written with. The old
//! grammar read it short -- a reducer's WHEN up to one column or parenthesized term, the
//! objective's up to one comparison -- and parsed the rest of it as operators around the
//! marker: `SUM(x) WHEN cap BETWEEN 2 AND 5` is `BETWEEN(SUM(x) WHEN cap, 2, 5)`. The
//! condition runs on through every operator whose first operand ends with it, up to the
//! outermost such operator that yields a condition; `top` receives that operator's place on
//! the path, or the marker's own when the condition was written whole. Nodes above
//! `path[first]` belong to the clause (its own comparison), never to the condition.
//! A constraint, as opposed to a term the bound of the clause was read into.
bool IsConstraint(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COMPARISON:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::CONJUNCTION:
		return true;
	case ExpressionClass::OPERATOR:
		return expr.GetExpressionType() == ExpressionType::COMPARE_IN ||
		       expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN;
	default:
		return false;
	}
}

//! A WHEN after a term rather than a constraint whose condition read on into what followed:
//! `sum(x) by (g) WHEN (c) <= 5` parses as the WHEN of `(c) <= 5`. The condition is the
//! comparison's left side; its right side was the clause's bound.
const ParsedExpression *SwallowedCondition(const FunctionExpression &when) {
	auto &condition = *when.children[1];
	if (IsConstraint(*when.children[0]) || condition.GetExpressionClass() != ExpressionClass::COMPARISON) {
		return nullptr;
	}
	auto &left = *condition.Cast<ComparisonExpression>().left;
	return IsConditionNode(left) ? &left : nullptr;
}

//! A removed WHEN's condition as the user wrote it.
struct WrittenWhen {
	string text;
	//! An OR keeps its parentheses beside another condition.
	bool is_or = false;

	string AsCondition() const {
		return is_or ? "(" + text + ")" : text;
	}
};

//! What the grammar reads as one term (c_expr): a column, a constant, a parameter, a call,
//! a cast written as a call, a scalar subquery. Anything else in a WHEN's condition position
//! was written in parentheses.
bool IsOneTerm(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
	case ExpressionClass::CONSTANT:
	case ExpressionClass::PARAMETER:
	case ExpressionClass::CAST:
		return true;
	case ExpressionClass::SUBQUERY:
		return expr.Cast<SubqueryExpression>().subquery_type == SubqueryType::SCALAR;
	case ExpressionClass::FUNCTION:
		return !expr.Cast<FunctionExpression>().is_operator;
	default:
		return false;
	}
}

//! Whether a WHEN's condition may have been read short. The grammar reads one term after a
//! reducer's WHEN, and after the objective's also one bare comparison of two terms
//! (`c > cap`); the rest of a longer condition parses as operators around the marker. A
//! parenthesized condition is whole, and the operators after it belong to the clause.
bool MayRunOn(const ParsedExpression &condition, bool objective) {
	if (IsOneTerm(condition)) {
		return true;
	}
	if (objective && condition.GetExpressionClass() == ExpressionClass::COMPARISON) {
		auto &comparison = condition.Cast<ComparisonExpression>();
		return IsOneTerm(*comparison.left) && IsOneTerm(*comparison.right);
	}
	return false;
}

//! Whether an operator's operands other than its first hold a reducer: a condition holds
//! none, so such an operator belongs to the clause (`SUM(x) WHEN (g = 'A') + SUM(y)`).
bool OtherOperandsHoldAReducer(const ParsedExpression &expr) {
	bool found = false;
	idx_t index = 0;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || (index++ > 0 && ContainsReducerCall(child)); });
	return found;
}

WrittenWhen WrittenCondition(const vector<PathStep> &path, idx_t first, idx_t &top, bool objective) {
	const idx_t marker = path.size() - 1;
	auto &when = path[marker].node->Cast<FunctionExpression>();
	WrittenWhen result;
	top = marker;
	if (auto *swallowed = SwallowedCondition(when)) {
		result.text = Written(*swallowed);
		result.is_or = swallowed->GetExpressionType() == ExpressionType::CONJUNCTION_OR;
		return result;
	}
	auto &own = *when.children[1];
	result.text = Written(own);
	result.is_or = own.GetExpressionType() == ExpressionType::CONJUNCTION_OR;
	if (!MayRunOn(own, objective)) {
		return result;
	}
	vector<bool> continues(path.size(), false);
	continues[marker] = true;
	for (idx_t i = marker; i-- > first;) {
		auto &step = path[i];
		if (step.child == 0 && ContinuesFirstOperand(*step.node) && !OtherOperandsHoldAReducer(*step.node)) {
			continues[i] = true;
		} else if (step.child + 1 != step.children || !EndsWithLastOperand(*step.node)) {
			break;
		}
	}
	// The condition ends at the outermost operator that continues it and yields a condition;
	// a bare comparison already is one, so any operator it runs on into extends it.
	const bool own_is_condition = IsConditionNode(own);
	for (idx_t i = first; i < marker; i++) {
		if (continues[i] && (own_is_condition || IsConditionNode(*path[i].node))) {
			top = i;
			break;
		}
	}
	// The text as written: each continuing operator's text after its first operand, in order.
	for (idx_t i = marker; i-- > top;) {
		if (!continues[i]) {
			continue;
		}
		auto node = path[i].node->Copy();
		*ChildSlot(*node, 0) = make_uniq<ColumnRefExpression>("__decide_condition__");
		result.text = StringUtil::Replace(Written(*node), "__decide_condition__", result.text);
		result.is_or = node->GetExpressionType() == ExpressionType::CONJUNCTION_OR;
	}
	return result;
}

//! Whether every node from `path[top]` down to the marker continues the condition: the
//! whole of `path[top]` is the marker and its condition.
bool ConditionIsWhole(const vector<PathStep> &path, idx_t top) {
	for (idx_t i = top; i + 1 < path.size(); i++) {
		if (path[i].child != 0 || !ContinuesFirstOperand(*path[i].node)) {
			return false;
		}
	}
	return true;
}

//! A clause's own relation, which a condition never extends over.
bool IsClauseRelation(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COMPARISON:
	case ExpressionClass::BETWEEN:
		return true;
	case ExpressionClass::OPERATOR:
		return expr.GetExpressionType() == ExpressionType::COMPARE_IN ||
		       expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN;
	default:
		return false;
	}
}

//===--------------------------------------------------------------------===//
// The WHENs after reducers
//===--------------------------------------------------------------------===//

//! One `SUM(e) WHEN c` and what to write instead.
struct ReducerWhenFix {
	string written;
	string spelling;
	bool in_norm = false;
	string condition;
};

//! Where a region of removed forms stands: a constraint, the objective, or another
//! expression (a subquery's select list).
enum class Region { CLAUSE, OBJECTIVE, OTHER };

//! Every WHEN after a reducer in `region` (a clause, an objective), in written order, until
//! another removed form: each one's reducer as written and its new spelling. `region` is
//! rewritten in place: each fixed WHEN, with the operators its condition ran on into,
//! becomes a column named by `placeholder(i)`.
vector<ReducerWhenFix> FixReducerWhens(unique_ptr<ParsedExpression> &region, Region kind) {
	vector<ReducerWhenFix> fixes;
	while (true) {
		vector<PathStep> path;
		if (!FindRemovedMarker(*region, path) || !IsDecideMarker(*path.back().node, WHEN_CONSTRAINT_TAG)) {
			return fixes;
		}
		auto &marker = path.back().node->Cast<FunctionExpression>();
		Reducer reducer;
		if (!ReadReducer(*marker.children[0], reducer)) {
			return fixes;
		}
		idx_t first = kind == Region::CLAUSE && IsClauseRelation(*region) ? 1 : 0;
		idx_t top;
		auto condition = WrittenCondition(path, first, top, kind == Region::OBJECTIVE);
		ReducerWhenFix fix;
		fix.written = ReducerAsWritten(*marker.children[0]);
		fix.condition = condition.text;
		reducer.removed.push_back(condition.AsCondition());
		fix.spelling = ReducerSpelling(reducer);
		fix.in_norm = fix.spelling.empty();
		fixes.push_back(std::move(fix));
		// Replace what was fixed, so the next search finds the next one.
		unique_ptr<ParsedExpression> *slot = &region;
		for (idx_t i = 0; i < top; i++) {
			slot = ChildSlot(**slot, path[i].child);
		}
		*slot = make_uniq<ColumnRefExpression>("__decide_fix_" + to_string(fixes.size() - 1) + "__");
	}
}

//! `expr` as written, with every WHEN after a reducer in the scope spellings.
string Converted(const ParsedExpression &expr) {
	auto copy = expr.Copy();
	auto fixes = FixReducerWhens(copy, Region::OBJECTIVE);
	auto text = Written(*copy);
	for (idx_t i = 0; i < fixes.size(); i++) {
		auto spelling = fixes[i].in_norm ? fixes[i].written : fixes[i].spelling;
		text = StringUtil::Replace(text, "__decide_fix_" + to_string(i) + "__", spelling);
	}
	return text;
}

string FixText(const ReducerWhenFix &fix) {
	if (fix.in_norm) {
		return "norm takes no when inside it: write when " + fix.condition +
		       ": in front of the constraint when the condition holds for all of it";
	}
	return "write " + fix.spelling;
}

//! What the message adds for the WHENs after reducers in `constraint`.
string ReducerWhenNote(const ParsedExpression &constraint) {
	auto copy = constraint.Copy();
	string note;
	for (auto &fix : FixReducerWhens(copy, Region::CLAUSE)) {
		note += "; the WHEN after " + fix.written + " is removed too: " + FixText(fix);
	}
	return note;
}

//! `SUM(e) WHEN c` inside a clause (or another expression): every such WHEN of it, named at
//! the first.
[[noreturn]] void RefuseReducerWhens(const ParsedExpression &clause, Region kind = Region::CLAUSE) {
	// The error points at the first WHEN in the clause itself: the copy below is rewritten
	// as each WHEN is fixed, so nothing in it outlives the fix.
	vector<PathStep> path;
	FindRemovedMarker(clause, path);
	const ParsedExpression &first_marker = path.empty() ? clause : *path.back().node;
	auto copy = clause.Copy();
	auto fixes = FixReducerWhens(copy, kind);
	D_ASSERT(!fixes.empty());
	string message;
	for (idx_t i = 0; i < fixes.size(); i++) {
		message += (i == 0 ? "" : "; ") + string("the WHEN after ") + fixes[i].written + " is removed" +
		           (i == 0 ? "" : " too") + "; " + FixText(fixes[i]);
	}
	Refuse(first_marker, message);
}

//===--------------------------------------------------------------------===//
// The postfix WHEN and PER after a constraint
//===--------------------------------------------------------------------===//

//! `C PER g` and `C WHEN c PER g` after a constraint.
[[noreturn]] void RefusePostfixPer(const FunctionExpression &per) {
	auto key = KeyText(per);
	const ParsedExpression *constraint = per.children[0].get();
	const ParsedExpression *when = nullptr;
	if (IsDecideMarker(*constraint, WHEN_CONSTRAINT_TAG) &&
	    IsConstraint(*constraint->Cast<FunctionExpression>().children[0])) {
		auto &marker = constraint->Cast<FunctionExpression>();
		when = marker.children[1].get();
		constraint = marker.children[0].get();
	}
	string message = "the postfix PER is removed" + string(when ? ", as is the WHEN before it" : "");
	if (!ContainsReducerCall(*constraint)) {
		// The removed PER grouped reducers only; a constraint without one holds on every row.
		message += "; a constraint with no reducer already holds on every row, so drop the PER";
		if (when) {
			message += " and write when " + Written(*when) + ": in front of the constraint";
		}
		Refuse(per, message);
	}
	vector<string> conditions;
	if (when) {
		conditions.push_back(ConditionText(*when));
	}
	auto front = FrontScope(conditions, key);
	front.pop_back(); // the space after its colon
	message += "; write " + front + " in front of the constraint and by (" + key + ") after each of its reducers";
	Refuse(per, message + ReducerWhenNote(*constraint));
}

//! `C WHEN c` after a constraint. When C is not a constraint, the condition read on into the
//! clause's bound (`sum(x) by (g) WHEN (c) <= 5`): the WHEN followed the reducer C ends with.
[[noreturn]] void RefusePostfixWhen(const FunctionExpression &when) {
	auto &constraint = *when.children[0];
	auto &condition = *when.children[1];
	if (SwallowedCondition(when)) {
		// Put the WHEN back on the reducer the constraint's text ends with.
		auto &bound = condition.Cast<ComparisonExpression>();
		const ParsedExpression *at = &constraint;
		while (!TakesReducerBy(UnwrapReducerMarkers(*at)) && !IsDecideMarker(*at, WHEN_CONSTRAINT_TAG) &&
		       EndsWithLastOperand(*at) && ChildCount(*at) > 0) {
			idx_t last = ChildCount(*at) - 1, index = 0;
			ParsedExpressionIterator::EnumerateChildren(*at, [&](const ParsedExpression &child) {
				if (index++ == last) {
					at = &child;
				}
			});
		}
		Reducer reducer;
		if (ReadReducer(*at, reducer)) {
			auto written = ReducerAsWritten(*at);
			reducer.removed.push_back(ConditionText(*bound.left));
			auto spelling = ReducerSpelling(reducer);
			if (spelling.empty()) {
				Refuse(when, "the WHEN after " + written + " is removed; norm takes no when inside it: write when " +
				                 Written(*bound.left) +
				                 ": in front of the constraint when the condition holds for all of it");
			}
			Refuse(when, "the WHEN after " + written + " is removed; write " + spelling);
		}
	}
	Refuse(when, "the postfix WHEN is removed; write when " + Written(condition) + ": in front of the constraint" +
	                 ReducerWhenNote(constraint));
}

//! The removed forms in one clause, outermost first: a form after the whole constraint is
//! named before one inside it.
void RefuseClause(const ParsedExpression &clause) {
	if (clause.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		ParsedExpressionIterator::EnumerateChildren(clause,
		                                            [&](const ParsedExpression &child) { RefuseClause(child); });
		return;
	}
	if (IsDecideMarker(clause, PER_CONSTRAINT_TAG)) {
		RefusePostfixPer(clause.Cast<FunctionExpression>());
	}
	if (IsDecideMarker(clause, WHEN_CONSTRAINT_TAG)) {
		auto &when = clause.Cast<FunctionExpression>();
		Reducer reducer;
		if (SwallowedCondition(when) && ReadReducer(*when.children[0], reducer)) {
			RefuseReducerWhens(clause);
		}
		RefusePostfixWhen(when);
	}
	vector<PathStep> path;
	if (!FindRemovedMarker(clause, path)) {
		return;
	}
	auto &marker = *path.back().node;
	if (IsDecideMarker(marker, PER_CONSTRAINT_TAG)) {
		RefusePostfixPer(marker.Cast<FunctionExpression>());
	}
	Reducer reducer;
	if (ReadReducer(*marker.Cast<FunctionExpression>().children[0], reducer)) {
		RefuseReducerWhens(clause);
	}
	RefusePostfixWhen(marker.Cast<FunctionExpression>());
}

//===--------------------------------------------------------------------===//
// The objective
//===--------------------------------------------------------------------===//

//! The WHEN written after the whole of `expr` (`E WHEN c`, `SUM(x) WHEN g IN ('A')`):
//! `condition` receives it as written and `body` what it followed.
bool WhenAfterWhole(const ParsedExpression &expr, WrittenWhen &condition, const ParsedExpression *&body) {
	vector<PathStep> path;
	if (!FindRemovedMarker(expr, path) || !IsDecideMarker(*path.back().node, WHEN_CONSTRAINT_TAG)) {
		return false;
	}
	idx_t top;
	auto written = WrittenCondition(path, 0, top, true);
	if (top != 0 || !ConditionIsWhole(path, 0)) {
		return false;
	}
	condition = std::move(written);
	body = path.back().node->Cast<FunctionExpression>().children[0].get();
	return true;
}

//! A reducer nested in a reducer anywhere in `expr`: the outer call, or null.
const FunctionExpression *NestedReducer(const ParsedExpression &expr) {
	auto &call = UnwrapReducerMarkers(expr);
	if (TakesReducerBy(call)) {
		auto &outer = call.Cast<FunctionExpression>();
		for (auto &child : outer.children) {
			if (ContainsReducerCall(*child)) {
				return &outer;
			}
		}
	}
	const FunctionExpression *found = nullptr;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!found) {
			found = NestedReducer(child);
		}
	});
	return found;
}

//! `MINIMIZE SUM(MAX(e)) PER g` (the two-level form), `MAXIMIZE SUM(e) PER g` (an objective
//! is produced once), with a WHEN after the objective or inside it.
[[noreturn]] void RefuseObjectivePer(const FunctionExpression &marker, const string &sense) {
	auto key = KeyText(marker);
	const ParsedExpression *body = marker.children[0].get();
	vector<string> front;
	WrittenWhen after;
	if (WhenAfterWhole(*body, after, body)) {
		front.push_back(after.AsCondition());
	}
	const string removed = "the postfix PER is removed; ";
	if (auto *outer = NestedReducer(*body)) {
		Reducer inner;
		bool two_level = &UnwrapReducerMarkers(*body) == outer && outer->children.size() == 1 &&
		                 ReadReducer(*outer->children[0], inner);
		if (!two_level) {
			Refuse(marker, removed + "the two-level form takes one reducer call inside the outer one, as in " + sense +
			                   " sum(per " + key + ": max(e) by (" + key + ")), and nothing beside it; " +
			                   Written(*body) + " is not supported yet");
		}
		if (!inner.per.empty()) {
			Refuse(marker, removed + "the two-level form " + StringUtil::Lower(outer->function_name) + "(per " + key +
			                   ": inner(e) by (" + key + ")) takes no per inside its inner reducer yet");
		}
		// A when inside the inner reducer goes in front of the objective.
		front.insert(front.end(), inner.conditions.begin(), inner.conditions.end());
		front.insert(front.end(), inner.removed.begin(), inner.removed.end());
		inner.conditions.clear();
		inner.removed.clear();
		inner.by.clear();
		auto outer_name = StringUtil::Lower(outer->function_name);
		string note;
		if (outer_name == "avg") {
			outer_name = "sum";
			note = " (an average over the classes is not supported yet; a sum ranks the solutions the same way)";
		}
		auto two_level_text = outer_name + "(per " + key + ": " + ReducerSpelling(inner) + " by (" + key + "))";
		Refuse(marker, removed + "write " + sense + " " + FrontScope(front, "") + two_level_text + note);
	}
	auto once = sense + " " + FrontScope(front, "") + Converted(*body);
	if (TakesReducerBy(UnwrapReducerMarkers(*body))) {
		auto &call = UnwrapReducerMarkers(*body).Cast<FunctionExpression>();
		auto name = StringUtil::Lower(call.function_name);
		auto per_class = "sum(per " + key + ": " + Converted(*body) + " by (" + key + "))";
		if (name == "avg") {
			Refuse(marker, removed + "write " + sense + " " + FrontScope(front, "") + per_class +
			                   " to add up one average per class as before, or " + once +
			                   " for one average over all rows");
		}
		if (name == "min" || name == "max") {
			Refuse(marker, removed + "an objective is produced once: write " + once + ", or " + sense + " " +
			                   FrontScope(front, "") + per_class + " to combine one value per class");
		}
	}
	Refuse(marker, removed + "an objective is produced once: write " + once);
}

void RefuseObjective(const ParsedExpression &objective, const string &sense) {
	if (IsDecideMarker(objective, PER_CONSTRAINT_TAG)) {
		RefuseObjectivePer(objective.Cast<FunctionExpression>(), sense);
	}
	vector<PathStep> path;
	if (!FindRemovedMarker(objective, path)) {
		return;
	}
	auto &marker = path.back().node->Cast<FunctionExpression>();
	if (IsDecideMarker(marker, PER_CONSTRAINT_TAG)) {
		RefuseObjectivePer(marker, sense);
	}
	WrittenWhen condition;
	const ParsedExpression *body;
	if (WhenAfterWhole(objective, condition, body)) {
		// After the whole objective or after its one reducer: the front form filters the
		// same rows either way.
		Refuse(marker,
		       "the postfix WHEN is removed; write " + sense + " when " + condition.text + ": " + Converted(*body));
	}
	Reducer reducer;
	if (!ReadReducer(*marker.children[0], reducer)) {
		Refuse(marker, "the postfix WHEN is removed; write the condition in front of the objective, as in " + sense +
		                   " when c: " + Written(*marker.children[0]));
	}
	auto copy = objective.Copy();
	auto fixes = FixReducerWhens(copy, Region::OBJECTIVE);
	D_ASSERT(!fixes.empty());
	string message;
	for (idx_t i = 0; i < fixes.size(); i++) {
		message += (i == 0 ? "" : "; ") + string("the WHEN after ") + fixes[i].written + " is removed" +
		           (i == 0 ? "" : " too") + "; " +
		           (fixes[i].in_norm ? "norm takes no when inside it" : "write " + fixes[i].spelling);
	}
	Refuse(marker, message);
}

//===--------------------------------------------------------------------===//
// Declarations
//===--------------------------------------------------------------------===//

//! A declaration from a stored parse tree: `T.x(TYPE)` (a qualified name) or
//! `scalar x(TYPE)` (a `scalar_` type marker). A fresh query never gets here: the
//! grammar refuses both, with the same messages.
void RefuseDeclaration(const ParsedExpression &declaration) {
	if (declaration.GetExpressionClass() != ExpressionClass::COMPARISON) {
		return;
	}
	auto &comparison = declaration.Cast<ComparisonExpression>();
	string marker;
	if (comparison.right->GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &constant = comparison.right->Cast<ConstantExpression>();
		if (constant.value.type() == LogicalType::VARCHAR) {
			marker = constant.value.ToString();
		}
	}
	bool scalar = StringUtil::StartsWith(marker, "scalar_");
	if (scalar) {
		marker = marker.substr(7);
	}
	string type = marker == "integer_variable" ? "INT" : marker == "real_variable" ? "REAL" : "BOOL";
	if (comparison.left->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return;
	}
	auto &name = comparison.left->Cast<ColumnRefExpression>();
	auto x = KeywordHelper::WriteOptionallyQuoted(name.GetColumnName());
	auto declared = x + "(" + type + ")";
	if (scalar) {
		auto written =
		    name.IsQualified() ? KeywordHelper::WriteOptionallyQuoted(name.GetTableName()) + "." + declared : declared;
		Refuse(declaration, "scalar " + written + " is removed; write per (): " + declared);
	}
	if (name.IsQualified()) {
		auto t = KeywordHelper::WriteOptionallyQuoted(name.GetTableName());
		Refuse(declaration, "the declaration " + t + "." + declared + " is removed; write per " + t + ": " + declared +
		                        " (per " + t + " keys on every column of " + t + "; write per " + t +
		                        ".k: " + declared + " to key on one column k)");
	}
}

//! The removed forms in an expression that is not a constraint: a WHEN there followed a
//! reducer, and its condition is whole.
void RefuseInExpression(const ParsedExpression &expr) {
	vector<PathStep> path;
	if (!FindRemovedMarker(expr, path)) {
		return;
	}
	auto &marker = path.back().node->Cast<FunctionExpression>();
	if (IsDecideMarker(marker, PER_CONSTRAINT_TAG)) {
		RefusePostfixPer(marker);
	}
	Reducer reducer;
	if (ReadReducer(*marker.children[0], reducer)) {
		RefuseReducerWhens(expr, Region::OTHER);
	}
	RefusePostfixWhen(marker);
}

//! The removed forms in a select node's own expressions. Only a DECIDE clause's text is
//! read with its spellings, so they reach a plain node only from a subquery inside a clause.
void RefuseInExpressions(const SelectNode &node) {
	auto refuse = [](const unique_ptr<ParsedExpression> &expr) {
		if (expr) {
			RefuseInExpression(*expr);
		}
	};
	for (auto &expr : node.select_list) {
		refuse(expr);
	}
	refuse(node.where_clause);
	for (auto &expr : node.groups.group_expressions) {
		refuse(expr);
	}
	refuse(node.having);
	refuse(node.qualify);
}

} // namespace

void RefuseRemovedDecideSpellings(const SelectNode &node) {
	RefuseInExpressions(node);
	for (auto &declaration : node.decide_variables) {
		RefuseDeclaration(*declaration);
	}
	if (node.decide_constraints) {
		RefuseClause(*node.decide_constraints);
	}
	if (node.decide_objective) {
		RefuseObjective(*node.decide_objective, node.decide_sense == DecideSense::MINIMIZE ? "MINIMIZE" : "MAXIMIZE");
	}
}

} // namespace duckdb
