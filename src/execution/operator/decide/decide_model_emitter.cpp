//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_model_emitter.cpp
//
// The tree's containers written as stage 06 reads them (decide_model_emitter.hpp).
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_model_emitter.hpp"

#include <set>

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/execution/operator/decide/decide_global_sink_state.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace duckdb {

//! A clause's parts a reducer may filter, whose filters are read in this order: its terms, its product groups.
static idx_t Parts(const DecideConstraint &clause) {
	return clause.lhs_terms.size() + clause.product_groups.size();
}

DecideModelEmitter::DecideModelEmitter(const PhysicalDecide &op, ClientContext &context, const DecideTree &tree,
                                       const DecideDescent &descent, DecideTreeColumns &columns,
                                       DecideGlobalSinkState &gstate, vector<SolverInput::KeyValues> &key_values,
                                       const DecideSourceNames &names, idx_t num_rows)
    : op(op), context(context), tree(tree), descent(descent), columns(columns), gstate(gstate), key_values(key_values),
      names(names), num_rows(num_rows), place_of_row(num_rows, DConstants::INVALID_INDEX) {
}

vector<pair<idx_t, idx_t>> DecideModelEmitter::Keep(idx_t root, vector<idx_t> &rows) {
	vector<pair<idx_t, idx_t>> kept;
	DecideEntryRows(tree, descent, root).ForEach([&](idx_t row, idx_t entry) {
		auto &place = place_of_row[row];
		if (place == DConstants::INVALID_INDEX) {
			place = kept.size();
			kept.emplace_back(row, entry);
			rows.push_back(row);
		} else {
			// A row routed to the root twice is read at the entry it reached last.
			kept[place].second = entry;
		}
	});
	D_ASSERT(std::is_sorted(rows.begin(), rows.end()));
	placed = kept.size();
	return kept;
}

void DecideModelEmitter::Unplace(const vector<idx_t> &rows) {
	for (auto row : rows) {
		place_of_row[row] = DConstants::INVALID_INDEX;
	}
	placed = 0;
}

SolverInput::KeyValues DecideGroupKeyValues(const DecideTree &tree, const DecideDescent &descent, idx_t node) {
	const auto key = tree.nodes[node]->key;
	auto &slots = descent.Containers()[node].groups;
	vector<vector<Value>> values(slots.size());
	for (idx_t slot = 0; slot < slots.size(); slot++) {
		values[slots.ranks[slot]] = descent.Label(key, slots.Key(slot));
	}
	SolverInput::KeyValues key_values;
	for (auto &element : DecideKeyElements(tree.keys, key)) {
		key_values.names.push_back(element.name);
	}
	key_values.values_of = [values](idx_t group) {
		return values[group];
	};
	return key_values;
}

DecideEntryRows::DecideEntryRows(const DecideTree &tree, const DecideDescent &descent, idx_t node) {
	auto &container = descent.Containers()[node];
	if (tree.nodes[node]->per != DecideTreePer::ROW) {
		routed = &container.routed;
	} else {
		rows = &container.rows;
	}
}

// The reads below run once per row a term is read on. Their marks are read through std::vector, whose operator[]
// checks no bound: each index is an entry or a row of the containers the loop walks.

template <class F>
void DecideModelEmitter::ForEachRead(idx_t site, const vector<bool> &reads, bool qualified, F &&read) const {
	const auto entries = descent.Containers()[site].rows.data();
	const std::vector<bool> &marked = reads;
	DecideEntryRows(tree, descent, site).ForEach([&](idx_t row, idx_t entry) {
		if (marked[entry] && (!qualified || entries[entry] == row)) {
			read(row, entry);
		}
	});
}

const vector<bool> &DecideModelEmitter::ReadEntriesOf(idx_t site, bool qualified, vector<bool> &first) {
	const auto &tested = columns.Tested(site);
	if (!qualified) {
		return tested;
	}
	first = columns.FirstOfEachInstance(site, tested);
	return first;
}

const vector<bool> &DecideModelEmitter::ReadEntriesOf(const DecideTermSite &term_site, idx_t qualifier_scope,
                                                      const vector<bool> &candidates, vector<bool> &first,
                                                      vector<bool> &selected_rows) {
	if (qualifier_scope == DConstants::INVALID_INDEX) {
		return candidates;
	}
	// The same keyed scope can occur in several nested reducers. Read the operand of this exact occurrence so a
	// sibling's `when` or `by` cannot select or drop this term's rows.
	const auto &qualified = columns.Tested(term_site.qualifier_operand);
	const auto qualifier_first = columns.FirstOfEachInstance(term_site.qualifier_operand, qualified);
	selected_rows.assign(num_rows, false);
	std::vector<bool> &marked_rows = selected_rows;
	for (auto read : ReadRows(term_site.qualifier_operand, qualifier_first, true)) {
		marked_rows[read.first] = true;
	}
	first.assign(candidates.size(), false);
	std::vector<bool> &selected_entries = first;
	const std::vector<bool> &marked = candidates;
	DecideEntryRows(tree, descent, term_site.node).ForEach([&](idx_t row, idx_t entry) {
		if (marked_rows[row] && marked[entry]) {
			selected_entries[entry] = true;
		}
	});
	return first;
}

template <class F>
void DecideModelEmitter::ForEachTermRead(idx_t site, const vector<bool> &reads, idx_t qualifier_scope,
                                         const vector<bool> &selected_rows, F &&read) const {
	if (qualifier_scope == DConstants::INVALID_INDEX) {
		ForEachRead(site, reads, false, std::forward<F>(read));
		return;
	}
	const std::vector<bool> &marked = reads;
	const std::vector<bool> &chosen_rows = selected_rows;
	DecideEntryRows(tree, descent, site).ForEach([&](idx_t row, idx_t entry) {
		if (marked[entry] && chosen_rows[row]) {
			read(row, entry);
		}
	});
}

template <class F>
void DecideModelEmitter::Filter(idx_t site, idx_t qualifier_scope, const vector<bool> &reads,
                                const vector<bool> &selected_rows, TermFilterState &filter, vector<bool> &members,
                                F &&read) {
	const auto &tested = columns.Tested(site);
	auto &container = descent.Containers()[site];
	filter.has_filter = qualifier_scope != DConstants::INVALID_INDEX;
	filter.mask.assign(placed, false);
	members.assign(placed, false);
	std::vector<bool> &mask = filter.mask;
	std::vector<bool> &member = members;
	const auto places = place_of_row.data();
	DecideEntryRows(tree, descent, site).ForEach([&](idx_t row, idx_t entry) {
		// A row the clause does not keep is read by none of its rows.
		const auto place = places[row];
		if (place == DConstants::INVALID_INDEX) {
			return;
		}
		// A row in no class of the reducer's own `per` key is no member of it (language/guide.md §5.1).
		if (tested[entry] && container.groups.InAClass(container.Slot(entry))) {
			member[place] = true;
		}
		if (reads[entry] && (qualifier_scope == DConstants::INVALID_INDEX || selected_rows[row])) {
			mask[place] = true;
			read(place, row, entry);
		}
	});
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
				group = groups[scope].ranks[group];
			}
		}
	}
	return mappings;
}

//! Where each field's values start in an entry's signature (DecideDescent::Signature), and where they end: a value, a
//! decision and its instance, a key's elements, a frame's timeline and, read relative to the anchor, its order key.
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
		} else {
			auto &frame = tree.nodes[tree.frame_nodes[field.frame]]->Cast<DecideFrameNode>();
			start += DecideKeyElements(tree.keys, frame.key).size() + (tree.frames[frame.frame].Relative() ? 1 : 0);
		}
		starts.push_back(start);
	}
	return starts;
}

vector<Value> DecideModelEmitter::Fallback(idx_t frame, idx_t row) const {
	const bool reaches = tree.frames[frame].fallback && gstate.frame_maps[frame].Missing(row);
	vector<Value> read {Value::BOOLEAN(reaches)};
	if (!reaches) {
		return read;
	}
	for (auto j : tree.frame_reads.fallback_columns[frame]) {
		read.push_back(columns.FrameValue(j, row));
	}
	for (auto decision : tree.frame_reads.fallback_decisions[frame]) {
		auto &scope = tree.variable_scopes[decision];
		if (!scope.IsGlobal()) {
			read.push_back(Value::UBIGINT(scope.IsKeyed() ? descent.DecisionRows()[scope.keyed_scope_idx][row] : row));
		}
	}
	return read;
}

DecideVariantDifference DecideModelEmitter::ObjectiveDifference(idx_t root, const vector<idx_t> &surviving) const {
	auto &node = *tree.nodes[root];
	auto &container = descent.Containers()[root];
	const auto starts = SignatureStarts(tree, node);
	auto varies = [&](idx_t column, string &values) {
		if (column >= container.layout.signature.size()) {
			return false;
		}
		// The range holds the values it is given, so they outlive it.
		vector<Value> read;
		for (auto entry : surviving) {
			read.push_back(descent.Signature(root, entry)[column]);
		}
		DecideValueRange range;
		for (auto &value : read) {
			range.Add(value);
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
	// A frame's keys read at each entry's row: a position by its least spelling (D69), a timeline by its signature's
	// values, and what its `else` reads compared as the variants compare it (Identity).
	read.frame = [&](idx_t field) {
		const auto f = node.signature[field].frame;
		auto &map = gstate.frame_maps[f];
		DecideFrameReader frame;
		for (auto entry : surviving) {
			const auto row = container.rows[entry];
			frame.rows.emplace_back(map.timeline[row], map.peer[row]);
		}
		frame.positions = [&](idx_t first, idx_t last) {
			auto name = [&](idx_t peer) {
				return columns.PositionName(f, map.peer_rows[map.peer_begin[peer]]);
			};
			return make_pair(name(first), name(last));
		};
		frame.timeline = [&](idx_t element, const Expression &, string &values) {
			return varies(starts[field] + element, values);
		};
		frame.fallbacks = [&]() {
			const auto first = Fallback(f, container.rows[surviving[0]]);
			return std::any_of(surviving.begin(), surviving.end(), [&](idx_t entry) {
				const auto fallback = Fallback(f, container.rows[entry]);
				return DecideDescentKeyLess(fallback, first) || DecideDescentKeyLess(first, fallback);
			});
		};
		return DecideFrameDifference(node.signature[field], tree.frames[f], tree.keys, frame);
	};
	return DecideObjectiveTwice(node.signature, tree.keys, tree.variable_scopes, tree.frames, op.display.source_text,
	                            read);
}

void DecideModelEmitter::KeyedMappings(vector<KeyedMapping> &mappings) {
	mappings = DecideDeclaredMappings(descent);
	for (idx_t scope = 0; scope < tree.keyed_scopes.size(); scope++) {
		if (tree.keyed_scopes[scope].signature != DConstants::INVALID_INDEX) {
			mappings[scope] = SignatureMapping(scope);
		}
	}
	keyed = &mappings;
	ReadAbsence(mappings);
}

DecideInstanceOfRow DecideModelEmitter::InstanceOfRow(idx_t variable) const {
	DecideInstanceOfRow instance;
	// A frame read is numbered past the plan's decisions and is read per row.
	if (variable < tree.variable_scopes.size()) {
		auto &scope = tree.variable_scopes[variable];
		instance.query_wide = scope.IsGlobal();
		if (scope.IsKeyed()) {
			instance.class_of_row = (*keyed)[scope.keyed_scope_idx].row_to_class.data();
		}
	}
	return instance;
}

idx_t DecideModelEmitter::Instances(idx_t variable) const {
	if (variable >= tree.variable_scopes.size()) {
		return num_rows;
	}
	auto &scope = tree.variable_scopes[variable];
	return scope.IsGlobal() ? 1 : scope.IsKeyed() ? (*keyed)[scope.keyed_scope_idx].num_classes : num_rows;
}

void DecideModelEmitter::AbsentInstances() {
	gstate.absent_instances.assign(gstate.absent_reads.size(), vector<bool>());
	for (idx_t variable = 0; variable < gstate.absent_reads.size(); variable++) {
		auto &absent = gstate.absent_reads[variable];
		if (absent.empty()) {
			continue;
		}
		// An instance is read where some row that reads it has a value.
		auto &none = gstate.absent_instances[variable];
		none.assign(Instances(variable), true);
		const auto instance = InstanceOfRow(variable);
		for (idx_t row = 0; row < absent.size(); row++) {
			if (!absent[row]) {
				none[instance.At(row)] = false;
			}
		}
	}
}

void DecideModelEmitter::FrameReads() {
	for (auto &read : gstate.frame_reads) {
		SolverInput::FrameRead frame_read;
		frame_read.base = read.first;
		auto &map = gstate.frame_maps[read.second];
		const auto instance = InstanceOfRow(read.first);
		frame_read.instances.resize(num_rows);
		for (idx_t row = 0; row < num_rows; row++) {
			const auto selected = map.SelectedRow(row);
			frame_read.instances[row] = instance.At(selected == DConstants::INVALID_INDEX ? row : selected);
		}
		gstate.evaluated_frame_reads.push_back(std::move(frame_read));
	}
}

vector<DecideInstanceOfRow> DecideModelEmitter::InstancesOfRows(const vector<idx_t> &variables) const {
	vector<DecideInstanceOfRow> instances;
	for (auto variable : variables) {
		instances.push_back(InstanceOfRow(variable));
	}
	return instances;
}

KeyedMapping DecideModelEmitter::SignatureMapping(idx_t scope) {
	// The reducer's operand, whose instances are its classes split by the values its body reads (D70, D71).
	const auto operand = tree.qualified_operands[scope];
	if (operand == DConstants::INVALID_INDEX) {
		throw InternalException("DECIDE tree: no reducer is qualified by keyed scope %llu", scope);
	}
	auto &signature = tree.signatures[tree.keyed_scopes[scope].signature];
	const auto variants = Variants(operand, &signature);
	KeyedMapping mapping;
	mapping.num_classes = variants.count;
	// How many classes of the key those values come from, for EXPLAIN ANALYZE (D71).
	if (variants.count > variants.num_classes) {
		op.RecordVariants(signature, "values", variants.count, variants.num_classes);
	}
	// A row the reducer does not read has no class.
	mapping.row_to_class.assign(num_rows, DConstants::INVALID_INDEX);
	const auto rows = DecideEntryRows(tree, descent, operand);
	for (idx_t place = 0; place < rows.size(); place++) {
		mapping.row_to_class[rows[place].first] = variants.of_entry[rows[place].second];
	}
	DecideMapRowsInNoClass(mapping);
	return mapping;
}

DecideModelEmitter::LinkMembers DecideModelEmitter::Members(idx_t node) {
	auto &body = tree.nodes[node]->compiled;
	const auto site = tree.OperandOf(node);
	auto &entries = descent.Containers()[site];
	auto &slots = descent.Containers()[node].groups;
	// The members are computed on the rows its reducer's `when` and NULL tests keep where every decision they read
	// has an instance, a row in no group of its `by` key among them, and a
	// relation-qualified reducer lists each class once in its group, at its first row (language/guide.md §5.1).
	LinkMembers members;
	members.reads = columns.Tested(site);
	for (auto &term : body.members) {
		if (!columns.Absence().MayBe(term)) {
			continue;
		}
		for (idx_t e = 0; e < entries.Count(); e++) {
			members.reads[e] = members.reads[e] && !columns.Absence().TermAt(term, entries.rows[e]);
		}
	}
	const auto qualified = tree.nodes[node]->Cast<DecideReducerNode>().qualifier_scope != DConstants::INVALID_INDEX;
	vector<bool> held(slots.size(), false);
	for (auto &read :
	     ReadRows(site, qualified ? columns.FirstOfEachInstance(site, members.reads) : members.reads, qualified)) {
		const auto slot = entries.parent_slots[read.second];
		if (slots.InAClass(slot)) {
			members.rows.push_back(read);
			held[slot] = true;
		}
	}
	// A group's number is its rank among the groups that hold a member, as groups are numbered, by value.
	vector<idx_t> by_rank(slots.size());
	for (idx_t slot = 0; slot < slots.size(); slot++) {
		by_rank[slots.ranks[slot]] = slot;
	}
	members.group_of_slot.assign(slots.size(), DConstants::INVALID_INDEX);
	for (auto slot : by_rank) {
		if (held[slot]) {
			members.group_of_slot[slot] = members.groups++;
		}
	}
	return members;
}

KeyedMapping DecideModelEmitter::HelperMapping(idx_t node_index) {
	auto &node = *tree.nodes[node_index];
	if (node.kind == DecideNodeKind::FRAME) {
		// A range frame's helper: a class per window of its frame.
		auto mapping = DecideFrameWindowMapping(gstate.frame_maps[node.Cast<DecideFrameNode>().frame]);
		DecideMapRowsInNoClass(mapping);
		return mapping;
	}
	auto &members = link_members[node_index];
	members = Members(node_index);
	KeyedMapping mapping;
	mapping.num_classes = members.groups;
	mapping.row_to_class.assign(num_rows, DConstants::INVALID_INDEX);
	// A group with no member has no helper, so a row of it reads none.
	auto &reducer = descent.Containers()[node_index];
	for (auto read : DecideEntryRows(tree, descent, node_index)) {
		mapping.row_to_class[read.first] = members.group_of_slot[reducer.Slot(read.second)];
	}
	DecideMapRowsInNoClass(mapping);
	return mapping;
}

void DecideModelEmitter::ReadAbsence(vector<KeyedMapping> &mappings) {
	gstate.absent_reads.assign(tree.decide_variables.size(), vector<bool>());
	columns.Absence().ReadClasses(mappings, gstate.absent_reads);
	gstate.evaluated_bilinear_links.clear();
	for (auto &definition : tree.definitions) {
		auto product = definition->TryCast<DecideBooleanProductDefinition>();
		if (!product) {
			continue;
		}
		BilinearLinkSpec link;
		link.aux_idx = product->helper;
		link.bool_var_idx = product->bool_factor;
		link.other_var_idx = product->other_factor;
		link.origin = product->origin;
		gstate.evaluated_bilinear_links.push_back(link);
	}
	// The rows defining each auxiliary (DecideConstraint::defined_var) and each helper's G, in the tree's order, which
	// puts what defines a decision before what reads it (D88): an auxiliary or a helper a definition or a helper's
	// member reads has its absence by then, whatever order the rewrites numbered them in.
	vector<idx_t> position(tree.nodes.size());
	for (idx_t i = 0; i < tree.eval_order.size(); i++) {
		position[tree.eval_order[i]] = i;
	}
	auto definitions = ClauseRoots();
	definitions.resize(tree.constraint_count);
	definitions.insert(definitions.end(), tree.function_nodes.begin(), tree.function_nodes.end());
	link_members.assign(tree.nodes.size(), LinkMembers());
	for (auto node : tree.eval_order) {
		if (tree.nodes[node]->Linked()) {
			definitions.push_back(node);
		}
	}
	std::sort(definitions.begin(), definitions.end(),
	          [&](idx_t left, idx_t right) { return position[left] < position[right]; });
	for (auto root : definitions) {
		auto &node = *tree.nodes[root];
		if (node.kind == DecideNodeKind::FUNCTION) {
			FunctionAbsence(root);
			for (auto row : node.Cast<DecideFunctionNode>().rows) {
				DefinitionAbsence(row, tree.ValueVar(node));
			}
		} else if (tree.OwnedBy(node, DecideHelperKind::L0_INDICATOR) ||
		           tree.OwnedBy(node, DecideHelperKind::DEFINITION)) {
			// The owning F node resolves absence from its linked rows.
		} else if (node.Linked()) {
			mappings[tree.variable_scopes[tree.ValueVar(node)].keyed_scope_idx] = HelperMapping(root);
		} else if (node.compiled.clause && node.compiled.clause->defined_var != DConstants::INVALID_INDEX &&
		           !tree.OwnedBy(node, DecideHelperKind::ABS)) {
			DefinitionAbsence(root, node.compiled.clause->defined_var);
		}
	}
	// A product's auxiliary has none where a factor has none (DecideAbsence::At), which stage 06 reads too. One per
	// row reads a keyed factor at its row's class.
	for (auto &product : gstate.evaluated_bilinear_links) {
		auto &scope = tree.variable_scopes[product.aux_idx];
		const bool per_row = !scope.IsKeyed() && !scope.IsGlobal();
		for (auto factor : {std::make_pair(product.bool_var_idx, &product.bool_instances),
		                    std::make_pair(product.other_var_idx, &product.other_instances)}) {
			const auto instance = InstanceOfRow(factor.first);
			for (idx_t row = 0; per_row && instance.class_of_row && row < num_rows; row++) {
				factor.second->push_back(instance.At(row));
			}
		}
		auto &absent = gstate.absent_reads[product.aux_idx];
		if (!columns.Absence().MayBe(product.aux_idx)) {
			continue;
		}
		for (idx_t row = 0; row < num_rows; row++) {
			if (columns.Absence().At(product.aux_idx, row)) {
				absent.resize(num_rows, false);
				absent[row] = true;
			}
		}
	}
}

void DecideModelEmitter::FunctionAbsence(idx_t function) {
	const auto &node = *tree.nodes[function];
	const auto variable = tree.ValueVar(node);
	if (variable < tree.variable_scopes.size() && tree.variable_scopes[variable].IsGlobal()) {
		return;
	}
	auto &absent = gstate.absent_reads[variable];
	vector<bool> present(num_rows, false);
	const auto &container = descent.Containers()[function];
	for (idx_t entry = 0; entry < container.Count(); entry++) {
		const auto row = container.rows[entry];
		if (row < num_rows && container.groups.InAClass(container.Slot(entry))) {
			present[row] = true;
		}
	}
	absent.assign(num_rows, false);
	for (idx_t row = 0; row < num_rows; row++) {
		absent[row] = !present[row];
	}
}

void DecideModelEmitter::DefinitionAbsence(idx_t root, idx_t variable) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	auto &absent = gstate.absent_reads[variable];
	const bool reducer_definition = tree.OwnedBy(*tree.nodes[root], DecideHelperKind::DEFINITION);
	auto mark = [&](idx_t row) {
		absent.resize(num_rows, false);
		absent[row] = true;
	};
	// A definition has no value where a decision it reads has no instance. That also means it does not read the
	// definition's data expressions there: a cast or arithmetic error in a row the helper drops must stay dropped.
	// Compute the full decision mask first, before reading its bound or coefficients.
	vector<const DecideTerm *> dependencies;
	for (auto &term : clause.lhs_terms) {
		dependencies.push_back(&term);
	}
	for (auto &group : clause.product_groups) {
		for (auto *form : {&group.left, &group.right}) {
			for (auto &term : *form) {
				dependencies.push_back(&term);
			}
		}
	}
	vector<bool> dependency_absent(num_rows, false);
	for (auto term : dependencies) {
		if (!columns.Absence().MayBe(*term)) {
			continue;
		}
		for (idx_t row = 0; row < num_rows; row++) {
			dependency_absent[row] = dependency_absent[row] || columns.Absence().TermAt(*term, row);
		}
	}
	for (idx_t row = 0; row < num_rows; row++) {
		if (dependency_absent[row]) {
			mark(row);
		}
	}
	// What it reads unreduced is NULL on a row it keeps, which its NULL tests keep too (DecideNode::member_tests):
	// its bound, then each term's coefficient, then each product group's forms' terms'. It reads no group's weight.
	auto null_rows = [&](idx_t node, idx_t program) {
		auto reads = columns.Tested(node);
		const auto &rows = descent.Containers()[node].rows;
		for (idx_t entry = 0; entry < reads.size(); entry++) {
			if (dependency_absent[rows[entry]]) {
				reads[entry] = false;
			}
		}
		const auto nulls = columns.NullEntries(node, program, reads);
		if (nulls.empty()) {
			return;
		}
		const std::vector<bool> &null_entries = nulls;
		DecideEntryRows(tree, descent, node).ForEach([&](idx_t row, idx_t entry) {
			// A NULL body excludes this row from its reducer's members. The signed helper can remain an unconstrained
			// value there; only a missing decision input makes the function value absent.
			if (null_entries[entry] && !reducer_definition) {
				mark(row);
			}
		});
	};
	null_rows(root, body.rhs);
	for (idx_t t = 0; t < clause.lhs_terms.size(); t++) {
		null_rows(body.linear[t].node, body.linear[t].program);
	}
	for (auto &sites : body.groups) {
		for (auto *form : {&sites.left, &sites.right}) {
			for (auto &site : *form) {
				null_rows(site.node, site.program);
			}
		}
	}
}

vector<idx_t> DecideModelEmitter::ClauseRoots() const {
	vector<idx_t> roots(tree.constraint_count + tree.singleton_row_count, DConstants::INVALID_INDEX);
	for (idx_t root = 0; root < tree.nodes.size(); root++) {
		auto &body = tree.nodes[root]->compiled;
		if (body.clause) {
			roots[body.clause_index] = root;
		}
	}
	return roots;
}

void DecideModelEmitter::Constraints() {
	const idx_t num_constraints = tree.constraint_count;
	const auto roots = ClauseRoots();
	vector<idx_t> abs_function_of_clause(roots.size(), DConstants::INVALID_INDEX);
	for (auto function : tree.function_nodes) {
		const auto &node = *tree.nodes[function];
		if (!tree.IsFunction(node, DecideHelperKind::ABS)) {
			continue;
		}
		for (auto root : node.Cast<DecideFunctionNode>().rows) {
			abs_function_of_clause[tree.nodes[root]->compiled.clause_index] = function;
		}
	}
	// The rows `M * z >= e` and `M * z >= -e` of a count of nonzeros written with no M, by clause.
	vector<idx_t> sized_indicator_of_clause(roots.size(), DConstants::INVALID_INDEX);
	for (auto function : tree.function_nodes) {
		const auto &node = *tree.nodes[function];
		if (!tree.IsFunction(node, DecideHelperKind::L0_INDICATOR)) {
			continue;
		}
		auto &indicator = tree.Definition(node)->Cast<DecideL0IndicatorDefinition>();
		for (idx_t r = 0; indicator.auto_m && r < DecideL0IndicatorDefinition::TOLERANCE_ROW; r++) {
			sized_indicator_of_clause[tree.nodes[node.Cast<DecideFunctionNode>().rows[r]]->compiled.clause_index] =
			    indicator.helper;
		}
	}
	// The folded bounds follow under DIAGNOSE, numbered after every other clause: only the repair model emits them.
	const idx_t num_evaluated = num_constraints + (tree.diagnose ? tree.singleton_row_count : 0);
	// A decision with no instance has no value wherever a clause reads it (D84), a folded bound included: the data
	// answers it as for a row, which counts the bound's skipped instances and warns when it constrains nothing, and the
	// bound stays in the box. A clause whose decisions have an instance at every row reads nothing here.
	for (idx_t c = num_evaluated; c < roots.size() && !tree.diagnose; c++) {
		auto &clause = *tree.nodes[roots[c]]->compiled.clause;
		const DecideAbsentReads absent_reads(columns.Absence(), clause.lhs_terms);
		if (clause.lhs_is_aggregate || !absent_reads.Any()) {
			continue;
		}
		// An easy MIN's or MAX's term stands on its members, which Groups reads, as for the clauses below.
		if (clause.was_minmax_easy) {
			Groups(roots[c], true);
		} else {
			Rows(roots[c], true);
		}
	}
	for (idx_t c = 0; c < num_evaluated; c++) {
		const auto root = roots[c];
		auto &clause = *tree.nodes[root]->compiled.clause;
		if (abs_function_of_clause[c] != DConstants::INVALID_INDEX) {
			// An ABS's envelope rows are no clauses of the model: stage 06 states the rows of the ABS from them.
			AbsEnvelope(abs_function_of_clause[c], root, c < num_constraints);
			continue;
		}
		auto emitted = clause.lhs_is_aggregate || clause.was_minmax_easy ? Groups(root, c < num_constraints)
		                                                                 : Rows(root, c < num_constraints);
		emitted.row.sized_indicator = sized_indicator_of_clause[c];
		Route(std::move(emitted), c >= num_constraints);
	}
}

DecideClauseRef DecideModelEmitter::Written(const DecideConstraint &clause) const {
	return DecideClauseRefOf(op.display.constraint_sources, op.display.source_text, clause.origin.source_clause_id);
}

void DecideModelEmitter::Route(EmittedClause &&clause, bool folded) {
	auto &sets = clause.sets;
	EvaluatedConstraint &row = clause.row;
	if (folded) {
		gstate.evaluated_singleton_row_constraints.push_back(std::move(row));
	} else if (row.comparison_type == ExpressionType::COMPARE_NOTEQUAL) {
		NotEqualClause not_equal;
		// An aggregate `<>` excludes every value its group's rows give, each once; an AVG's terms stay sums, so its
		// values are scaled by the group's size instead (KeepAvgAsSumUnderNotEqual).
		for (idx_t e = 0; row.lhs_is_aggregate && e < sets.groups.size(); e++) {
			const auto first = NumericCast<int64_t>(not_equal.excluded.size());
			const double scale = clause.avg_rhs_scale ? static_cast<double>(sets.begin[e + 1] - sets.begin[e]) : 1.0;
			not_equal.excluded_begin.push_back(not_equal.excluded.size());
			for (idx_t k = sets.begin[e]; k < sets.begin[e + 1]; k++) {
				const idx_t place = sets.rows[k];
				if (!clause.no_exclusion.empty() && clause.no_exclusion[place]) {
					continue;
				}
				const double value = clause.row.rhs_values.Get(place) * scale;
				if (std::find(not_equal.excluded.begin() + first, not_equal.excluded.end(), value) ==
				    not_equal.excluded.end()) {
					not_equal.excluded.push_back(value);
				}
			}
		}
		not_equal.excluded_begin.push_back(not_equal.excluded.size());
		not_equal.row = std::move(row);
		gstate.evaluated_not_equals.push_back(std::move(not_equal));
	} else if (!clause.products.empty()) {
		gstate.evaluated_quadratics.push_back({std::move(row), std::move(clause.products), std::move(sets.begin),
		                                       std::move(sets.rows), std::move(clause.rows)});
	} else {
		gstate.evaluated_constraints.push_back(std::move(row));
	}
}

void DecideModelEmitter::AbsEnvelope(idx_t function, idx_t root, bool counted) {
	auto &node = *tree.nodes[function];
	auto &definition = tree.Definition(node)->Cast<DecideAbsDefinition>();
	const auto &envelopes = node.Cast<DecideFunctionNode>().rows;
	auto link = std::find_if(gstate.evaluated_abs_links.begin(), gstate.evaluated_abs_links.end(),
	                         [&](const AbsLinkSpec &candidate) { return candidate.aux_idx == definition.helper; });
	if (link == gstate.evaluated_abs_links.end()) {
		gstate.evaluated_abs_links.emplace_back();
		link = gstate.evaluated_abs_links.end() - 1;
		link->aux_idx = definition.helper;
		link->exact = definition.exact;
		link->written = definition.label;
		link->label = node.text;
	}
	// An envelope row holds no aggregate (stage 05 gives every reducer a function reads a helper decision) and is
	// built fresh, never the easy half of a MIN/MAX: it is one row per routed instance of the F edge's scope.
	auto envelope = Rows(root, counted);
	D_ASSERT(envelope.products.empty());
	EvaluatedConstraint &row = envelope.row;
	(root == envelopes[DecideAbsDefinition::NEGATED_ROW] ? link->negated : link->operand) = std::move(row);
}

EmittedClause DecideModelEmitter::Rows(idx_t root, bool counted) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	auto &node = *tree.nodes[root];
	const auto entries = descent.Containers()[root].Count();
	EmittedClause result;
	result.row = InitializeEvaluatedConstraint(clause, body.clause_index, [&](const DecideTerm &term) {
		return gstate.ReadVariable(term.variable_index, term.frame_read);
	});
	auto &evaluated = result.row;
	// The rows its root holds, which its `when` routed, each numbered by its place among them and read at the entry
	// standing for it. The clause keeps those its NULL tests keep (DecideNode::member_tests: a helper's row reads a
	// reducer's body on the reducer's members): one model row stands on each, or under `per K:` on one row of each
	// variant of a class.
	const auto rows = Keep(root, result.rows);
	const idx_t kept = rows.size();
	vector<bool> tested = columns.Tested(root);
	vector<bool> definition_absent(kept, false);
	if (tree.OwnedBy(node, DecideHelperKind::DEFINITION) && clause.defined_var < gstate.absent_reads.size()) {
		const auto &absent = gstate.absent_reads[clause.defined_var];
		const auto &entry_rows = descent.Containers()[root].rows;
		for (idx_t entry = 0; entry < tested.size(); entry++) {
			const auto row = entry_rows[entry];
			if (row < absent.size() && absent[row]) {
				tested[entry] = false;
				definition_absent[place_of_row[row]] = true;
			}
		}
	}
	const std::vector<bool> &kept_entries = tested;
	// Each term's coefficient and the bound, at the root's entries.
	vector<bool> nulls(entries, false);
	vector<vector<double>> coefficients;
	for (idx_t t = 0; t < clause.lhs_terms.size(); t++) {
		auto &site = body.linear[t];
		D_ASSERT(site.node == root);
		coefficients.push_back(columns.Numbers(root, site.program, tested, &nulls));
	}
	if (clause.scope.per == DecideClauseScope::PerRole::ONE_ROW_PER_CLASS) {
		Classes(root, rows, result);
	} else if (clause.scope.when) {
		evaluated.row_group_ids.assign(kept, DConstants::INVALID_INDEX);
		for (idx_t place = 0; place < kept; place++) {
			if (kept_entries[rows[place].second]) {
				evaluated.row_group_ids[place] = 0;
			}
		}
		evaluated.num_groups = 1;
	}
	const auto instances = CountInstances(evaluated.row_group_ids, evaluated.num_groups, kept, false);
	vector<double> bound;
	const auto bound_is_read = DescribeBound(clause, kept, tree.diagnose, op.display.source_text,
	                                         op.display.constraint_sources, tree.keyed_scopes, tree.keys, evaluated);
	if (bound_is_read) {
		bound = columns.Numbers(root, body.rhs, tested, &nulls);
	}
	// A term reading a decision with no instance at the row has no value there (D84), nor has an auxiliary where what
	// defines it has none (D88).
	const DecideAbsentReads absent_reads(columns.Absence(), clause.lhs_terms);
	vector<bool> null_rows = std::move(definition_absent);
	std::vector<bool> &nulled = null_rows;
	const std::vector<bool> &null_entries = nulls;
	for (idx_t place = 0; place < kept; place++) {
		const auto at = rows[place];
		if (kept_entries[at.second]) {
			nulled[place] = null_entries[at.second] || absent_reads.At(at.first);
		}
	}
	if (clause.admitted_lower) {
		Admit(body, evaluated);
	}
	// Its product groups, which no reducer filters.
	Polynomial(root, vector<TermFilterState>(Parts(clause)), null_rows, result);
	DecideAbsence::SkipNullInstances(null_rows, false, kept, evaluated.row_group_ids, evaluated.num_groups);
	if (counted) {
		gstate.CountInstances(evaluated.provenance.origin.source_clause_id, instances,
		                      CountInstances(evaluated.row_group_ids, evaluated.num_groups, kept, false));
		if (clause.admitted_lower) {
			CheckDomain(root, evaluated);
		}
	}
	// The clause's rows, written from the entries: one per row its group keeps, every row with no group. An entry the
	// NULL tests drop reads nothing: a row standing on one holds no term and a bound of 0.
	auto &records = evaluated.records;
	const auto written = Written(clause);
	const auto &variables = evaluated.variable_indices;
	const bool admits = evaluated.admitted_lower.logical_size != 0;
	// Read through raw pointers: the loop runs once per kept row, and each index is a place or an entry of the lists
	// it walks.
	const auto groups = evaluated.row_group_ids.empty() ? nullptr : evaluated.row_group_ids.data();
	const auto kept_rows = rows.data();
	const auto bounds = bound.data();
	vector<const double *> term_values;
	for (auto &numbers : coefficients) {
		term_values.push_back(numbers.data());
	}
	const auto terms = term_values.data();
	const auto decisions = variables.data();
	const auto instances_of_rows = InstancesOfRows(variables);
	const auto term_instances = instances_of_rows.data();
	const idx_t term_count = variables.size();
	for (idx_t place = 0; place < kept; place++) {
		if (groups && groups[place] == DConstants::INVALID_INDEX) {
			continue;
		}
		const auto row = kept_rows[place].first;
		const auto entry = kept_rows[place].second;
		const bool read = kept_entries[entry];
		double constant = 0.0;
		for (idx_t t = 0; read && t < term_count; t++) {
			const double coefficient = terms[t][entry];
			if (decisions[t] == DConstants::INVALID_INDEX) {
				AddCoefficient(constant, coefficient, written);
			} else if (coefficient != 0.0) {
				records.variables.push_back(decisions[t]);
				records.instances.push_back(term_instances[t].At(row));
				records.coefficients.push_back(coefficient);
			}
		}
		records.term_begin.push_back(records.variables.size());
		records.constants.push_back(constant);
		records.bounds.push_back(!bound_is_read ? evaluated.rhs_values.Get(place) : read ? bounds[entry] : 0.0);
		records.groups.push_back(groups ? groups[place] : DConstants::INVALID_INDEX);
		records.data_rows.push_back(row);
		if (admits) {
			records.admitted_lower.push_back(evaluated.admitted_lower.Get(place));
			records.admitted_upper.push_back(evaluated.admitted_upper.Get(place));
		}
		// A row's product groups are read at its place.
		if (!result.products.empty()) {
			result.sets.rows.push_back(place);
			result.sets.begin.push_back(result.sets.rows.size());
		}
	}
	WriteMembersIn(evaluated);
	Unplace(result.rows);
	return result;
}

void DecideModelEmitter::CheckDomain(idx_t root, const EvaluatedClauseColumns &evaluated) {
	auto &clause = *tree.nodes[root]->compiled.clause;
	// The box check keeps only a lowered floor of an IN's bounds, which diagnosis drops as one clause.
	if (clause.origin.removal_group_id != DConstants::INVALID_INDEX || !clause.admitted_lower->IsFoldable() ||
	    !clause.admitted_upper->IsFoldable()) {
		return;
	}
	const auto var = clause.lhs_terms[0].variable_index;
	if (var >= tree.domains.size() || !tree.domains[var].checked ||
	    clause.origin.source_clause_id >= op.display.constraint_sources.size()) {
		return;
	}
	auto &domain = tree.domains[var];
	// The rows its term's site reads, whose members (an easy MIN's or MAX's reducer filter keeps some) each write a
	// row.
	const auto site = tree.nodes[root]->compiled.linear[0].node;
	auto &members = columns.Tested(site);
	for (auto read : DecideEntryRows(tree, descent, site)) {
		const auto row = place_of_row[read.first];
		// A row is written only for an instance: a row in no class of a `per`, or off a `when`, is never checked.
		if (!members[read.second] || row == DConstants::INVALID_INDEX ||
		    (!evaluated.row_group_ids.empty() && evaluated.row_group_ids[row] == DConstants::INVALID_INDEX)) {
			continue;
		}
		// The ends it admits, an open end (infinite) leaving the box as it is.
		auto box = domain.box;
		if (std::isfinite(evaluated.admitted_upper[row])) {
			DecideFoldSingletonRow(box, '<', evaluated.admitted_upper[row]);
		}
		if (std::isfinite(evaluated.admitted_lower[row])) {
			DecideFoldSingletonRow(box, '>', evaluated.admitted_lower[row]);
		}
		auto refusal = DecideRefuseDomain(box, domain.is_bool, domain.name);
		if (!refusal.message.empty()) {
			throw InvalidInputException(
			    refusal.message, DecideClauseErrorInfo(op.display.constraint_sources, clause.origin.source_clause_id));
		}
	}
}

void DecideModelEmitter::Classes(idx_t root, const vector<pair<idx_t, idx_t>> &rows, EmittedClause &result) {
	auto &clause = *tree.nodes[root]->compiled.clause;
	auto &evaluated = result.row;
	const auto signature =
	    clause.scope.signature == DConstants::INVALID_INDEX ? nullptr : &tree.signatures[clause.scope.signature];
	const auto variants = Variants(root, signature);
	// Each kept row's variant and class, by place.
	vector<idx_t> variant_of, class_of_row;
	for (auto &at : rows) {
		variant_of.push_back(variants.of_entry[at.second]);
		class_of_row.push_back(variants.class_of_entry[at.second]);
	}
	vector<idx_t> class_of, first_rows, variants_per_class;
	const auto defined = clause.defined_var;
	DecideOneRowPerVariant(
	    variant_of, class_of_row, variants.count, variants.num_classes,
	    [&](idx_t place) {
		    return defined < gstate.absent_reads.size() && !gstate.absent_reads[defined].empty() &&
		           gstate.absent_reads[defined][result.rows[place]];
	    },
	    evaluated.row_group_ids, class_of, first_rows, variants_per_class);
	evaluated.num_groups = variants.count;
	// A variant is named by its class, and by its own field values where its class has more than one.
	vector<idx_t> class_ids;
	vector<GroupName> class_names;
	if (clause.scope.class_key != DConstants::INVALID_INDEX) {
		KeyClasses(root, rows, class_ids, class_names);
	}
	if (signature && variants.count > variants.num_classes) {
		op.RecordVariants(*signature, "constraints", variants.count, variants.num_classes);
		evaluated.group_names = NameVariants(root, *signature, rows, class_of, first_rows, variants_per_class);
	} else {
		for (idx_t variant = 0; variant < variants.count && !class_names.empty(); variant++) {
			evaluated.group_names.push_back(class_names[class_of[variant]]);
		}
	}
	if (!tree.diagnose) {
		return;
	}
	// Each variant's row stands for the classes of every kept row it reads (D28), as an aggregate row's group does:
	// two classes that read one value of a helper are one row. It is named as its own where its class has more than
	// one.
	evaluated.group_class_set = DecideGroupClassSets(
	    clause.scope, result.rows, variant_of, variants.count,
	    [&](vector<idx_t> &ids, vector<GroupName> &named) {
		    ids = class_ids;
		    named = class_names;
	    },
	    gstate.class_sets);
	if (signature) {
		DecideNameVariantClassSets(class_of, variants_per_class, evaluated.group_names, evaluated.group_class_set,
		                           gstate.class_sets);
	}
}

vector<Value> DecideModelEmitter::Identity(idx_t node, idx_t entry) const {
	auto &container = descent.Containers()[node];
	auto &fields = tree.nodes[node]->signature;
	const auto starts = SignatureStarts(tree, *tree.nodes[node]);
	// A clause that merges classes tells its variants apart by what its rows read, not by their class.
	auto identity =
	    DecideMergesClasses(*tree.nodes[node]) ? vector<Value>() : container.groups.Key(container.Slot(entry));
	const auto read = descent.Signature(node, entry);
	const auto row = container.rows[entry];
	for (idx_t i = 0; i < fields.size(); i++) {
		if (fields[i].kind != DecideSignatureFieldKind::FRAME) {
			identity.insert(identity.end(), read.begin() + starts[i], read.begin() + starts[i + 1]);
			continue;
		}
		// A frame by its keys (D82): the anchor's position, as its order key compares, when it reads
		// relative to the anchor, else its timeline; and what its `else` reads where the anchor reaches it, at the row
		// an entry stands for.
		const auto f = fields[i].frame;
		auto &map = gstate.frame_maps[f];
		identity.push_back(Value::UBIGINT(tree.frames[f].Relative() ? map.peer[row] : map.timeline[row]));
		auto fallback = Fallback(f, row);
		identity.insert(identity.end(), fallback.begin(), fallback.end());
	}
	return identity;
}

DecideVariants DecideModelEmitter::Variants(idx_t node, const DecideSignature *signature, bool by_row) const {
	auto &container = descent.Containers()[node];
	const auto entries = container.Count();
	// They are numbered by value, their class's and then their fields', unless a field reads a decision's instance
	// or a frame's keys, which number them by their first rows. No field reads a query-wide
	// decision, which one instance holds for every class.
	bool by_rows = false;
	vector<idx_t> frames;
	for (idx_t f = 0; signature && f < signature->fields.size(); f++) {
		auto &field = signature->fields[f];
		by_rows = by_rows || field.kind == DecideSignatureFieldKind::DECISION ||
		          field.kind == DecideSignatureFieldKind::FRAME;
		if (field.kind == DecideSignatureFieldKind::FRAME) {
			frames.push_back(field.frame);
		}
	}
	// A variant is a class of the node's key and the values its sigma reads. Entries are made in row order, so a
	// variant's first entry holds its first row.
	auto less = [](const vector<Value> &left, const vector<Value> &right) {
		return DecideDescentKeyLess(left, right);
	};
	std::map<vector<Value>, idx_t, decltype(less)> variant_ids(less);
	std::map<vector<Value>, idx_t, decltype(less)> class_ids(less);
	vector<vector<Value>> identities(entries);
	// The entries in a class of the node's key: a reducer reads no value of a row in none, which its operand holds
	// (DecideNode::holds_rows_in_no_class). Each counts its class, as the classes are counted over the rows it keeps;
	// one at an anchor where a frame its fields read reads nothing is no variant, since it has no instance (D84).
	vector<idx_t> held;
	for (idx_t e = 0; e < entries; e++) {
		const auto row = container.rows[e];
		if (!container.groups.InAClass(container.Slot(e))) {
			continue;
		}
		class_ids.emplace(container.groups.Key(container.Slot(e)), 0);
		if (std::none_of(frames.begin(), frames.end(), [&](idx_t f) { return gstate.frame_maps[f].Absent(row); })) {
			held.push_back(e);
			identities[e] = Identity(node, e);
			variant_ids.emplace(identities[e], row);
		}
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
	if (!by_row) {
		return result;
	}
	result.of_entry.assign(entries, DConstants::INVALID_INDEX);
	result.class_of_entry.assign(entries, DConstants::INVALID_INDEX);
	for (auto e : held) {
		result.of_entry[e] = variant_ids[identities[e]];
		result.class_of_entry[e] = class_ids[container.groups.Key(container.Slot(e))];
	}
	return result;
}

vector<GroupName> DecideModelEmitter::NameVariants(idx_t root, const DecideSignature &signature,
                                                   const vector<pair<idx_t, idx_t>> &rows,
                                                   const vector<idx_t> &class_of, const vector<idx_t> &first_places,
                                                   const vector<idx_t> &variants_per_class) {
	auto &node = *tree.nodes[root];
	auto &container = descent.Containers()[root];
	auto &names = node.compiled.names;
	// Each variant's first row, and the entry standing for it.
	vector<idx_t> entry_of, first_rows;
	for (auto place : first_places) {
		first_rows.push_back(rows[place].first);
		entry_of.push_back(rows[place].second);
	}
	// Each variant's values as a message shows them: what its class key and its fields read at its first row, but
	// a collated column of its class by the least spelling among the clause's rows of the class
	// (DecideNodeGroups::spellings).
	auto &elements = DecideKeyElements(tree.keys, node.key);
	SolverInput::KeyValues key_values;
	key_values.names = node.compiled.texts;
	vector<vector<Value>> values;
	for (idx_t variant = 0; variant < entry_of.size(); variant++) {
		const auto entry = entry_of[variant];
		const auto slot = container.Slot(entry);
		values.emplace_back();
		for (idx_t i = 0; i < names.size(); i++) {
			const auto collated = i < elements.size() && elements[i].collated;
			values.back().push_back(collated ? container.groups.Spelling(slot).label[i]
			                                 : columns.ValueAt(root, names[i], entry));
		}
		// And a collated position by its least spelling among its rows.
		for (auto &position : node.compiled.position_names) {
			values.back()[position.first] = columns.PositionName(position.second, first_rows[variant]);
		}
	}
	DecideNameVariants(
	    signature.fields, elements.size(), [values](idx_t variant) { return values[variant]; }, class_of,
	    variants_per_class, first_rows, key_values);
	const auto named = this->key_values.size();
	this->key_values.push_back(std::move(key_values));
	vector<GroupName> result;
	for (idx_t variant = 0; variant < first_rows.size(); variant++) {
		result.push_back({named, variant});
	}
	return result;
}

void DecideModelEmitter::KeyClasses(idx_t root, const vector<pair<idx_t, idx_t>> &rows, vector<idx_t> &class_ids,
                                    vector<GroupName> &class_names) {
	auto &container = descent.Containers()[root];
	const auto named = key_values.size();
	key_values.push_back(DecideGroupKeyValues(tree, descent, root));
	// The root's slots by K's values; a grouped root holds its kept rows in no class of K in a slot of its own.
	auto &slots = container.groups;
	vector<idx_t> by_rank(slots.size());
	for (idx_t slot = 0; slot < slots.size(); slot++) {
		by_rank[slots.ranks[slot]] = slot;
	}
	vector<idx_t> class_of_slot(slots.size(), DConstants::INVALID_INDEX);
	for (idx_t rank = 0; rank < by_rank.size(); rank++) {
		const auto slot = by_rank[rank];
		if (slots.InAClass(slot)) {
			class_of_slot[slot] = class_names.size();
			class_names.push_back({named, rank});
		}
	}
	class_ids.resize(rows.size());
	for (idx_t place = 0; place < rows.size(); place++) {
		class_ids[place] = class_of_slot[container.Slot(rows[place].second)];
	}
}

void DecideModelEmitter::ReadBesideReducers(idx_t root, const DecideTerm &term, idx_t program,
                                            const vector<pair<idx_t, idx_t>> &rows, CoefficientColumn &column,
                                            vector<bool> &null_rows) {
	vector<bool> nulls(descent.Containers()[root].Count(), false);
	auto numbers = columns.Numbers(root, program, columns.Tested(root), &nulls);
	DecideAbsentReads absent_reads(columns.Absence());
	absent_reads.Add(term);
	column.AssignDense(rows.size());
	for (idx_t place = 0; place < rows.size(); place++) {
		const auto read = rows[place];
		column.Set(place, numbers[read.second]);
		null_rows[place] = null_rows[place] || nulls[read.second] || absent_reads.At(read.first);
	}
}

vector<pair<idx_t, idx_t>> DecideModelEmitter::ReadRows(idx_t site, const vector<bool> &reads, bool qualified) const {
	vector<pair<idx_t, idx_t>> rows;
	ForEachRead(site, reads, qualified, [&](idx_t row, idx_t entry) { rows.emplace_back(row, entry); });
	return rows;
}

vector<bool> DecideModelEmitter::ReadInReducer(const DecideTerm &term, const DecideTermSite &site, idx_t program,
                                               CoefficientColumn &column, TermFilterState &filter,
                                               vector<bool> &null_rows) {
	// Its member tests are its filter's NULL tests of what its body reads and of its decisions' keys, so every
	// member's decision has an instance (D84); a coefficient is NULL on a member only through a value the whole query
	// shares, which no test reads, and leaves the member's group no value. It is computed on every member, and a
	// relation-qualified one's repeats are left out afterwards.
	vector<bool> nulls(descent.Containers()[site.node].Count(), false);
	vector<bool> first, selected_rows, candidates;
	const auto &present = columns.ReadEntries(site.node, term, candidates);
	const auto &reads = ReadEntriesOf(site, term.qualifier_scope_idx, present, first, selected_rows);
	// Evaluate the coefficient before the relation qualifier removes rows outside its classes. The old lowering reads
	// these values on the reducer's kept rows, then applies the qualifier to the members; a NaN on a row in no class
	// still raises here.
	auto numbers = columns.Numbers(site.node, program, present, &nulls);
	filter.avg_scale = term.avg_scale;
	vector<bool> members;
	column.AssignDense(placed);
	const auto coefficients = column.MutableDense().data();
	const auto values = numbers.data();
	const std::vector<bool> &null_entries = nulls;
	std::vector<bool> &nulled = null_rows;
	DecideAbsentReads absent_reads(columns.Absence());
	absent_reads.Add(term);
	Filter(site.node, term.qualifier_scope_idx, reads, selected_rows, filter, members,
	       [&](idx_t place, idx_t row, idx_t entry) {
		       if (absent_reads.At(row)) {
			       nulled[place] = true;
			       return;
		       }
		       coefficients[place] = values[entry];
		       if (null_entries[entry]) {
			       nulled[place] = true;
		       }
	       });
	return members;
}

idx_t DecideModelEmitter::ByReducer(idx_t root) const {
	auto &node = *tree.nodes[root];
	for (auto child : node.children) {
		if (tree.nodes[child]->kind == DecideNodeKind::REDUCER &&
		    tree.nodes[child]->key == node.compiled.clause->group_key) {
			return child;
		}
	}
	throw InternalException("DECIDE tree: %s reads no G of its `by` key", node.text);
}

bool DecideModelEmitter::ReadGroupedTerms(idx_t root, const vector<pair<idx_t, idx_t>> &rows,
                                          EvaluatedClauseColumns &evaluated, vector<TermFilterState> &filters,
                                          vector<bool> &active, vector<bool> &null_rows) {
	auto &node = *tree.nodes[root];
	auto &body = node.compiled;
	auto &clause = *body.clause;
	auto &terms = clause.lhs_terms;
	bool has_local_filters = false;
	bool unfiltered = false;
	// A filter that keeps every kept row filters nothing.
	auto read_filter = [&](const vector<bool> &members, TermFilterState &filter) {
		const std::vector<bool> &member_rows = members;
		if (std::find(member_rows.begin(), member_rows.end(), false) == member_rows.end()) {
			unfiltered = true;
			return;
		}
		filter.has_filter = true;
		has_local_filters = true;
		std::vector<bool> &active_rows = active;
		for (idx_t row = 0; row < placed; row++) {
			if (member_rows[row]) {
				active_rows[row] = true;
			}
		}
	};
	for (idx_t t = 0; t < terms.size(); t++) {
		auto &site = body.linear[t];
		if (site.node == root) {
			// A term beside the reducers is read on each kept row, and counted once in its group.
			ReadBesideReducers(root, terms[t], site.program, rows, evaluated.row_coefficients[t], null_rows);
			unfiltered = true;
			continue;
		}
		read_filter(ReadInReducer(terms[t], site, site.program, evaluated.row_coefficients[t], filters[t], null_rows),
		            filters[t]);
	}
	// A product group's filter, on the members of its reducer, which is read with the terms'; its weight and its
	// forms' coefficients follow once the rows are grouped (Polynomial).
	vector<bool> members;
	for (idx_t g = 0; g < clause.product_groups.size(); g++) {
		auto &filter = filters[terms.size() + g];
		const auto &site = body.groups[g].weight;
		const auto scope = clause.product_groups[g].qualifier_scope_idx;
		vector<bool> first, selected_rows;
		const auto &reads = ReadEntriesOf(site, scope, columns.Tested(site.node), first, selected_rows);
		Filter(site.node, scope, reads, selected_rows, filter, members, [](idx_t, idx_t, idx_t) {});
		read_filter(members, filter);
	}
	if (unfiltered) {
		std::fill(active.begin(), active.end(), true);
	}
	return has_local_filters;
}

EmittedClause DecideModelEmitter::Groups(idx_t root, bool counted) {
	auto &node = *tree.nodes[root];
	auto &body = node.compiled;
	auto &clause = *body.clause;
	EmittedClause result;
	result.row = InitializeEvaluatedConstraint(clause, body.clause_index, [&](const DecideTerm &term) {
		return gstate.ReadVariable(term.variable_index, term.frame_read);
	});
	auto &evaluated = result.row;
	// The rows the clause keeps: every row its root holds, which its `when` routed, each numbered by its place among
	// them and read at the entry standing for it.
	const auto rows = Keep(root, result.rows);
	const idx_t kept = rows.size();
	vector<bool> null_rows(kept, false);
	// Each term's filter, then each product group's.
	vector<TermFilterState> filters(Parts(clause));
	// The rows some reducer's filter keeps, which alone emit their group's row when every term has a filter.
	vector<bool> active(kept, false);
	const auto has_local_filters = ReadGroupedTerms(root, rows, evaluated, filters, active, null_rows);
	// Each kept row's group of the `by` key: its slot in the clause's G of that key, numbered by value.
	const auto keyed = clause.group_key != DConstants::INVALID_INDEX;
	const auto by = keyed ? ByReducer(root) : DConstants::INVALID_INDEX;
	// A `per K:` that names classes is one instance per class and distinct value its fields read (D70), as a per-row
	// clause's (Classes), though its groups merge the instances.
	if (clause.scope.per == DecideClauseScope::PerRole::NAMES_CLASSES &&
	    clause.scope.signature != DConstants::INVALID_INDEX) {
		auto &signature = tree.signatures[clause.scope.signature];
		const auto variants = Variants(root, &signature, false);
		if (variants.count > variants.num_classes) {
			op.RecordVariants(signature, "constraints", variants.count, variants.num_classes);
		}
	}
	vector<idx_t> group_of(kept, DConstants::INVALID_INDEX);
	idx_t num_key_groups = 0;
	if (keyed) {
		auto &container = descent.Containers()[by];
		const auto entry_rows = container.rows.data();
		const auto ranks = container.groups.ranks.data();
		const auto groups = group_of.data();
		for (idx_t e = 0; e < container.Count(); e++) {
			const auto place = place_of_row[entry_rows[e]];
			if (place != DConstants::INVALID_INDEX) {
				groups[place] = ranks[container.Slot(e)];
			}
		}
		num_key_groups = container.groups.size();
	}
	const DecideKeyGroups key_groups {group_of, num_key_groups};
	idx_t grouped_instances = 0;
	vector<idx_t> kept_row_group_ids;
	// A `per K:` that names classes constrains no row in none of them: each kept row's class.
	auto &root_container = descent.Containers()[root];
	vector<idx_t> class_of_row(kept, DConstants::INVALID_INDEX);
	for (idx_t place = 0; place < kept; place++) {
		const auto slot = root_container.Slot(rows[place].second);
		if (root_container.groups.InAClass(slot)) {
			class_of_row[place] = slot;
		}
	}
	const auto kept_groups = GroupRowsByShape(
	    clause, kept, has_local_filters, active, keyed ? &key_groups : nullptr, class_of_row,
	    tree.diagnose || clause.tightest_frame_bound ? &kept_row_group_ids : nullptr, evaluated, grouped_instances);
	if (keyed) {
		Name(by, kept_groups, evaluated);
	}
	// The classes each group's row stands for, which a diagnosis reports one by one (D28).
	if (tree.diagnose) {
		evaluated.group_class_set = DecideGroupClassSets(
		    clause.scope, result.rows, kept_row_group_ids, evaluated.num_groups,
		    [&](vector<idx_t> &class_ids, vector<GroupName> &class_names) {
			    KeyClasses(root, rows, class_ids, class_names);
		    },
		    gstate.class_sets);
	}
	// A group in which a reducer reads no member has no value there, so neither has its instance (D68).
	vector<const TermFilterState *> reducers;
	for (auto &filter : filters) {
		reducers.push_back(&filter);
	}
	DecideAbsence::SkipEmptyReducers(reducers, kept, evaluated.row_group_ids, evaluated.num_groups);
	// A decision beside the reducers counts once in each group's row; an easy MIN's or MAX's rows are its members',
	// whose terms stage 05 reads as written on each.
	if (clause.lhs_is_aggregate) {
		CountTupleTermsOncePerGroup(clause.lhs_terms, evaluated);
	}
	const auto bound_null = GroupBound(root, rows, null_rows, result);
	if (clause.admitted_lower) {
		Admit(body, evaluated);
	}
	result.avg_rhs_scale = HoistAvgIntoNotEqualBound(clause, filters);
	ScaleAvgTerms(clause, filters, kept, evaluated);
	Polynomial(root, filters, null_rows, result);
	if (!bound_null.empty()) {
		// Taken once every rule that drops an instance has run but the next, which reads `null_rows`.
		TakeTightestFrameBound(evaluated.comparison_type, RenderDecideSource(*clause.rhs_expr, names),
		                       DecideSpanLocation(op.display.source_text, clause.rhs_expr->GetQueryLocation()),
		                       bound_null, kept_row_group_ids, evaluated.row_group_ids, evaluated.num_groups,
		                       evaluated.group_names, key_values, evaluated.rhs_values.MutableDense(), null_rows);
	}
	DecideAbsence::SkipNullInstances(null_rows, true, kept, evaluated.row_group_ids, evaluated.num_groups);
	if (counted) {
		gstate.CountInstances(evaluated.provenance.origin.source_clause_id, grouped_instances,
		                      CountInstances(evaluated.row_group_ids, evaluated.num_groups, kept, true));
		if (clause.admitted_lower) {
			CheckDomain(root, evaluated);
		}
	}
	// The clause's rows, from its columns: one per group that holds a row.
	result.sets = DecideClauseRowSets(evaluated, kept);
	DecideWriteClauseRecords(evaluated, result.sets, result.rows, InstancesOfRows(evaluated.variable_indices),
	                         Written(clause));
	WriteMembersIn(evaluated);
	Unplace(result.rows);
	return result;
}

vector<bool> DecideModelEmitter::GroupBound(idx_t root, const vector<pair<idx_t, idx_t>> &rows, vector<bool> &null_rows,
                                            EmittedClause &result) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	auto &evaluated = result.row;
	const idx_t kept = rows.size();
	if (!DescribeBound(clause, kept, tree.diagnose, op.display.source_text, op.display.constraint_sources,
	                   tree.keyed_scopes, tree.keys, evaluated)) {
		return vector<bool>();
	}
	// A tightest frame bound's NULL bounds nothing, rather than leaving its instance no value (D85), and a row in no
	// class of the clause's `per K:` reads none, as no instance reads it there (TakeTightestFrameBound).
	const auto tightest = clause.tightest_frame_bound;
	vector<bool> bound_null(tightest ? kept : 0, false);
	auto &container = descent.Containers()[root];
	auto reads = columns.Tested(root);
	for (idx_t e = 0; tightest && clause.scope.per == DecideClauseScope::PerRole::NAMES_CLASSES && e < reads.size();
	     e++) {
		reads[e] = reads[e] && container.groups.InAClass(container.Slot(e));
	}
	vector<bool> nulls(container.Count(), false);
	auto bound = columns.Numbers(root, body.rhs, reads, &nulls);
	evaluated.rhs_values.AssignDense(kept);
	const auto values = evaluated.rhs_values.MutableDense().data();
	const auto computed = bound.data();
	// A grouped `<>` excludes each value its rows' bounds take (language/guide.md §6.4): a NULL one excludes none, and
	// its row stays a member of the group's sum. Every `<>` here is one (a MIN or MAX under `<>` is refused), and keeps
	// every value of its bound rather than a tightest one.
	const auto excludes = evaluated.comparison_type == ExpressionType::COMPARE_NOTEQUAL;
	D_ASSERT(!excludes || (clause.lhs_is_aggregate && !tightest));
	std::vector<bool> &marked = tightest ? bound_null : excludes ? result.no_exclusion : null_rows;
	if (excludes) {
		marked.assign(kept, false);
	}
	const std::vector<bool> &null_entries = nulls;
	const std::vector<bool> &read_entries = reads;
	for (idx_t place = 0; place < kept; place++) {
		const auto entry = rows[place].second;
		values[place] = computed[entry];
		if (null_entries[entry] || (tightest && !read_entries[entry])) {
			marked[place] = true;
		}
	}
	if (excludes) {
		// A group none of whose rows has a bound excludes nothing: its instance has no value.
		vector<bool> bounded(evaluated.num_groups, false);
		auto group_of = [&](idx_t row) {
			return evaluated.row_group_ids[row];
		};
		for (idx_t row = 0; row < kept; row++) {
			const auto group = group_of(row);
			if (group != DConstants::INVALID_INDEX && !marked[row]) {
				bounded[group] = true;
			}
		}
		for (idx_t row = 0; row < kept; row++) {
			const auto group = group_of(row);
			if (group != DConstants::INVALID_INDEX && !bounded[group]) {
				null_rows[row] = true;
			}
		}
	}
	return bound_null;
}

void DecideModelEmitter::Admit(const DecideBody &body, EvaluatedClauseColumns &evaluated) {
	// Computed on the members of its term's site, the entries its member tests keep: an easy MIN's or MAX's on its
	// reducer's members alone, whose filter the descent routed. A row no member stands on emits no model row, so what
	// it would admit is never read. An end is never NULL: a NULL coefficient or bound admits every value, and every
	// other end is a number the row's coefficient and bound pick (DecideSingletonRow::lower). So it skips no instance,
	// and a NULL would be an internal error (ExtractDoubleColumn).
	const auto site = body.linear[0].node;
	auto &members = columns.Tested(site);
	const auto lower = columns.Numbers(site, body.lower, members, nullptr);
	const auto upper = columns.Numbers(site, body.upper, members, nullptr);
	evaluated.admitted_lower.AssignDense(placed);
	evaluated.admitted_upper.AssignDense(placed);
	for (auto read : DecideEntryRows(tree, descent, site)) {
		const auto place = place_of_row[read.first];
		if (place != DConstants::INVALID_INDEX) {
			evaluated.admitted_lower.Set(place, lower[read.second]);
			evaluated.admitted_upper.Set(place, upper[read.second]);
		}
	}
}

void DecideModelEmitter::ReadPart(const DecideTermSite &site, idx_t qualifier_scope, int sign, const DecideTerm *term,
                                  CoefficientColumn &column, vector<bool> &null_rows) {
	vector<bool> first, selected_rows;
	const auto &tested = columns.Tested(site.node);
	const auto &reads = ReadEntriesOf(site, qualifier_scope, tested, first, selected_rows);
	vector<bool> nulls(reads.size(), false);
	// Stage 05 leaves a weight of 1 out.
	auto numbers = site.program == DConstants::INVALID_INDEX ? vector<double>(reads.size(), double(sign))
	                                                         : columns.Numbers(site.node, site.program, reads, &nulls);
	DecideAbsentReads absent_reads(columns.Absence());
	if (term) {
		absent_reads.Add(*term);
	}
	column.AssignDense(placed);
	const auto coefficients = column.MutableDense().data();
	ForEachTermRead(site.node, reads, qualifier_scope, selected_rows, [&](idx_t row, idx_t entry) {
		const auto place = place_of_row[row];
		if (place != DConstants::INVALID_INDEX) {
			coefficients[place] = numbers[entry];
			null_rows[place] = null_rows[place] || nulls[entry] || absent_reads.At(row);
		}
	});
}

void DecideModelEmitter::Polynomial(idx_t root, const vector<TermFilterState> &filters, vector<bool> &null_rows,
                                    EmittedClause &result) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	auto &evaluated = result.row;
	for (idx_t g = 0; g < clause.product_groups.size(); g++) {
		auto &group = clause.product_groups[g];
		auto &sites = body.groups[g];
		EvaluatedProductGroup out;
		out.square = group.square;
		out.weight_sign = group.weight_sign;
		// Its weight, 0 on a row it does not read, then each form term's coefficient, whose decision has no value
		// where it has no instance (D84).
		ReadPart(sites.weight, group.qualifier_scope_idx, group.sign, nullptr, out.weight, null_rows);
		auto read_form = [&](const vector<DecideTerm> &terms, const vector<DecideTermSite> &form,
		                     vector<idx_t> &variables, vector<CoefficientColumn> &coefficients) {
			for (idx_t t = 0; t < terms.size(); t++) {
				auto &term = terms[t];
				variables.push_back(gstate.ReadVariable(term.variable_index, term.frame_read));
				coefficients.emplace_back();
				ReadPart(form[t], group.qualifier_scope_idx, term.sign, &term, coefficients.back(), null_rows);
			}
		};
		read_form(group.left, sites.left, out.left_variables, out.left_coefficients);
		read_form(group.right, sites.right, out.right_variables, out.right_coefficients);
		auto &filter = filters[clause.lhs_terms.size() + g];
		if (group.avg_scale) {
			ScaleAvgRows(out.weight, filter.has_filter, filter.mask, placed, evaluated.row_group_ids,
			             evaluated.num_groups);
		}
		result.products.push_back(std::move(out));
	}
}

void DecideModelEmitter::Name(idx_t by, const vector<idx_t> &kept_groups, EvaluatedClauseColumns &evaluated) {
	// Each group is named by its key's values, which the solver input reads by its rank.
	const auto values = key_values.size();
	key_values.push_back(DecideGroupKeyValues(tree, descent, by));
	for (auto group : kept_groups) {
		evaluated.group_names.push_back({values, group});
	}
}

void DecideModelEmitter::Objective(PhysicalDecide::EvaluatedClauses &evaluated) {
	auto found = std::find_if(tree.roots.begin(), tree.roots.end(),
	                          [&](idx_t root) { return tree.nodes[root]->compiled.objective != nullptr; });
	if (found == tree.roots.end()) {
		return;
	}
	const auto root = *found;
	auto &node = *tree.nodes[root];
	auto &body = node.compiled;
	auto &objective = *body.objective;
	auto &container = descent.Containers()[root];
	// The objective is produced once (D72): a second variant over its kept rows would be a second objective, which
	// is checked before any clause is read.
	const auto signature =
	    tree.objective_signature == DConstants::INVALID_INDEX ? nullptr : &tree.signatures[tree.objective_signature];
	// Counted first, each row's variant read only for a second variant's message.
	if (Variants(root, signature, false).count > 1) {
		const auto variants = Variants(root, signature);
		// Each variant at its first row's entry.
		vector<idx_t> surviving(variants.count, DConstants::INVALID_INDEX);
		const auto rows = DecideEntryRows(tree, descent, root);
		for (idx_t place = 0; place < rows.size(); place++) {
			const auto variant = variants.of_entry[rows[place].second];
			if (variant != DConstants::INVALID_INDEX && surviving[variant] == DConstants::INVALID_INDEX) {
				surviving[variant] = rows[place].second;
			}
		}
		auto difference = ObjectiveDifference(root, surviving);
		throw InvalidInputException(difference.message, Exception::InitializeExtraInfo(difference.place));
	}
	// The rows its `when` keeps.
	vector<bool> kept(num_rows, false);
	std::vector<bool> &marked = kept;
	for (auto &read : container.routed) {
		marked[read.first] = true;
	}
	evaluated.linear_filters = ObjectiveTerms(
	    root, objective.terms, body.linear, gstate.evaluated_objective_coefficients, gstate.objective_variable_indices);
	// A term outside every reducer counts once, on the first row the objective keeps.
	CountTupleTermsOnce(objective.terms, gstate.evaluated_objective_coefficients, &kept, num_rows);
	// A product group is read as a term is, on the members of the G it ranges over or on each row the objective
	// keeps: its weight, 0 on every other row, and its forms' coefficients, which stage 05 read as 0 where NULL
	// (DropNullObjectiveTerms). A row on which a form's decision has no instance gives the group no value, so it is
	// dropped there as a term is (D84). One outside every reducer counts once.
	auto &groups = objective.product_groups;
	evaluated.group_filters.resize(groups.size());
	for (idx_t g = 0; g < groups.size(); g++) {
		auto &group = groups[g];
		auto &sites = body.groups[g];
		auto &filter = evaluated.group_filters[g];
		const auto site = sites.weight.node;
		const auto &tested = columns.Tested(site);
		vector<bool> first, selected_rows;
		const auto &reads = ReadEntriesOf(sites.weight, group.qualifier_scope_idx, tested, first, selected_rows);
		EvaluatedProductGroup out;
		out.square = group.square;
		out.weight_sign = group.weight_sign;
		const auto weight = sites.weight.program == DConstants::INVALID_INDEX
		                        ? vector<double>(reads.size(), double(group.sign))
		                        : columns.Numbers(site, sites.weight.program, reads, nullptr);
		out.weight.AssignDense(num_rows);
		vector<const DecideTerm *> absent;
		vector<vector<double>> numbers;
		vector<CoefficientColumn *> coefficients;
		auto read_form = [&](const vector<DecideTerm> &terms, const vector<DecideTermSite> &form,
		                     vector<idx_t> &variables, vector<CoefficientColumn> &columns_out) {
			columns_out.resize(terms.size());
			for (idx_t t = 0; t < terms.size(); t++) {
				auto &term = terms[t];
				variables.push_back(gstate.ReadVariable(term));
				if (columns.Absence().MayBe(term)) {
					absent.push_back(&term);
				}
				numbers.push_back(columns.Numbers(site, form[t].program, reads, nullptr));
				columns_out[t].AssignDense(num_rows);
				coefficients.push_back(&columns_out[t]);
			}
		};
		read_form(group.left, sites.left, out.left_variables, out.left_coefficients);
		read_form(group.right, sites.right, out.right_variables, out.right_coefficients);
		filter.avg_scale = group.avg_scale;
		filter.has_filter = true;
		filter.mask.assign(num_rows, false);
		ForEachTermRead(site, reads, group.qualifier_scope_idx, selected_rows, [&](idx_t row, idx_t entry) {
			for (auto term : absent) {
				if (columns.Absence().TermAt(*term, row)) {
					return;
				}
			}
			filter.mask[row] = true;
			out.weight.Set(row, weight[entry]);
			for (idx_t c = 0; c < coefficients.size(); c++) {
				coefficients[c]->Set(row, numbers[c][entry]);
			}
		});
		AvgGroups(group.avg_scale, group.avg_group_key, group.child, site, filter);
		gstate.evaluated_objective_groups.push_back(std::move(out));
	}
	CountProductsOnce(groups, gstate.evaluated_objective_groups, &kept, num_rows);
}

vector<TermFilterState> DecideModelEmitter::ObjectiveTerms(idx_t root, const vector<DecideTerm> &terms,
                                                           const vector<DecideTermSite> &sites,
                                                           vector<CoefficientColumn> &coefficients,
                                                           vector<idx_t> &variables) {
	auto &container = descent.Containers()[root];
	vector<TermFilterState> filters(terms.size());
	coefficients.resize(terms.size());
	for (idx_t t = 0; t < terms.size(); t++) {
		auto &term = terms[t];
		auto &site = sites[t];
		const auto program = site.program;
		auto &filter = filters[t];
		variables.push_back(gstate.ReadVariable(term.variable_index, term.frame_read));
		filter.avg_scale = term.avg_scale;
		filter.has_filter = true;
		filter.mask.assign(num_rows, false);
		coefficients[t].AssignDense(num_rows);
		// Stage 05 read a NULL coefficient as 0 (DropNullObjectiveTerms), so none is read here. An entry that reads no
		// instance of the term's decision gives it no value, so the term is dropped there and its coefficient never
		// read (D84). The coefficient is still evaluated on the rows the term's own scope keeps before the relation
		// qualifier selects members; this preserves errors from a coefficient on a row in no qualified class.
		vector<bool> absent, first;
		const auto &present = columns.ReadEntries(site.node, term, absent);
		vector<bool> selected_rows;
		const auto &reads = ReadEntriesOf(site, term.qualifier_scope_idx, present, first, selected_rows);
		auto numbers = columns.Numbers(site.node, program, present, nullptr);
		const std::vector<bool> &marked = reads;
		std::vector<bool> &mask = filter.mask;
		const auto values = coefficients[t].MutableDense().data();
		const auto computed = numbers.data();
		auto read = [&](idx_t row, idx_t entry) {
			// A keyed E entry may represent rows where this auxiliary alternates between present and absent. Its
			// coefficient is shared by the entry, but the objective term is present only on rows with a value.
			if (marked[entry] && !columns.Absence().TermAt(term, row)) {
				mask[row] = true;
				values[row] = computed[entry];
			}
		};
		if (site.node == root) {
			// A term outside every reducer is read on each row the objective keeps, at its variant's entry, and
			// counted once there (Objective).
			for (auto &routed : container.routed) {
				if (term.qualifier_scope_idx == DConstants::INVALID_INDEX || selected_rows[routed.first]) {
					read(routed.first, routed.second);
				}
			}
		} else {
			// A term in a reducer is read on each member of the G it ranges over: an entry of its operand, which
			// the reducer's `when` routed and its member tests keep. A reducer that keeps none reads nothing, so its
			// terms are dropped (D84).
			ForEachTermRead(site.node, reads, term.qualifier_scope_idx, selected_rows, read);
		}
		AvgGroups(term.avg_scale, term.avg_group_key, term.child, site.node, filter);
	}
	return filters;
}

void DecideModelEmitter::AvgGroups(bool avg_scale, idx_t avg_group_key, idx_t child, idx_t site,
                                   TermFilterState &filter) {
	if (!avg_scale || avg_group_key == DConstants::INVALID_INDEX) {
		return;
	}
	// An AVG of a `by` key divides by the members of its own group (D81): the slot of the G each lies in.
	filter.avg_num_groups = descent.Containers()[child].groups.size();
	filter.avg_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
	auto &members = descent.Containers()[site];
	for (idx_t e = 0; e < members.Count(); e++) {
		filter.avg_group_ids[members.rows[e]] = members.parent_slots[e];
	}
}

void DecideModelEmitter::FrameMembers(idx_t node, ReducerValueMembers &out) {
	auto &tree_node = *tree.nodes[node];
	auto &map = gstate.frame_maps[tree_node.Cast<DecideFrameNode>().frame];
	auto &body = tree.nodes[node]->compiled;
	auto &entries = descent.Containers()[node].rows;
	// Each term's coefficient on the rows `rows` marks, its anchors there, each an entry of the O.
	auto read = [&](const vector<DecideTerm> &terms, const vector<DecideTermSite> &sites, const vector<bool> &rows,
	                vector<idx_t> &variable_ids) {
		vector<bool> reads(entries.size());
		for (idx_t e = 0; e < entries.size(); e++) {
			reads[e] = rows[entries[e]];
		}
		vector<CoefficientColumn> coefficients(terms.size());
		for (idx_t t = 0; t < terms.size(); t++) {
			variable_ids.push_back(gstate.ReadVariable(terms[t].variable_index, terms[t].frame_read));
			auto numbers = columns.Numbers(node, sites[t].program, reads, nullptr);
			coefficients[t].AssignDense(num_rows);
			for (idx_t e = 0; e < entries.size(); e++) {
				if (reads[e]) {
					coefficients[t].Set(entries[e], numbers[e]);
				}
			}
		}
		return coefficients;
	};
	vector<idx_t> payload_vars, fallback_vars;
	const auto payload = read(body.members, body.linear, map.ReadRows(), payload_vars);
	const auto fallback = read(body.fallback, body.fallback_sites, map.FallbackAnchors(), fallback_vars);
	DecideListFrameMembers(map, tree.ReducerOf(tree_node),
	                       DecideClauseRefOf(op.display.constraint_sources, op.display.source_text,
	                                         tree.Definition(tree_node)->origin.source_clause_id),
	                       tree_node.text, payload, payload_vars, InstancesOfRows(payload_vars), fallback,
	                       fallback_vars, InstancesOfRows(fallback_vars), out);
}

void DecideModelEmitter::ValueLinks() {
	// Reducer definitions are emitted from their G/O nodes, children first, each member term in its place; node-owned
	// metadata supplies the helper's formulation sides, qualifier and label.
	for (auto node : tree.eval_order) {
		auto &tree_node = *tree.nodes[node];
		if (!tree_node.Linked()) {
			continue;
		}
		auto &definition = tree.Definition(tree_node)->Cast<DecideReducerValueDefinition>();
		auto &body = tree_node.compiled;
		auto spec = ReducerValueLinkSpec();
		spec.value_var = definition.helper;
		spec.reducer = definition.reducer;
		spec.need_envelope = definition.NeedsEnvelope();
		spec.need_closing = definition.NeedsClosing();
		spec.label = definition.label;
		spec.source_clause_id = definition.origin.source_clause_id;
		spec.frame = tree_node.kind == DecideNodeKind::FRAME;
		if (spec.frame) {
			FrameMembers(node, spec.members);
			HandLink(std::move(spec), definition.form);
			continue;
		}
		const auto site = tree.OperandOf(node);
		auto &entries = descent.Containers()[site].parent_slots;
		auto &members = link_members[node];
		// Each G's members by group, from its operand's entries: each row once, at the entry that read it last, in
		// the input's order.
		vector<pair<idx_t, idx_t>> reads;
		for (auto &read : members.rows) {
			auto &place = place_of_row[read.first];
			if (place == DConstants::INVALID_INDEX) {
				place = reads.size();
				reads.push_back(read);
			} else {
				reads[place].second = read.second;
			}
		}
		for (auto &read : reads) {
			place_of_row[read.first] = DConstants::INVALID_INDEX;
		}
		std::sort(reads.begin(), reads.end());
		vector<vector<pair<idx_t, idx_t>>> reads_by_group(members.groups);
		for (auto &read : reads) {
			reads_by_group[members.group_of_slot[entries[read.second]]].push_back(read);
		}
		// Read a term over the entries its own filter keeps, including entries that have no group. Those terms
		// are evaluated before rows without a helper value are dropped.
		vector<idx_t> variables(body.members.size(), DConstants::INVALID_INDEX);
		vector<vector<double>> coefficients(body.members.size());
		for (idx_t t = 0; t < body.members.size(); t++) {
			auto &term = body.members[t];
			auto &site_t = body.linear[t];
			coefficients[t] = columns.Numbers(site, site_t.program, members.reads, nullptr);
			variables[t] = gstate.ReadVariable(term.variable_index, term.frame_read);
		}
		// A MIN or MAX of equal members is that member once. Members can be equal only when none reads a per-row
		// decision: two rows of a group then read the same instances (a nested reducer's value, a keyed decision). A
		// frame read is a read per row: its variable is numbered past the plan's (DecideGlobalSinkState::ReadVariable).
		bool distinct = IsDecideExtremum(definition.reducer);
		const auto instances = InstancesOfRows(variables);
		for (idx_t t = 0; t < variables.size() && distinct; t++) {
			distinct =
			    variables[t] == DConstants::INVALID_INDEX || instances[t].query_wide || instances[t].class_of_row;
		}
		std::set<vector<double>> listed;
		for (idx_t group = 0; group < reads_by_group.size(); group++) {
			auto &group_reads = reads_by_group[group];
			if (group_reads.empty()) {
				continue;
			}
			// A group's number is its helper's instance (HelperMapping).
			spec.members.AddGroup(group);
			listed.clear();
			const auto avg_scale =
			    definition.reducer == DecideReducerKind::AVG ? 1.0 / static_cast<double>(group_reads.size()) : 1.0;
			for (auto &read : group_reads) {
				const auto row = read.first;
				const auto entry = read.second;
				double constant = 0.0;
				for (idx_t term = 0; term < variables.size(); term++) {
					if (variables[term] == DConstants::INVALID_INDEX) {
						constant += coefficients[term][entry] * avg_scale;
					}
				}
				if (!std::isfinite(constant)) {
					throw DecideTooLargeError(DecideClauseRefOf(op.display.constraint_sources, op.display.source_text,
					                                            spec.source_clause_id));
				}
				if (distinct) {
					vector<double> member {constant};
					for (idx_t term = 0; term < variables.size(); term++) {
						if (variables[term] != DConstants::INVALID_INDEX) {
							member.push_back(coefficients[term][entry]);
							member.push_back(static_cast<double>(instances[term].At(row)));
						}
					}
					if (!listed.insert(std::move(member)).second) {
						continue;
					}
				}
				spec.members.AddMember(constant);
				for (idx_t term = 0; term < variables.size(); term++) {
					if (variables[term] == DConstants::INVALID_INDEX) {
						continue;
					}
					const auto coefficient = coefficients[term][entry] * avg_scale;
					if (coefficient != 0.0) {
						spec.members.AddTerm(variables[term], instances[term].At(row), coefficient);
					}
				}
			}
		}
		HandLink(std::move(spec), definition.form);
	}
	// The helpers' sizes, which DECIDB_PROFILE reports.
	if (helper_counts.helpers > 0) {
		DecideProfileScope::Counter("anr.helpers", static_cast<double>(helper_counts.helpers));
		DecideProfileScope::Counter("anr.helper_instances", static_cast<double>(helper_counts.instances));
		DecideProfileScope::Counter("anr.members", static_cast<double>(helper_counts.members));
		DecideProfileScope::Counter("anr.member_terms", static_cast<double>(helper_counts.terms));
		DecideProfileScope::Counter("anr.inlined_helpers", static_cast<double>(helper_counts.written_in));
	}
}

void DecideModelEmitter::HandLink(ReducerValueLinkSpec &&spec, DecideHelperForm form) {
	auto &members = spec.members;
	// A helper listed before it that the plan writes in is its members here too.
	WriteMembersIn(members.member_begin, members.variables, members.instances, members.coefficients, members.constants,
	               false);
	helper_counts.helpers++;
	helper_counts.instances += members.GroupCount();
	helper_counts.members += members.constants.size();
	helper_counts.terms += members.variables.size();
	if (form == DecideHelperForm::VARIABLE) {
		gstate.evaluated_value_links.push_back(std::move(spec));
		return;
	}
	auto &scope = tree.variable_scopes[spec.value_var];
	WrittenHelper helper;
	helper.whole = !IsDecideExtremum(spec.reducer);
	helper.label = spec.label;
	helper.group_of_instance.assign(scope.IsGlobal() ? 1 : (*keyed)[scope.keyed_scope_idx].num_classes,
	                                DConstants::INVALID_INDEX);
	// A SUM or AVG is its members; a MIN or MAX group of one member is that member (deck p. 112, R44), the one fact
	// of the data read here. The other groups keep their columns and their links.
	ReducerValueMembers linked;
	for (idx_t g = 0; g < members.GroupCount(); g++) {
		const idx_t first = members.group_begin[g], last = members.group_begin[g + 1];
		if (helper.whole || last - first == 1) {
			helper.group_of_instance[members.group_instances[g]] = g;
			continue;
		}
		linked.AddGroup(members.group_instances[g]);
		for (idx_t m = first; m < last; m++) {
			linked.AddMember(members.constants[m]);
			for (idx_t t = members.member_begin[m]; t < members.member_begin[m + 1]; t++) {
				linked.AddTerm(members.variables[t], members.instances[t], members.coefficients[t]);
			}
		}
	}
	// The instances with no column, which the column layout reads (SolverInput::written_in).
	gstate.written_in.resize(tree.variable_scopes.size());
	auto &written = gstate.written_in[spec.value_var];
	for (auto group : helper.group_of_instance) {
		written.push_back(helper.whole || group != DConstants::INVALID_INDEX);
	}
	helper.members = std::move(members);
	written_helper_of.resize(tree.variable_scopes.size(), DConstants::INVALID_INDEX);
	written_helper_of[spec.value_var] = written_helpers.size();
	written_helpers.push_back(std::move(helper));
	if (linked.GroupCount() > 0) {
		spec.members = std::move(linked);
		gstate.evaluated_value_links.push_back(std::move(spec));
	} else {
		helper_counts.written_in++;
	}
}

bool DecideModelEmitter::WriteMembersIn(vector<idx_t> &begin, vector<idx_t> &variables, vector<idx_t> &instances,
                                        vector<double> &coefficients, vector<double> &constants,
                                        bool absent_reads_nothing) const {
	auto helper_of = [&](idx_t variable) {
		return variable < written_helper_of.size() ? written_helper_of[variable] : DConstants::INVALID_INDEX;
	};
	if (std::none_of(variables.begin(), variables.end(),
	                 [&](idx_t variable) { return helper_of(variable) != DConstants::INVALID_INDEX; })) {
		return false;
	}
	vector<idx_t> out_begin {0}, out_variables, out_instances;
	vector<double> out_coefficients;
	auto add = [&](idx_t variable, idx_t instance, double coefficient) {
		out_variables.push_back(variable);
		out_instances.push_back(instance);
		out_coefficients.push_back(coefficient);
	};
	for (idx_t e = 0; e + 1 < begin.size(); e++) {
		for (idx_t t = begin[e]; t < begin[e + 1]; t++) {
			const auto entry = helper_of(variables[t]);
			if (entry == DConstants::INVALID_INDEX) {
				add(variables[t], instances[t], coefficients[t]);
				continue;
			}
			auto &helper = written_helpers[entry];
			const idx_t group = helper.group_of_instance[instances[t]];
			const double coefficient = coefficients[t];
			if (group == DConstants::INVALID_INDEX && !helper.whole) {
				add(variables[t], instances[t], coefficient);
				continue;
			}
			if (coefficient == 0.0) {
				continue;
			}
			if (group == DConstants::INVALID_INDEX) {
				if (absent_reads_nothing) {
					continue;
				}
				throw InternalException("DECIDE: a row reads %s where it is absent", helper.label);
			}
			auto &members = helper.members;
			for (idx_t m = members.group_begin[group]; m < members.group_begin[group + 1]; m++) {
				constants[e] += coefficient * members.constants[m];
				for (idx_t u = members.member_begin[m]; u < members.member_begin[m + 1]; u++) {
					add(members.variables[u], members.instances[u], coefficient * members.coefficients[u]);
				}
			}
		}
		out_begin.push_back(out_variables.size());
	}
	begin = std::move(out_begin);
	variables = std::move(out_variables);
	instances = std::move(out_instances);
	coefficients = std::move(out_coefficients);
	return true;
}

void DecideModelEmitter::WriteMembersIn(EvaluatedConstraint &clause) const {
	auto &records = clause.records;
	if (!WriteMembersIn(records.term_begin, records.variables, records.instances, records.coefficients,
	                    records.constants, false)) {
		return;
	}
	// Its decisions are those its rows name: a helper written in gives way to its members'.
	clause.members_written_in = true;
	vector<bool> named(tree.variable_scopes.size() + gstate.frame_reads.size(), false);
	std::vector<bool> &seen = named;
	clause.variable_indices.clear();
	for (auto variable : records.variables) {
		if (!seen[variable]) {
			seen[variable] = true;
			clause.variable_indices.push_back(variable);
		}
	}
}

void DecideModelEmitter::ObjectiveCoefficients() {
	auto &term_variables = gstate.objective_variable_indices;
	auto &term_columns = gstate.evaluated_objective_coefficients;
	auto &sums = gstate.objective_coefficients;
	const auto written =
	    DecideClauseRefOf(op.display.constraint_sources, op.display.source_text, DConstants::INVALID_INDEX);
	sums.resize(tree.variable_scopes.size() + gstate.frame_reads.size());
	auto add = [&](idx_t variable, idx_t instance, double coefficient) {
		auto &of_variable = sums[variable];
		if (of_variable.empty()) {
			of_variable.assign(Instances(variable), 0.0);
		}
		AddCoefficient(of_variable[instance], coefficient, written);
	};
	// Its terms on the helpers written where they are read, as one row: their members' terms, and the constants the
	// members carry.
	vector<idx_t> variables, instances;
	vector<double> coefficients;
	vector<bool> written_in(term_variables.size(), false);
	for (idx_t t = 0; t < term_variables.size(); t++) {
		const idx_t variable = term_variables[t];
		if (variable >= written_helper_of.size() || written_helper_of[variable] == DConstants::INVALID_INDEX) {
			continue;
		}
		written_in[t] = true;
		const auto instance = InstanceOfRow(variable);
		for (idx_t row = 0; row < num_rows; row++) {
			const double coefficient = term_columns[t].Get(row);
			if (coefficient != 0.0) {
				variables.push_back(variable);
				instances.push_back(instance.At(row));
				coefficients.push_back(coefficient);
			}
		}
	}
	vector<idx_t> begin {0, variables.size()};
	vector<double> constant {0.0};
	WriteMembersIn(begin, variables, instances, coefficients, constant, true);
	// A term with no decision (a data column) moves no optimum: it is the objective's constant. Summed without a
	// check: no solve reads it, and a sum past a DOUBLE only makes the reported value infinite.
	gstate.objective_constant = tree.objective_constant_offset + constant[0];
	for (idx_t t = 0; t < term_variables.size(); t++) {
		const idx_t variable = term_variables[t];
		auto &column = term_columns[t];
		if (variable == DConstants::INVALID_INDEX) {
			for (idx_t row = 0; row < num_rows; row++) {
				gstate.objective_constant += column.Get(row);
			}
		} else if (!written_in[t]) {
			// Every row's coefficient counts. A term beside the reducers counts once: its coefficient is kept on
			// one row and is 0 on the others (CountTupleTermsOnce).
			const auto instance = InstanceOfRow(variable);
			for (idx_t row = 0; row < num_rows; row++) {
				const double coefficient = column.Get(row);
				if (coefficient != 0.0) {
					add(variable, instance.At(row), coefficient);
				}
			}
		}
		column = CoefficientColumn();
	}
	for (idx_t u = 0; u < variables.size(); u++) {
		add(variables[u], instances[u], coefficients[u]);
	}
	term_variables.clear();
	term_columns.clear();
}

} // namespace duckdb
