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
    "GROUP_ROWS",
    "PRODUCT_TERMS",
    "SQUARE_TERMS",
    "TIGHTEST_BOUND",
    "NOT_EQUAL_ROWS",
    "DEFINED_READS",
    "EXTREMUM_ROWS",
    "MEMBER_ROWS",
    "CLASS_ROWS",
    "CLASS_SETS",
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

//! The shapes of a clause beyond its rows and groups, from what stage 06 reads of it (EvaluatedConstraint): its
//! products and squares, what its bound and rows read, how its rows are made, then the classes a diagnosis reports they
//! stand for.
static void ClauseShapes(const DecideConstraint &clause, bool diagnose, vector<DecideModelShape> &shapes) {
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
	if (clause.scope.per == DecideClauseScope::PerRole::ONE_ROW_PER_CLASS) {
		shapes.push_back(DecideModelShape::CLASS_ROWS);
	}
	// A group's row stands for the classes of its kept rows (EvaluatedConstraint::group_class_set).
	if (diagnose && clause.lhs_is_aggregate) {
		shapes.push_back(DecideModelShape::CLASS_SETS);
	}
}

//! The shapes the query has beyond rows, declared classes and a linear objective: each clause's, the objective's, then
//! what the query hands stage 06 beside its clauses. A helper's classes reach stage 06 with its link.
static vector<DecideModelShape> QueryShapes(const DecideTree &tree, const PhysicalDecide &op) {
	vector<DecideModelShape> shapes;
	for (auto root : tree.roots) {
		auto &body = tree.nodes[root]->compiled;
		if (body.clause) {
			ClauseShapes(*body.clause, op.diagnose, shapes);
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
                                       DecideTreeColumns &columns, DecideGlobalSinkState &gstate, PerGroupCache &groups,
                                       const DecideSourceNames &names, idx_t num_rows, DecideErrorLedger &errors)
    : op(op), tree(tree), descent(descent), columns(columns), gstate(gstate), groups(groups), names(names),
      num_rows(num_rows), errors(errors) {
}

SolverInput::KeyValues DecideGroupKeyValues(const DecideTree &tree, const DecideDescent &descent, idx_t node) {
	const auto key = tree.nodes[node]->key;
	auto &slots = descent.Containers()[node].groups;
	vector<vector<Value>> values(slots.size());
	for (auto &slot : slots) {
		values[slot.rank] = descent.Label(key, slot.key);
	}
	SolverInput::KeyValues key_values;
	for (auto &element : DecideKeyElements(tree.keys, key)) {
		key_values.null_modes.push_back(element.null_mode);
	}
	key_values.values_of = [values](idx_t group) {
		return values[group];
	};
	return key_values;
}

vector<pair<idx_t, idx_t>> DecideEntryRows(const DecideTree &tree, const DecideDescent &descent, idx_t node) {
	auto &container = descent.Containers()[node];
	if (tree.nodes[node]->per != DecideTreePer::ROW) {
		return container.routed;
	}
	vector<pair<idx_t, idx_t>> rows;
	for (idx_t e = 0; e < container.entries.size(); e++) {
		rows.emplace_back(container.entries[e].row, e);
	}
	return rows;
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
		auto &clause = *tree.nodes[roots[c]]->compiled.clause;
		auto evaluated = clause.lhs_is_aggregate ? Groups(roots[c]) : Rows(roots[c], c < num_constraints);
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

void DecideModelEmitter::ReadBesideReducers(idx_t root, const DecideTerm &term, const DecideTermSite &site,
                                            const vector<pair<idx_t, idx_t>> &rows, CoefficientColumn &column) {
	// A term beside the reducers stays in its group's row only as data of the `by` key's own columns times a decision
	// of the group's class or of none (language/guide.md §6.4): NULL, or with no instance, only on a kept row in no
	// group, which emits nothing.
	vector<bool> in_no_group(descent.Containers()[root].entries.size(), false);
	auto numbers = columns.Numbers(root, site.program, columns.Tested(root), &in_no_group, &site, &term.inputs);
	column.AssignDense(num_rows);
	for (auto &read : rows) {
		column.Set(read.first, numbers[read.second]);
	}
}

vector<bool> DecideModelEmitter::ReadInReducer(const DecideTerm &term, const DecideTermSite &site,
                                               CoefficientColumn &column, TermFilterState &filter) {
	auto &entries = descent.Containers()[site.node].entries;
	// Its member tests are its filter's NULL tests of what its body reads and of its decisions' keys, so no member's
	// coefficient is NULL and every member's decision has an instance (D84).
	const auto &tested = columns.Tested(site.node);
	auto numbers = columns.Numbers(site.node, site.program, tested, nullptr, &site, &term.inputs);
	// A relation-qualified reducer reads each class of its qualifier once in each group, at its first row
	// (language/guide.md §5.1).
	const auto qualified = term.qualifier_scope_idx != DConstants::INVALID_INDEX;
	const auto reads = qualified ? columns.FirstOfEachInstance(site.node, tested) : tested;
	filter.avg_scale = term.avg_scale;
	filter.has_filter = qualified;
	filter.mask.assign(num_rows, false);
	column.AssignDense(num_rows);
	for (idx_t e = 0; e < entries.size(); e++) {
		if (reads[e]) {
			filter.mask[entries[e].row] = true;
			column.Set(entries[e].row, numbers[e]);
		}
	}
	vector<bool> members(num_rows, false);
	for (auto &read : DecideEntryRows(tree, descent, site.node)) {
		members[read.first] = members[read.first] || tested[read.second];
	}
	return members;
}

idx_t DecideModelEmitter::ByReducer(idx_t root) const {
	auto &node = *tree.nodes[root];
	for (auto child : node.children) {
		if (tree.nodes[child]->kind == DecideNodeKind::REDUCER && tree.nodes[child]->key == node.facts->group_key) {
			return child;
		}
	}
	throw InternalException("DECIDE tree: %s reads no G of its `by` key", node.text);
}

bool DecideModelEmitter::ReadGroupedTerms(idx_t root, const vector<pair<idx_t, idx_t>> &rows, const vector<bool> &kept,
                                          EvaluatedConstraint &evaluated, vector<TermFilterState> &filters,
                                          vector<bool> &active) {
	auto &body = tree.nodes[root]->compiled;
	auto &terms = body.clause->lhs_terms;
	bool has_local_filters = false;
	bool unfiltered = false;
	for (idx_t t = 0; t < terms.size(); t++) {
		auto &site = body.linear[t];
		if (site.node == root) {
			// A term beside the reducers is read on each kept row, and counted once in its group.
			ReadBesideReducers(root, terms[t], site, rows, evaluated.row_coefficients[t]);
			unfiltered = true;
			continue;
		}
		const auto members = ReadInReducer(terms[t], site, evaluated.row_coefficients[t], filters[t]);
		// A filter that keeps every kept row filters nothing, as flat reads it (ReadTermFilters).
		bool filters_rows = false;
		for (idx_t row = 0; row < num_rows && !filters_rows; row++) {
			filters_rows = kept[row] && !members[row];
		}
		if (!filters_rows) {
			unfiltered = true;
			continue;
		}
		filters[t].has_filter = true;
		has_local_filters = true;
		for (idx_t row = 0; row < num_rows; row++) {
			active[row] = active[row] || members[row];
		}
	}
	if (unfiltered) {
		std::fill(active.begin(), active.end(), true);
	}
	return has_local_filters;
}

EvaluatedConstraint DecideModelEmitter::Groups(idx_t root) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	auto evaluated = InitializeEvaluatedConstraint(clause, body.clause_index,
	                                               [&](const DecideTerm &term) { return gstate.ReadVariable(term); });
	// The rows the clause keeps: every row its root holds, which its `when` routed, with the entry standing for each.
	const auto rows = DecideEntryRows(tree, descent, root);
	vector<bool> kept(num_rows, false);
	for (auto &read : rows) {
		kept[read.first] = true;
	}
	vector<bool> null_rows(num_rows, false);
	vector<TermFilterState> filters(clause.lhs_terms.size());
	// The rows some reducer's filter keeps, which alone emit their group's row when every term has a filter.
	vector<bool> active(num_rows, false);
	const auto has_local_filters = ReadGroupedTerms(root, rows, kept, evaluated, filters, active);
	// Each kept row's group of the `by` key: its slot in the clause's G of that key, numbered by value.
	const auto keyed = clause.group_key != DConstants::INVALID_INDEX;
	const auto by = keyed ? ByReducer(root) : DConstants::INVALID_INDEX;
	vector<idx_t> group_of(num_rows, DConstants::INVALID_INDEX);
	idx_t num_key_groups = 0;
	if (keyed) {
		auto &container = descent.Containers()[by];
		for (auto &entry : container.entries) {
			group_of[entry.row] = container.groups[entry.slot].rank;
		}
		num_key_groups = container.groups.size();
	}
	const DecideKeyGroups key_groups {group_of, num_key_groups};
	idx_t grouped_instances = 0;
	// A `per K:` that names classes holds once per class, so not at all with every kept row's K NULL.
	auto &classes = descent.Containers()[root].groups;
	const auto kept_groups = GroupRowsByShape(
	    clause, num_rows, kept, has_local_filters, active, keyed ? &key_groups : nullptr,
	    [&]() {
		    return std::any_of(classes.begin(), classes.end(),
		                       [](const DecideDescentGroup &slot) { return slot.in_a_class; });
	    },
	    nullptr, evaluated, grouped_instances);
	if (keyed) {
		Name(by, kept_groups, evaluated);
	}
	// A group in which a reducer reads no member has no value there, so neither has its instance (D68).
	vector<const TermFilterState *> reducers;
	for (auto &filter : filters) {
		reducers.push_back(&filter);
	}
	SkipGroupsWithAnEmptyReducer(reducers, num_rows, evaluated.row_group_ids, evaluated.num_groups);
	CountTupleTermsOncePerGroup(clause.lhs_terms, num_rows, evaluated);
	if (DescribeBound(clause, num_rows, names, op.source_fragments, op.keyed_scopes, op.keys, evaluated)) {
		vector<bool> nulls(descent.Containers()[root].entries.size(), false);
		auto bound = columns.Numbers(root, body.rhs, columns.Tested(root), &nulls);
		evaluated.rhs_values.AssignDense(num_rows);
		for (auto &read : rows) {
			evaluated.rhs_values.Set(read.first, bound[read.second]);
			null_rows[read.first] = null_rows[read.first] || nulls[read.second];
		}
	}
	D_ASSERT(!clause.admitted_lower);
	ScaleAvgTerms(clause, filters, num_rows, evaluated);
	SkipNullInstances(null_rows, true, num_rows, evaluated.row_group_ids, evaluated.num_groups);
	gstate.CountInstances(evaluated.source_clause_id, grouped_instances,
	                      CountInstances(evaluated.row_group_ids, evaluated.num_groups, num_rows, true));
	return evaluated;
}

void DecideModelEmitter::Name(idx_t by, const vector<idx_t> &kept_groups, EvaluatedConstraint &evaluated) {
	// Each group is named by its key's values, which the solver input reads by its rank.
	auto &slots = descent.Containers()[by].groups;
	vector<idx_t> first_rows(slots.size());
	for (auto &slot : slots) {
		first_rows[slot.rank] = slot.first_row;
	}
	const auto values = groups.key_values.size();
	groups.key_values.push_back(DecideGroupKeyValues(tree, descent, by));
	for (auto group : kept_groups) {
		evaluated.group_names.push_back({values, group, first_rows[group]});
	}
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
		// read (D84). A relation-qualified reducer reads each class once, at its first row (language/guide.md §5.1).
		const auto present = columns.ReadEntries(site.node, term.variable_index);
		auto numbers = columns.Numbers(site.node, site.program, present, nullptr, &site, &term.inputs);
		const auto reads = term.qualifier_scope_idx == DConstants::INVALID_INDEX
		                       ? present
		                       : columns.FirstOfEachInstance(site.node, present);
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
