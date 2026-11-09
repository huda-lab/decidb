//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_model_emitter.cpp
//
// The tree's containers written as stage 06 reads them (decide_model_emitter.hpp).
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_model_emitter.hpp"

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/operator/decide/decide_global_sink_state.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"

#include <algorithm>
#include <limits>

namespace duckdb {

static const char *const DECIDE_MODEL_SHAPE_NAMES[] = {
    "ROWS",
    "REPAIR_ROWS",
    "KEYED_DECLARED",
    "OBJECTIVE_LINEAR",
    "PRODUCT_TERMS",
    "SQUARE_TERMS",
    "TIGHTEST_BOUND",
    "NOT_EQUAL_ROWS",
    "DEFINED_READS",
    "EXTREMUM_ROWS",
    "MEMBER_ROWS",
    "GROUP_ROWS",
    "CLASS_ROWS",
    "OBJECTIVE_SQUARES",
    "OBJECTIVE_PRODUCTS",
    "OBJECTIVE_EXTREMUM",
    "OBJECTIVE_COMPOSED",
    "COMPOSED",
    "VALUE_LINKS",
    "FRAME_READS",
    "PRODUCT_LINKS",
    "KEYED_SIGNATURE",
};

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

//! The shapes the query has beyond rows, declared classes and a linear objective: each clause's, the objective's, then
//! what the query hands stage 06 beside its clauses. A helper's classes reach stage 06 with its link.
static vector<DecideModelShape> QueryShapes(const DecideTree &tree, const PhysicalDecide &op) {
	vector<DecideModelShape> shapes;
	for (auto root : tree.roots) {
		auto &body = tree.nodes[root]->compiled;
		if (body.clause) {
			ClauseShapes(*body.clause, shapes);
		}
		if (body.objective && body.objective->has_quadratic) {
			shapes.push_back(DecideModelShape::OBJECTIVE_SQUARES);
		}
		if (body.objective && body.objective->has_bilinear) {
			shapes.push_back(DecideModelShape::OBJECTIVE_PRODUCTS);
		}
	}
	// A MIN or MAX objective, whose auxiliary stage 06 writes over its terms' rows.
	if (op.flat_objective_agg != ObjectiveAggregateType::NONE) {
		shapes.push_back(DecideModelShape::OBJECTIVE_EXTREMUM);
	}
	if (!op.composed_minmax_objective_terms.empty()) {
		shapes.push_back(DecideModelShape::OBJECTIVE_COMPOSED);
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
	for (auto &scope : op.keyed_scopes) {
		if (scope.signature != DConstants::INVALID_INDEX) {
			shapes.push_back(DecideModelShape::KEYED_SIGNATURE);
			break;
		}
	}
	return shapes;
}

string DecideModelEmitter::Uncovered(const DecideTree &tree, const PhysicalDecide &op) {
	auto shapes = QueryShapes(tree, op);
	return shapes.empty() ? string() : DECIDE_MODEL_SHAPE_NAMES[static_cast<idx_t>(shapes[0])];
}

DecideModelEmitter::DecideModelEmitter(const PhysicalDecide &op, const DecideTree &tree, const DecideDescent &descent,
                                       DecideTreeColumns &columns, DecideGlobalSinkState &gstate,
                                       const DecideSourceNames &names, idx_t num_rows, DecideErrorLedger &errors)
    : op(op), tree(tree), descent(descent), columns(columns), gstate(gstate), names(names), num_rows(num_rows),
      errors(errors) {
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

string DecideObjectiveDifference(const DecideTree &tree, const DecideNode &node, const DecideNodeContainer &container,
                                 const vector<idx_t> &surviving) {
	// Where each field's values start in an entry's signature (DecideDescent::Signature): a value, a decision and its
	// instance, a key's elements. A frame's is never read: it is told apart whole.
	vector<idx_t> starts;
	idx_t start = 0;
	for (auto &field : node.signature) {
		starts.push_back(start);
		if (field.kind == DecideSignatureFieldKind::VALUE) {
			start++;
		} else if (field.kind == DecideSignatureFieldKind::DECISION) {
			start += 2;
		} else if (field.kind == DecideSignatureFieldKind::BY_KEY) {
			start += DecideKeyElements(tree.keys, field.key).size();
		}
	}
	auto varies = [&](idx_t column, string &values) {
		DecideValueRange range;
		for (auto entry : surviving) {
			auto &signature = container.entries[entry].signature;
			if (column >= signature.size()) {
				return false;
			}
			range.Add(signature[column]);
		}
		return range.Spans(values);
	};
	DecideVariantReader read;
	read.values = [&](idx_t field, idx_t element, const Expression &, string &values) {
		return varies(starts[field] + element, values);
	};
	read.instances = [&](idx_t field) {
		string values;
		return varies(starts[field] + 1, values);
	};
	read.frame = [&](idx_t field) {
		return StringUtil::Format("'%s' has more than one variant over the kept rows", node.signature[field].text);
	};
	return DecideObjectiveTwiceMessage(node.signature, tree.keys, tree.variable_scopes, read);
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
	auto &tested = columns.Tested(root);
	auto scatter = [&](const vector<double> &numbers, CoefficientColumn &column) {
		column.AssignDense(num_rows);
		for (idx_t e = 0; e < entries.size(); e++) {
			column.Set(entries[e].row, numbers[e]);
		}
	};
	for (idx_t t = 0; t < clause.lhs_terms.size(); t++) {
		auto &site = body.linear[t];
		D_ASSERT(site.node == root);
		scatter(columns.Numbers(root, site.program, tested, &nulls, &site, &clause.lhs_terms[t].inputs),
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
		scatter(columns.Numbers(root, body.rhs, tested, &nulls), evaluated.rhs_values);
	}
	if (clause.admitted_lower) {
		// A bound on one decision admits a range of its values on each row, which the model builder folds into the
		// decision's column. Only an easy extremum's is computed on some rows alone
		// (DecideConstraint::admitted_member).
		D_ASSERT(body.admits == DConstants::INVALID_INDEX);
		scatter(columns.Numbers(root, body.lower, tested, &nulls), evaluated.admitted_lower);
		scatter(columns.Numbers(root, body.upper, tested, &nulls), evaluated.admitted_upper);
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

vector<TermFilterState> DecideModelEmitter::Objective() {
	vector<TermFilterState> filters;
	auto found = std::find_if(tree.roots.begin(), tree.roots.end(),
	                          [&](idx_t root) { return tree.nodes[root]->compiled.objective != nullptr; });
	if (found == tree.roots.end()) {
		return filters;
	}
	const auto root = *found;
	auto &body = tree.nodes[root]->compiled;
	auto &terms = body.objective->terms;
	auto &container = descent.Containers()[root];
	// The objective is produced once (D72): a second variant over its kept rows would be a second objective, which
	// flat checks before it reads any clause.
	if (container.instance_entries.size() > 1) {
		errors.Add(DecideErrorRank {DecideErrorPhase::SCOPE_CHECK},
		           ErrorData(InvalidInputException(
		               DecideObjectiveDifference(tree, *tree.nodes[root], container, container.instance_entries))));
	}
	// The rows its `when` keeps.
	vector<bool> kept(num_rows, false);
	for (auto &read : container.routed) {
		kept[read.first] = true;
	}
	auto &coefficients = gstate.evaluated_objective_coefficients;
	coefficients.resize(terms.size());
	filters.resize(terms.size());
	for (idx_t t = 0; t < terms.size(); t++) {
		auto &term = terms[t];
		auto &site = body.linear[t];
		auto &filter = filters[t];
		gstate.objective_variable_indices.push_back(gstate.ReadVariable(term));
		filter.avg_scale = term.avg_scale;
		filter.has_filter = true;
		filter.mask.assign(num_rows, false);
		coefficients[t].AssignDense(num_rows);
		// Stage 05 read a NULL coefficient as 0 (DropNullObjectiveTerms), so none is read here. An entry that reads no
		// instance of the term's decision gives it no value, so the term is dropped there and its coefficient never
		// read (D84).
		const auto reads = columns.ReadEntries(site.node, term.variable_index);
		auto numbers = columns.Numbers(site.node, site.program, reads, nullptr, &site, &term.inputs);
		auto read = [&](idx_t row, idx_t entry) {
			if (reads[entry]) {
				filter.mask[row] = true;
				coefficients[t].Set(row, numbers[entry]);
			}
		};
		auto &entries = descent.Containers()[site.node].entries;
		if (site.node == root) {
			// A term outside every reducer is read on each row the objective keeps, at its variant's entry, and
			// counted once below.
			for (auto &routed : container.routed) {
				read(routed.first, routed.second);
			}
		} else {
			// A term in a reducer is read on each member of the G it ranges over: an entry of its operand, which
			// the reducer's `when` routed and its member tests keep. A reducer that keeps none reads nothing, so its
			// terms are dropped (D84).
			for (idx_t e = 0; e < entries.size(); e++) {
				read(entries[e].row, e);
			}
		}
		if (term.avg_scale && term.avg_group_key != DConstants::INVALID_INDEX) {
			// An AVG of a `by` key divides by the members of its own group (D81): the slot of the G each lies in.
			filter.avg_num_groups = descent.Containers()[term.child].groups.size();
			filter.avg_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
			for (auto &entry : entries) {
				filter.avg_group_ids[entry.row] = entry.parent_slot;
			}
		}
	}
	// A term outside every reducer counts once, on the first row the objective keeps.
	CountTupleTermsOnce(terms, coefficients, &kept, num_rows);
	return filters;
}

} // namespace duckdb
