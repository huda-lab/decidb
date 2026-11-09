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
#include <map>

namespace duckdb {

static const char *const DECIDE_MODEL_SHAPE_NAMES[] = {
    "ROWS",           "REPAIR_ROWS",   "KEYED_DECLARED",     "OBJECTIVE_LINEAR",  "GROUP_ROWS",
    "NOT_EQUAL_ROWS", "DEFINED_READS", "CLASS_ROWS",         "CLASS_SETS",        "KEYED_SIGNATURE",
    "EXTREMUM_ROWS",  "MEMBER_ROWS",   "OBJECTIVE_EXTREMUM", "COMPOSED",          "OBJECTIVE_COMPOSED",
    "PRODUCT_TERMS",  "SQUARE_TERMS",  "TIGHTEST_BOUND",     "OBJECTIVE_SQUARES", "OBJECTIVE_PRODUCTS",
    "VALUE_LINKS",    "FRAME_READS",   "PRODUCT_LINKS",
};

//! The shapes of a clause beyond its rows, groups, classes and extrema, from what stage 06 reads of it
//! (EvaluatedConstraint): its products and squares, what its bound reads.
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
}

//! The shapes the query has beyond rows, classes, extrema, composed clauses, declared classes and a linear or extremum
//! objective: each clause's, the objective's, then what the query hands stage 06 beside its clauses. A helper's
//! classes reach stage 06 with its link.
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

DecideModelEmitter::DecideModelEmitter(const PhysicalDecide &op, ClientContext &context, const DecideTree &tree,
                                       const DecideDescent &descent, DecideTreeColumns &columns,
                                       DecideGlobalSinkState &gstate, PerGroupCache &groups,
                                       const DecideSourceNames &names, idx_t num_rows, DecideErrorLedger &errors)
    : op(op), context(context), tree(tree), descent(descent), columns(columns), gstate(gstate), groups(groups),
      names(names), num_rows(num_rows), errors(errors) {
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

//! Where each field's values start in an entry's signature (DecideDescent::Signature), and where they end: a value, a
//! decision and its instance, a key's elements. A frame's is never read: it is told apart whole.
static vector<idx_t> SignatureStarts(const DecideTree &tree, const DecideNode &node) {
	vector<idx_t> starts {0};
	for (auto &field : node.signature) {
		auto start = starts.back();
		if (field.kind == DecideSignatureFieldKind::VALUE) {
			start++;
		} else if (field.kind == DecideSignatureFieldKind::DECISION) {
			start += 2;
		} else if (field.kind == DecideSignatureFieldKind::BY_KEY) {
			start += DecideKeyElements(tree.keys, field.key).size();
		}
		starts.push_back(start);
	}
	return starts;
}

string DecideObjectiveDifference(const DecideTree &tree, const DecideNode &node, const DecideNodeContainer &container,
                                 const vector<idx_t> &surviving) {
	const auto starts = SignatureStarts(tree, node);
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
	auto mappings = DecideDeclaredMappings(descent);
	for (idx_t scope = 0; scope < op.keyed_scopes.size(); scope++) {
		if (op.keyed_scopes[scope].signature != DConstants::INVALID_INDEX) {
			mappings[scope] = SignatureMapping(scope);
		}
	}
	ReadAbsence();
	return mappings;
}

KeyedMapping DecideModelEmitter::SignatureMapping(idx_t scope) {
	// The reducer's operand, whose instances are its classes split by the values its body reads (D70, D71).
	const auto operand = tree.qualified_operands[scope];
	if (operand == DConstants::INVALID_INDEX) {
		throw InternalException("DECIDE tree: no reducer is qualified by keyed scope %llu", scope);
	}
	auto &signature = op.signatures[op.keyed_scopes[scope].signature];
	const auto variants = Variants(operand, &signature);
	KeyedMapping mapping;
	mapping.num_classes = variants.count;
	// How many classes of the key those values come from, for EXPLAIN ANALYZE (D71).
	if (variants.count > variants.num_classes) {
		op.RecordVariants(signature, "values", variants.count, variants.num_classes);
	}
	// A row the reducer does not read has no class: it maps to one spare class (KeyedMapping::absent_class).
	mapping.row_to_class = variants.of_row;
	for (auto &id : mapping.row_to_class) {
		if (id == DConstants::INVALID_INDEX) {
			mapping.absent_class = variants.count;
			id = mapping.absent_class;
		}
	}
	if (mapping.absent_class != DConstants::INVALID_INDEX) {
		mapping.num_classes++;
	}
	return mapping;
}

void DecideModelEmitter::ReadAbsence() {
	gstate.absent_reads.assign(op.decide_variables.size(), vector<bool>());
	columns.ReadAbsence(gstate.absent_reads);
	// The rows defining each auxiliary (DecideConstraint::defined_var), in the tree's order, which puts what defines a
	// decision before what reads it (D88): an auxiliary a definition reads has its absence by then, whatever order the
	// rewrites numbered them in.
	vector<idx_t> position(tree.nodes.size());
	for (idx_t i = 0; i < tree.eval_order.size(); i++) {
		position[tree.eval_order[i]] = i;
	}
	auto definitions = ClauseRoots();
	definitions.resize(op.prepared.constraints.size());
	std::sort(definitions.begin(), definitions.end(),
	          [&](idx_t left, idx_t right) { return position[left] < position[right]; });
	for (auto root : definitions) {
		auto &body = tree.nodes[root]->compiled;
		auto &clause = *body.clause;
		if (clause.defined_var == DConstants::INVALID_INDEX) {
			continue;
		}
		auto &absent = gstate.absent_reads[clause.defined_var];
		auto mark = [&](idx_t row) {
			absent.resize(num_rows, false);
			absent[row] = true;
		};
		// What it reads unreduced is NULL on a row it keeps: its bound, then each term's coefficient, which flat reads
		// in that order, so what one raises ranks there (DecideNode::value_rank).
		auto rank = tree.nodes[root]->value_rank;
		auto null_rows = [&](idx_t node, idx_t program, idx_t position) {
			rank.position = position;
			const auto nulls = columns.NullEntries(node, program, rank);
			for (auto &read : DecideEntryRows(tree, descent, node)) {
				if (nulls[read.second]) {
					mark(read.first);
				}
			}
		};
		null_rows(root, body.rhs, 0);
		for (idx_t t = 0; t < clause.lhs_terms.size(); t++) {
			null_rows(body.linear[t].node, body.linear[t].program, 1 + t);
			// A decision it reads has no instance on a row, wherever the row lies.
			for (idx_t row = 0; row < num_rows; row++) {
				if (columns.AbsentAt(clause.lhs_terms[t].variable_index, row)) {
					mark(row);
				}
			}
		}
	}
}

vector<idx_t> DecideModelEmitter::ClauseRoots() const {
	vector<idx_t> roots(op.prepared.constraints.size() + op.prepared.singleton_row_constraints.size(),
	                    DConstants::INVALID_INDEX);
	for (auto root : tree.roots) {
		auto &body = tree.nodes[root]->compiled;
		if (body.clause) {
			roots[body.clause_index] = root;
		}
	}
	return roots;
}

void DecideModelEmitter::Constraints() {
	const idx_t num_constraints = op.prepared.constraints.size();
	const auto roots = ClauseRoots();
	// The folded bounds follow under DIAGNOSE, numbered after every other clause: only the repair model emits them.
	const idx_t num_evaluated = num_constraints + (op.diagnose ? op.prepared.singleton_row_constraints.size() : 0);
	for (idx_t c = 0; c < num_evaluated; c++) {
		auto &clause = *tree.nodes[roots[c]]->compiled.clause;
		auto evaluated = clause.lhs_is_aggregate || clause.was_minmax_easy ? Groups(roots[c], c < num_constraints)
		                                                                   : Rows(roots[c], c < num_constraints);
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
	// The rows the clause keeps, which its `when` routed, with the entry standing for each: one model row stands on
	// each, or under `per K:` on one row of each variant of a class.
	const auto rows = DecideEntryRows(tree, descent, root);
	vector<bool> nulls(entries.size(), false);
	auto &tested = columns.Tested(root);
	auto scatter = [&](const vector<double> &numbers, CoefficientColumn &column) {
		column.AssignDense(num_rows);
		for (auto &read : rows) {
			column.Set(read.first, numbers[read.second]);
		}
	};
	for (idx_t t = 0; t < clause.lhs_terms.size(); t++) {
		auto &site = body.linear[t];
		D_ASSERT(site.node == root);
		scatter(columns.Numbers(root, site.program, tested, &nulls, &site, &clause.lhs_terms[t].inputs),
		        evaluated.row_coefficients[t]);
	}
	if (clause.scope.per == DecideClauseScope::PerRole::ONE_ROW_PER_CLASS) {
		Classes(root, rows, evaluated);
	} else if (clause.scope.when) {
		evaluated.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
		for (auto &read : rows) {
			evaluated.row_group_ids[read.first] = 0;
		}
		evaluated.num_groups = 1;
	}
	const auto instances = CountInstances(evaluated.row_group_ids, evaluated.num_groups, num_rows, false);
	if (DescribeBound(clause, num_rows, names, op.source_fragments, op.keyed_scopes, op.keys, evaluated)) {
		scatter(columns.Numbers(root, body.rhs, tested, &nulls), evaluated.rhs_values);
	}
	// A term reading a decision with no instance at the row has no value there (D84), nor has an auxiliary where what
	// defines it has none (D88).
	vector<bool> null_rows(num_rows, false);
	for (auto &read : rows) {
		null_rows[read.first] = nulls[read.second];
		for (auto &term : clause.lhs_terms) {
			null_rows[read.first] = null_rows[read.first] || columns.AbsentAt(term.variable_index, read.first);
		}
	}
	if (clause.admitted_lower) {
		Admit(body, evaluated);
	}
	SkipNullInstances(null_rows, false, num_rows, evaluated.row_group_ids, evaluated.num_groups);
	if (counted) {
		gstate.CountInstances(evaluated.source_clause_id, instances,
		                      CountInstances(evaluated.row_group_ids, evaluated.num_groups, num_rows, false));
	}
	return evaluated;
}

void DecideModelEmitter::Classes(idx_t root, const vector<pair<idx_t, idx_t>> &rows, EvaluatedConstraint &evaluated) {
	auto &clause = *tree.nodes[root]->compiled.clause;
	const auto signature =
	    clause.scope.signature == DConstants::INVALID_INDEX ? nullptr : &op.signatures[clause.scope.signature];
	const auto variants = Variants(root, signature);
	vector<idx_t> class_of, first_rows, variants_per_class;
	DecideOneRowPerVariant(variants.of_row, variants.class_of_row, variants.count, variants.num_classes,
	                       gstate.absent_reads, clause.defined_var, evaluated.row_group_ids, class_of, first_rows,
	                       variants_per_class);
	evaluated.num_groups = variants.count;
	evaluated.rows_stand_with_their_class = !clause.scope.when;
	// A variant is named by its class, and by its own field values where its class has more than one.
	vector<idx_t> class_ids;
	vector<GroupName> class_names;
	if (clause.scope.class_key != DConstants::INVALID_INDEX) {
		KeyClasses(root, class_ids, class_names);
	}
	if (variants.count > variants.num_classes) {
		op.RecordVariants(*signature, "constraints", variants.count, variants.num_classes);
		evaluated.group_names = NameVariants(root, *signature, class_of, first_rows, variants_per_class);
	} else {
		for (idx_t variant = 0; variant < variants.count && !class_names.empty(); variant++) {
			evaluated.group_names.push_back(class_names[class_of[variant]]);
		}
	}
	if (!op.diagnose) {
		return;
	}
	// Each variant's row stands for its class (D28), and is named as its own where its class has more than one.
	vector<bool> kept(num_rows, false);
	for (auto &read : rows) {
		kept[read.first] = true;
	}
	evaluated.group_class_set = DecideGroupClassSets(
	    clause.scope, num_rows, kept, evaluated.row_group_ids, variants.count,
	    [&](vector<idx_t> &ids, vector<GroupName> &named) {
		    ids = class_ids;
		    named = class_names;
	    },
	    gstate.class_sets);
	DecideNameVariantClassSets(class_of, variants_per_class, evaluated.group_names, evaluated.group_class_set,
	                           gstate.class_sets);
}

DecideVariants DecideModelEmitter::Variants(idx_t node, const DecideSignature *signature) const {
	auto &container = descent.Containers()[node];
	auto &entries = container.entries;
	// The entries in a class of the node's key: a reducer reads no value of a row in none, which its operand holds
	// (DecideNode::holds_rows_in_no_class).
	vector<idx_t> held;
	for (idx_t e = 0; e < entries.size(); e++) {
		if (container.groups[entries[e].slot].in_a_class) {
			held.push_back(e);
		}
	}
	// A variant is a class of the node's key and the values its sigma reads. Entries are made in row order, so a
	// variant's first entry holds its first row.
	auto less = [](const vector<Value> &left, const vector<Value> &right) {
		return DecideDescentKeyLess(left, right);
	};
	std::map<vector<Value>, idx_t, decltype(less)> variant_ids(less);
	std::map<vector<Value>, idx_t, decltype(less)> class_ids(less);
	vector<vector<Value>> identities(entries.size());
	for (auto e : held) {
		auto &key = container.groups[entries[e].slot].key;
		class_ids.emplace(key, 0);
		identities[e] = key;
		identities[e].insert(identities[e].end(), entries[e].signature.begin(), entries[e].signature.end());
		variant_ids.emplace(identities[e], entries[e].row);
	}
	// Flat numbers them by value, their class's and then their fields', unless a field reads a decision's instance,
	// which numbers them by their first rows (BuildSignatureIds); so does a frame's, which has no arm yet
	// (FRAME_READS). No field reads a query-wide decision, which one instance holds for every class.
	bool by_rows = false;
	for (idx_t f = 0; signature && f < signature->fields.size(); f++) {
		by_rows = by_rows || signature->fields[f].kind == DecideSignatureFieldKind::DECISION;
	}
	vector<pair<idx_t, const vector<Value> *>> order;
	for (auto &variant : variant_ids) {
		order.emplace_back(variant.second, &variant.first);
	}
	if (by_rows) {
		std::sort(order.begin(), order.end());
	}
	DecideVariants result;
	for (auto &id : class_ids) {
		id.second = result.num_classes++;
	}
	for (auto &variant : order) {
		variant_ids[*variant.second] = result.count++;
	}
	vector<idx_t> variant_of(entries.size(), DConstants::INVALID_INDEX);
	vector<idx_t> class_of(entries.size(), DConstants::INVALID_INDEX);
	for (auto e : held) {
		variant_of[e] = variant_ids[identities[e]];
		class_of[e] = class_ids[container.groups[entries[e].slot].key];
	}
	result.of_row.assign(num_rows, DConstants::INVALID_INDEX);
	result.class_of_row.assign(num_rows, DConstants::INVALID_INDEX);
	for (auto &read : DecideEntryRows(tree, descent, node)) {
		result.of_row[read.first] = variant_of[read.second];
		result.class_of_row[read.first] = class_of[read.second];
	}
	return result;
}

vector<GroupName> DecideModelEmitter::NameVariants(idx_t root, const DecideSignature &signature,
                                                   const vector<idx_t> &class_of, const vector<idx_t> &first_rows,
                                                   const vector<idx_t> &variants_per_class) {
	auto &node = *tree.nodes[root];
	auto &container = descent.Containers()[root];
	auto &names = node.compiled.names;
	// The entry standing for each variant's first row.
	vector<idx_t> entry_of(first_rows.size());
	vector<idx_t> variant_at(num_rows, DConstants::INVALID_INDEX);
	for (idx_t variant = 0; variant < first_rows.size(); variant++) {
		variant_at[first_rows[variant]] = variant;
	}
	for (auto &read : DecideEntryRows(tree, descent, root)) {
		if (variant_at[read.first] != DConstants::INVALID_INDEX) {
			entry_of[variant_at[read.first]] = read.second;
		}
	}
	// Each variant's values as flat's reader shows them: what its class key and its fields read at its first row, but
	// a collated column of its class by the least spelling among the clause's rows of the class
	// (DecideDescentGroup::spelling).
	auto &elements = DecideKeyElements(tree.keys, node.key);
	SolverInput::KeyValues key_values;
	for (auto &element : elements) {
		key_values.null_modes.push_back(element.null_mode);
	}
	key_values.null_modes.resize(names.size(), DecideKeyNullMode::NO_GROUP);
	vector<vector<Value>> values;
	for (auto entry : entry_of) {
		auto &spelling = container.groups[container.entries[entry].slot].spelling;
		values.emplace_back();
		for (idx_t i = 0; i < names.size(); i++) {
			const auto collated = i < elements.size() && elements[i].collated;
			values.back().push_back(collated ? spelling.label[i] : columns.ValueAt(root, names[i], entry));
		}
	}
	DecideNameVariants(
	    signature.fields, elements.size(), [values](idx_t variant) { return values[variant]; }, class_of,
	    variants_per_class, first_rows, key_values);
	const auto named = groups.key_values.size();
	groups.key_values.push_back(std::move(key_values));
	vector<GroupName> result;
	for (idx_t variant = 0; variant < first_rows.size(); variant++) {
		result.push_back({named, variant, first_rows[variant]});
	}
	return result;
}

void DecideModelEmitter::KeyClasses(idx_t root, vector<idx_t> &class_ids, vector<GroupName> &class_names) {
	auto &container = descent.Containers()[root];
	const auto named = groups.key_values.size();
	groups.key_values.push_back(DecideGroupKeyValues(tree, descent, root));
	// The root's slots by K's values; a grouped root holds its kept rows in no class of K in a slot of its own.
	vector<idx_t> by_rank(container.groups.size());
	for (idx_t slot = 0; slot < container.groups.size(); slot++) {
		by_rank[container.groups[slot].rank] = slot;
	}
	vector<idx_t> class_of_slot(container.groups.size(), DConstants::INVALID_INDEX);
	for (idx_t rank = 0; rank < by_rank.size(); rank++) {
		auto &slot = container.groups[by_rank[rank]];
		if (slot.in_a_class) {
			class_of_slot[by_rank[rank]] = class_names.size();
			class_names.push_back({named, rank, slot.first_row});
		}
	}
	class_ids.assign(num_rows, DConstants::INVALID_INDEX);
	for (auto &read : DecideEntryRows(tree, descent, root)) {
		class_ids[read.first] = class_of_slot[container.entries[read.second].slot];
	}
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

vector<pair<idx_t, idx_t>> DecideModelEmitter::ReadRows(idx_t site, const vector<bool> &reads, bool qualified) const {
	auto &entries = descent.Containers()[site].entries;
	vector<pair<idx_t, idx_t>> rows;
	for (auto &read : DecideEntryRows(tree, descent, site)) {
		if (reads[read.second] && (!qualified || entries[read.second].row == read.first)) {
			rows.push_back(read);
		}
	}
	return rows;
}

vector<bool> DecideModelEmitter::ReadInReducer(const DecideTerm &term, const DecideTermSite &site,
                                               CoefficientColumn &column, TermFilterState &filter) {
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
	for (auto &read : ReadRows(site.node, reads, qualified)) {
		filter.mask[read.first] = true;
		column.Set(read.first, numbers[read.second]);
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

EvaluatedConstraint DecideModelEmitter::Groups(idx_t root, bool counted) {
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
	vector<idx_t> kept_row_group_ids;
	// A `per K:` that names classes holds once per class, so not at all with every kept row's K NULL.
	auto &classes = descent.Containers()[root].groups;
	const auto kept_groups = GroupRowsByShape(
	    clause, num_rows, kept, has_local_filters, active, keyed ? &key_groups : nullptr,
	    [&]() {
		    return std::any_of(classes.begin(), classes.end(),
		                       [](const DecideDescentGroup &slot) { return slot.in_a_class; });
	    },
	    op.diagnose ? &kept_row_group_ids : nullptr, evaluated, grouped_instances);
	if (keyed) {
		Name(by, kept_groups, evaluated);
	}
	// The classes each group's row stands for, which a diagnosis reports one by one (D28). With no `by` key no group
	// numbers the kept rows, and a clause with no groups is one group of every kept row.
	if (op.diagnose) {
		evaluated.group_class_set = DecideGroupClassSets(
		    clause.scope, num_rows, kept, kept_row_group_ids,
		    evaluated.row_group_ids.empty() ? 1 : evaluated.num_groups,
		    [&](vector<idx_t> &class_ids, vector<GroupName> &class_names) { KeyClasses(root, class_ids, class_names); },
		    gstate.class_sets);
	}
	// A group in which a reducer reads no member has no value there, so neither has its instance (D68).
	vector<const TermFilterState *> reducers;
	for (auto &filter : filters) {
		reducers.push_back(&filter);
	}
	SkipGroupsWithAnEmptyReducer(reducers, num_rows, evaluated.row_group_ids, evaluated.num_groups);
	// A decision beside the reducers counts once in each group's row; an easy MIN's or MAX's rows are its members',
	// whose terms stage 05 reads as written on each.
	if (clause.lhs_is_aggregate) {
		CountTupleTermsOncePerGroup(clause.lhs_terms, num_rows, evaluated);
	}
	if (DescribeBound(clause, num_rows, names, op.source_fragments, op.keyed_scopes, op.keys, evaluated)) {
		vector<bool> nulls(descent.Containers()[root].entries.size(), false);
		auto bound = columns.Numbers(root, body.rhs, columns.Tested(root), &nulls);
		evaluated.rhs_values.AssignDense(num_rows);
		for (auto &read : rows) {
			evaluated.rhs_values.Set(read.first, bound[read.second]);
			null_rows[read.first] = null_rows[read.first] || nulls[read.second];
		}
	}
	if (clause.admitted_lower) {
		Admit(body, evaluated);
	}
	HoistAvgIntoNotEqualBound(clause, filters, evaluated);
	ScaleAvgTerms(clause, filters, num_rows, evaluated);
	SkipNullInstances(null_rows, true, num_rows, evaluated.row_group_ids, evaluated.num_groups);
	if (counted) {
		gstate.CountInstances(evaluated.source_clause_id, grouped_instances,
		                      CountInstances(evaluated.row_group_ids, evaluated.num_groups, num_rows, true));
	}
	return evaluated;
}

void DecideModelEmitter::Admit(const DecideBody &body, EvaluatedConstraint &evaluated) {
	// Computed on the members of its term's site, the entries its member tests keep: an easy MIN's or MAX's on its
	// reducer's members alone, whose filter the descent routed. A row no member stands on emits no model row, so what
	// it would admit is never read. An end is never NULL: a NULL coefficient or bound admits every value, and every
	// other end is a number the row's coefficient and bound pick (DecideSingletonRow::lower). So it skips no instance,
	// and a NULL would be an internal error (ExtractDoubleColumn).
	const auto site = body.linear[0].node;
	auto &members = columns.Tested(site);
	const auto lower = columns.Numbers(site, body.lower, members, nullptr);
	const auto upper = columns.Numbers(site, body.upper, members, nullptr);
	evaluated.admitted_lower.AssignDense(num_rows);
	evaluated.admitted_upper.AssignDense(num_rows);
	for (auto &read : DecideEntryRows(tree, descent, site)) {
		evaluated.admitted_lower.Set(read.first, lower[read.second]);
		evaluated.admitted_upper.Set(read.first, upper[read.second]);
	}
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
		const auto qualified = term.qualifier_scope_idx != DConstants::INVALID_INDEX;
		const auto reads = qualified ? columns.FirstOfEachInstance(site.node, present) : present;
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
			for (auto &member : ReadRows(site.node, reads, qualified)) {
				read(member.first, member.second);
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

void DecideModelEmitter::Composed(PhysicalDecide::EvaluatedClauses &evaluated, DataChunk &first) {
	// Each composed clause's root, by its place among them, and the objective's, which hold their terms' G's.
	vector<idx_t> clauses(op.composed_minmax_constraints.size(), DConstants::INVALID_INDEX);
	idx_t objective = DConstants::INVALID_INDEX;
	for (auto root : tree.roots) {
		auto &node = *tree.nodes[root];
		if (!node.compiled.composed.empty()) {
			(node.objective ? objective : clauses[node.compiled.order]) = root;
		}
	}
	for (idx_t c = 0; c < clauses.size(); c++) {
		auto &spec = op.composed_minmax_constraints[c];
		PhysicalDecide::EvaluatedClauses::ComposedConstraint clause;
		clause.rhs = DecideComposedBound(context, spec);
		bool null_factor = false;
		clause.terms = ComposedTerms(clauses[c], spec.terms, "constraint", first, null_factor);
		// The rows its `when` keeps are the rows its root holds, and their classes those of its `per K:`.
		auto kept_rows = [&]() {
			vector<bool> kept(num_rows, false);
			for (auto &read : DecideEntryRows(tree, descent, clauses[c])) {
				kept[read.first] = true;
			}
			return kept;
		};
		auto class_set = [&](const vector<bool> &kept) {
			auto classes = [&](vector<idx_t> &class_ids, vector<GroupName> &class_names) {
				KeyClasses(clauses[c], class_ids, class_names);
			};
			return DecideGroupClassSets(spec.scope, num_rows, kept, {}, 1, classes, gstate.class_sets)[0];
		};
		op.FinishComposedClause(gstate, num_rows, spec, null_factor, kept_rows, class_set, std::move(clause),
		                        evaluated);
	}
	if (objective != DConstants::INVALID_INDEX) {
		bool null_factor = false;
		auto terms = ComposedTerms(objective, op.composed_minmax_objective_terms, "objective", first, null_factor);
		// A term with a NULL factor reads 0; one that reads no row is dropped.
		terms.erase(std::remove_if(terms.begin(), terms.end(), DecideComposedTermReadsNoRow), terms.end());
		evaluated.composed_objective_terms = std::move(terms);
	}
}

vector<ComposedMinMaxTermData> DecideModelEmitter::ComposedTerms(idx_t root,
                                                                 const vector<LogicalDecide::ComposedMinMaxTerm> &terms,
                                                                 const char *what, DataChunk &first,
                                                                 bool &null_factor) {
	auto &body = tree.nodes[root]->compiled;
	const auto clause = op.WrittenClause(tree.nodes[root]->source_clause_id);
	vector<ComposedMinMaxTermData> result;
	for (idx_t t = 0; t < terms.size(); t++) {
		auto &term = terms[t];
		auto evaluated = DecideComposedTerm(term, op.source_fragments, op.keyed_scopes, op.keys);
		if (term.scale) {
			// A value the whole query shares, read as flat reads it, on the input's first rows, whatever the clause
			// keeps: before the term's members, which follow its predecessor's (DecideErrorRank::part).
			try {
				auto lowered = TransformToChunkExpression(*term.scale, context);
				ExpressionExecutor executor(context, *lowered);
				DataChunk value;
				value.Initialize(context, {lowered->return_type});
				executor.Execute(first, value);
				evaluated.scale =
				    DecideQueryWideScale(value.data[0].GetValue(0), term.scale_divides, what,
				                         {DecideValueRole::COEFFICIENT, *term.scale, names, clause}, null_factor);
			} catch (std::exception &ex) {
				auto rank = tree.nodes[body.composed[t]]->clause_rank;
				rank.part = t;
				rank.expression = DecideBlockExpression(DecideBlock::TERMS, 0, false);
				rank.row = 0;
				errors.Add(rank, ErrorData(ex));
			}
		}
		// Its members are its G's: the entries of its operand its member tests keep, and a relation-qualified one's
		// first of each class (language/guide.md §5.1). Every other row reads 0.
		auto &members = tree.nodes[body.composed[t]]->compiled;
		const auto site = DecideTreeOperand(tree, body.composed[t]);
		const auto &tested = columns.Tested(site);
		const auto qualified = term.qualifier_scope_idx != DConstants::INVALID_INDEX;
		const auto reads = qualified ? columns.FirstOfEachInstance(site, tested) : tested;
		const auto rows = ReadRows(site, reads, qualified);
		evaluated.filter_mask.assign(num_rows, false);
		for (auto &read : rows) {
			evaluated.filter_mask[read.first] = true;
		}
		for (idx_t i = 0; i < members.members.size(); i++) {
			auto &inner = members.members[i];
			evaluated.variables.push_back(gstate.ReadVariable(inner));
			auto numbers =
			    columns.Numbers(site, members.linear[i].program, reads, nullptr, &members.linear[i], &inner.inputs);
			evaluated.per_term_coefs.emplace_back(num_rows, 0.0);
			for (auto &read : rows) {
				evaluated.per_term_coefs.back()[read.first] = numbers[read.second];
			}
		}
		result.push_back(std::move(evaluated));
	}
	return result;
}

} // namespace duckdb
