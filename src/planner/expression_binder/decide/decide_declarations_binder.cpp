#include "duckdb/planner/expression_binder/decide/decide_declarations_binder.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_constraints_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_binder/decide/decide_generation.hpp"
#include "duckdb/planner/expression_binder/decide/decide_objective_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"

#include <algorithm>
#include <unordered_set>

namespace duckdb {

// ABS, MIN/MAX and `<>` bind here as ordinary expressions -- a BoundFunctionExpression,
// a BoundAggregateExpression, a BoundComparisonExpression. Choosing a mathematical
// formulation for them is layer 5's job (`src/optimizer/decide/`), which is what lets
// this layer resolve names and types without also deciding how a construct is encoded.

// Rewrite qualified `Table.var` ColumnRefs into bare `var` ColumnRefs when
// `Table.var` matches a decision whose key is exactly that relation. After this
// pass, every reference to a decision variable is unqualified, so the regular
// DuckDB binder (used for SELECT/ORDER/etc.) and the per-row branch of
// DecideConstraintsBinder both resolve it through the generic `decide_variables`
// binding instead of routing `Table` to the real table binding.
static void RewriteScopedVarRefs(unique_ptr<ParsedExpression> &expr,
                                 const case_insensitive_map_t<idx_t> &variables) {
	if (!expr) {
		return;
	}
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			string qualified = colref.GetTableName() + "." + colref.GetColumnName();
			if (variables.count(qualified)) {
				auto alias = colref.GetAlias();
				expr = make_uniq<ColumnRefExpression>(colref.GetColumnName());
				if (!alias.empty()) {
					expr->alias = alias;
				}
				return;
			}
		}
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		RewriteScopedVarRefs(child, variables);
	});
}

//! The declaration-level bounds are constraints: `PER K: x(INT) >= lo` is exactly
//! `PER K: x >= lo` written in SUCH THAT, generated once per instance of x. They join
//! the constraint tree here, before binding, so every later stage sees one kind of
//! bound and the diagnosis can quote them like any other clause.
//! `PER K: <constraint>` for a declaration's own key, so a synthesized row is generated
//! once per instance of the decision it describes.
static unique_ptr<ParsedExpression> WrapWithDeclarationScope(const DecideDeclaration &declaration,
                                                             unique_ptr<ParsedExpression> constraint) {
	if (declaration.scope_kind == DecideScopeKind::ROW) {
		return constraint;
	}
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(constraint));
	for (auto &elem : declaration.scope_key) {
		children.push_back(elem->Copy());
	}
	auto wrapper = make_uniq<FunctionExpression>(PER_CONSTRAINT_TAG, std::move(children));
	wrapper->is_operator = true;
	return std::move(wrapper);
}

static unique_ptr<ParsedExpression> MakeDeclarationBound(const DecideDeclaration &declaration, ExpressionType type,
                                                         const ParsedExpression &bound) {
	unique_ptr<ParsedExpression> comparison = make_uniq<ComparisonExpression>(
	    type, make_uniq<ColumnRefExpression>(declaration.name), bound.Copy());
	return WrapWithDeclarationScope(declaration, std::move(comparison));
}

//! `x <op> bound * switch`: the rows a SEMI domain's range becomes.
static unique_ptr<ParsedExpression> MakeSwitchedBound(const DecideDeclaration &declaration, ExpressionType type,
                                                      const ParsedExpression &bound, const string &switch_name) {
	vector<unique_ptr<ParsedExpression>> product;
	product.push_back(bound.Copy());
	product.push_back(make_uniq<ColumnRefExpression>(switch_name));
	auto scaled = make_uniq<FunctionExpression>("*", std::move(product));
	scaled->is_operator = true;
	return MakeDeclarationBound(declaration, type, *scaled);
}

//! A TEXT decision as the binder knows it: its name, its values, and the hidden
//! indicator that stands for each value.
struct TextDomainSpec {
	string name;
	vector<string> values;
	vector<string> indicator_names;

	idx_t IndexOf(const string &value) const {
		for (idx_t i = 0; i < values.size(); i++) {
			if (values[i] == value) {
				return i;
			}
		}
		return DConstants::INVALID_INDEX;
	}
	string Spelled() const {
		vector<string> quoted;
		for (auto &value : values) {
			quoted.push_back("'" + value + "'");
		}
		return "[" + StringUtil::Join(quoted, ", ") + "]";
	}
};

static const TextDomainSpec *TextDecisionOf(const ParsedExpression &expr, const vector<TextDomainSpec> &domains) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return nullptr;
	}
	auto &colref = expr.Cast<ColumnRefExpression>();
	if (colref.IsQualified()) {
		return nullptr;
	}
	for (auto &domain : domains) {
		if (StringUtil::CIEquals(domain.name, colref.GetColumnName())) {
			return &domain;
		}
	}
	return nullptr;
}

static string TextConstantOf(const ParsedExpression &expr, bool &ok) {
	ok = expr.GetExpressionClass() == ExpressionClass::CONSTANT &&
	     expr.Cast<ConstantExpression>().value.type().id() == LogicalTypeId::VARCHAR;
	return ok ? expr.Cast<ConstantExpression>().value.GetValue<string>() : string();
}

//! A TEXT decision is written only as `x = 'v'`, `x <> 'v'` or `x IN ('v', ...)`. Each
//! becomes the same comparison over the value's indicator (`z_v >= 1`, `z_v <= 0`,
//! `z_a + z_b >= 1`), carrying the spelling the user wrote so every later rendering
//! quotes `x = 'v'` and never the indicator. The domain itself is the one-hot row
//! the declaration adds beside the indicators.
static void RewriteTextComparisons(unique_ptr<ParsedExpression> &expr, const vector<TextDomainSpec> &domains,
                                   vector<string> &fragments, const char *clause) {
	if (!expr || domains.empty()) {
		return;
	}
	auto indicator_sum = [&](const TextDomainSpec &domain, const vector<idx_t> &chosen) {
		unique_ptr<ParsedExpression> sum;
		for (auto i : chosen) {
			unique_ptr<ParsedExpression> ref = make_uniq<ColumnRefExpression>(domain.indicator_names[i]);
			if (!sum) {
				sum = std::move(ref);
				continue;
			}
			vector<unique_ptr<ParsedExpression>> children;
			children.push_back(std::move(sum));
			children.push_back(std::move(ref));
			auto plus = make_uniq<FunctionExpression>("+", std::move(children));
			plus->is_operator = true;
			sum = std::move(plus);
		}
		return sum;
	};
	// The written spelling, kept as the user would type it (ToString would quote the
	// name and parenthesize the comparison).
	auto replace = [&](unique_ptr<ParsedExpression> lowered, const string &written) {
		fragments.push_back(written);
		lowered->alias = MakeSourceFragmentTag(fragments.size() - 1);
		expr = std::move(lowered);
	};
	auto quoted = [](const string &value) {
		return "'" + value + "'";
	};
	auto value_index = [&](const TextDomainSpec &domain, const ParsedExpression &operand) {
		bool ok;
		string value = TextConstantOf(operand, ok);
		if (!ok) {
			throw BinderException(*expr, "TEXT decision '%s' is compared with one of its values %s, not with '%s'",
			                      domain.name, domain.Spelled(), operand.ToString());
		}
		idx_t index = domain.IndexOf(value);
		if (index == DConstants::INVALID_INDEX) {
			throw BinderException(*expr, "TEXT decision '%s' has no value '%s'; its values are %s", domain.name, value,
			                      domain.Spelled());
		}
		return index;
	};

	if (expr->GetExpressionClass() == ExpressionClass::COMPARISON) {
		auto &comparison = expr->Cast<ComparisonExpression>();
		auto *left = TextDecisionOf(*comparison.left, domains);
		auto *right = TextDecisionOf(*comparison.right, domains);
		if ((left || right) && string(clause) == "objective") {
			throw BinderException(*expr, "A TEXT decision has no numeric value, so '%s' cannot enter an objective; "
			                             "weigh a BOOL decision forced by IF %s: instead.",
			                      expr->ToString(), expr->ToString());
		}
		if (left || right) {
			if (left && right) {
				throw BinderException(*expr, "Two TEXT decisions cannot be compared with each other; compare each "
				                             "with one of its values.");
			}
			auto &domain = left ? *left : *right;
			auto &operand = left ? *comparison.right : *comparison.left;
			if (comparison.type != ExpressionType::COMPARE_EQUAL && comparison.type != ExpressionType::COMPARE_NOTEQUAL) {
				throw BinderException(*expr, "TEXT decision '%s' supports = and <> against one of its values %s.",
				                      domain.name, domain.Spelled());
			}
			idx_t index = value_index(domain, operand);
			const bool equal = comparison.type == ExpressionType::COMPARE_EQUAL;
			string written = domain.name + (equal ? " = " : " <> ") + quoted(domain.values[index]);
			replace(make_uniq<ComparisonExpression>(
			            equal ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : ExpressionType::COMPARE_LESSTHANOREQUALTO,
			            indicator_sum(domain, {index}), make_uniq<ConstantExpression>(Value::INTEGER(equal ? 1 : 0))),
			        written);
			return;
		}
	}
	if (expr->GetExpressionClass() == ExpressionClass::OPERATOR) {
		auto &op = expr->Cast<OperatorExpression>();
		if ((op.type == ExpressionType::COMPARE_IN || op.type == ExpressionType::COMPARE_NOT_IN) &&
		    !op.children.empty()) {
			if (auto *domain = TextDecisionOf(*op.children[0], domains)) {
				vector<idx_t> chosen;
				for (idx_t i = 1; i < op.children.size(); i++) {
					idx_t index = value_index(*domain, *op.children[i]);
					if (std::find(chosen.begin(), chosen.end(), index) == chosen.end()) {
						chosen.push_back(index);
					}
				}
				const bool in = op.type == ExpressionType::COMPARE_IN;
				vector<string> spelled;
				for (auto index : chosen) {
					spelled.push_back(quoted(domain->values[index]));
				}
				string written = domain->name + (in ? " IN (" : " NOT IN (") + StringUtil::Join(spelled, ", ") + ")";
				replace(make_uniq<ComparisonExpression>(
				            in ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : ExpressionType::COMPARE_LESSTHANOREQUALTO,
				            indicator_sum(*domain, chosen), make_uniq<ConstantExpression>(Value::INTEGER(in ? 1 : 0))),
				        written);
				return;
			}
		}
	}
	if (auto *domain = TextDecisionOf(*expr, domains)) {
		throw BinderException(*expr,
		                      "TEXT decision '%s' can only be compared with = or <> (or IN) to one of its values "
		                      "%s in a %s; it has no numeric value.",
		                      domain->name, domain->Spelled(), clause);
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		RewriteTextComparisons(child, domains, fragments, clause);
	});
}

static void AppendConstraint(unique_ptr<ParsedExpression> &tree, unique_ptr<ParsedExpression> constraint) {
	if (!tree) {
		tree = std::move(constraint);
		return;
	}
	tree = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(tree), std::move(constraint));
}

DecideDeclarationsBinder::DecideDeclarationsBinder(Binder &binder, ClientContext &context)
    : binder(binder), context(context) {
}

void DecideDeclarationsBinder::BindDeclarations(SelectNode &statement, BoundSelectNode &result) {
	auto &bind_context = binder.bind_context;

	case_insensitive_map_t<idx_t> decide_variable_names;
	vector<string> var_names;
	vector<LogicalType> var_types;
	vector<bool> is_boolean_var;
	vector<DecideDomain> domains;
	vector<vector<string>> text_values;
	vector<string> decide_source_fragments;

	// Keyed scopes: one EntityScopeInfo per distinct key, shared by every declaration,
	// reducer PER / BY, and constraint PER that names the same columns.
	vector<EntityScopeInfo> entity_scopes;
	vector<DecideVarScopeInfo> variable_scopes;
	case_insensitive_map_t<idx_t> table_scope_map;
	// Declarations whose domain is stated through hidden decisions (below).
	vector<idx_t> semi_declarations;
	vector<idx_t> text_declarations;

	for (auto &declaration : statement.decide_variables) {
		const string &name = declaration.name;
		if (bind_context.GetMatchingBinding(name)) {
			throw BinderException("DECIDE variable '%s' conflicts with an existing column name.", name);
		}
		if (decide_variable_names.count(name)) {
			throw BinderException("Duplicate DECIDE variable name '%s'.", name);
		}
		idx_t var_idx = var_names.size();
		decide_variable_names.emplace(name, var_idx);

		switch (declaration.scope_kind) {
		case DecideScopeKind::ROW:
			variable_scopes.push_back(DecideVarScopeInfo::Row());
			break;
		case DecideScopeKind::GLOBAL:
			variable_scopes.push_back(DecideVarScopeInfo::Scalar());
			break;
		case DecideScopeKind::KEY: {
			string error;
			for (auto &elem : declaration.scope_key) {
				if (ExpressionContainsDecideVariable(*elem, decide_variable_names)) {
					throw BinderException("DECIDE variable '%s': '%s' is a decision; a key generates one decision "
					                      "per value of known data, so it names columns or relations",
					                      name, elem->ToString());
				}
			}
			auto scope_idx =
			    FindOrCreateKeyScope(bind_context, declaration.scope_key, entity_scopes, table_scope_map, error);
			if (scope_idx == DConstants::INVALID_INDEX) {
				throw BinderException("DECIDE variable '%s': %s", name, error);
			}
			entity_scopes[scope_idx].scoped_variable_indices.push_back(var_idx);
			variable_scopes.push_back(DecideVarScopeInfo::Entity(scope_idx));
			// `PER T: x` may still be referenced as `T.x` when the key is exactly one
			// relation, the spelling the relation-scoped form has always had.
			if (declaration.scope_key.size() == 1 &&
			    declaration.scope_key[0]->GetExpressionClass() == ExpressionClass::COLUMN_REF &&
			    declaration.scope_key[0]->Cast<ColumnRefExpression>().column_names.size() == 1 &&
			    entity_scopes[scope_idx].source_table_indices.size() == 1) {
				decide_variable_names.emplace(
				    declaration.scope_key[0]->Cast<ColumnRefExpression>().GetColumnName() + "." + name, var_idx);
			}
			break;
		}
		}

		// An `INT` decision is BIGINT, not INTEGER. Nothing bounds a decision to int32:
		// the solver works in doubles, and an optimum driven by an aggregate row
		// (`SUM(x) <= 5000000000`) has no per-variable bound to inspect here. A narrower
		// column would truncate that answer on readback. `BOOL` stays INTEGER -- its
		// domain is 0/1, so it cannot overflow. The 0/1 domain itself travels as
		// `is_boolean_var`, never as a synthesized constraint. A TEXT decision is a
		// VARCHAR column whose one-hot solver encoding is the model formulation's.
		LogicalType type;
		bool is_boolean = false;
		switch (declaration.domain) {
		case DecideDomain::INT:
		case DecideDomain::SEMIINT:
			type = LogicalType::BIGINT;
			break;
		case DecideDomain::REAL:
		case DecideDomain::SEMIREAL:
			type = LogicalType::DOUBLE;
			break;
		case DecideDomain::BOOL:
			type = LogicalType::INTEGER;
			is_boolean = true;
			break;
		case DecideDomain::TEXT:
			type = LogicalType::VARCHAR;
			break;
		}
		if (declaration.domain == DecideDomain::SEMIREAL || declaration.domain == DecideDomain::SEMIINT) {
			if (!declaration.lower_bound || !declaration.upper_bound) {
				throw BinderException(
				    "DECIDE variable '%s' is %s and needs both bounds: write %s(%s) BETWEEN lo AND hi", name,
				    DecideDomainName(declaration.domain), name, DecideDomainName(declaration.domain));
			}
		}
		if (declaration.domain == DecideDomain::TEXT) {
			// Values compare exactly (case-sensitive), so 'a' and 'A' are two values.
			std::unordered_set<string> seen;
			for (auto &value : declaration.text_values) {
				if (!seen.insert(value).second) {
					throw BinderException("DECIDE variable '%s': TEXT value '%s' is listed twice", name, value);
				}
			}
		}
		var_names.push_back(name);
		var_types.push_back(type);
		is_boolean_var.push_back(is_boolean);
		domains.push_back(declaration.domain);
		text_values.push_back(declaration.text_values);
		if (declaration.domain == DecideDomain::SEMIREAL || declaration.domain == DecideDomain::SEMIINT) {
			semi_declarations.push_back(var_idx);
		} else if (declaration.domain == DecideDomain::TEXT) {
			text_declarations.push_back(var_idx);
		}

		if (declaration.lower_bound && declaration.upper_bound &&
		    declaration.lower_bound->GetExpressionClass() == ExpressionClass::CONSTANT &&
		    declaration.upper_bound->GetExpressionClass() == ExpressionClass::CONSTANT) {
			auto &lo = declaration.lower_bound->Cast<ConstantExpression>().value;
			auto &hi = declaration.upper_bound->Cast<ConstantExpression>().value;
			if (lo.type().IsNumeric() && hi.type().IsNumeric() && !lo.IsNull() && !hi.IsNull() &&
			    lo.GetValue<double>() > hi.GetValue<double>()) {
				throw BinderException("DECIDE variable '%s': BETWEEN %s AND %s is empty; the lower bound is above the "
				                      "upper bound.",
				                      name, lo.ToString(), hi.ToString());
			}
		}
		// Declaration bounds. A SEMI domain's bounds describe the "on" range, stated
		// through its switch below; a plain domain's bounds are constraints.
		if (declaration.domain != DecideDomain::SEMIREAL && declaration.domain != DecideDomain::SEMIINT) {
			if (declaration.lower_bound) {
				AppendConstraint(statement.decide_constraints,
				                 MakeDeclarationBound(declaration, ExpressionType::COMPARE_GREATERTHANOREQUALTO,
				                                      *declaration.lower_bound));
			}
			if (declaration.upper_bound) {
				AppendConstraint(statement.decide_constraints,
				                 MakeDeclarationBound(declaration, ExpressionType::COMPARE_LESSTHANOREQUALTO,
				                                      *declaration.upper_bound));
			}
		}
	}

	// Capture user var count BEFORE any rewrites that add auxiliary variables
	idx_t num_user_vars = var_names.size();

	// A declaration bound is a constant or a column the key determines (spec §4.1):
	// a decision or a reducer there is a constraint, and is written as one. Checked
	// once every name is declared, so a later declarator's name is a decision too.
	for (auto &declaration : statement.decide_variables) {
		for (auto *bound : {declaration.lower_bound.get(), declaration.upper_bound.get()}) {
			if (!bound) {
				continue;
			}
			if (ExpressionContainsDecideVariable(*bound, decide_variable_names) || ContainsDecideAggregate(*bound)) {
				throw BinderException(*bound,
				                      "DECIDE variable '%s': a declaration bound is a constant or a column; '%s' "
				                      "reads a decision or a reducer, so write it as a SUCH THAT constraint.",
				                      declaration.name, bound->ToString());
			}
		}
	}

	// The domains a hidden decision stands for. Declared here, beside the decision and
	// with its scope, so every later stage sees ordinary BOOL decisions and ordinary
	// rows; only the readback knows a TEXT column from its indicators.
	auto declare_hidden_bool = [&](const string &hidden_name, idx_t like_var) {
		idx_t hidden_idx = var_names.size();
		decide_variable_names.emplace(hidden_name, hidden_idx);
		var_names.push_back(hidden_name);
		var_types.push_back(LogicalType::INTEGER);
		is_boolean_var.push_back(true);
		domains.push_back(DecideDomain::BOOL);
		text_values.emplace_back();
		variable_scopes.push_back(variable_scopes[like_var]);
		if (variable_scopes[like_var].scope == DecideVarScope::ENTITY) {
			entity_scopes[variable_scopes[like_var].entity_scope_idx].scoped_variable_indices.push_back(hidden_idx);
		}
		return hidden_idx;
	};
	// SEMIREAL / SEMIINT: `x = 0 OR lo <= x <= hi`, as a switch `on` and the rows
	// `x <= hi * on` and `x >= lo * on`, plus the floor of the box when `lo` is
	// negative (the default box starts at 0, and only a constant floor can widen it).
	for (auto var_idx : semi_declarations) {
		auto &declaration = statement.decide_variables[var_idx];
		string switch_name = "__semi_on_" + declaration.name + "__";
		declare_hidden_bool(switch_name, var_idx);
		AppendConstraint(statement.decide_constraints,
		                 MakeSwitchedBound(declaration, ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                                   *declaration.upper_bound, switch_name));
		AppendConstraint(statement.decide_constraints,
		                 MakeSwitchedBound(declaration, ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		                                   *declaration.lower_bound, switch_name));
		// The ceiling itself as a plain bound too (`x <= GREATEST(hi, 0)`, since 0 is
		// always admitted): implied by `x <= hi * on`, and what gives the column a
		// finite box for a Big-M (a guard, a product) to read.
		vector<unique_ptr<ParsedExpression>> ceiling_children;
		ceiling_children.push_back(declaration.upper_bound->Copy());
		ceiling_children.push_back(make_uniq<ConstantExpression>(Value::INTEGER(0)));
		auto ceiling = make_uniq<FunctionExpression>("greatest", std::move(ceiling_children));
		AppendConstraint(statement.decide_constraints,
		                 MakeDeclarationBound(declaration, ExpressionType::COMPARE_LESSTHANOREQUALTO, *ceiling));
		// The floor of the box. A negative constant is stated as the plain bound the
		// optimizer absorbs into the column box; a data floor is a row per instance.
		auto &lower = *declaration.lower_bound;
		if (lower.GetExpressionClass() == ExpressionClass::CONSTANT) {
			auto &value = lower.Cast<ConstantExpression>().value;
			if (value.type().IsNumeric() && !value.IsNull() && value.GetValue<double>() < 0.0) {
				AppendConstraint(statement.decide_constraints,
				                 MakeDeclarationBound(declaration, ExpressionType::COMPARE_GREATERTHANOREQUALTO, lower));
			}
		} else {
			vector<unique_ptr<ParsedExpression>> floor_children;
			floor_children.push_back(lower.Copy());
			floor_children.push_back(make_uniq<ConstantExpression>(Value::INTEGER(0)));
			auto floor = make_uniq<FunctionExpression>("least", std::move(floor_children));
			AppendConstraint(statement.decide_constraints,
			                 MakeDeclarationBound(declaration, ExpressionType::COMPARE_GREATERTHANOREQUALTO, *floor));
		}
	}
	// TEXT IN [...]: one hidden BOOL indicator per value, exactly one of them 1.
	vector<TextDomainSpec> text_domains;
	for (auto var_idx : text_declarations) {
		auto &declaration = statement.decide_variables[var_idx];
		TextDomainSpec spec;
		spec.name = declaration.name;
		spec.values = declaration.text_values;
		DecideTextDomain domain;
		domain.variable_index = var_idx;
		domain.values = declaration.text_values;
		for (idx_t i = 0; i < declaration.text_values.size(); i++) {
			string indicator_name = "__text_" + declaration.name + "_" + to_string(i) + "__";
			spec.indicator_names.push_back(indicator_name);
			domain.indicator_indices.push_back(declare_hidden_bool(indicator_name, var_idx));
		}
		unique_ptr<ParsedExpression> one_hot;
		for (auto &indicator_name : spec.indicator_names) {
			unique_ptr<ParsedExpression> ref = make_uniq<ColumnRefExpression>(indicator_name);
			if (!one_hot) {
				one_hot = std::move(ref);
				continue;
			}
			vector<unique_ptr<ParsedExpression>> children;
			children.push_back(std::move(one_hot));
			children.push_back(std::move(ref));
			auto plus = make_uniq<FunctionExpression>("+", std::move(children));
			plus->is_operator = true;
			one_hot = std::move(plus);
		}
		unique_ptr<ParsedExpression> sum_to_one = make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL, std::move(one_hot), make_uniq<ConstantExpression>(Value::INTEGER(1)));
		decide_source_fragments.push_back(declaration.name + " IN " + spec.Spelled());
		sum_to_one->alias = MakeSourceFragmentTag(decide_source_fragments.size() - 1);
		AppendConstraint(statement.decide_constraints, WrapWithDeclarationScope(declaration, std::move(sum_to_one)));
		result.decide_text_domains.push_back(std::move(domain));
		text_domains.push_back(std::move(spec));
	}
	RewriteTextComparisons(statement.decide_constraints, text_domains, decide_source_fragments, "constraint");
	for (auto &objective : statement.decide_objectives) {
		RewriteTextComparisons(objective.expression, text_domains, decide_source_fragments, "objective");
	}

	RewriteScopedVarRefs(statement.decide_constraints, decide_variable_names);
	for (auto &objective : statement.decide_objectives) {
		RewriteScopedVarRefs(objective.expression, decide_variable_names);
	}
	for (auto &sel_expr : statement.select_list) {
		RewriteScopedVarRefs(sel_expr, decide_variable_names);
	}

	// This is the only point where cast authorship is still observable. Reject
	// explicit casts over decisions before DECIDE generates any parsed nodes and
	// before DuckDB binding adds its own type-reconciliation casts. SELECT-list
	// expressions are intentionally outside this solver-algebra policy.
	if (statement.decide_constraints) {
		ValidateDecideNoExplicitDecisionCasts(*statement.decide_constraints, decide_variable_names);
	}
	for (auto &objective : statement.decide_objectives) {
		ValidateDecideNoExplicitDecisionCasts(*objective.expression, decide_variable_names);
	}

	// NORM and IN bind as explicit markers. Their mathematical formulation
	// (indicators, linking rows, and aggregate rewrites) belongs to
	// DecideOptimizer, after types/scopes/casts are known. The binder creates no
	// auxiliaries of its own.
	idx_t num_auxiliary_vars = var_names.size() - num_user_vars;

	if (statement.decide_constraints) {
		// Reject scalar functions like sqrt(x), exp(x), floor(x) wrapping a
		// DECIDE variable, before anything downstream has to interpret them.
		ValidateDecideNoNonLinearScalar(context, *statement.decide_constraints, decide_variable_names);
		// A `<`, `>` or `<>` over a REAL decision has no exact encoding -- all three
		// are encoded by stepping the bound one integer unit -- and the declared type
		// says so without reading a row.
		ValidateDecideNoIntegerStepComparisonOnReal(*statement.decide_constraints, decide_variable_names,
		                                            var_types);
		// Source-only casts and scalar subqueries lose their written spelling
		// during binding/flattening. Give those atoms stable fragment ids now;
		// the DECIDE binder carries the tags onto the bound nodes.
		TagDecideSourceFragments(*statement.decide_constraints, decide_source_fragments);
	}
	for (auto &objective : statement.decide_objectives) {
		ValidateDecideNoNonLinearScalar(context, *objective.expression, decide_variable_names);
	}

	bind_context.AddGenericBinding(result.decide_index, "decide_variables", var_names, var_types);
	// Names declared `PER ()`, so the constraint and objective binders can tell a
	// query-wide decision from a row-scoped one.
	case_insensitive_set_t scalar_variable_names;
	for (idx_t v = 0; v < variable_scopes.size() && v < var_names.size(); v++) {
		if (variable_scopes[v].IsScalar()) {
			scalar_variable_names.insert(var_names[v]);
		}
	}
	// What a keyed reducer needs to resolve `PER K` / `BY (K)`. The binders may
	// append a key-only entity scope to `entity_scopes`, which is why this runs
	// before the vector is moved onto the operator below.
	DecideQualifierContext qualifier_context;
	qualifier_context.decide_index = result.decide_index;
	qualifier_context.entity_scopes = &entity_scopes;
	qualifier_context.table_scope_map = &table_scope_map;
	qualifier_context.variable_scopes = &variable_scopes;
	qualifier_context.variable_domains = &domains;
	qualifier_context.frames = &result.decide_frames;
	qualifier_context.source_fragments = &decide_source_fragments;
	// Isolate with brackets to avoid multiple active binders.
	{
		DecideConstraintsBinder decide_constraints_binder(binder, context, decide_variable_names, scalar_variable_names,
		                                                  &qualifier_context);
		unique_ptr<ParsedExpression> constraints = std::move(statement.decide_constraints);
		result.decide_constraints = decide_constraints_binder.Bind(constraints);
		if (result.decide_constraints) {
			// The other half of the integer-step gate: every other operand's type is only
			// known now that binding has resolved it.
			ValidateDecideIntegralComparisonOperands(*result.decide_constraints, result.decide_index);
			// Degree has one owner, and this is it, on the bound tree.
			ValidateDecideConstraintDegree(*result.decide_constraints, result.decide_index);
			// Generation is well defined only when every value an instance reads is a
			// function of the key that generated it (spec §6.3); proved from the schema.
			ValidateDecideGenerationTree(*result.decide_constraints, qualifier_context, bind_context);
			ValidateDecideNoNestedReducers(*result.decide_constraints);
			// A factor on a reducer that its BY key determines is one value per reduced
			// group; proved here, admitted as a scale by the canonicalizer.
			TagGroupWideReducerFactors(*result.decide_constraints, qualifier_context, bind_context);
			result.decide_constraint_sources = InitializeConstraintSourceInfo(
			    *result.decide_constraints, decide_source_fragments, entity_scopes, result.decide_index);
			result.decide_source_fragments = decide_source_fragments;
		}
	}
	result.decide_sense = DecideSense::FEASIBILITY;
	for (idx_t stage = 0; stage < statement.decide_objectives.size(); stage++) {
		auto &objective_clause = statement.decide_objectives[stage];
		DecideObjectiveBinder decide_objective_binder(binder, context, decide_variable_names, scalar_variable_names,
		                                              &qualifier_context);
		decide_objective_binder.decide_sense = objective_clause.sense;
		unique_ptr<ParsedExpression> objective = std::move(objective_clause.expression);
		auto bound = decide_objective_binder.Bind(objective);
		if (bound) {
			ValidateDecideObjectiveDegree(*bound, result.decide_index);
			// An objective is generated exactly once (`PER ()`), so everything it reads
			// directly must be one value for the whole query: a reducer, a query-wide
			// decision, or a constant.
			auto error = CheckDeterminedByGeneration(*bound, DecideGenerationScope::Global(), qualifier_context,
			                                         bind_context);
			if (!error.empty()) {
				// The generation repair shared with `PER ()` constraints offers a key; an
				// objective has none to offer, so its repair is the nested reducer.
				error = StringUtil::Replace(error, "or generate PER a key that determines it",
				                            "or reduce it per key inside a nested reducer, e.g. MAX(PER k: SUM(...) BY (k))");
				throw BinderException(*bound, "%s objective: %s",
				                      objective_clause.sense == DecideSense::MAXIMIZE ? "MAXIMIZE" : "MINIMIZE",
				                      error);
			}
		}
		if (stage == 0) {
			result.decide_sense = objective_clause.sense;
			result.decide_objective = std::move(bound);
		} else {
			BoundDecideObjectiveStage tail_stage;
			tail_stage.sense = objective_clause.sense;
			tail_stage.expression = std::move(bound);
			result.decide_objective_tail.push_back(std::move(tail_stage));
		}
	}
	// Update types in bind context to reflect the determined types from DECIDE clause
	bind_context.GetBindingsList().back()->types = var_types;
	for (idx_t i = 0; i < var_names.size(); i++) {
		result.decide_variables.push_back(
		    make_uniq<BoundColumnRefExpression>(var_names[i], var_types[i], ColumnBinding(result.decide_index, i)));
	}
	result.num_auxiliary_vars = num_auxiliary_vars;
	result.is_boolean_var = is_boolean_var;
	result.decide_domains = std::move(domains);
	result.decide_text_values = std::move(text_values);

	// Create BoundColumnRefExpressions for every key column. These live on
	// LogicalDecide so that DuckDB's binder column-reference tracking (the initial
	// Get column_id selection and the column pruner's rebinding pass) treats key
	// columns as live. Without them, a key column that appears nowhere else would be
	// pruned from the scan, leaving the VarIndexer with a degenerate key that silently
	// collapses distinct instances.
	for (auto &scope : entity_scopes) {
		for (idx_t k = 0; k < scope.entity_key_bindings.size(); k++) {
			result.entity_key_expressions.push_back(make_uniq<BoundColumnRefExpression>(
			    "entity_key_" + scope.table_alias, scope.entity_key_column_types[k], scope.entity_key_bindings[k]));
		}
	}

	result.entity_scopes = std::move(entity_scopes);
	result.variable_scopes = variable_scopes;

	// Hide auxiliary vars from SELECT * by truncating the bind context binding.
	if (num_auxiliary_vars > 0) {
		auto &binding = *bind_context.GetBindingsList().back();
		for (idx_t i = num_user_vars; i < var_names.size(); i++) {
			binding.name_map.erase(var_names[i]);
		}
		binding.names.resize(num_user_vars);
		binding.types.resize(num_user_vars);
	}
}

} // namespace duckdb
