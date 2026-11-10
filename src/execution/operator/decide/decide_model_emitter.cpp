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

//! A clause's parts a reducer may filter, which flat reads the filters of in this order: its terms, its products, its
//! squares.
static idx_t Parts(const DecideConstraint &clause) {
	return clause.lhs_terms.size() + clause.bilinear_terms.size() + clause.quadratic_groups.size();
}

DecideModelEmitter::DecideModelEmitter(const PhysicalDecide &op, ClientContext &context, const DecideTree &tree,
                                       const DecideDescent &descent, DecideTreeColumns &columns,
                                       DecideGlobalSinkState &gstate, PerGroupCache &groups,
                                       const DecideSourceNames &names, idx_t num_rows, DecideErrorLedger &errors,
                                       DataChunk &first)
    : op(op), context(context), tree(tree), descent(descent), columns(columns), gstate(gstate), groups(groups),
      names(names), num_rows(num_rows), errors(errors), first(first) {
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
		key_values.null_modes.push_back(element.null_mode);
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
			auto &frame = *tree.nodes[tree.frame_nodes[field.frame]];
			start += DecideKeyElements(tree.keys, frame.key).size() + (frame.frame_spec->Relative() ? 1 : 0);
		}
		starts.push_back(start);
	}
	return starts;
}

vector<Value> DecideModelEmitter::Fallback(idx_t frame, idx_t row) const {
	const bool reaches = op.frames[frame].fallback && gstate.frame_maps[frame].Missing(row);
	vector<Value> read {Value::BOOLEAN(reaches)};
	if (!reaches) {
		return read;
	}
	for (auto j : op.prepared.frame_fallback_columns[frame]) {
		read.push_back(columns.FrameValue(j, row));
	}
	for (auto decision : op.prepared.frame_fallback_decisions[frame]) {
		auto &scope = tree.variable_scopes[decision];
		if (!scope.IsGlobal()) {
			read.push_back(Value::UBIGINT(scope.IsKeyed() ? descent.DecisionRows()[scope.keyed_scope_idx][row] : row));
		}
	}
	return read;
}

string DecideModelEmitter::ObjectiveDifference(idx_t root, const vector<idx_t> &surviving) const {
	auto &node = *tree.nodes[root];
	auto &container = descent.Containers()[root];
	const auto starts = SignatureStarts(tree, node);
	auto varies = [&](idx_t column, string &values) {
		if (column >= container.layout.columns) {
			return false;
		}
		// The range holds the values it is given, so they outlive it.
		vector<Value> read;
		for (auto entry : surviving) {
			read.push_back(container.values.GetValue(column, entry));
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
		return DecideFrameDifference(node.signature[field], op.frames[f], tree.keys, frame);
	};
	return DecideObjectiveTwiceMessage(node.signature, tree.keys, tree.variable_scopes, read);
}

void DecideModelEmitter::KeyedMappings(vector<KeyedMapping> &mappings) {
	mappings = DecideDeclaredMappings(descent);
	for (idx_t scope = 0; scope < op.keyed_scopes.size(); scope++) {
		if (op.keyed_scopes[scope].signature != DConstants::INVALID_INDEX) {
			mappings[scope] = SignatureMapping(scope);
		}
	}
	ReadAbsence(mappings);
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
	// A row the reducer does not read has no class.
	mapping.row_to_class = variants.of_row;
	DecideMapRowsInNoClass(mapping);
	return mapping;
}

DecideModelEmitter::LinkMembers DecideModelEmitter::Members(idx_t link) {
	auto &body = tree.nodes[link]->compiled;
	const auto site = DecideTreeOperand(tree, link);
	auto &entries = descent.Containers()[site];
	auto &slots = descent.Containers()[link].groups;
	// Flat computes the members on the rows its reducer's `when` and NULL tests keep where every decision they read
	// has an instance (PhysicalDecide::ValueLinkMembers), a row in no group of its `by` key among them, and a
	// relation-qualified reducer lists each class once in its group, at its first row (language/guide.md §5.1).
	LinkMembers members;
	members.reads = columns.Tested(site);
	for (idx_t e = 0; e < entries.Count(); e++) {
		for (auto &term : body.members) {
			members.reads[e] = members.reads[e] && !columns.TermAbsentAt(term, entries.rows[e]);
		}
	}
	const auto qualified =
	    op.reducer_value_links[tree.nodes[link]->link].qualifier_scope_idx != DConstants::INVALID_INDEX;
	vector<bool> held(slots.size(), false);
	for (auto &read :
	     ReadRows(site, qualified ? columns.FirstOfEachInstance(site, members.reads) : members.reads, qualified)) {
		const auto slot = entries.parent_slots[read.second];
		if (slots.InAClass(slot)) {
			members.rows.push_back(read);
			held[slot] = true;
		}
	}
	// A group's number is its rank among the groups that hold a member, as flat numbers them by value.
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

KeyedMapping DecideModelEmitter::HelperMapping(idx_t link) {
	auto &node = *tree.nodes[link];
	if (node.kind == DecideNodeKind::FRAME) {
		// A range frame's helper: a class per window of its frame (PhysicalDecide::BuildScopeMapping).
		auto mapping = DecideFrameWindowMapping(gstate.frame_maps[node.frame]);
		DecideMapRowsInNoClass(mapping);
		return mapping;
	}
	auto &members = link_members[node.link];
	members = Members(link);
	KeyedMapping mapping;
	mapping.num_classes = members.groups;
	mapping.row_to_class.assign(num_rows, DConstants::INVALID_INDEX);
	// A group with no member has no helper, so a row of it reads none (PhysicalDecide::BuildScopeMapping).
	auto &slots = descent.Containers()[link].slots;
	for (auto read : DecideEntryRows(tree, descent, link)) {
		mapping.row_to_class[read.first] = members.group_of_slot[slots[read.second]];
	}
	DecideMapRowsInNoClass(mapping);
	return mapping;
}

void DecideModelEmitter::ReadAbsence(vector<KeyedMapping> &mappings) {
	gstate.absent_reads.assign(op.decide_variables.size(), vector<bool>());
	columns.ReadAbsence(mappings, gstate.absent_reads);
	// The rows defining each auxiliary (DecideConstraint::defined_var) and each helper's G, in the tree's order, which
	// puts what defines a decision before what reads it (D88): an auxiliary or a helper a definition or a helper's
	// member reads has its absence by then, whatever order the rewrites numbered them in.
	vector<idx_t> position(tree.nodes.size());
	for (idx_t i = 0; i < tree.eval_order.size(); i++) {
		position[tree.eval_order[i]] = i;
	}
	auto definitions = ClauseRoots();
	definitions.resize(op.prepared.constraints.size());
	link_members.assign(op.reducer_value_links.size(), LinkMembers());
	for (auto &link : op.reducer_value_links) {
		definitions.push_back(tree.value_nodes[link.value_var]);
	}
	std::sort(definitions.begin(), definitions.end(),
	          [&](idx_t left, idx_t right) { return position[left] < position[right]; });
	for (auto root : definitions) {
		auto &node = *tree.nodes[root];
		if (node.link != DConstants::INVALID_INDEX) {
			mappings[tree.variable_scopes[node.value_var].keyed_scope_idx] = HelperMapping(root);
		} else if (node.compiled.clause->defined_var != DConstants::INVALID_INDEX) {
			DefinitionAbsence(root);
		}
	}
	// A product's auxiliary has none where a factor has none (DecideTreeColumns::AbsentAt), which stage 06 reads too.
	for (auto &product : op.bilinear_links) {
		auto &absent = gstate.absent_reads[product.aux_idx];
		for (idx_t row = 0; row < num_rows; row++) {
			if (columns.AbsentAt(product.aux_idx, row)) {
				absent.resize(num_rows, false);
				absent[row] = true;
			}
		}
	}
}

void DecideModelEmitter::DefinitionAbsence(idx_t root) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	auto &absent = gstate.absent_reads[clause.defined_var];
	auto mark = [&](idx_t row) {
		absent.resize(num_rows, false);
		absent[row] = true;
	};
	// What it reads unreduced is NULL on a row it keeps, which its NULL tests keep too (DecideNode::member_tests):
	// its bound, then each term's coefficient, then each square's terms', which flat reads in that order, so what
	// one raises ranks there (DecideNode::value_rank). It reads no product's coefficient (DefinitionNulls).
	auto rank = tree.nodes[root]->value_rank;
	idx_t position = 0;
	auto null_rows = [&](idx_t node, idx_t program) {
		rank.position = position++;
		const auto nulls = columns.NullEntries(node, program, columns.Tested(node), rank);
		for (auto read : DecideEntryRows(tree, descent, node)) {
			if (nulls[read.second]) {
				mark(read.first);
			}
		}
	};
	// A decision it reads has no instance on a row, wherever the row lies: a term's, a product's factors, a
	// square's terms'.
	auto absent_rows = [&](const DecideTerm &term) {
		for (idx_t row = 0; row < num_rows; row++) {
			if (columns.TermAbsentAt(term, row)) {
				mark(row);
			}
		}
	};
	null_rows(root, body.rhs);
	for (idx_t t = 0; t < clause.lhs_terms.size(); t++) {
		null_rows(body.linear[t].node, body.linear[t].program);
		absent_rows(clause.lhs_terms[t]);
	}
	for (auto &product : clause.bilinear_terms) {
		absent_rows(DecideTerm(product.var_a, nullptr));
		absent_rows(DecideTerm(product.var_b, nullptr));
	}
	for (idx_t g = 0; g < clause.quadratic_groups.size(); g++) {
		for (idx_t t = 0; t < body.squares[g].size(); t++) {
			null_rows(body.squares[g][t].node, body.squares[g][t].program);
			absent_rows(clause.quadratic_groups[g].inner_terms[t]);
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
	const auto entries = descent.Containers()[root].Count();
	auto evaluated = InitializeEvaluatedConstraint(clause, body.clause_index,
	                                               [&](const DecideTerm &term) { return gstate.ReadVariable(term); });
	// The rows the clause keeps, which its `when` routed and its NULL tests keep (DecideNode::member_tests: a helper's
	// row reads a reducer's body on the reducer's members), with the entry standing for each: one model row stands on
	// each, or under `per K:` on one row of each variant of a class.
	auto &tested = columns.Tested(root);
	const auto rows = ReadRows(root, tested, false);
	vector<bool> nulls(entries, false);
	auto scatter = [&](const vector<double> &numbers, CoefficientColumn &column) {
		column.AssignDense(num_rows);
		for (auto read : rows) {
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
		for (auto read : rows) {
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
	for (auto read : rows) {
		null_rows[read.first] = nulls[read.second];
		for (auto &term : clause.lhs_terms) {
			null_rows[read.first] = null_rows[read.first] || columns.TermAbsentAt(term, read.first);
		}
	}
	if (clause.admitted_lower) {
		Admit(body, evaluated);
	}
	// Its products and squares, which no reducer filters.
	Polynomial(root, vector<TermFilterState>(Parts(clause)), null_rows, evaluated);
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
	for (auto read : rows) {
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

vector<Value> DecideModelEmitter::Identity(idx_t node, idx_t entry) const {
	auto &container = descent.Containers()[node];
	auto &fields = tree.nodes[node]->signature;
	const auto starts = SignatureStarts(tree, *tree.nodes[node]);
	auto identity = container.groups.Key(container.slots[entry]);
	const auto read = container.Signature(entry);
	const auto row = container.rows[entry];
	for (idx_t i = 0; i < fields.size(); i++) {
		if (fields[i].kind != DecideSignatureFieldKind::FRAME) {
			identity.insert(identity.end(), read.begin() + starts[i], read.begin() + starts[i + 1]);
			continue;
		}
		// A frame by its keys (D82, FrameFieldClasses): the anchor's position, as its order key compares, when it reads
		// relative to the anchor, else its timeline; and what its `else` reads where the anchor reaches it, at the row
		// an entry stands for.
		const auto f = fields[i].frame;
		auto &map = gstate.frame_maps[f];
		identity.push_back(Value::UBIGINT(op.frames[f].Relative() ? map.peer[row] : map.timeline[row]));
		auto fallback = Fallback(f, row);
		identity.insert(identity.end(), fallback.begin(), fallback.end());
	}
	return identity;
}

DecideVariants DecideModelEmitter::Variants(idx_t node, const DecideSignature *signature) const {
	auto &container = descent.Containers()[node];
	const auto entries = container.Count();
	// Flat numbers them by value, their class's and then their fields', unless a field reads a decision's instance
	// or a frame's keys, which number them by their first rows (BuildSignatureIds). No field reads a query-wide
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
	// (DecideNode::holds_rows_in_no_class). Each counts its class, as flat counts the classes over the rows it keeps
	// (LookupOrBuildPerGroupIds); one at an anchor where a frame its fields read reads nothing is no variant, since it
	// has no instance (D84).
	vector<idx_t> held;
	for (idx_t e = 0; e < entries; e++) {
		const auto row = container.rows[e];
		if (!container.groups.InAClass(container.slots[e])) {
			continue;
		}
		class_ids.emplace(container.groups.Key(container.slots[e]), 0);
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
	vector<idx_t> variant_of(entries, DConstants::INVALID_INDEX);
	vector<idx_t> class_of(entries, DConstants::INVALID_INDEX);
	for (auto e : held) {
		variant_of[e] = variant_ids[identities[e]];
		class_of[e] = class_ids[container.groups.Key(container.slots[e])];
	}
	result.of_row.assign(num_rows, DConstants::INVALID_INDEX);
	result.class_of_row.assign(num_rows, DConstants::INVALID_INDEX);
	for (auto read : DecideEntryRows(tree, descent, node)) {
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
	for (auto read : DecideEntryRows(tree, descent, root)) {
		if (variant_at[read.first] != DConstants::INVALID_INDEX) {
			entry_of[variant_at[read.first]] = read.second;
		}
	}
	// Each variant's values as flat's reader shows them: what its class key and its fields read at its first row, but
	// a collated column of its class by the least spelling among the clause's rows of the class
	// (DecideNodeGroups::spellings).
	auto &elements = DecideKeyElements(tree.keys, node.key);
	SolverInput::KeyValues key_values;
	for (auto &element : elements) {
		key_values.null_modes.push_back(element.null_mode);
	}
	key_values.null_modes.resize(names.size(), DecideKeyNullMode::NO_GROUP);
	vector<vector<Value>> values;
	for (idx_t variant = 0; variant < entry_of.size(); variant++) {
		const auto entry = entry_of[variant];
		const auto slot = container.slots[entry];
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
			class_names.push_back({named, rank, slots.first_rows[slot]});
		}
	}
	class_ids.assign(num_rows, DConstants::INVALID_INDEX);
	for (auto read : DecideEntryRows(tree, descent, root)) {
		class_ids[read.first] = class_of_slot[container.slots[read.second]];
	}
}

void DecideModelEmitter::ReadBesideReducers(idx_t root, const DecideTerm &term, const DecideTermSite &site,
                                            const DecideEntryRows &rows, CoefficientColumn &column,
                                            vector<bool> &null_rows) {
	vector<bool> nulls(descent.Containers()[root].Count(), false);
	auto numbers = columns.Numbers(root, site.program, columns.Tested(root), &nulls, &site, &term.inputs);
	column.AssignDense(num_rows);
	for (auto read : rows) {
		column.Set(read.first, numbers[read.second]);
		null_rows[read.first] = null_rows[read.first] || nulls[read.second];
	}
}

vector<pair<idx_t, idx_t>> DecideModelEmitter::ReadRows(idx_t site, const vector<bool> &reads, bool qualified) const {
	auto &entries = descent.Containers()[site].rows;
	vector<pair<idx_t, idx_t>> rows;
	for (auto read : DecideEntryRows(tree, descent, site)) {
		if (reads[read.second] && (!qualified || entries[read.second] == read.first)) {
			rows.push_back(read);
		}
	}
	return rows;
}

vector<bool> DecideModelEmitter::ReadEntriesOf(idx_t site, bool qualified) {
	const auto &tested = columns.Tested(site);
	return qualified ? columns.FirstOfEachInstance(site, tested) : tested;
}

vector<pair<idx_t, idx_t>> DecideModelEmitter::Filter(idx_t site, bool qualified, TermFilterState &filter,
                                                      vector<bool> &members) {
	auto rows = ReadRows(site, ReadEntriesOf(site, qualified), qualified);
	filter.has_filter = qualified;
	filter.mask.assign(num_rows, false);
	for (auto read : rows) {
		filter.mask[read.first] = true;
	}
	const auto &tested = columns.Tested(site);
	members.assign(num_rows, false);
	for (auto read : DecideEntryRows(tree, descent, site)) {
		members[read.first] = members[read.first] || tested[read.second];
	}
	return rows;
}

vector<bool> DecideModelEmitter::ReadInReducer(const DecideTerm &term, const DecideTermSite &site,
                                               CoefficientColumn &column, TermFilterState &filter,
                                               vector<bool> &null_rows) {
	// Its member tests are its filter's NULL tests of what its body reads and of its decisions' keys, so every
	// member's decision has an instance (D84); a coefficient is NULL on a member only through a value the whole query
	// shares, which no test reads, and leaves the member's group no value. Flat computes it on every member, and
	// leaves a relation-qualified one's repeats out afterwards.
	vector<bool> nulls(descent.Containers()[site.node].Count(), false);
	auto numbers = columns.Numbers(site.node, site.program, columns.Tested(site.node), &nulls, &site, &term.inputs);
	filter.avg_scale = term.avg_scale;
	vector<bool> members;
	column.AssignDense(num_rows);
	for (auto &read : Filter(site.node, term.qualifier_scope_idx != DConstants::INVALID_INDEX, filter, members)) {
		column.Set(read.first, numbers[read.second]);
		null_rows[read.first] = null_rows[read.first] || nulls[read.second];
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

bool DecideModelEmitter::ReadGroupedTerms(idx_t root, const DecideEntryRows &rows, const vector<bool> &kept,
                                          EvaluatedConstraint &evaluated, vector<TermFilterState> &filters,
                                          vector<bool> &active, vector<bool> &null_rows) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	auto &terms = clause.lhs_terms;
	bool has_local_filters = false;
	bool unfiltered = false;
	// A filter that keeps every kept row filters nothing, as flat reads it (ReadTermFilters).
	auto read_filter = [&](const vector<bool> &members, TermFilterState &filter) {
		bool filters_rows = false;
		for (idx_t row = 0; row < num_rows && !filters_rows; row++) {
			filters_rows = kept[row] && !members[row];
		}
		if (!filters_rows) {
			unfiltered = true;
			return;
		}
		filter.has_filter = true;
		has_local_filters = true;
		for (idx_t row = 0; row < num_rows; row++) {
			active[row] = active[row] || members[row];
		}
	};
	for (idx_t t = 0; t < terms.size(); t++) {
		auto &site = body.linear[t];
		if (site.node == root) {
			// A term beside the reducers is read on each kept row, and counted once in its group.
			ReadBesideReducers(root, terms[t], site, rows, evaluated.row_coefficients[t], null_rows);
			unfiltered = true;
			continue;
		}
		read_filter(ReadInReducer(terms[t], site, evaluated.row_coefficients[t], filters[t], null_rows), filters[t]);
	}
	// A product's and a square's filter, on the members of its reducer, which flat reads with the terms'; their
	// coefficients follow once the rows are grouped (Polynomial).
	vector<bool> members;
	for (idx_t i = 0; i < clause.bilinear_terms.size(); i++) {
		auto &filter = filters[terms.size() + i];
		Filter(body.products[i].node, clause.bilinear_terms[i].qualifier_scope_idx != DConstants::INVALID_INDEX, filter,
		       members);
		read_filter(members, filter);
	}
	for (idx_t g = 0; g < clause.quadratic_groups.size(); g++) {
		auto &filter = filters[terms.size() + clause.bilinear_terms.size() + g];
		Filter(body.squares[g].front().node,
		       clause.quadratic_groups[g].qualifier_scope_idx != DConstants::INVALID_INDEX, filter, members);
		read_filter(members, filter);
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
	for (auto read : rows) {
		kept[read.first] = true;
	}
	vector<bool> null_rows(num_rows, false);
	// Each term's filter, then each product's, then each square's.
	vector<TermFilterState> filters(Parts(clause));
	// The rows some reducer's filter keeps, which alone emit their group's row when every term has a filter.
	vector<bool> active(num_rows, false);
	const auto has_local_filters = ReadGroupedTerms(root, rows, kept, evaluated, filters, active, null_rows);
	// Each kept row's group of the `by` key: its slot in the clause's G of that key, numbered by value.
	const auto keyed = clause.group_key != DConstants::INVALID_INDEX;
	const auto by = keyed ? ByReducer(root) : DConstants::INVALID_INDEX;
	vector<idx_t> group_of(num_rows, DConstants::INVALID_INDEX);
	idx_t num_key_groups = 0;
	if (keyed) {
		auto &container = descent.Containers()[by];
		for (idx_t e = 0; e < container.Count(); e++) {
			group_of[container.rows[e]] = container.groups.ranks[container.slots[e]];
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
		    for (idx_t slot = 0; slot < classes.size(); slot++) {
			    if (classes.InAClass(slot)) {
				    return true;
			    }
		    }
		    return false;
	    },
	    op.diagnose || clause.tightest_frame_bound ? &kept_row_group_ids : nullptr, evaluated, grouped_instances);
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
	const auto bound_null = GroupBound(root, rows, null_rows, evaluated);
	if (clause.admitted_lower) {
		Admit(body, evaluated);
	}
	HoistAvgIntoNotEqualBound(clause, filters, evaluated);
	ScaleAvgTerms(clause, filters, num_rows, evaluated);
	Polynomial(root, filters, null_rows, evaluated);
	if (!bound_null.empty()) {
		// Taken once every rule that drops an instance has run but the next, which reads `null_rows`, after every part
		// of the clause.
		try {
			TakeTightestFrameBound(clause.comparison_type, evaluated.rhs_written, kept, bound_null, kept_row_group_ids,
			                       evaluated.row_group_ids, evaluated.num_groups, true,
			                       evaluated.rhs_values.MutableDense(), null_rows);
		} catch (std::exception &ex) {
			auto rank = tree.nodes[root]->clause_rank;
			rank.part = 1 + clause.quadratic_groups.size();
			errors.Add(rank, ErrorData(ex));
		}
	}
	SkipNullInstances(null_rows, true, num_rows, evaluated.row_group_ids, evaluated.num_groups);
	if (counted) {
		gstate.CountInstances(evaluated.source_clause_id, grouped_instances,
		                      CountInstances(evaluated.row_group_ids, evaluated.num_groups, num_rows, true));
	}
	return evaluated;
}

vector<bool> DecideModelEmitter::GroupBound(idx_t root, const DecideEntryRows &rows, vector<bool> &null_rows,
                                            EvaluatedConstraint &evaluated) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	if (!DescribeBound(clause, num_rows, names, op.source_fragments, op.keyed_scopes, op.keys, evaluated)) {
		return vector<bool>();
	}
	// A tightest frame bound's NULL bounds nothing, rather than leaving its instance no value (D85), and a row in no
	// class of the clause's `per K:` reads none, as no instance reads it there (TakeTightestFrameBound).
	const auto tightest = clause.tightest_frame_bound;
	vector<bool> bound_null(tightest ? num_rows : 0, false);
	auto &container = descent.Containers()[root];
	auto reads = columns.Tested(root);
	for (idx_t e = 0; tightest && clause.scope.per == DecideClauseScope::PerRole::NAMES_CLASSES && e < reads.size();
	     e++) {
		reads[e] = reads[e] && container.groups.InAClass(container.slots[e]);
	}
	vector<bool> nulls(container.Count(), false);
	auto bound = columns.Numbers(root, body.rhs, reads, &nulls);
	evaluated.rhs_values.AssignDense(num_rows);
	for (auto read : rows) {
		evaluated.rhs_values.Set(read.first, bound[read.second]);
		auto &marked = tightest ? bound_null : null_rows;
		marked[read.first] = marked[read.first] || nulls[read.second] || (tightest && !reads[read.second]);
	}
	return bound_null;
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
	for (auto read : DecideEntryRows(tree, descent, site)) {
		evaluated.admitted_lower.Set(read.first, lower[read.second]);
		evaluated.admitted_upper.Set(read.first, upper[read.second]);
	}
}

Value DecideModelEmitter::FirstRowValue(const Expression &factor) {
	auto lowered = TransformToChunkExpression(factor, context);
	ExpressionExecutor executor(context, *lowered);
	DataChunk value;
	value.Initialize(context, {lowered->return_type});
	executor.Execute(first, value);
	return value.data[0].GetValue(0);
}

void DecideModelEmitter::ReadPart(const DecideTermSite &site, bool qualified, int sign,
                                  const vector<unique_ptr<Expression>> &inputs, const vector<idx_t> &variables,
                                  CoefficientColumn &column, vector<bool> &null_rows) {
	const auto reads = ReadEntriesOf(site.node, qualified);
	vector<bool> nulls(reads.size(), false);
	// Stage 05 leaves a coefficient of 1 out.
	auto numbers = site.program == DConstants::INVALID_INDEX
	                   ? vector<double>(reads.size(), double(sign))
	                   : columns.Numbers(site.node, site.program, reads, &nulls, &site, &inputs);
	column.AssignDense(num_rows);
	for (auto &read : ReadRows(site.node, reads, qualified)) {
		column.Set(read.first, numbers[read.second]);
		null_rows[read.first] = null_rows[read.first] || nulls[read.second];
		for (auto variable : variables) {
			null_rows[read.first] = null_rows[read.first] || columns.AbsentAt(variable, read.first);
		}
	}
}

double DecideModelEmitter::SquareSign(idx_t root, idx_t g, vector<bool> &null_rows) {
	auto &node = *tree.nodes[root];
	auto &group = node.compiled.clause->quadratic_groups[g];
	if (!group.scale) {
		return group.sign;
	}
	try {
		bool is_null = false;
		const auto sign = ScaleSquaredGroup(
		    FirstRowValue(*group.scale), group,
		    {DecideValueRole::COEFFICIENT, *group.scale, names, op.WrittenClause(node.source_clause_id)}, is_null);
		// A NULL one makes every instance of the clause NULL.
		for (auto read : DecideEntryRows(tree, descent, root)) {
			null_rows[read.first] = null_rows[read.first] || is_null;
		}
		return sign;
	} catch (std::exception &ex) {
		auto rank = node.clause_rank;
		rank.part = 1 + g;
		rank.expression = DecideBlockExpression(DecideBlock::SQUARES, 0, false);
		rank.group = g;
		rank.row = 0;
		errors.Add(rank, ErrorData(ex));
		return group.sign;
	}
}

void DecideModelEmitter::Polynomial(idx_t root, const vector<TermFilterState> &filters, vector<bool> &null_rows,
                                    EvaluatedConstraint &evaluated) {
	auto &body = tree.nodes[root]->compiled;
	auto &clause = *body.clause;
	auto &products = clause.bilinear_terms;
	for (idx_t i = 0; i < products.size(); i++) {
		auto &product = products[i];
		EvaluatedConstraint::BilinearTerm column {product.var_a, product.var_b, CoefficientColumn()};
		ReadPart(body.products[i], product.qualifier_scope_idx != DConstants::INVALID_INDEX, product.sign,
		         product.inputs, {product.var_a, product.var_b}, column.row_coefficients, null_rows);
		auto &filter = filters[clause.lhs_terms.size() + i];
		if (product.avg_scale) {
			ScaleAvgRows(column.row_coefficients, filter.has_filter, filter.mask, false, num_rows,
			             evaluated.row_group_ids, evaluated.num_groups);
		}
		evaluated.bilinear_terms.push_back(std::move(column));
	}
	evaluated.has_quadratic = clause.has_quadratic;
	for (idx_t g = 0; g < clause.quadratic_groups.size(); g++) {
		auto &group = clause.quadratic_groups[g];
		auto &filter = filters[clause.lhs_terms.size() + products.size() + g];
		EvaluatedConstraint::QuadraticGroup square;
		square.sign = SquareSign(root, g, null_rows);
		for (idx_t t = 0; t < group.inner_terms.size(); t++) {
			auto &inner = group.inner_terms[t];
			square.variable_indices.push_back(gstate.ReadVariable(inner));
			square.row_coefficients.emplace_back();
			ReadPart(body.squares[g][t], group.qualifier_scope_idx != DConstants::INVALID_INDEX, inner.sign,
			         inner.inputs, {inner.variable_index}, square.row_coefficients.back(), null_rows);
			if (group.avg_scale) {
				ScaleAvgRows(square.row_coefficients.back(), filter.has_filter, filter.mask, true, num_rows,
				             evaluated.row_group_ids, evaluated.num_groups);
			}
		}
		evaluated.quadratic_groups.push_back(std::move(square));
	}
}

void DecideModelEmitter::Name(idx_t by, const vector<idx_t> &kept_groups, EvaluatedConstraint &evaluated) {
	// Each group is named by its key's values, which the solver input reads by its rank.
	auto &slots = descent.Containers()[by].groups;
	vector<idx_t> first_rows(slots.size());
	for (idx_t slot = 0; slot < slots.size(); slot++) {
		first_rows[slots.ranks[slot]] = slots.first_rows[slot];
	}
	const auto values = groups.key_values.size();
	groups.key_values.push_back(DecideGroupKeyValues(tree, descent, by));
	for (auto group : kept_groups) {
		evaluated.group_names.push_back({values, group, first_rows[group]});
	}
}

void DecideModelEmitter::Objective(PhysicalDecide::EvaluatedClauses &evaluated) {
	auto found = std::find_if(tree.roots.begin(), tree.roots.end(),
	                          [&](idx_t root) { return tree.nodes[root]->compiled.objective != nullptr; });
	if (found == tree.roots.end()) {
		return;
	}
	const auto root = *found;
	auto &body = tree.nodes[root]->compiled;
	auto &objective = *body.objective;
	auto &container = descent.Containers()[root];
	// The objective is produced once (D72): a second variant over its kept rows would be a second objective, which
	// flat checks before it reads any clause.
	const auto signature =
	    op.objective_signature == DConstants::INVALID_INDEX ? nullptr : &op.signatures[op.objective_signature];
	const auto variants = Variants(root, signature);
	if (variants.count > 1) {
		// Each variant at its first row's entry.
		vector<idx_t> surviving(variants.count, DConstants::INVALID_INDEX);
		for (auto &read : container.routed) {
			const auto variant = variants.of_row[read.first];
			if (variant != DConstants::INVALID_INDEX && surviving[variant] == DConstants::INVALID_INDEX) {
				surviving[variant] = read.second;
			}
		}
		errors.Add(DecideErrorRank {DecideErrorPhase::SCOPE_CHECK},
		           ErrorData(InvalidInputException(ObjectiveDifference(root, surviving))));
	}
	// The rows its `when` keeps.
	vector<bool> kept(num_rows, false);
	for (auto &read : container.routed) {
		kept[read.first] = true;
	}
	evaluated.linear_filters = ObjectiveTerms(
	    root, objective.terms, body.linear, gstate.evaluated_objective_coefficients, gstate.objective_variable_indices);
	// A term outside every reducer counts once, on the first row the objective keeps.
	CountTupleTermsOnce(objective.terms, gstate.evaluated_objective_coefficients, &kept, num_rows);
	// Each squared term lies in a reducer, and is read as a term there is.
	if (objective.has_quadratic) {
		gstate.has_quadratic_objective = true;
		gstate.quadratic_sign = objective.quadratic_sign;
		evaluated.quadratic_filters =
		    ObjectiveTerms(root, objective.squared_terms, body.squares[0], gstate.evaluated_quadratic_coefficients,
		                   gstate.quadratic_variable_indices);
	}
	// A product is read as a term is, on the members of the G it ranges over or on each row the objective keeps, but
	// stage 05 dropped none of its rows for a decision with no instance (DropNullObjectiveTerms), so none is dropped
	// here; one outside every reducer counts once.
	auto &products = objective.bilinear_terms;
	evaluated.bilinear_filters.resize(products.size());
	for (idx_t i = 0; i < products.size(); i++) {
		auto &product = products[i];
		auto &site = body.products[i];
		auto &filter = evaluated.bilinear_filters[i];
		const auto &tested = columns.Tested(site.node);
		// Stage 05 leaves a coefficient of 1 out.
		auto numbers = site.program == DConstants::INVALID_INDEX
		                   ? vector<double>(tested.size(), double(product.sign))
		                   : columns.Numbers(site.node, site.program, tested, nullptr, &site, &product.inputs);
		EvaluatedConstraint::BilinearTerm column {product.var_a, product.var_b, CoefficientColumn()};
		column.row_coefficients.AssignDense(num_rows);
		const auto qualified = product.qualifier_scope_idx != DConstants::INVALID_INDEX;
		filter.avg_scale = product.avg_scale;
		filter.has_filter = true;
		filter.mask.assign(num_rows, false);
		for (auto &read : ReadRows(site.node, ReadEntriesOf(site.node, qualified), qualified)) {
			filter.mask[read.first] = true;
			column.row_coefficients.Set(read.first, numbers[read.second]);
		}
		AvgGroups(product.avg_scale, product.avg_group_key, product.child, site.node, filter);
		gstate.evaluated_bilinear_terms.push_back(std::move(column));
	}
	CountProductsOnce(products, gstate.evaluated_bilinear_terms, &kept, num_rows);
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
		auto &filter = filters[t];
		variables.push_back(gstate.ReadVariable(term));
		filter.avg_scale = term.avg_scale;
		filter.has_filter = true;
		filter.mask.assign(num_rows, false);
		coefficients[t].AssignDense(num_rows);
		// Stage 05 read a NULL coefficient as 0 (DropNullObjectiveTerms), so none is read here. An entry that reads no
		// instance of the term's decision gives it no value, so the term is dropped there and its coefficient never
		// read (D84). A relation-qualified reducer reads each class once, at its first row (language/guide.md §5.1).
		const auto present = columns.ReadEntries(site.node, term);
		auto numbers = columns.Numbers(site.node, site.program, present, nullptr, &site, &term.inputs);
		const auto qualified = term.qualifier_scope_idx != DConstants::INVALID_INDEX;
		const auto reads = qualified ? columns.FirstOfEachInstance(site.node, present) : present;
		auto read = [&](idx_t row, idx_t entry) {
			if (reads[entry]) {
				filter.mask[row] = true;
				coefficients[t].Set(row, numbers[entry]);
			}
		};
		if (site.node == root) {
			// A term outside every reducer is read on each row the objective keeps, at its variant's entry, and
			// counted once there (Objective).
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

void DecideModelEmitter::Composed(PhysicalDecide::EvaluatedClauses &evaluated) {
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
		clause.terms = ComposedTerms(clauses[c], spec.terms, "constraint", null_factor);
		// The rows its `when` keeps are the rows its root holds, and their classes those of its `per K:`.
		auto kept_rows = [&]() {
			vector<bool> kept(num_rows, false);
			for (auto read : DecideEntryRows(tree, descent, clauses[c])) {
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
		auto terms = ComposedTerms(objective, op.composed_minmax_objective_terms, "objective", null_factor);
		// A term with a NULL factor reads 0; one that reads no row is dropped.
		terms.erase(std::remove_if(terms.begin(), terms.end(), DecideComposedTermReadsNoRow), terms.end());
		evaluated.composed_objective_terms = std::move(terms);
	}
}

vector<ComposedMinMaxTermData> DecideModelEmitter::ComposedTerms(idx_t root,
                                                                 const vector<LogicalDecide::ComposedMinMaxTerm> &terms,
                                                                 const char *what, bool &null_factor) {
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
				evaluated.scale =
				    DecideQueryWideScale(FirstRowValue(*term.scale), term.scale_divides, what,
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
		const auto qualified = term.qualifier_scope_idx != DConstants::INVALID_INDEX;
		const auto reads = ReadEntriesOf(site, qualified);
		const auto rows = ReadRows(site, reads, qualified);
		evaluated.filter_mask.assign(num_rows, false);
		for (auto read : rows) {
			evaluated.filter_mask[read.first] = true;
		}
		for (idx_t i = 0; i < members.members.size(); i++) {
			auto &inner = members.members[i];
			evaluated.variables.push_back(gstate.ReadVariable(inner));
			auto numbers =
			    columns.Numbers(site, members.linear[i].program, reads, nullptr, &members.linear[i], &inner.inputs);
			evaluated.per_term_coefs.emplace_back(num_rows, 0.0);
			for (auto read : rows) {
				evaluated.per_term_coefs.back()[read.first] = numbers[read.second];
			}
		}
		result.push_back(std::move(evaluated));
	}
	return result;
}

void DecideModelEmitter::FrameMembers(idx_t node, const LogicalDecide::ReducerValueLink &link,
                                      ReducerValueMembers &out) {
	auto &map = gstate.frame_maps[link.frame];
	auto &body = tree.nodes[node]->compiled;
	auto &entries = descent.Containers()[node].rows;
	// Each term's coefficient on the rows `rows` marks, its anchors there, each an entry of the O.
	auto read = [&](const vector<DecideTerm> &terms, const vector<DecideTermSite> &sites, const vector<bool> &rows) {
		vector<bool> reads(entries.size());
		for (idx_t e = 0; e < entries.size(); e++) {
			reads[e] = rows[entries[e]];
		}
		vector<CoefficientColumn> coefficients(terms.size());
		for (idx_t t = 0; t < terms.size(); t++) {
			auto numbers = columns.Numbers(node, sites[t].program, reads, nullptr, &sites[t], &terms[t].inputs);
			coefficients[t].AssignDense(num_rows);
			for (idx_t e = 0; e < entries.size(); e++) {
				if (reads[e]) {
					coefficients[t].Set(entries[e], numbers[e]);
				}
			}
		}
		return coefficients;
	};
	const auto payload = read(body.members, body.linear, map.ReadRows());
	const auto fallback = read(body.fallback, body.fallback_sites, map.FallbackAnchors());
	vector<idx_t> payload_vars, fallback_vars;
	for (auto &term : body.members) {
		payload_vars.push_back(gstate.ReadVariable(term));
	}
	for (auto &term : body.fallback) {
		fallback_vars.push_back(gstate.ReadVariable(term));
	}
	DecideListFrameMembers(map, link, tree.nodes[node]->text, payload, payload_vars, fallback, fallback_vars, out);
}

void DecideModelEmitter::ValueLinks() {
	for (auto &link : op.reducer_value_links) {
		const auto node = tree.value_nodes[link.value_var];
		auto &body = tree.nodes[node]->compiled;
		if (link.frame != DConstants::INVALID_INDEX) {
			auto spec = DecideValueLinkSpec(link);
			FrameMembers(node, link, spec.members);
			gstate.evaluated_value_links.push_back(std::move(spec));
			continue;
		}
		const auto site = DecideTreeOperand(tree, node);
		auto &entries = descent.Containers()[site].parent_slots;
		auto &members = link_members[tree.nodes[node]->link];
		// Its members' rows, each in its group, as flat's reducer partitions them by its `by` key.
		EvaluatedConstraint listed;
		listed.num_groups = members.groups;
		listed.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
		vector<bool> listed_rows(num_rows, false);
		for (auto &read : members.rows) {
			listed.row_group_ids[read.first] = members.group_of_slot[entries[read.second]];
			listed_rows[read.first] = true;
		}
		// Each term's coefficient on every row it is computed on, a row in no group among them, which flat reads, and
		// an AVG's divided by its group's members.
		for (idx_t t = 0; t < body.members.size(); t++) {
			auto &term = body.members[t];
			auto &site_t = body.linear[t];
			auto numbers = columns.Numbers(site, site_t.program, members.reads, nullptr, &site_t, &term.inputs);
			listed.row_coefficients.emplace_back();
			auto &column = listed.row_coefficients.back();
			column.AssignDense(num_rows);
			for (auto &read : members.rows) {
				column.Set(read.first, numbers[read.second]);
			}
			listed.variable_indices.push_back(gstate.ReadVariable(term));
			if (link.agg_name == "avg") {
				ScaleAvgRows(column, true, listed_rows, false, num_rows, listed.row_group_ids, listed.num_groups);
			}
		}
		auto spec = DecideValueLinkSpec(link);
		DecideListRowMembers(listed, num_rows, link.label, spec.members);
		gstate.evaluated_value_links.push_back(std::move(spec));
	}
	DecideCountValueLinks(gstate.evaluated_value_links);
}

} // namespace duckdb
