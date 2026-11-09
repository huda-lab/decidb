//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_model_emitter.cpp
//
// The tree's containers written as stage 06 reads them (decide_model_emitter.hpp).
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_model_emitter.hpp"

#include "duckdb/execution/operator/decide/decide_global_sink_state.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"

#include <limits>

namespace duckdb {

static const char *const DECIDE_MODEL_SHAPE_NAMES[] = {
    "ROWS",           "REPAIR_ROWS",   "KEYED_DECLARED", "PRODUCT_TERMS", "SQUARE_TERMS", "TIGHTEST_BOUND",
    "NOT_EQUAL_ROWS", "DEFINED_READS", "EXTREMUM_ROWS",  "MEMBER_ROWS",   "GROUP_ROWS",   "CLASS_ROWS",
    "OBJECTIVE",      "COMPOSED",      "VALUE_LINKS",    "FRAME_READS",   "PRODUCT_LINKS"};

//! The shapes of a clause beyond its rows, from what stage 06 reads of it (EvaluatedConstraint): its products and
//! squares, what its bound and rows read, then how its rows are made.
static void ClauseShapes(const DecideConstraint &clause, vector<DecideModelShape> &shapes) {
	if (clause.has_bilinear) {
		shapes.push_back(DecideModelShape::PRODUCT_TERMS);
	}
	if (clause.has_quadratic) {
		shapes.push_back(DecideModelShape::SQUARE_TERMS);
	}
	if (clause.tightest_frame_bound) {
		shapes.push_back(DecideModelShape::TIGHTEST_BOUND);
	}
	if (clause.ne_clause_idx != DConstants::INVALID_INDEX) {
		shapes.push_back(DecideModelShape::NOT_EQUAL_ROWS);
	}
	// A row defining an auxiliary leaves it with no value where its reads have none (SolverInput::absent_reads).
	if (clause.defined_var != DConstants::INVALID_INDEX) {
		shapes.push_back(DecideModelShape::DEFINED_READS);
	}
	if (clause.minmax_clause_idx != DConstants::INVALID_INDEX) {
		shapes.push_back(DecideModelShape::EXTREMUM_ROWS);
	}
	if (clause.was_minmax_easy) {
		shapes.push_back(DecideModelShape::MEMBER_ROWS);
	}
	if (clause.lhs_is_aggregate) {
		shapes.push_back(DecideModelShape::GROUP_ROWS);
	}
	if (clause.scope.per == DecideClauseScope::PerRole::ONE_ROW_PER_CLASS) {
		shapes.push_back(DecideModelShape::CLASS_ROWS);
	}
}

//! The shapes the query has beyond rows and declared classes: each clause's, then what the query hands stage 06 beside
//! its clauses. A helper's classes reach stage 06 with its link, and a relation-qualified reducer's with a shape of its
//! clause or of the objective.
static vector<DecideModelShape> QueryShapes(const DecideTree &tree, const PhysicalDecide &op) {
	vector<DecideModelShape> shapes;
	for (auto root : tree.roots) {
		auto &body = tree.nodes[root]->compiled;
		if (body.clause) {
			ClauseShapes(*body.clause, shapes);
		}
	}
	if (op.decide_objective) {
		shapes.push_back(DecideModelShape::OBJECTIVE);
	}
	if (!op.composed_minmax_constraints.empty()) {
		shapes.push_back(DecideModelShape::COMPOSED);
	}
	if (!op.reducer_value_links.empty()) {
		shapes.push_back(DecideModelShape::VALUE_LINKS);
	}
	if (!op.frames.empty()) {
		shapes.push_back(DecideModelShape::FRAME_READS);
	}
	// A product with a BOOLEAN factor the optimizer made an auxiliary of (LogicalDecide::BilinearLink), whose rows
	// stage 06 writes and whose absence its factors' make.
	if (!op.bilinear_links.empty()) {
		shapes.push_back(DecideModelShape::PRODUCT_LINKS);
	}
	return shapes;
}

string DecideModelEmitter::Uncovered(const DecideTree &tree, const PhysicalDecide &op) {
	auto shapes = QueryShapes(tree, op);
	return shapes.empty() ? string() : DECIDE_MODEL_SHAPE_NAMES[static_cast<idx_t>(shapes[0])];
}

DecideModelEmitter::DecideModelEmitter(const PhysicalDecide &op, const DecideTree &tree, const DecideDescent &descent,
                                       DecideTreeColumns &columns, DecideGlobalSinkState &gstate,
                                       const DecideSourceNames &names, idx_t num_rows)
    : op(op), tree(tree), descent(descent), columns(columns), gstate(gstate), names(names), num_rows(num_rows) {
}

vector<KeyedMapping> DecideDeclaredMappings(const DecideDescent &descent) {
	auto &groups = descent.DecisionGroups();
	auto &rows = descent.DecisionRows();
	vector<KeyedMapping> mappings(groups.size());
	for (idx_t scope = 0; scope < groups.size(); scope++) {
		auto &mapping = mappings[scope];
		mapping.num_classes = groups[scope].size();
		mapping.row_to_class = rows[scope];
		for (auto &group : mapping.row_to_class) {
			if (group == DConstants::INVALID_INDEX) {
				if (mapping.absent_class == DConstants::INVALID_INDEX) {
					mapping.absent_class = mapping.num_classes++;
				}
				group = mapping.absent_class;
			} else {
				group = groups[scope][group].rank;
			}
		}
	}
	return mappings;
}

vector<KeyedMapping> DecideModelEmitter::KeyedMappings() {
	// No covered query reads an auxiliary, so no decision has an absence beyond its class.
	gstate.absent_reads.assign(op.decide_variables.size(), vector<bool>());
	return DecideDeclaredMappings(descent);
}

void DecideModelEmitter::Constraints() {
	auto &prepared = op.prepared;
	const idx_t num_constraints = prepared.constraints.size();
	vector<idx_t> roots(num_constraints + prepared.singleton_row_constraints.size(), DConstants::INVALID_INDEX);
	for (auto root : tree.roots) {
		auto &body = tree.nodes[root]->compiled;
		if (body.clause) {
			roots[body.clause_index] = root;
		}
	}
	// The folded bounds follow under DIAGNOSE, numbered after every other clause: only the repair model emits them.
	const idx_t num_evaluated = num_constraints + (op.diagnose ? prepared.singleton_row_constraints.size() : 0);
	for (idx_t c = 0; c < num_evaluated; c++) {
		auto evaluated = Rows(roots[c], c < num_constraints);
		(c < num_constraints ? gstate.evaluated_constraints : gstate.evaluated_singleton_row_constraints)
		    .push_back(std::move(evaluated));
	}
}

EvaluatedConstraint DecideModelEmitter::Rows(idx_t root, bool counted) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	auto &entries = descent.Containers()[root].entries;
	auto evaluated = InitializeEvaluatedConstraint(clause, body.clause_index,
	                                               [&](const DecideTerm &term) { return gstate.ReadVariable(term); });
	// The rows the clause keeps are its root's entries, which its `when` routed; one model row stands on each.
	vector<bool> nulls(entries.size(), false);
	auto scatter = [&](const vector<double> &numbers, CoefficientColumn &column) {
		column.AssignDense(num_rows);
		for (idx_t e = 0; e < entries.size(); e++) {
			column.Set(entries[e].row, numbers[e]);
		}
	};
	for (idx_t t = 0; t < clause.lhs_terms.size(); t++) {
		auto &site = body.linear[t];
		D_ASSERT(site.node == root);
		scatter(columns.Numbers(root, site.program, nulls, &site, &clause.lhs_terms[t].inputs),
		        evaluated.row_coefficients[t]);
	}
	if (clause.scope.when) {
		evaluated.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
		for (auto &entry : entries) {
			evaluated.row_group_ids[entry.row] = 0;
		}
		evaluated.num_groups = 1;
	}
	const auto instances = CountInstances(evaluated.row_group_ids, evaluated.num_groups, num_rows, false);
	if (DescribeBound(clause, num_rows, names, op.source_fragments, op.keyed_scopes, op.keys, evaluated)) {
		scatter(columns.Numbers(root, body.rhs, nulls), evaluated.rhs_values);
	}
	if (clause.admitted_lower) {
		// A bound on one decision admits a range of its values on each row, which the model builder folds into the
		// decision's column. Only an easy extremum's is computed on some rows alone
		// (DecideConstraint::admitted_member).
		D_ASSERT(body.admits == DConstants::INVALID_INDEX);
		scatter(columns.Numbers(root, body.lower, nulls), evaluated.admitted_lower);
		scatter(columns.Numbers(root, body.upper, nulls), evaluated.admitted_upper);
	}
	// A term reading a decision with no instance at the row has no value there (D84).
	for (auto &term : clause.lhs_terms) {
		auto absent = columns.Absent(root, term.variable_index);
		for (idx_t e = 0; e < entries.size(); e++) {
			nulls[e] = nulls[e] || absent[e];
		}
	}
	vector<bool> null_rows(num_rows, false);
	for (idx_t e = 0; e < entries.size(); e++) {
		null_rows[entries[e].row] = nulls[e];
	}
	SkipNullInstances(null_rows, false, num_rows, evaluated.row_group_ids, evaluated.num_groups);
	if (counted) {
		gstate.CountInstances(evaluated.source_clause_id, instances,
		                      CountInstances(evaluated.row_group_ids, evaluated.num_groups, num_rows, false));
	}
	return evaluated;
}

} // namespace duckdb
