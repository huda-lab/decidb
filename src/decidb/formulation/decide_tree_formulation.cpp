#include "duckdb/decidb/formulation/decide_tree_formulation.hpp"

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

namespace {

//! A symbolic linear form in final column coordinates. It is built from the
//! synthesized tree values, never from DecidePreparedModel's term registry.
struct TreeLinearForm {
	std::map<int, double> terms;
	std::map<pair<int, int>, double> quadratic;
	//! 0 linear, +1 convex squares, -1 concave squares, 2 indefinite/unknown.
	int curvature = 0;
	double constant = 0;

	void Add(const TreeLinearForm &other, double scale, const string &written) {
		AddCoefficient(constant, other.constant * scale, written);
		for (auto &term : other.terms) {
			AddCoefficient(terms[term.first], term.second * scale, written);
		}
		for (auto &term : other.quadratic) {
			AddCoefficient(quadratic[term.first], term.second * scale, written);
		}
		if (!other.quadratic.empty() && scale != 0) {
			const auto sign = other.curvature == 2 ? 2 : (scale < 0 ? -other.curvature : other.curvature);
			curvature = curvature == 0 ? sign : curvature == sign ? curvature : 2;
		}
	}

	void Scale(double scale, const string &written) {
		TreeLinearForm result;
		result.Add(*this, scale, written);
		*this = std::move(result);
	}

	void Compact() {
		for (auto it = terms.begin(); it != terms.end();) {
			if (std::abs(it->second) < 1e-15) {
				it = terms.erase(it);
			} else {
				++it;
			}
		}
		for (auto it = quadratic.begin(); it != quadratic.end();) {
			if (std::abs(it->second) < 1e-15) {
				it = quadratic.erase(it);
			} else {
				++it;
			}
		}
		if (quadratic.empty()) {
			curvature = 0;
		}
	}
};

struct TreeExtremumColumn {
	idx_t column;
	bool envelope = false;
	bool closing = false;
};

class TreeFormulator {
public:
	TreeFormulator(const DecideTree &tree_p, const DecideDescent &descent_p, const DecideTreeEvaluation &evaluation_p,
	               SolverInput &input_p, const VarIndexer &indexer_p, NativeConstructPolicy policy_p,
	               const vector<string> &names_p, bool repair_p)
	    : tree(tree_p), descent(descent_p), evaluation(evaluation_p), input(input_p), indexer(indexer_p),
	      policy(policy_p), names(names_p), repair(repair_p) {
		if (names.size() < input.num_decide_vars) {
			names.resize(input.num_decide_vars, "decision");
		}
		// The rigid box is the decision's box without the user's folded bounds (solver_input.hpp), so where the two
		// floors differ the decision's floor is one the user wrote.
		folded_floors.assign(input.lower_bounds.size(), -std::numeric_limits<double>::infinity());
		for (idx_t variable = 0; variable < input.rigid_lower_bounds.size(); variable++) {
			if (input.rigid_lower_bounds[variable] != input.lower_bounds[variable]) {
				folded_floors[variable] = input.lower_bounds[variable];
			}
		}
	}

	void Run() {
		evaluation.RequireSupported();
		LiftedFloors();
		// Data bounds must settle before an extremum derives its own range. This
		// is a dependency of formulation, independent of the written clause order.
		for (auto root : tree.roots) {
			auto &node = *tree.nodes[root];
			if (!node.objective && node.child_reads.empty()) {
				Root(root);
			}
		}
		enable_auxiliaries = true;
		for (auto &link : tree.bilinear_links) {
			EnsureAuxiliary(link.aux_idx);
		}
		for (auto &link : tree.abs_maximize_links) {
			EnsureAuxiliary(link.aux_idx);
		}
		for (auto root : tree.roots) {
			auto &node = *tree.nodes[root];
			if (!node.objective && !node.child_reads.empty()) {
				Root(root);
			}
		}
		for (auto root : tree.roots) {
			if (tree.nodes[root]->objective) {
				Objective(root);
			}
		}
		// A `<>` sizes its Big-M from the box, so it is lowered once every bound and derived range has settled, as
		// stage 06 lowers it after the bounds it reads.
		for (auto &deferred : not_equals) {
			written = deferred.written;
			NotEqual(std::move(deferred.form), std::move(deferred.row));
		}
	}

private:
	const DecideTree &tree;
	const DecideDescent &descent;
	const DecideTreeEvaluation &evaluation;
	SolverInput &input;
	const VarIndexer &indexer;
	NativeConstructPolicy policy;
	vector<string> names;
	bool repair;
	vector<double> folded_floors;
	struct DeferredNotEqual {
		TreeLinearForm form;
		SolverInput::RawConstraint row;
		string written;
	};
	vector<DeferredNotEqual> not_equals;
	std::unordered_map<const DecideTreeValue *, TreeExtremumColumn> extrema;
	std::unordered_map<idx_t, idx_t> objective_term;
	std::unordered_map<idx_t, vector<idx_t>> instance_rows;
	std::unordered_map<idx_t, idx_t> stored_key_values;
	std::unordered_map<const DecideTreeValue *, DecideTreeValuePtr> absolute_values;
	std::unordered_set<int> integral_columns;
	std::unordered_set<idx_t> completed_auxiliaries;
	std::unordered_set<idx_t> busy_auxiliaries;
	std::unordered_map<idx_t, vector<DecideTreeValuePtr>> synthesized_rows;
	std::unordered_map<const DecideDescentGroup *, idx_t> group_class_sets;
	bool enable_auxiliaries = false;
	string written;
	idx_t source = DConstants::INVALID_INDEX;
	idx_t removal = DConstants::INVALID_INDEX;

	idx_t KeyValues(idx_t node) {
		auto known = stored_key_values.find(node);
		if (known != stored_key_values.end()) {
			return known->second;
		}
		if (tree.nodes[node]->key == DConstants::INVALID_INDEX) {
			return DConstants::INVALID_INDEX;
		}
		auto &groups = descent.Containers()[node].groups;
		vector<vector<Value>> values(groups.size());
		for (auto &group : groups) {
			values[group.rank] = group.key;
		}
		SolverInput::KeyValues keys;
		for (auto &element : DecideKeyElements(tree.keys, tree.nodes[node]->key)) {
			keys.null_modes.push_back(element.null_mode);
		}
		keys.values_of = [values](idx_t group) {
			return values[group];
		};
		const auto index = input.key_values.size();
		input.key_values.push_back(std::move(keys));
		stored_key_values.emplace(node, index);
		return index;
	}

	GroupName Name(idx_t node, idx_t slot, idx_t row) {
		GroupName name;
		name.key_values = KeyValues(node);
		name.group = descent.Containers()[node].groups[slot].rank;
		name.row = row;
		return name;
	}

	//! A row names the classes its clause's `per` makes (SolverInput::ClassSet). With none written, an instance of a
	//! grouped clause stands for every kept row of its group, whichever row holds its entry: one set per group.
	void Provenance(idx_t root, const DecideDescentEntry &entry, SolverInput::RawConstraint &row) {
		auto &node = *tree.nodes[root];
		const bool grouped = node.facts && (node.facts->lhs_is_aggregate || node.facts->was_minmax_easy);
		const DecideNodeContainer *members = nullptr;
		const DecideDescentGroup *group = nullptr;
		if (grouped) {
			for (auto &read : node.child_reads) {
				auto &child = *tree.nodes[read.node];
				if (child.kind != DecideNodeKind::REDUCER || child.key != node.facts->group_key) {
					continue;
				}
				members = &descent.Containers()[read.node];
				for (auto &member : members->entries) {
					if (member.row == entry.row) {
						group = &members->groups[member.slot];
						row.group_key = group->rank;
						row.group_name = Name(read.node, member.slot, entry.row);
						break;
					}
				}
				if (group) {
					break;
				}
			}
		}
		if (!group && node.per != DecideTreePer::ROW) {
			row.group_key = descent.Containers()[root].groups[entry.slot].rank;
			row.group_name = Name(root, entry.slot, entry.row);
		}
		if (group && node.facts->scope.per == DecideClauseScope::PerRole::NONE) {
			auto known = group_class_sets.find(group);
			if (known == group_class_sets.end()) {
				SolverInput::ClassSet classes {SolverInput::ClassSet::Kind::KEPT_ROWS, {}, {}};
				for (auto index : group->entries) {
					classes.rows.push_back(members->entries[index].row);
				}
				known = group_class_sets.emplace(group, input.class_sets.size()).first;
				input.class_sets.push_back(std::move(classes));
			}
			row.class_set = known->second;
			return;
		}
		SolverInput::ClassSet classes;
		if (node.per == DecideTreePer::ONE) {
			classes.kind = SolverInput::ClassSet::Kind::WHOLE_CLAUSE;
		} else if (node.per == DecideTreePer::KEY) {
			classes.kind = SolverInput::ClassSet::Kind::KEY_VALUES;
			classes.classes.push_back(Name(root, entry.slot, entry.row));
		} else {
			classes.kind = SolverInput::ClassSet::Kind::KEPT_ROWS;
			classes.rows.push_back(entry.row);
		}
		row.class_set = input.class_sets.size();
		input.class_sets.push_back(std::move(classes));
	}

	void MergeClasses(idx_t target, idx_t source_set) {
		if (target == source_set) {
			return;
		}
		auto &to = input.class_sets[target];
		auto &from = input.class_sets[source_set];
		for (auto row : from.rows) {
			if (std::find(to.rows.begin(), to.rows.end(), row) == to.rows.end()) {
				to.rows.push_back(row);
			}
		}
		for (auto &name : from.classes) {
			bool seen = false;
			for (auto &known : to.classes) {
				seen |= known.key_values == name.key_values && known.group == name.group;
			}
			if (!seen) {
				to.classes.push_back(name);
			}
		}
	}

	vector<idx_t> SurvivingEntries(idx_t node, bool with_rhs) const {
		auto &container = descent.Containers()[node];
		auto &values = evaluation.Results()[node];
		vector<bool> taken(container.instance_entries.size(), false);
		vector<idx_t> result;
		if (!values.instances.empty()) {
			for (auto entry : values.instance_entries) {
				if (!with_rhs || (entry < values.rhs.size() && values.rhs[entry] && values.rhs[entry]->Present())) {
					result.push_back(entry);
				}
			}
			return result;
		}
		for (idx_t entry = 0; entry < container.entries.size(); entry++) {
			const auto instance = container.entries[entry].instance;
			if (taken[instance] || entry >= values.entries.size() || !values.entries[entry] ||
			    !values.entries[entry]->Present()) {
				continue;
			}
			if (with_rhs && (entry >= values.rhs.size() || !values.rhs[entry] || !values.rhs[entry]->Present())) {
				continue;
			}
			taken[instance] = true;
			result.push_back(entry);
		}
		return result;
	}

	[[noreturn]] void Unsupported(const string &operation) const {
		throw InvalidInputException("DECIDE: the tree evaluator cannot formulate %s in %s yet", operation, written);
	}

	double Number(const DecideTreeValuePtr &held, DecideValueRole role = DecideValueRole::COEFFICIENT) const {
		auto &value = held->data;
		if (value.IsNull()) {
			throw InternalException("DECIDE tree formulation read a NULL surviving value");
		}
		if (!value.type().IsNumeric() && value.type().id() != LogicalTypeId::BOOLEAN) {
			Unsupported(value.type().ToString());
		}
		const auto number = value.GetValue<double>();
		if (std::isnan(number) || (!std::isfinite(number) && role != DecideValueRole::BOUND)) {
			throw InvalidInputException(
			    DecideInvalidValueMessage(role, held->operation.empty() ? written : held->operation));
		}
		return number;
	}

	TreeLinearForm Compile(const DecideTreeValuePtr &value, int push,
	                       DecideValueRole role = DecideValueRole::COEFFICIENT) {
		if (!value) {
			throw InternalException("DECIDE tree formulation read an uncomputed value");
		}
		switch (value->kind) {
		case DecideTreeValueKind::DATA: {
			TreeLinearForm form;
			form.constant = Number(value, role);
			return form;
		}
		case DecideTreeValueKind::DECISION: {
			if (value->variable >= indexer.var_scope.size() || value->row >= input.num_rows) {
				throw InternalException("DECIDE tree value refers to a missing decision instance");
			}
			if (enable_auxiliaries) {
				EnsureAuxiliary(value->variable);
			}
			const auto column = indexer.ColumnOrNone(value->variable, value->row);
			if (column == DConstants::INVALID_INDEX) {
				throw InternalException("DECIDE tree retained a rewritten helper instead of its synthesized value");
			}
			TreeLinearForm form;
			form.terms[NumericCast<int>(column)] = 1;
			return form;
		}
		case DecideTreeValueKind::SCALAR:
			return Scalar(*value, push);
		case DecideTreeValueKind::REDUCER:
			return Reducer(value, push);
		case DecideTreeValueKind::ERROR:
			value->error.Throw();
		case DecideTreeValueKind::ABSENT:
			throw InternalException("DECIDE tree formulation read an absent surviving value");
		case DecideTreeValueKind::UNSUPPORTED:
			Unsupported(value->operation);
		}
		throw InternalException("DECIDE tree formulation read an unknown value kind");
	}

	//! What a root bounds at an entry: an easy extremum each member directly, as MAX(e) <= K is every member e <= K,
	//! with no symbolic intermediate column; any other root the entry's value.
	static vector<DecideTreeValuePtr> Bounded(const DecideNode &node, const DecideTreeValuePtr &value, int push) {
		if (node.facts && node.facts->was_minmax_easy && value->kind == DecideTreeValueKind::REDUCER &&
		    ((value->operation == "max" && push > 0) || (value->operation == "min" && push < 0))) {
			return value->operands;
		}
		return {value};
	}

	void LiftedFloors() {
		const auto infinity = std::numeric_limits<double>::infinity();
		vector<vector<double>> lowered(input.num_decide_vars);
		for (auto root : tree.roots) {
			auto &node = *tree.nodes[root];
			if (!node.facts || node.facts->lifted_floor == DConstants::INVALID_INDEX) {
				continue;
			}
			const auto variable = node.facts->lifted_floor;
			if (variable >= tree.singleton_row_boxes.size() || tree.singleton_row_boxes[variable].lower_written) {
				continue;
			}
			written = node.text;
			auto &values = evaluation.Results()[root];
			const int push = node.comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
			                         node.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO
			                     ? -1
			                     : 1;
			for (auto entry : SurvivingEntries(root, true)) {
				// The floor is on each decision the root bounds, whichever row its instance's entry is.
				for (auto &bounded : Bounded(node, values.entries[entry], push)) {
					auto form = Compile(bounded, 0);
					form.Add(Compile(values.rhs[entry], 0), -1, written);
					form.Compact();
					if (form.terms.size() != 1 || !form.quadratic.empty()) {
						throw InternalException("DECIDE tree lifted floor is not a bound on one decision");
					}
					const auto term = *form.terms.begin();
					double floor = -form.constant / term.second;
					if (node.comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
					    node.comparison_type == ExpressionType::COMPARE_LESSTHAN) {
						floor = DecideStrictBound(true, floor);
					}
					floor = MaxValue(floor, -DECIDE_SOLVER_INFINITY);
					if (lowered[variable].empty()) {
						lowered[variable].assign(indexer.NamedInstances(variable), infinity);
					}
					const auto instance = indexer.InstanceOfColumn(variable, NumericCast<idx_t>(term.first));
					if (instance >= lowered[variable].size()) {
						continue;
					}
					auto &current = lowered[variable][instance];
					current = current == infinity ? floor : MaxValue(current, floor);
				}
			}
		}
		input.instance_lower_bounds.resize(input.num_decide_vars);
		for (idx_t variable = 0; variable < lowered.size(); variable++) {
			if (lowered[variable].empty()) {
				continue;
			}
			double hull = infinity;
			auto &refinement = input.instance_lower_bounds[variable];
			refinement.resize(lowered[variable].size());
			for (idx_t instance = 0; instance < refinement.size(); instance++) {
				const bool reaches = lowered[variable][instance] != infinity;
				hull = MinValue(hull, reaches ? (repair ? -DECIDE_SOLVER_INFINITY : lowered[variable][instance]) : 0);
				refinement[instance] = reaches ? -infinity : 0;
			}
			input.lower_bounds[variable] = hull;
			input.rigid_lower_bounds[variable] = -NumericLimits<double>::Maximum();
		}
	}

	DecideTreeValuePtr At(idx_t variable, idx_t row) {
		if (variable < tree.value_nodes.size() && tree.value_nodes[variable] != DConstants::INVALID_INDEX) {
			auto known = synthesized_rows.find(variable);
			if (known == synthesized_rows.end()) {
				const auto node = tree.value_nodes[variable];
				vector<DecideTreeValuePtr> values(input.num_rows);
				auto &container = descent.Containers()[node];
				auto &result = evaluation.Results()[node];
				for (idx_t entry = 0; entry < container.entries.size(); entry++) {
					values[container.entries[entry].row] = tree.nodes[node]->kind == DecideNodeKind::REDUCER
					                                           ? result.groups[container.entries[entry].slot]
					                                           : result.entries[entry];
				}
				known = synthesized_rows.emplace(variable, std::move(values)).first;
			}
			return known->second[row];
		}
		if (indexer.ColumnOrNone(variable, row) == DConstants::INVALID_INDEX) {
			return nullptr;
		}
		auto value = make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::DECISION, input.variable_types[variable]);
		value->variable = variable;
		value->row = row;
		value->instance = indexer.InstanceOf(variable, row);
		return value;
	}

	void AuxiliaryRange(idx_t variable, idx_t instance, double lo, double hi) {
		input.instance_reach_lower.resize(input.num_decide_vars);
		input.instance_reach_upper.resize(input.num_decide_vars);
		auto &lower = input.instance_reach_lower[variable];
		auto &upper = input.instance_reach_upper[variable];
		if (lower.empty()) {
			lower.assign(indexer.NamedInstances(variable), -std::numeric_limits<double>::infinity());
		}
		if (upper.empty()) {
			upper.assign(indexer.NamedInstances(variable), std::numeric_limits<double>::infinity());
		}
		// A derived range is one more fact about the auxiliary: it narrows what a folded bound left, never replaces it.
		// The decision's floor lowers to admit the range, so a floor the user wrote moves into the instance first.
		lower[instance] = MaxValue(MaxValue(lower[instance], lo), folded_floors[variable]);
		upper[instance] = MinValue(upper[instance], hi);
		input.lower_bounds[variable] = MinValue(input.lower_bounds[variable], lo);
	}

	void Structural(TreeLinearForm form, int result, const vector<pair<int, double>> &extra, char sense, double bound) {
		AddCoefficient(form.terms[result], 1, written);
		for (auto &term : extra) {
			AddCoefficient(form.terms[term.first], term.second, written);
		}
		form.Compact();
		SolverInput::RawConstraint row;
		for (auto &term : form.terms) {
			row.indices.push_back(term.first);
			row.coefficients.push_back(term.second);
		}
		row.sense = sense;
		row.rhs = bound - form.constant;
		row.kind = ConstraintKind::STRUCTURAL;
		row.source_clause_id = source;
		row.removal_group_id = removal;
		input.global_constraints.push_back(std::move(row));
	}

	void EnsureAuxiliary(idx_t variable) {
		if (completed_auxiliaries.count(variable) || busy_auxiliaries.count(variable)) {
			return;
		}
		busy_auxiliaries.insert(variable);
		for (auto &link : tree.bilinear_links) {
			if (link.aux_idx != variable) {
				continue;
			}
			for (idx_t instance = 0; instance < indexer.NumInstances(variable); instance++) {
				if (!indexer.HasColumn(variable, instance)) {
					continue;
				}
				const auto column = indexer.InstanceColumn(variable, instance);
				const auto row = ObjectiveRow(variable, column);
				if (row == DConstants::INVALID_INDEX) {
					continue;
				}
				auto boolean = At(link.bool_var_idx, row);
				auto other = At(link.other_var_idx, row);
				if (!boolean || !other || !boolean->Present() || !other->Present()) {
					continue;
				}
				auto b = Compile(boolean, 0);
				auto x = Compile(other, 0);
				b.Compact();
				if (b.terms.size() != 1 || b.constant != 0 || b.terms.begin()->second != 1 || !b.quadratic.empty()) {
					Unsupported("a Boolean product factor without its own column");
				}
				const auto reach = Range(x);
				if (reach.Unbounded()) {
					const auto blame =
					    reach.unbounded_var == DConstants::INVALID_INDEX ? link.other_var_idx : reach.unbounded_var;
					throw InvalidInputException("Bilinear term requires a finite upper bound on variable '%s'. "
					                            "Add a constraint like '%s <= <bound>' to provide one.",
					                            names[blame], names[blame]);
				}
				const auto binary = b.terms.begin()->first;
				const auto result = NumericCast<int>(column);
				AuxiliaryRange(variable, instance, MinValue(0.0, reach.lo), MaxValue(0.0, reach.hi));
				Structural(TreeLinearForm(), result, {{binary, -reach.hi}}, '<', 0);
				x.Scale(-1, written);
				Structural(x, result, {{binary, -reach.hi}}, '>', -reach.hi);
				if (reach.lo < 0) {
					Structural(x, result, {{binary, -reach.lo}}, '<', -reach.lo);
					Structural(TreeLinearForm(), result, {{binary, -reach.lo}}, '>', 0);
				} else {
					Structural(x, result, {}, '<', 0);
				}
			}
		}
		for (auto &link : tree.abs_maximize_links) {
			if (link.aux_idx == variable) {
				Absolute(link);
			}
		}
		busy_auxiliaries.erase(variable);
		completed_auxiliaries.insert(variable);
	}

	void Absolute(const LogicalDecide::AbsMaximizeLink &link) {
		idx_t positive = DConstants::INVALID_INDEX;
		for (auto node : tree.definition_nodes[link.aux_idx]) {
			if (tree.nodes[node]->facts && tree.nodes[node]->facts->abs_aux_idx == link.aux_idx &&
			    tree.nodes[node]->facts->abs_is_pos_bound) {
				positive = node;
				break;
			}
		}
		if (positive == DConstants::INVALID_INDEX) {
			Unsupported("ABS without its defining expression");
		}
		auto &values = evaluation.Results()[positive];
		auto &container = descent.Containers()[positive];
		vector<idx_t> representatives(indexer.NamedInstances(link.aux_idx), DConstants::INVALID_INDEX);
		for (auto entry : SurvivingEntries(positive, true)) {
			const auto instance = indexer.InstanceOf(link.aux_idx, container.entries[entry].row);
			if (instance < representatives.size() && representatives[instance] == DConstants::INVALID_INDEX) {
				representatives[instance] = entry;
			}
		}
		for (idx_t instance = 0; instance < representatives.size(); instance++) {
			const auto entry = representatives[instance];
			if (entry == DConstants::INVALID_INDEX || !indexer.HasColumn(link.aux_idx, instance)) {
				continue;
			}
			const auto result = NumericCast<int>(indexer.InstanceColumn(link.aux_idx, instance));
			auto inner = Compile(values.entries[entry], 0);
			inner.Add(Compile(values.rhs[entry], 0), -1, written);
			if (inner.terms[result] != 1) {
				Unsupported("an ABS definition without a unit output coefficient");
			}
			inner.terms.erase(result);
			inner.Scale(-1, written);
			const auto reach = Range(inner);
			if (reach.Unbounded() && link.y_idx != DConstants::INVALID_INDEX) {
				decide_linearize::ThrowUnboundedBigMNaming(true, reach.unbounded_var, names, "ABS", written);
			}
			const auto upper =
			    reach.Unbounded() ? DECIDE_SOLVER_INFINITY : MaxValue(std::abs(reach.lo), std::abs(reach.hi));
			AuxiliaryRange(link.aux_idx, instance, 0, upper);
			if (link.y_idx == DConstants::INVALID_INDEX) {
				inner.Compact();
				int argument;
				if (inner.terms.size() == 1 && inner.terms.begin()->second == 1 && inner.constant == 0) {
					argument = inner.terms.begin()->first;
				} else {
					argument =
					    NumericCast<int>(decide_linearize::AddGlobalContinuousAux(input, indexer, reach, 0, written));
					auto negative = inner;
					negative.Scale(-1, written);
					Structural(negative, argument, {}, '=', 0);
				}
				GeneralConstraintSpec construct;
				construct.kind = GeneralConstraintKind::ABS;
				construct.result_column = result;
				construct.argument_columns.push_back(argument);
				construct.source_clause_id = source;
				construct.removal_group_id = removal;
				input.general_constraints.push_back(std::move(construct));
			} else {
				const auto row = container.entries[entry].row;
				const auto sign = NumericCast<int>(indexer.Get(link.y_idx, row));
				const auto negative_relaxation = 2 * MaxValue(0.0, -reach.lo);
				const auto positive_relaxation = 2 * MaxValue(0.0, reach.hi);
				auto negative = inner;
				negative.Scale(-1, written);
				Structural(negative, result, {{sign, negative_relaxation}}, '<', negative_relaxation);
				Structural(inner, result, {{sign, -positive_relaxation}}, '<', 0);
			}
		}
	}

	TreeLinearForm Scalar(const DecideTreeValue &value, int push) {
		auto &op = value.operation;
		auto &args = value.operands;
		if (value.expression && value.expression->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			if (args.size() != 1 || (value.type.IsIntegral() && !args[0]->type.IsIntegral())) {
				Unsupported("a non-linear cast");
			}
			return Compile(args[0], push);
		}
		if ((op == "+" || op == "-") && !args.empty() && args.size() <= 2) {
			if (args.size() == 1) {
				auto form = Compile(args[0], op == "-" ? -push : push);
				if (op == "-") {
					form.Scale(-1, written);
				}
				return form;
			}
			auto form = Compile(args[0], push);
			form.Add(Compile(args[1], op == "-" ? -push : push), op == "-" ? -1 : 1, written);
			return form;
		}
		if ((op == "*" || op == "/") && args.size() == 2) {
			idx_t scalar = DConstants::INVALID_INDEX;
			if (args[1]->kind == DecideTreeValueKind::DATA) {
				scalar = 1;
			} else if (op == "*" && args[0]->kind == DecideTreeValueKind::DATA) {
				scalar = 0;
			}
			if (scalar == DConstants::INVALID_INDEX) {
				if (op == "*") {
					return Multiply(Compile(args[0], 0), Compile(args[1], 0));
				}
				Unsupported("division by a decision value");
			}
			double factor = Number(args[scalar]);
			if (op == "/") {
				if (factor == 0) {
					throw InvalidInputException(DecideInvalidValueMessage(DecideValueRole::COEFFICIENT, written));
				}
				factor = 1 / factor;
			}
			if (factor == 0) {
				return TreeLinearForm();
			}
			auto form = Compile(args[1 - scalar], factor < 0 ? -push : push);
			form.Scale(factor, written);
			return form;
		}
		if ((op == "power" || op == "pow" || op == "^") && args.size() == 2 &&
		    args[1]->kind == DecideTreeValueKind::DATA && Number(args[1]) == 2) {
			auto form = Compile(args[0], 0);
			return Multiply(form, form);
		}
		if (op == "abs" && args.size() == 1) {
			auto found = absolute_values.find(&value);
			if (found == absolute_values.end()) {
				auto negative = make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::SCALAR, value.type);
				negative->operation = "-";
				negative->operands.push_back(args[0]);
				auto absolute = make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::REDUCER, value.type);
				absolute->node = value.node;
				absolute->operation = "max";
				absolute->operands.push_back(args[0]);
				absolute->operands.push_back(std::move(negative));
				found = absolute_values.emplace(&value, std::move(absolute)).first;
			}
			return Reducer(found->second, push);
		}
		Unsupported(op.empty() ? "a scalar expression" : op);
	}

	TreeLinearForm Multiply(const TreeLinearForm &left, const TreeLinearForm &right) const {
		if (!left.quadratic.empty() || !right.quadratic.empty()) {
			Unsupported("a polynomial of degree greater than two");
		}
		TreeLinearForm result;
		AddCoefficient(result.constant, left.constant * right.constant, written);
		for (auto &term : left.terms) {
			AddCoefficient(result.terms[term.first], term.second * right.constant, written);
		}
		for (auto &term : right.terms) {
			AddCoefficient(result.terms[term.first], term.second * left.constant, written);
		}
		for (auto &a : left.terms) {
			for (auto &b : right.terms) {
				const pair<int, int> monomial(MaxValue(a.first, b.first), MinValue(a.first, b.first));
				AddCoefficient(result.quadratic[monomial], a.second * b.second, written);
			}
		}
		result.curvature = left.terms == right.terms ? 1 : 2;
		result.Compact();
		return result;
	}

	AuxRange Range(const TreeLinearForm &form) const {
		if (!form.quadratic.empty()) {
			Unsupported("an extremum over a quadratic decision expression");
		}
		double lo = form.constant, hi = form.constant;
		bool low_open = false, high_open = false;
		idx_t blame = DConstants::INVALID_INDEX;
		for (auto &term : form.terms) {
			if (std::abs(term.second) < 1e-15) {
				continue;
			}
			double lower, upper;
			const auto column = idx_t(term.first);
			if (column >= indexer.global_block_start) {
				const auto global = column - indexer.global_block_start;
				lower = input.global_lower_bounds[global];
				upper = input.global_upper_bounds[global];
			} else {
				const auto variable = indexer.OwnerOf(column);
				const auto instance = indexer.InstanceOfColumn(variable, column);
				lower = input.InstanceBoxLower(variable, instance, input.lower_bounds);
				upper = input.InstanceBoxUpper(variable, instance, input.upper_bounds);
				if ((DecideIsOpenBelow(lower) || DecideIsOpenAbove(upper)) && blame == DConstants::INVALID_INDEX) {
					blame = variable;
				}
			}
			if (term.second < 0) {
				std::swap(lower, upper);
			}
			const auto low = term.second * lower;
			const auto high = term.second * upper;
			if (DecideIsOpenBelow(low) || !std::isfinite(low)) {
				low_open = true;
			} else {
				AddCoefficient(lo, low, written);
			}
			if (DecideIsOpenAbove(high) || !std::isfinite(high)) {
				high_open = true;
			} else {
				AddCoefficient(hi, high, written);
			}
		}
		AuxRange range;
		range.CoverRowSided(low_open ? -std::numeric_limits<double>::infinity() : lo,
		                    high_open ? std::numeric_limits<double>::infinity() : hi, blame);
		return range;
	}

	TreeLinearForm Reducer(const DecideTreeValuePtr &value, int push) {
		auto &op = value->operation;
		if (op == "sum" || op == "avg") {
			TreeLinearForm form;
			for (auto &member : value->operands) {
				form.Add(Compile(member, push), 1, written);
			}
			if (op == "avg" && !value->operands.empty()) {
				form.Scale(1.0 / double(value->operands.size()), written);
			}
			return form;
		}
		if (op != "min" && op != "max") {
			Unsupported(op);
		}
		if (value->operands.empty()) {
			throw InternalException("DECIDE tree retained an empty extremum");
		}
		if (value->operands.size() == 1) {
			return Compile(value->operands[0], push);
		}
		const bool is_max = op == "max";
		const bool envelope = push == 0 || (is_max ? push > 0 : push < 0);
		const bool closing = push == 0 || !envelope;
		vector<TreeLinearForm> members;
		vector<AuxRange> ranges;
		AuxRange family;
		for (auto &operand : value->operands) {
			members.push_back(Compile(operand, push));
			ranges.push_back(Range(members.back()));
			family.Cover(ranges.back());
		}
		auto found = extrema.find(value.get());
		if (found == extrema.end()) {
			const auto label = value->node < tree.nodes.size() ? tree.nodes[value->node]->text : op;
			const auto column = decide_linearize::AddGlobalContinuousAux(input, indexer, family, 0, label);
			if (value->type.IsIntegral()) {
				integral_columns.insert(NumericCast<int>(column));
			}
			found = extrema.emplace(value.get(), TreeExtremumColumn {column, false, false}).first;
		}
		auto &cached = found->second;
		const bool add_envelope = envelope && !cached.envelope;
		const bool add_closing = closing && !cached.closing;
		if (add_envelope || add_closing) {
			ExtremumLinkSpec spec;
			spec.result_column = cached.column;
			spec.is_max = is_max;
			spec.need_envelope = add_envelope;
			spec.need_closing = add_closing;
			spec.closing_underivable = family.Unbounded();
			spec.closing_big_m = family.Unbounded() ? 0 : family.Span();
			spec.blame_var = family.unbounded_var;
			spec.written = written;
			spec.label = value->node < tree.nodes.size() ? tree.nodes[value->node]->text : op;
			spec.source_clause_id = source;
			spec.removal_group_id = removal;
			ExtremumMemberSource member_source;
			member_source.count = members.size();
			member_source.fill_link = [&](idx_t index, MinMaxLinkRow &link) {
				for (auto &term : members[index].terms) {
					if (std::abs(term.second) >= 1e-15) {
						link.indices.push_back(term.first);
						link.coefficients.push_back(-term.second);
					}
				}
				link.constant = members[index].constant;
			};
			member_source.range = [&](idx_t index) {
				return ranges[index];
			};
			EmitExtremumLink(input, indexer, spec, member_source, names, policy);
			cached.envelope |= add_envelope;
			cached.closing |= add_closing;
		}
		TreeLinearForm result;
		result.terms[NumericCast<int>(cached.column)] = 1;
		return result;
	}

	bool FoldBound(const DecideNode &node, const TreeLinearForm &form, char sense, double rhs) {
		// A box holds one interval, and `<>` admits two.
		if (sense == '!' || repair || (!node.child_reads.empty() && !(node.facts && node.facts->was_minmax_easy)) ||
		    !form.quadratic.empty() || form.terms.size() != 1 || node.defined_var != DConstants::INVALID_INDEX) {
			return false;
		}
		const auto term = *form.terms.begin();
		const auto column = idx_t(term.first);
		if (column >= indexer.global_block_start || term.second == 0) {
			return false;
		}
		const auto variable = indexer.OwnerOf(column);
		const auto instance = indexer.InstanceOfColumn(variable, column);
		const auto bound = rhs / term.second;
		if (term.second < 0) {
			sense = sense == '<' ? '>' : sense == '>' ? '<' : '=';
		}
		input.instance_reach_lower.resize(input.num_decide_vars);
		input.instance_reach_upper.resize(input.num_decide_vars);
		auto &lower = input.instance_reach_lower[variable];
		auto &upper = input.instance_reach_upper[variable];
		const auto count = indexer.NamedInstances(variable);
		if (lower.empty()) {
			lower.assign(count, -std::numeric_limits<double>::infinity());
		}
		if (upper.empty()) {
			upper.assign(count, std::numeric_limits<double>::infinity());
		}
		if (sense == '>' || sense == '=') {
			lower[instance] = MaxValue(lower[instance], bound);
		}
		if (sense == '<' || sense == '=') {
			upper[instance] = MinValue(upper[instance], bound);
		}
		return true;
	}

	bool Integral(const TreeLinearForm &form) const {
		for (auto &term : form.terms) {
			if (std::abs(term.second - std::round(term.second)) > 1e-9) {
				return false;
			}
			if (idx_t(term.first) >= indexer.global_block_start) {
				if (!integral_columns.count(term.first)) {
					return false;
				}
			} else {
				const auto variable = indexer.OwnerOf(term.first);
				if (!input.variable_types[variable].IsIntegral() &&
				    input.variable_types[variable] != LogicalType::BOOLEAN) {
					return false;
				}
			}
		}
		for (auto &term : form.quadratic) {
			if (std::abs(term.second - std::round(term.second)) > 1e-9) {
				return false;
			}
			for (auto column : {term.first.first, term.first.second}) {
				if (idx_t(column) >= indexer.global_block_start) {
					if (!integral_columns.count(column)) {
						return false;
					}
				} else {
					const auto variable = indexer.OwnerOf(column);
					if (!input.variable_types[variable].IsIntegral() &&
					    input.variable_types[variable] != LogicalType::BOOLEAN) {
						return false;
					}
				}
			}
		}
		return true;
	}

	double ComparisonScale(const DecideTreeValuePtr &value) const {
		if (!value) {
			return 1;
		}
		double scale = 1;
		for (auto &operand : value->operands) {
			// A common multiple suffices; avoid rationalizing arbitrary floating
			// coefficients that the bind-time integer gate has already refused.
			const auto factor = ComparisonScale(operand);
			if (factor > 1) {
				scale *= factor;
			}
		}
		if (value->kind == DecideTreeValueKind::REDUCER && value->operation == "avg") {
			scale *= double(value->operands.size());
		}
		if (!std::isfinite(scale)) {
			throw InvalidInputException(DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, written));
		}
		return scale;
	}

	void NotEqual(TreeLinearForm form, SolverInput::RawConstraint provenance) {
		if (!form.quadratic.empty() || !Integral(form)) {
			throw InternalException("DECIDE: a '<>' whose left-hand side is not integer-valued reached the "
			                        "model builder. Both halves of that refusal are stated on declared types "
			                        "at bind time — ValidateDecideNoIntegerStepComparisonOnReal for the "
			                        "decision, ValidateDecideIntegralComparisonOperands for every other "
			                        "operand — so arriving here is an invariant violation, not a query error.");
		}
		const auto rhs = -form.constant;
		if (std::abs(rhs - std::round(rhs)) >= 1e-9) {
			return;
		}
		const auto reach = Range(form);
		const auto lo = reach.lo_unbounded ? -std::numeric_limits<double>::infinity() : reach.lo;
		const auto hi = reach.hi_unbounded ? std::numeric_limits<double>::infinity() : reach.hi;
		if (hi < -0.5 || lo > 0.5) {
			return;
		}
		auto branch = [&](char sense, double bound) {
			auto row = provenance;
			row.sense = sense;
			row.rhs = bound;
			return row;
		};
		if (lo > -0.5) {
			input.global_constraints.push_back(branch('>', rhs + 1));
			return;
		}
		if (hi < 0.5) {
			input.global_constraints.push_back(branch('<', rhs - 1));
			return;
		}
		if (!std::isfinite(lo) || !std::isfinite(hi)) {
			decide_linearize::ThrowUnboundedBigMNaming(reach.Unbounded(), reach.unbounded_var, names, "<>", written);
		}
		const auto binary = decide_linearize::AddGlobalBinaryAux(input, indexer, 0, written);
		const auto upper_m = hi + 1;
		const auto lower_m = 1 - lo;
		auto lower = branch('<', rhs - 1);
		lower.indices.push_back(NumericCast<int>(binary));
		lower.coefficients.push_back(-upper_m);
		lower.indicator_col = binary;
		auto upper = branch('>', rhs + 1 - lower_m);
		upper.indices.push_back(NumericCast<int>(binary));
		upper.coefficients.push_back(-lower_m);
		upper.indicator_col = binary;
		input.global_constraints.push_back(std::move(lower));
		input.global_constraints.push_back(std::move(upper));
	}

	void Root(idx_t root) {
		auto &node = *tree.nodes[root];
		// A folded bound on an inlined reducer must still constrain that synthesized value: its old helper's
		// declaration box has no column in this model. Only a bound on a retained declaration stays implicit.
		if ((!repair && ((node.folded || node.repair_rows) && node.child_reads.empty())) || node.value_check ||
		    (node.hidden && node.comparison_type == ExpressionType::INVALID)) {
			return;
		}
		written = node.text;
		source = node.source_clause_id;
		removal = node.removal_group_id;
		char sense;
		int push;
		switch (node.comparison_type) {
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			sense = '<';
			push = 1;
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			sense = '>';
			push = -1;
			break;
		case ExpressionType::COMPARE_EQUAL:
			sense = '=';
			push = 0;
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
			sense = '!';
			push = 0;
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			sense = '<';
			push = 1;
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			sense = '>';
			push = -1;
			break;
		default:
			Unsupported("this comparison");
		}
		auto &state = evaluation.Results()[root];
		auto &container = descent.Containers()[root];
		struct EmittedGroup {
			TreeLinearForm form;
			idx_t row;
			bool quadratic;
		};
		vector<EmittedGroup> emitted;
		std::unordered_map<size_t, vector<idx_t>> grouped;
		const bool collapse = node.facts && (node.facts->lhs_is_aggregate || node.facts->was_minmax_easy);
		for (auto index : SurvivingEntries(root, true)) {
			if (index >= state.entries.size() || !state.entries[index] || !state.entries[index]->Present()) {
				continue;
			}
			if (index >= state.rhs.size() || !state.rhs[index] || !state.rhs[index]->Present()) {
				continue;
			}
			for (auto &body : Bounded(node, state.entries[index], push)) {
				auto form = Compile(body, push);
				const auto lhs_constant = form.constant;
				if (state.rhs[index]->kind == DecideTreeValueKind::DATA) {
					const auto bound = Number(state.rhs[index], DecideValueRole::BOUND);
					if (std::isinf(bound)) {
						if (sense == '!' || (sense == '<' && bound > 0) || (sense == '>' && bound < 0)) {
							continue;
						}
						SolverInput::RawConstraint impossible;
						impossible.sense = sense;
						impossible.rhs = bound;
						impossible.kind = node.facts ? node.facts->kind : ConstraintKind::USER_PARAMETER;
						impossible.shape = ElasticShape::SHARED_SCALAR;
						impossible.source_clause_id = source;
						impossible.repair_group_id = root;
						impossible.removal_group_id = removal;
						Provenance(root, container.entries[index], impossible);
						input.global_constraints.push_back(std::move(impossible));
						continue;
					}
				}
				form.Add(Compile(state.rhs[index], -push, DecideValueRole::BOUND), -1, written);
				form.Compact();
				const bool strict = node.comparison_type == ExpressionType::COMPARE_LESSTHAN ||
				                    node.comparison_type == ExpressionType::COMPARE_GREATERTHAN;
				if (strict || sense == '!') {
					form.Scale(ComparisonScale(state.entries[index]), written);
				}
				double rhs = -form.constant;
				const auto typed = rhs + lhs_constant;
				if (strict) {
					if (!Integral(form)) {
						throw InternalException(
						    "Strict inequality with a non-integer-valued left-hand side reached "
						    "the tree model builder; the bind-time type gate should have rejected it");
					}
					rhs = DecideStrictBound(sense == '>', rhs);
				}
				if (FoldBound(node, form, sense, rhs)) {
					continue;
				}
				SolverInput::RawConstraint row;
				for (auto &term : form.terms) {
					row.indices.push_back(term.first);
					row.coefficients.push_back(term.second);
				}
				row.sense = sense;
				row.rhs = rhs;
				row.kind = node.facts                                      ? node.facts->kind
				           : node.defined_var == DConstants::INVALID_INDEX ? ConstraintKind::USER_PARAMETER
				                                                           : ConstraintKind::STRUCTURAL;
				const bool shared_rhs =
				    node.facts && node.facts->rhs_expr &&
				    (node.facts->rhs_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT ||
				     IsQueryWideBoundTag(node.facts->rhs_expr->GetAlias()));
				row.shape = shared_rhs ? ElasticShape::SHARED_SCALAR : ElasticShape::PER_ROW_DATA;
				row.source_clause_id = source;
				row.repair_group_id = root;
				row.removal_group_id = removal;
				row.is_aggregate = node.facts && node.facts->lhs_is_aggregate;
				if (!shared_rhs && source < input.constraint_sources.size()) {
					row.rhs_label = input.constraint_sources[source].canonical_rhs;
				}
				row.rhs_mechanism_offset = -lhs_constant;
				row.strict = strict;
				row.typed_k = typed;
				Provenance(root, container.entries[index], row);
				if (collapse && sense != '!') {
					size_t hash = 0;
					for (auto &term : form.terms) {
						hash = MurmurHash64(hash ^ std::hash<int>()(term.first) ^ std::hash<double>()(term.second));
					}
					for (auto &term : form.quadratic) {
						hash = MurmurHash64(hash ^ std::hash<int>()(term.first.first) ^
						                    std::hash<int>()(term.first.second) ^ std::hash<double>()(term.second));
					}
					auto &bucket = grouped[hash];
					bool merged = false;
					for (auto position : bucket) {
						auto &prior = emitted[position];
						if (prior.form.terms != form.terms || prior.form.quadratic != form.quadratic) {
							continue;
						}
						auto &previous = prior.quadratic ? input.raw_quadratic_constraints[prior.row].linear
						                                 : input.global_constraints[prior.row];
						if (sense == '=' && previous.rhs != row.rhs) {
							const auto bound = source < input.constraint_sources.size()
							                       ? input.constraint_sources[source].written_rhs
							                       : written;
							throw InvalidInputException(
							    "`%s` takes more than one value here (%g and %g) — an `=` bound cannot hold "
							    "two different values at once, so this DECIDE query is infeasible.",
							    bound, MinValue(previous.rhs, row.rhs), MaxValue(previous.rhs, row.rhs));
						}
						if ((sense == '<' && row.rhs < previous.rhs) || (sense == '>' && row.rhs > previous.rhs)) {
							previous.rhs = row.rhs;
							previous.rhs_mechanism_offset = row.rhs_mechanism_offset;
							previous.typed_k = row.typed_k;
						}
						MergeClasses(previous.class_set, row.class_set);
						merged = true;
						break;
					}
					if (merged) {
						continue;
					}
					bucket.push_back(emitted.size());
					emitted.push_back({form,
					                   form.quadratic.empty() ? input.global_constraints.size()
					                                          : input.raw_quadratic_constraints.size(),
					                   !form.quadratic.empty()});
				}
				if (sense == '!') {
					not_equals.push_back({std::move(form), std::move(row), written});
				} else if (form.quadratic.empty()) {
					input.global_constraints.push_back(std::move(row));
				} else {
					SolverInput::RawQuadraticConstraint polynomial;
					polynomial.linear = std::move(row);
					for (auto &term : form.quadratic) {
						polynomial.quadratic.push_back({term.first.first, term.first.second, term.second});
					}
					input.raw_quadratic_constraints.push_back(std::move(polynomial));
				}
			}
		}
	}

	idx_t ObjectiveRow(idx_t variable, idx_t column) {
		if (indexer.var_scope[variable] == DecideVarScope::ROW) {
			return indexer.InstanceOfColumn(variable, column);
		}
		if (indexer.var_scope[variable] == DecideVarScope::GLOBAL) {
			return 0;
		}
		auto found = instance_rows.find(variable);
		if (found == instance_rows.end()) {
			vector<idx_t> first(indexer.NamedInstances(variable), DConstants::INVALID_INDEX);
			for (idx_t row = 0; row < input.num_rows; row++) {
				const auto instance = indexer.InstanceOf(variable, row);
				if (instance < first.size() && first[instance] == DConstants::INVALID_INDEX) {
					first[instance] = row;
				}
			}
			found = instance_rows.emplace(variable, std::move(first)).first;
		}
		return found->second[indexer.InstanceOfColumn(variable, column)];
	}

	void Objective(idx_t root) {
		auto &node = *tree.nodes[root];
		written = input.objective_written.empty() ? node.text : input.objective_written;
		source = removal = DConstants::INVALID_INDEX;
		auto &state = evaluation.Results()[root];
		auto &container = descent.Containers()[root];
		const auto push = input.sense == DecideSense::MAXIMIZE ? -1 : 1;
		auto surviving = SurvivingEntries(root, false);
		if (surviving.size() > 1) {
			ObjectiveDifference(node, container, surviving);
		}
		for (auto index : surviving) {
			if (index >= state.entries.size() || !state.entries[index] || !state.entries[index]->Present()) {
				continue;
			}
			auto form = Compile(state.entries[index], push);
			form.Compact();
			AddCoefficient(input.objective_constant, form.constant, written);
			for (auto &term : form.quadratic) {
				input.raw_quadratic_objective.push_back({term.first.first, term.first.second, term.second});
			}
			if (!form.quadratic.empty()) {
				input.raw_quadratic_objective_nonconvex |=
				    form.curvature == 2 ||
				    (input.sense == DecideSense::MAXIMIZE ? form.curvature > 0 : form.curvature < 0);
			}
			for (auto &term : form.terms) {
				const auto column = idx_t(term.first);
				if (column >= indexer.global_block_start) {
					AddCoefficient(input.global_obj_coeffs[column - indexer.global_block_start], term.second, written);
					continue;
				}
				const auto variable = indexer.OwnerOf(column);
				auto found = objective_term.find(variable);
				if (found == objective_term.end()) {
					const auto term_index = input.objective_variable_indices.size();
					input.objective_variable_indices.push_back(variable);
					input.objective_coefficients.push_back(CoefficientColumn::MakeDense(input.num_rows));
					found = objective_term.emplace(variable, term_index).first;
				}
				const auto row = ObjectiveRow(variable, column);
				if (row == DConstants::INVALID_INDEX) {
					throw InternalException("DECIDE tree objective refers to a class no row names");
				}
				auto &coefficient = input.objective_coefficients[found->second].dense_values[row];
				AddCoefficient(coefficient, term.second, written);
			}
		}
	}

	[[noreturn]] void ObjectiveDifference(const DecideNode &node, const DecideNodeContainer &container,
	                                      const vector<idx_t> &surviving) const {
		idx_t offset = 0;
		string reason = "the kept rows have more than one variant";
		auto varies = [&](idx_t column, string &values) {
			const Value *least = nullptr, *greatest = nullptr;
			auto before = [](const Value &left, const Value &right) {
				return !left.IsNull() && (right.IsNull() || left < right);
			};
			for (auto entry : surviving) {
				if (column >= container.entries[entry].signature.size()) {
					return false;
				}
				auto &value = container.entries[entry].signature[column];
				if (!least || before(value, *least)) {
					least = &value;
				}
				if (!greatest || before(*greatest, value)) {
					greatest = &value;
				}
			}
			if (!least || Value::NotDistinctFrom(*least, *greatest)) {
				return false;
			}
			values = DecideSqlLiteral(*least) + ", " + DecideSqlLiteral(*greatest);
			return true;
		};
		bool found = false;
		for (auto &field : node.signature) {
			string values;
			if (field.kind == DecideSignatureFieldKind::VALUE) {
				if (varies(offset, values)) {
					reason = StringUtil::Format("'%s' takes more than one value over the kept rows (%s)", field.text,
					                            values);
					found = true;
				}
				offset++;
			} else if (field.kind == DecideSignatureFieldKind::DECISION) {
				if (varies(offset + 1, values)) {
					auto &scope = tree.variable_scopes[field.decision];
					reason = StringUtil::Format(
					    "the decision '%s' (declared per %s) has more than one instance over the kept rows", field.text,
					    scope.HasDeclaredKey() ? scope.declared_key : string("row"));
					found = true;
				}
				offset += 2;
			} else if (field.kind == DecideSignatureFieldKind::BY_KEY) {
				for (auto &element : DecideKeyElements(tree.keys, field.key)) {
					if (!found && varies(offset, values)) {
						reason = StringUtil::Format("'%s' in %s takes more than one value over the kept rows (%s)",
						                            element.name, field.text, values);
						found = true;
					}
					offset++;
				}
			} else {
				reason = StringUtil::Format("'%s' has more than one variant over the kept rows", field.text);
				found = true;
			}
			if (found) {
				break;
			}
		}
		throw InvalidInputException(
		    "DECIDE objective: %s, so the objective would be produced more than once; move it inside a reducer or "
		    "fix it with when.",
		    reason);
	}
};

} // namespace

void FormulateDecideTree(const DecideTree &tree, const DecideDescent &descent, const DecideTreeEvaluation &evaluation,
                         SolverInput &input, const VarIndexer &indexer, NativeConstructPolicy policy,
                         const vector<string> &variable_names, bool repair) {
	TreeFormulator(tree, descent, evaluation, input, indexer, policy, variable_names, repair).Run();
}

} // namespace duckdb
