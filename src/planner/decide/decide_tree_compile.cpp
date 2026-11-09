//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/planner/decide/decide_tree_compile.cpp
//
// The tree's bodies from stage 05's product (language/guide.md §6.4): the second half of the tree's construction
// (DecideTreeConstruction, a Builder with a director in plan_decide.cpp). Stage 05 lowers the clauses once, for flat,
// and names on each reducer's terms the G they range over; this binds the clause roots to stage 05's clauses, copies
// the records, and finds the node each term is computed on.
//===----------------------------------------------------------------------===//

#include "duckdb/planner/decide/decide_tree.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <sstream>

namespace duckdb {

//! The compile's helpers, named apart: the planner's DECIDE files build as one unit.
namespace decide_tree_compile {

static unique_ptr<Expression> CopyOrNull(const unique_ptr<Expression> &expression) {
	return expression ? expression->Copy() : nullptr;
}

static vector<unique_ptr<Expression>> CopyAll(const vector<unique_ptr<Expression>> &expressions) {
	vector<unique_ptr<Expression>> result;
	for (auto &expression : expressions) {
		result.push_back(CopyOrNull(expression));
	}
	return result;
}

static DecideTerm CopyTerm(const DecideTerm &term) {
	DecideTerm result(term.variable_index, CopyOrNull(term.coefficient), term.sign);
	result.filter = CopyOrNull(term.filter);
	result.avg_scale = term.avg_scale;
	result.avg_group_key = term.avg_group_key;
	result.reduction = term.reduction;
	result.qualifier_scope_idx = term.qualifier_scope_idx;
	result.inputs = CopyAll(term.inputs);
	result.frame_read = term.frame_read;
	result.child = term.child;
	return result;
}

static vector<DecideTerm> CopyTerms(const vector<DecideTerm> &terms) {
	vector<DecideTerm> result;
	for (auto &term : terms) {
		result.push_back(CopyTerm(term));
	}
	return result;
}

static BilinearConstraintTerm CopyBilinear(const BilinearConstraintTerm &term) {
	BilinearConstraintTerm result;
	result.var_a = term.var_a;
	result.var_b = term.var_b;
	result.coefficient = CopyOrNull(term.coefficient);
	result.sign = term.sign;
	result.filter = CopyOrNull(term.filter);
	result.avg_scale = term.avg_scale;
	result.qualifier_scope_idx = term.qualifier_scope_idx;
	result.inputs = CopyAll(term.inputs);
	result.child = term.child;
	return result;
}

static DecideObjective::BilinearTerm CopyObjectiveBilinear(const DecideObjective::BilinearTerm &term) {
	DecideObjective::BilinearTerm result;
	result.var_a = term.var_a;
	result.var_b = term.var_b;
	result.coefficient = CopyOrNull(term.coefficient);
	result.sign = term.sign;
	result.filter = CopyOrNull(term.filter);
	result.avg_scale = term.avg_scale;
	result.avg_group_key = term.avg_group_key;
	result.qualifier_scope_idx = term.qualifier_scope_idx;
	result.reduction = term.reduction;
	result.inputs = CopyAll(term.inputs);
	result.child = term.child;
	return result;
}

static DecideConstraint::QuadraticGroup CopyQuadraticGroup(const DecideConstraint::QuadraticGroup &group) {
	DecideConstraint::QuadraticGroup result;
	result.inner_terms = CopyTerms(group.inner_terms);
	result.sign = group.sign;
	result.scale = CopyOrNull(group.scale);
	result.scale_divides = group.scale_divides;
	result.filter = CopyOrNull(group.filter);
	result.avg_scale = group.avg_scale;
	result.qualifier_scope_idx = group.qualifier_scope_idx;
	result.child = group.child;
	return result;
}

static unique_ptr<DecideConstraint> CopyConstraint(const DecideConstraint &clause) {
	auto result = make_uniq<DecideConstraint>();
	result->lhs_terms = CopyTerms(clause.lhs_terms);
	result->rhs_expr = CopyOrNull(clause.rhs_expr);
	result->tightest_frame_bound = clause.tightest_frame_bound;
	result->comparison_type = clause.comparison_type;
	result->lhs_is_aggregate = clause.lhs_is_aggregate;
	result->was_minmax_easy = clause.was_minmax_easy;
	result->minmax_clause_idx = clause.minmax_clause_idx;
	result->minmax_agg_type = clause.minmax_agg_type;
	result->ne_clause_idx = clause.ne_clause_idx;
	result->abs_aux_idx = clause.abs_aux_idx;
	result->abs_is_pos_bound = clause.abs_is_pos_bound;
	result->defined_var = clause.defined_var;
	result->scope.when = CopyOrNull(clause.scope.when);
	result->scope.per = clause.scope.per;
	result->scope.class_key = clause.scope.class_key;
	result->scope.signature = clause.scope.signature;
	result->group_key = clause.group_key;
	result->kind = clause.kind;
	result->source_clause_id = clause.source_clause_id;
	result->removal_group_id = clause.removal_group_id;
	result->admitted_lower = CopyOrNull(clause.admitted_lower);
	result->admitted_upper = CopyOrNull(clause.admitted_upper);
	result->admitted_a = CopyOrNull(clause.admitted_a);
	result->admitted_r = CopyOrNull(clause.admitted_r);
	result->admitted_member = CopyOrNull(clause.admitted_member);
	result->lifted_floor = clause.lifted_floor;
	result->lifted_floor_k = clause.lifted_floor_k;
	for (auto &term : clause.bilinear_terms) {
		result->bilinear_terms.push_back(CopyBilinear(term));
	}
	result->has_bilinear = clause.has_bilinear;
	for (auto &group : clause.quadratic_groups) {
		result->quadratic_groups.push_back(CopyQuadraticGroup(group));
	}
	result->has_quadratic = clause.has_quadratic;
	return result;
}

static unique_ptr<DecideObjective> CopyObjective(const DecideObjective &objective) {
	auto result = make_uniq<DecideObjective>();
	result->terms = CopyTerms(objective.terms);
	result->when_condition = CopyOrNull(objective.when_condition);
	result->squared_terms = CopyTerms(objective.squared_terms);
	result->has_quadratic = objective.has_quadratic;
	result->quadratic_sign = objective.quadratic_sign;
	for (auto &term : objective.bilinear_terms) {
		result->bilinear_terms.push_back(CopyObjectiveBilinear(term));
	}
	result->has_bilinear = objective.has_bilinear;
	return result;
}

} // namespace decide_tree_compile

idx_t DecideTreeOperand(const DecideTree &tree, idx_t reducer) {
	idx_t operand = DConstants::INVALID_INDEX;
	for (auto child : tree.nodes[reducer]->children) {
		if (tree.nodes[child]->kind != DecideNodeKind::SCOPE) {
			continue;
		}
		if (operand != DConstants::INVALID_INDEX) {
			throw InternalException("DECIDE tree: %s has two operands", tree.nodes[reducer]->text);
		}
		operand = child;
	}
	if (operand == DConstants::INVALID_INDEX) {
		throw InternalException("DECIDE tree: %s has no operand", tree.nodes[reducer]->text);
	}
	return operand;
}

idx_t DecideTreeMembersOf(const DecideTree &tree, idx_t reducer) {
	while (true) {
		const Expression *body = tree.nodes[DecideTreeOperand(tree, reducer)]->body.get();
		if (!body || body->GetExpressionClass() != ExpressionClass::BOUND_REF ||
		    body->Cast<BoundReferenceExpression>().index < tree.input_column_count) {
			return reducer;
		}
		const auto inner = body->Cast<BoundReferenceExpression>().index - tree.input_column_count;
		if (tree.nodes[inner]->kind != DecideNodeKind::REDUCER) {
			return reducer;
		}
		reducer = inner;
	}
}

namespace decide_tree_compile {

//! Where a term of `holder`'s body is computed: on the operand of the G it ranges over, else on the holder itself.
static DecideTermSite Site(const DecideTree &tree, idx_t holder, idx_t child) {
	DecideTermSite site;
	site.node = child == DConstants::INVALID_INDEX ? holder : DecideTreeOperand(tree, child);
	return site;
}

static void SiteClause(const DecideTree &tree, idx_t holder, DecideBody &body) {
	auto &clause = *body.clause;
	for (auto &term : clause.lhs_terms) {
		body.linear.push_back(Site(tree, holder, term.child));
	}
	for (auto &term : clause.bilinear_terms) {
		body.products.push_back(Site(tree, holder, term.child));
	}
	for (auto &group : clause.quadratic_groups) {
		body.squares.emplace_back();
		for (idx_t t = 0; t < group.inner_terms.size(); t++) {
			body.squares.back().push_back(Site(tree, holder, group.child));
		}
	}
}

static void SiteObjective(const DecideTree &tree, idx_t holder, DecideBody &body) {
	auto &objective = *body.objective;
	for (auto &term : objective.terms) {
		body.linear.push_back(Site(tree, holder, term.child));
	}
	for (auto &term : objective.bilinear_terms) {
		body.products.push_back(Site(tree, holder, term.child));
	}
	body.squares.emplace_back();
	for (auto &term : objective.squared_terms) {
		body.squares.back().push_back(Site(tree, holder, term.child));
	}
}

//! A G's or an O's members: computed on the G's operand, or on the O, which holds a frame's positions.
static void SiteMembers(const DecideTree &tree, idx_t holder, DecideBody &body) {
	const auto site = tree.nodes[holder]->kind == DecideNodeKind::FRAME ? holder : DecideTreeOperand(tree, holder);
	body.linear.assign(body.members.size(), DecideTermSite {site});
	body.fallback_sites.assign(body.fallback.size(), DecideTermSite {site});
}

//! The clause roots in the order stage 05 lowers the clauses: a folded bound with no repair rows has no clause, one
//! with them is a singleton row, every other root is a constraint.
static void BindClauses(const DecidePreparedModel &prepared, DecideTree &tree, const vector<idx_t> &roots) {
	idx_t constraint = 0;
	idx_t singleton = 0;
	for (auto root : roots) {
		auto &node = *tree.nodes[root];
		if (node.folded && !node.repair_rows) {
			continue;
		}
		const bool folded = node.folded;
		auto &list = folded ? prepared.singleton_row_constraints : prepared.constraints;
		const auto index = folded ? singleton++ : constraint++;
		if (index >= list.size() || list[index]->source_clause_id != node.source_clause_id ||
		    list[index]->comparison_type != node.comparison_type) {
			throw InternalException("DECIDE tree: root %llu is not stage 05's clause %llu", root, index);
		}
		auto &body = node.compiled;
		body.clause = CopyConstraint(*list[index]);
		body.clause_index = folded ? prepared.constraints.size() + index : index;
		body.phase = DecideErrorPhase::CONSTRAINTS;
		body.order = body.clause_index;
		SiteClause(tree, root, body);
	}
	if (constraint != prepared.constraints.size() || singleton != prepared.singleton_row_constraints.size()) {
		throw InternalException("DECIDE tree: %llu clause roots bound to %llu clauses and %llu singleton rows",
		                        roots.size(), prepared.constraints.size(), prepared.singleton_row_constraints.size());
	}
}

//! The composed clauses, which flat evaluates after the objective, then the objective's composed terms (the last
//! slot): each term's G holds its inner terms.
static void CopyComposed(const LogicalDecide &op, DecideTree &tree, const DecideTreeSources &sources) {
	for (idx_t spec = 0; spec < sources.composed_reducers.size(); spec++) {
		auto &terms = spec < op.composed_minmax_constraints.size() ? op.composed_minmax_constraints[spec].terms
		                                                           : op.composed_minmax_objective_terms;
		auto &reducers = sources.composed_reducers[spec];
		for (idx_t t = 0; t < reducers.size(); t++) {
			auto &body = tree.nodes[reducers[t]]->compiled;
			body.members = CopyTerms(terms[t].inner_terms);
			body.phase = DecideErrorPhase::COMPOSED;
			body.order = spec;
			SiteMembers(tree, reducers[t], body);
		}
	}
	for (idx_t spec = 0; spec < sources.composed_roots.size(); spec++) {
		auto &body = tree.nodes[sources.composed_roots[spec]]->compiled;
		body.phase = DecideErrorPhase::COMPOSED;
		body.order = spec;
	}
}

//! Each helper's G or O holds its reducer's members and a frame's else terms.
static void CopyLinks(const LogicalDecide &op, DecideTree &tree, const vector<idx_t> &link_nodes) {
	for (idx_t link = 0; link < link_nodes.size(); link++) {
		auto &spec = op.reducer_value_links[link];
		auto &body = tree.nodes[link_nodes[link]]->compiled;
		body.members = CopyTerms(spec.inner_terms);
		body.fallback = CopyTerms(spec.fallback_terms);
		body.phase = DecideErrorPhase::VALUE_LINKS;
		body.order = link;
		SiteMembers(tree, link_nodes[link], body);
	}
}

//! Compiles stage 05's expressions into programs over a node's layout, as DuckDB's ColumnBindingResolver turns a plan's
//! column references into chunk positions: by flat's own rules (ToChunkReferences), with each leaf mapped to the
//! layout. A read the tree computes no value for yet becomes a NULL of its type and names the reason on the body.
class DecideProgramCompiler {
public:
	DecideProgramCompiler(ClientContext &context, const LogicalDecide &op, DecideTree &tree)
	    : context(context), op(op), tree(tree), first_terms(tree.nodes.size(), DConstants::INVALID_INDEX) {
	}

	//! A program on `site` computing `expression`, ranked with `owner`'s body; returns its index on the site.
	idx_t Add(idx_t site, DecideProgramRole role, const Expression &expression, idx_t owner, idx_t stage, idx_t group,
	          idx_t position, int sign = 1) {
		auto &owner_body = tree.nodes[owner]->compiled;
		DecideProgram program;
		program.role = role;
		program.expression = Translate(site, expression, owner_body);
		program.written = expression.Copy();
		program.sign = sign;
		program.owner = owner;
		program.stage = stage;
		program.group = group;
		program.position = position;
		auto &programs = tree.nodes[site]->programs;
		programs.push_back(std::move(program));
		return programs.size() - 1;
	}

	//! A term's coefficient and its inputs on its site.
	void AddTerm(const DecideTerm &term, DecideTermSite &site, idx_t owner, idx_t stage, idx_t group, idx_t position) {
		site.program =
		    Add(site.node, DecideProgramRole::COEFFICIENT, *term.coefficient, owner, stage, group, position, term.sign);
		AddInputs(term.inputs, site, owner, stage, group, position);
		first_terms[site.node] = MinValue(first_terms[site.node], position);
	}

	//! A product's coefficient, none when stage 05 left it out (1), and its inputs.
	template <class PRODUCT>
	void AddProduct(const PRODUCT &term, DecideTermSite &site, idx_t owner, idx_t position) {
		if (term.coefficient) {
			site.program =
			    Add(site.node, DecideProgramRole::COEFFICIENT, *term.coefficient, owner, 3, 0, position, term.sign);
		}
		AddInputs(term.inputs, site, owner, 3, 0, position);
	}

	vector<DecideProgram> &Programs(idx_t node) {
		return tree.nodes[node]->programs;
	}

	//! A node's member tests, over its own carried columns, once every term is compiled. Flat reads them in their
	//! reducer's filter, which it reads for each of its owner's terms in their order, window by window, before any
	//! coefficient: so a test's group is the first term its node holds, and its position its place among the tests.
	void AddTests(idx_t index) {
		auto &node = *tree.nodes[index];
		for (idx_t i = 0; i < node.member_tests.size(); i++) {
			DecideProgram program;
			program.role = DecideProgramRole::TEST;
			program.expression = TranslateTest(index, *node.member_tests[i]);
			program.written = node.member_tests[i]->Copy();
			program.group = first_terms[index];
			program.position = i;
			node.tests.push_back(node.programs.size());
			node.programs.push_back(std::move(program));
		}
	}

private:
	ClientContext &context;
	const LogicalDecide &op;
	DecideTree &tree;
	//! Per node, the place among its owner's terms of the first term it holds; INVALID while it holds none.
	vector<idx_t> first_terms;

	void AddInputs(const vector<unique_ptr<Expression>> &inputs, DecideTermSite &site, idx_t owner, idx_t stage,
	               idx_t group, idx_t position) {
		for (auto &input : inputs) {
			site.inputs.push_back(Add(site.node, DecideProgramRole::VALUE, *input, owner, stage, group, position));
		}
	}

	static unique_ptr<Expression> Uncompiled(DecideBody &body, const char *reason, const LogicalType &type) {
		if (body.uncompiled.empty()) {
			body.uncompiled = reason;
		}
		return make_uniq<BoundConstantExpression>(Value(type));
	}

	//! A source column's place in `site`'s layout: among its carried columns, else among its program columns, added.
	idx_t Column(idx_t site, const LogicalType &type, idx_t column) {
		auto &node = *tree.nodes[site];
		const BoundReferenceExpression read(type, column);
		for (idx_t i = 0; i < node.columns.size(); i++) {
			if (node.columns[i]->Equals(read)) {
				return i;
			}
		}
		for (idx_t i = 0; i < node.program_columns.size(); i++) {
			if (node.program_columns[i]->Equals(read)) {
				return node.columns.size() + i;
			}
		}
		node.program_columns.push_back(read.Copy());
		return node.columns.size() + node.program_columns.size() - 1;
	}

	//! Stage 05's `expression` over `site`'s layout. Stage 05 reads the source as the operator's input columns, a data
	//! reducer as its record's placeholder, a frame as a column after the input's, and an admitted end its inputs a and
	//! r as DecideAdmittedInput.
	unique_ptr<Expression> Translate(idx_t site, const Expression &expression, DecideBody &body) {
		DecideChunkLeaf leaf = [&](const Expression &leaf_expression) -> unique_ptr<Expression> {
			const auto kind = leaf_expression.GetExpressionClass();
			if (kind != ExpressionClass::BOUND_REF && kind != ExpressionClass::BOUND_COLUMN_REF) {
				return nullptr;
			}
			auto &type = leaf_expression.return_type;
			for (auto &retained : op.retained_bound_aggregates) {
				if (retained.placeholder->Equals(leaf_expression)) {
					return Uncompiled(body, "reads a data reducer", type);
				}
			}
			if (kind == ExpressionClass::BOUND_REF) {
				const auto column = leaf_expression.Cast<BoundReferenceExpression>().index;
				if (column >= op.prepared.frame_column_base) {
					return Uncompiled(body, "reads a frame", type);
				}
				return make_uniq<BoundReferenceExpression>(type, Column(site, type, column));
			}
			auto &binding = leaf_expression.Cast<BoundColumnRefExpression>().binding;
			if (binding.table_index == DConstants::INVALID_INDEX) {
				// An admitted end's input, bound once the node's layout is known (BindAdmittedInputs).
				return leaf_expression.Copy();
			}
			if (binding.table_index == op.decide_index) {
				throw InternalException("DECIDE tree: a coefficient reads a decision: %s", leaf_expression.ToString());
			}
			throw InternalException("DECIDE tree: a coefficient reads an unresolved column: %s",
			                        leaf_expression.ToString());
		};
		return ToChunkReferences(expression, context, leaf);
	}

	//! A member test, which Lower already read in the tree's terms: a source column among its node's carried ones.
	unique_ptr<Expression> TranslateTest(idx_t index, const Expression &test) {
		auto &node = *tree.nodes[index];
		DecideChunkLeaf leaf = [&](const Expression &leaf_expression) -> unique_ptr<Expression> {
			auto &type = leaf_expression.return_type;
			if (leaf_expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				// A generated NULL test reads data: a reducer's own (DecideCanonicalizer::SkipNullData) or a key's.
				throw InternalException("DECIDE tree: a member test of %s reads a decision", node.text);
			}
			if (leaf_expression.GetExpressionClass() != ExpressionClass::BOUND_REF) {
				return nullptr;
			}
			const auto column = leaf_expression.Cast<BoundReferenceExpression>().index;
			if (column >= tree.input_column_count) {
				return Uncompiled(node.compiled, "a member test reads a child", type);
			}
			for (idx_t i = 0; i < node.columns.size(); i++) {
				if (node.columns[i]->Equals(leaf_expression)) {
					return make_uniq<BoundReferenceExpression>(type, i);
				}
			}
			throw InternalException("DECIDE tree: a member test of %s reads a column its node does not carry",
			                        node.text);
		};
		return ToChunkReferences(test, context, leaf);
	}
};

//! A clause root's programs: its terms, its bound and admitted ends, its products and squares.
static void CompileClause(DecideProgramCompiler &compiler, idx_t root, DecideBody &body) {
	auto &clause = *body.clause;
	for (idx_t t = 0; t < clause.lhs_terms.size(); t++) {
		compiler.AddTerm(clause.lhs_terms[t], body.linear[t], root, 1, 0, t);
	}
	// A constant bound is a program too: it folds where it is executed.
	const auto role = clause.tightest_frame_bound ? DecideProgramRole::TIGHTEST_BOUND : DecideProgramRole::BOUND;
	body.rhs = compiler.Add(root, role, *clause.rhs_expr, root, 2, 0, 0);
	if (clause.admitted_lower) {
		// A bound on one decision, admitted on each row its term is read on: an easy extremum's on its members. As
		// flat computes them: the members, then a and r on them, then each end over the row and those two.
		const auto site = body.linear[0].node;
		if (clause.admitted_member) {
			body.admits = compiler.Add(site, DecideProgramRole::ADMITS, *clause.admitted_member, root, 2, 0, 1);
		}
		const auto a = compiler.Add(site, DecideProgramRole::VALUE, *clause.admitted_a, root, 2, 0, 2);
		const auto r = compiler.Add(site, DecideProgramRole::VALUE, *clause.admitted_r, root, 2, 0, 3);
		body.lower = compiler.Add(site, DecideProgramRole::BOUND, *clause.admitted_lower, root, 2, 0, 4);
		body.upper = compiler.Add(site, DecideProgramRole::BOUND, *clause.admitted_upper, root, 2, 0, 5);
		auto &programs = compiler.Programs(site);
		for (auto end : {body.lower, body.upper}) {
			programs[end].written = clause.rhs_expr->Copy();
			programs[end].arguments = {a, r};
		}
	}
	for (idx_t i = 0; i < clause.bilinear_terms.size(); i++) {
		compiler.AddProduct(clause.bilinear_terms[i], body.products[i], root, i);
	}
	for (idx_t g = 0; g < clause.quadratic_groups.size(); g++) {
		auto &group = clause.quadratic_groups[g];
		body.square_scales.push_back(group.scale
		                                 ? compiler.Add(root, DecideProgramRole::VALUE, *group.scale, root, 4, g, 0)
		                                 : DConstants::INVALID_INDEX);
		for (idx_t t = 0; t < group.inner_terms.size(); t++) {
			compiler.AddTerm(group.inner_terms[t], body.squares[g][t], root, 4, g, 1 + t);
		}
	}
}

//! The objective's programs: its terms then its squared terms, then its products.
static void CompileObjective(DecideProgramCompiler &compiler, idx_t root, DecideBody &body) {
	auto &objective = *body.objective;
	for (idx_t t = 0; t < objective.terms.size(); t++) {
		compiler.AddTerm(objective.terms[t], body.linear[t], root, 1, 0, t);
	}
	for (idx_t t = 0; t < objective.squared_terms.size(); t++) {
		compiler.AddTerm(objective.squared_terms[t], body.squares[0][t], root, 1, 0, objective.terms.size() + t);
	}
	for (idx_t i = 0; i < objective.bilinear_terms.size(); i++) {
		compiler.AddProduct(objective.bilinear_terms[i], body.products[i], root, i);
	}
}

//! A composed term's or a helper's members, then a frame helper's else terms.
static void CompileMembers(DecideProgramCompiler &compiler, idx_t holder, DecideBody &body) {
	for (idx_t t = 0; t < body.members.size(); t++) {
		compiler.AddTerm(body.members[t], body.linear[t], holder, 1, 0, t);
	}
	for (idx_t t = 0; t < body.fallback.size(); t++) {
		compiler.AddTerm(body.fallback[t], body.fallback_sites[t], holder, 1, 0, body.members.size() + t);
	}
}

static void CompilePrograms(ClientContext &context, const LogicalDecide &op, DecideTree &tree) {
	DecideProgramCompiler compiler(context, op, tree);
	for (idx_t i = 0; i < tree.nodes.size(); i++) {
		auto &body = tree.nodes[i]->compiled;
		if (body.clause) {
			CompileClause(compiler, i, body);
		} else if (body.objective) {
			CompileObjective(compiler, i, body);
		} else {
			CompileMembers(compiler, i, body);
		}
	}
	for (idx_t i = 0; i < tree.nodes.size(); i++) {
		compiler.AddTests(i);
	}
}

//! An admitted end reads its inputs a and r after its node's layout, which is known once every program on the node is
//! compiled: they are bound there as flat binds them after its chunk's columns (DecideBindAdmittedInputs). A program
//! with no inputs is left as it is.
static void BindAdmittedInputs(DecideTree &tree) {
	for (auto &node : tree.nodes) {
		const auto width = node->columns.size() + node->program_columns.size();
		const BoundReferenceExpression a(LogicalType::DOUBLE, width);
		const BoundReferenceExpression r(LogicalType::DOUBLE, width + 1);
		for (auto &program : node->programs) {
			program.expression = DecideBindAdmittedInputs(*program.expression, a, r);
		}
	}
}

} // namespace decide_tree_compile

void CompileDecideTree(ClientContext &context, const LogicalDecide &op, DecideTree &tree,
                       const DecideTreeSources &sources) {
	decide_tree_compile::BindClauses(op.prepared, tree, sources.clause_roots);
	if (op.prepared.objective) {
		auto &body = tree.nodes[sources.objective_root]->compiled;
		body.objective = decide_tree_compile::CopyObjective(*op.prepared.objective);
		body.phase = DecideErrorPhase::OBJECTIVE;
		body.order = 0;
		decide_tree_compile::SiteObjective(tree, sources.objective_root, body);
	}
	decide_tree_compile::CopyComposed(op, tree, sources);
	decide_tree_compile::CopyLinks(op, tree, sources.link_nodes);
	decide_tree_compile::CompilePrograms(context, op, tree);
	decide_tree_compile::BindAdmittedInputs(tree);
}

namespace decide_tree_compile {

static string Signed(idx_t value) {
	return value == DConstants::INVALID_INDEX ? "-1" : to_string(value);
}

static string PhaseName(DecideErrorPhase phase) {
	switch (phase) {
	case DecideErrorPhase::CONSTRAINTS:
		return "constraints";
	case DecideErrorPhase::OBJECTIVE:
		return "objective";
	case DecideErrorPhase::COMPOSED:
		return "composed";
	case DecideErrorPhase::VALUE_LINKS:
		return "links";
	default:
		return "other";
	}
}

static string RoleName(DecideProgramRole role) {
	switch (role) {
	case DecideProgramRole::COEFFICIENT:
		return "COEFFICIENT";
	case DecideProgramRole::BOUND:
		return "BOUND";
	case DecideProgramRole::TIGHTEST_BOUND:
		return "TIGHTEST_BOUND";
	case DecideProgramRole::TEST:
		return "TEST";
	case DecideProgramRole::ADMITS:
		return "ADMITS";
	case DecideProgramRole::VALUE:
		return "VALUE";
	}
	throw InternalException("DECIDE tree: an unknown program role");
}

static string Label(const unique_ptr<Expression> &coefficient) {
	return coefficient ? coefficient->GetName() : string();
}

static void Terms(std::ostringstream &out, const vector<DecideTerm> &terms, const vector<DecideTermSite> &sites,
                  const string &indent, const char *kind) {
	for (idx_t t = 0; t < terms.size(); t++) {
		out << indent << kind << " " << t << " var=" << Signed(terms[t].variable_index)
		    << " child=" << Signed(terms[t].child) << " site=" << Signed(sites[t].node)
		    << " program=" << Signed(sites[t].program) << " label=" << Label(terms[t].coefficient) << "\n";
	}
}

template <class PRODUCT>
static void Products(std::ostringstream &out, const vector<PRODUCT> &terms, const vector<DecideTermSite> &sites) {
	for (idx_t t = 0; t < terms.size(); t++) {
		out << "  product " << t << " vars=" << terms[t].var_a << "," << terms[t].var_b
		    << " child=" << Signed(terms[t].child) << " site=" << Signed(sites[t].node)
		    << " program=" << Signed(sites[t].program) << "\n";
	}
}

//! What a node's block holds: a clause, the objective, a composed clause, a G's members, or entries with programs.
static string Head(const DecideNode &node) {
	auto &body = node.compiled;
	string head = body.clause                               ? "clause " + to_string(body.clause_index)
	              : body.objective                          ? "objective"
	              : body.phase == DecideErrorPhase::INVALID ? "entries"
	              : node.root                               ? "composed " + to_string(body.order)
	                                                        : "members";
	if (body.phase != DecideErrorPhase::INVALID) {
		head += " rank=" + PhaseName(body.phase) + ":" + to_string(body.order);
	}
	return head;
}

static void ClauseLines(std::ostringstream &out, const DecideConstraint &clause, const DecideBody &body) {
	Terms(out, clause.lhs_terms, body.linear, "  ", "term");
	Products(out, clause.bilinear_terms, body.products);
	for (idx_t g = 0; g < clause.quadratic_groups.size(); g++) {
		auto &group = clause.quadratic_groups[g];
		out << "  square " << g << " child=" << Signed(group.child) << " sign=" << group.sign
		    << " scale=" << Signed(body.square_scales[g]) << "\n";
		Terms(out, group.inner_terms, body.squares[g], "    ", "inner");
	}
	out << "  bound rhs=" << Signed(body.rhs) << " admits=" << Signed(body.admits) << " lower=" << Signed(body.lower)
	    << " upper=" << Signed(body.upper) << "\n";
}

static void ObjectiveLines(std::ostringstream &out, const DecideObjective &objective, const DecideBody &body) {
	Terms(out, objective.terms, body.linear, "  ", "term");
	Products(out, objective.bilinear_terms, body.products);
	if (!objective.squared_terms.empty()) {
		out << "  square 0 sign=" << objective.quadratic_sign << "\n";
		Terms(out, objective.squared_terms, body.squares[0], "    ", "inner");
	}
}

//! A program as it computes: an alias would print in place of the subtree it names.
static string Computed(const Expression &expression) {
	auto copy = expression.Copy();
	ExpressionIterator::EnumerateExpression(copy, [](Expression &child) { child.alias.clear(); });
	return copy->ToString();
}

static void ProgramLines(std::ostringstream &out, const DecideNode &node) {
	for (idx_t c = 0; c < node.program_columns.size(); c++) {
		out << "  column #" << node.columns.size() + c << " reads input "
		    << node.program_columns[c]->Cast<BoundReferenceExpression>().index << "\n";
	}
	for (idx_t p = 0; p < node.programs.size(); p++) {
		auto &program = node.programs[p];
		out << "  program " << p << " " << RoleName(program.role) << " expr=" << Computed(*program.expression)
		    << " written=" << program.written->ToString() << " stage=" << program.stage << " group=" << program.group
		    << " position=" << program.position << " owner=" << Signed(program.owner);
		for (idx_t a = 0; a < program.arguments.size(); a++) {
			out << (a ? "," : " arguments=") << program.arguments[a];
		}
		out << "\n";
	}
}

} // namespace decide_tree_compile

string DecideTree::BodiesToString() const {
	std::ostringstream out;
	for (idx_t i = 0; i < nodes.size(); i++) {
		auto &node = *nodes[i];
		auto &body = node.compiled;
		if (body.phase == DecideErrorPhase::INVALID && node.programs.empty() && body.uncompiled.empty()) {
			continue;
		}
		out << "node " << i << " " << decide_tree_compile::Head(node) << "\n";
		if (body.clause) {
			decide_tree_compile::ClauseLines(out, *body.clause, body);
		}
		if (body.objective) {
			decide_tree_compile::ObjectiveLines(out, *body.objective, body);
		}
		decide_tree_compile::Terms(out, body.members, body.linear, "  ", "term");
		if (!body.fallback.empty()) {
			out << "  fallback\n";
			decide_tree_compile::Terms(out, body.fallback, body.fallback_sites, "    ", "term");
		}
		decide_tree_compile::ProgramLines(out, node);
		if (!body.uncompiled.empty()) {
			out << "  uncompiled: " << body.uncompiled << "\n";
		}
	}
	return out.str();
}

} // namespace duckdb
