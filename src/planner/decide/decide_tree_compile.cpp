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
#include "duckdb/planner/expression/bound_reference_expression.hpp"

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

} // namespace decide_tree_compile

void CompileDecideTree(const LogicalDecide &op, DecideTree &tree, const DecideTreeSources &sources) {
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

static string Label(const unique_ptr<Expression> &coefficient) {
	return coefficient ? coefficient->GetName() : string();
}

static void Terms(std::ostringstream &out, const vector<DecideTerm> &terms, const vector<DecideTermSite> &sites,
                  const string &indent, const char *kind) {
	for (idx_t t = 0; t < terms.size(); t++) {
		out << indent << kind << " " << t << " var=" << Signed(terms[t].variable_index)
		    << " child=" << Signed(terms[t].child) << " site=" << Signed(sites[t].node)
		    << " label=" << Label(terms[t].coefficient) << "\n";
	}
}

template <class PRODUCT>
static void Products(std::ostringstream &out, const vector<PRODUCT> &terms, const vector<DecideTermSite> &sites) {
	for (idx_t t = 0; t < terms.size(); t++) {
		out << "  product " << t << " vars=" << terms[t].var_a << "," << terms[t].var_b
		    << " child=" << Signed(terms[t].child) << " site=" << Signed(sites[t].node) << "\n";
	}
}

} // namespace decide_tree_compile

string DecideTree::BodiesToString() const {
	std::ostringstream out;
	for (idx_t i = 0; i < nodes.size(); i++) {
		auto &body = nodes[i]->compiled;
		if (body.phase == DecideErrorPhase::INVALID) {
			continue;
		}
		out << "node " << i << " ";
		if (body.clause) {
			out << "clause " << body.clause_index;
		} else if (body.objective) {
			out << "objective";
		} else if (nodes[i]->root) {
			out << "composed " << body.order;
		} else {
			out << "members";
		}
		out << " rank=" << decide_tree_compile::PhaseName(body.phase) << ":" << body.order << "\n";
		if (body.clause) {
			auto &clause = *body.clause;
			decide_tree_compile::Terms(out, clause.lhs_terms, body.linear, "  ", "term");
			decide_tree_compile::Products(out, clause.bilinear_terms, body.products);
			for (idx_t g = 0; g < clause.quadratic_groups.size(); g++) {
				auto &group = clause.quadratic_groups[g];
				out << "  square " << g << " child=" << decide_tree_compile::Signed(group.child)
				    << " sign=" << group.sign << "\n";
				decide_tree_compile::Terms(out, group.inner_terms, body.squares[g], "    ", "inner");
			}
		}
		if (body.objective) {
			auto &objective = *body.objective;
			decide_tree_compile::Terms(out, objective.terms, body.linear, "  ", "term");
			decide_tree_compile::Products(out, objective.bilinear_terms, body.products);
			if (!objective.squared_terms.empty()) {
				out << "  square 0 sign=" << objective.quadratic_sign << "\n";
				decide_tree_compile::Terms(out, objective.squared_terms, body.squares[0], "    ", "inner");
			}
		}
		decide_tree_compile::Terms(out, body.members, body.linear, "  ", "term");
		if (!body.fallback.empty()) {
			out << "  fallback\n";
			decide_tree_compile::Terms(out, body.fallback, body.fallback_sites, "    ", "term");
		}
	}
	return out.str();
}

} // namespace duckdb
