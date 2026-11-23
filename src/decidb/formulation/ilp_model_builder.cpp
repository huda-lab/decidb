#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/decide_profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

DecideClauseRef DecideWrittenClause(const SolverInput &input, idx_t source_clause_id) {
	return source_clause_id < input.constraint_sources.size()
	           ? DecideClauseRefOf(input.constraint_sources, input.source_text, source_clause_id)
	           : input.objective_written;
}

static uint64_t PackQKey(int row, int col) {
	return (static_cast<uint64_t>(row) << 32) | static_cast<uint32_t>(col);
}

//! A linear form on one row: its decisions, as flat columns with their nonzero coefficients, and its constant.
struct FormRow {
	vector<std::pair<int, double>> terms;
	double constant = 0.0;
};

static FormRow ReadFormRow(const vector<idx_t> &variables, const vector<CoefficientColumn> &columns,
                           const vector<TermInstances> &instances, idx_t place, idx_t row, const VarIndexer &indexer,
                           const DecideClauseRef &written) {
	FormRow form;
	// `variables` and `columns` are pushed together (DecideModelEmitter), and each column holds the data row `row` at
	// `place`.
	for (idx_t t = 0; t < variables.size(); t++) {
		double a = columns[t].Get(place);
		if (a == 0.0) {
			continue;
		}
		if (variables[t] == DConstants::INVALID_INDEX) {
			AddCoefficient(form.constant, a, written);
			continue;
		}
		form.terms.emplace_back(static_cast<int>(indexer.InstanceColumn(variables[t], instances[t].At(place, row))), a);
	}
	return form;
}

//! Q's entry for the monomial `x_a * x_b`, which `x_b * x_a` shares: the lower triangle (ilp_model.hpp).
static double &QEntry(std::unordered_map<uint64_t, double> &q, int a, int b) {
	return q[PackQKey(std::max(a, b), std::min(a, b))];
}

//! A square `w * (a·x + c)^2` on one row: the symmetric rank-one update `w * a * aᵀ` (BLAS SYR), its triangle computed
//! once and an off-diagonal pair doubled; `2wc·a` to the linear part and `wc^2` to the constant.
template <class ADD_LINEAR>
static void AddSquareRow(const FormRow &form, double w, std::unordered_map<uint64_t, double> &q, ADD_LINEAR &add_linear,
                         double &constant, const DecideClauseRef &written) {
	for (idx_t i = 0; i < form.terms.size(); i++) {
		for (idx_t j = 0; j <= i; j++) {
			double val = w * form.terms[i].second * form.terms[j].second;
			if (i != j) {
				val *= 2.0;
			}
			AddCoefficient(QEntry(q, form.terms[i].first, form.terms[j].first), val, written);
		}
	}
	if (form.constant != 0.0) {
		for (auto &term : form.terms) {
			add_linear(term.first, w * 2.0 * form.constant * term.second);
		}
		AddCoefficient(constant, w * form.constant * form.constant, written);
	}
}

//! A product `w * (a·x + c) * (b·x + d)` on one row: the rank-one update `w * a * bᵀ`, each pair added to the monomial
//! it is; `wd·a + wc·b` to the linear part and `wcd` to the constant.
template <class ADD_LINEAR>
static void AddProductRow(const FormRow &left, const FormRow &right, double w, std::unordered_map<uint64_t, double> &q,
                          ADD_LINEAR &add_linear, double &constant, const DecideClauseRef &written) {
	for (auto &l : left.terms) {
		for (auto &r : right.terms) {
			AddCoefficient(QEntry(q, l.first, r.first), w * l.second * r.second, written);
		}
	}
	if (right.constant != 0.0) {
		for (auto &l : left.terms) {
			add_linear(l.first, w * l.second * right.constant);
		}
	}
	if (left.constant != 0.0) {
		for (auto &r : right.terms) {
			add_linear(r.first, w * left.constant * r.second);
		}
	}
	AddCoefficient(constant, w * left.constant * right.constant, written);
}

//! One product group (EvaluatedProductGroup) on the data row `row`, which its columns hold at `place`: Q gets
//! `w * l * rᵀ` in the monomial convention, the cross terms with a form's constant go to `add_linear`, and the product
//! of the constants to `constant`.
template <class ADD_LINEAR>
static void AddProductGroupRow(const EvaluatedProductGroup &group, idx_t place, idx_t row, const VarIndexer &indexer,
                               std::unordered_map<uint64_t, double> &q, ADD_LINEAR &&add_linear, double &constant,
                               const DecideClauseRef &written) {
	double w = group.weight.Get(place);
	if (w == 0.0) {
		return;
	}
	auto left =
	    ReadFormRow(group.left_variables, group.left_coefficients, group.left_instances, place, row, indexer, written);
	if (group.square) {
		AddSquareRow(left, w, q, add_linear, constant, written);
		return;
	}
	auto right = ReadFormRow(group.right_variables, group.right_coefficients, group.right_instances, place, row,
	                         indexer, written);
	AddProductRow(left, right, w, q, add_linear, constant, written);
}

static void BuildVarIndexer(VarIndexer &idx, const SolverInput &input) {
	idx.num_rows = input.num_rows;
	idx_t num_decide_vars = input.num_decide_vars;

	idx.var_scope.resize(num_decide_vars, DecideVarScope::ROW);
	idx.row_var_offset.resize(num_decide_vars, DConstants::INVALID_INDEX);
	idx.keyed_var_base.resize(num_decide_vars, DConstants::INVALID_INDEX);
	idx.var_keyed_mapping_idx.resize(num_decide_vars, DConstants::INVALID_INDEX);
	idx.scalar_var_index.resize(num_decide_vars, DConstants::INVALID_INDEX);

	// Classify variables and compute row-block offsets
	idx_t row_var_count = 0;
	for (idx_t v = 0; v < num_decide_vars; v++) {
		// A variable with no recorded scope (synthesized auxiliaries appended after
		// the declaration list) is row-scoped, matching the previous default.
		auto scope = v < input.variable_scopes.size() ? input.variable_scopes[v].scope : DecideVarScope::ROW;
		idx.var_scope[v] = scope;
		if (scope == DecideVarScope::ROW) {
			idx.row_var_offset[v] = row_var_count;
			idx.row_offset_var.push_back(v);
			row_var_count++;
		}
	}
	idx.num_row_vars = row_var_count;

	// Row block: num_rows * num_row_vars
	idx.keyed_block_start = input.num_rows * row_var_count;

	// The instances stage 08 wrote where they are read, which have no column.
	auto written_in = [&](idx_t v) -> const vector<bool> * {
		return v < input.written_in.size() && !input.written_in[v].empty() ? &input.written_in[v] : nullptr;
	};
	// Keyed block: assign base offsets per keyed variable.
	idx_t keyed_offset = 0;
	for (idx_t v = 0; v < num_decide_vars; v++) {
		if (idx.var_scope[v] != DecideVarScope::KEYED) {
			continue;
		}
		idx_t scope_idx = input.variable_scopes[v].keyed_scope_idx;
		D_ASSERT(scope_idx < idx.keyed_mappings.size());
		idx.var_keyed_mapping_idx[v] = scope_idx;
		idx.keyed_var_base[v] = idx.keyed_block_start + keyed_offset;
		idx.keyed_block_var.push_back(v);
		// An instance no row reads -- the absent class -- or one written into its readers has no column.
		auto &mapping = idx.keyed_mappings[scope_idx];
		auto written = written_in(v);
		if (mapping.absent_class == DConstants::INVALID_INDEX && !written) {
			keyed_offset += mapping.num_classes;
			continue;
		}
		idx.instance_slot.resize(num_decide_vars);
		idx.slot_instance.resize(num_decide_vars);
		auto &slots = idx.instance_slot[v];
		slots.assign(mapping.num_classes, DConstants::INVALID_INDEX);
		for (idx_t inst = 0; inst < mapping.num_classes; inst++) {
			if (inst == mapping.absent_class || (written && (*written)[inst])) {
				continue;
			}
			slots[inst] = idx.slot_instance[v].size();
			idx.slot_instance[v].push_back(inst);
		}
		keyed_offset += idx.slot_instance[v].size();
	}

	// Scalar block: exactly one column per query-wide variable, independent of num_rows
	idx.scalar_block_start = idx.keyed_block_start + keyed_offset;
	idx_t scalar_count = 0;
	for (idx_t v = 0; v < num_decide_vars; v++) {
		if (idx.var_scope[v] != DecideVarScope::GLOBAL || written_in(v)) {
			continue;
		}
		idx.scalar_var_index[v] = idx.scalar_block_start + scalar_count;
		idx.scalar_slot_var.push_back(v);
		scalar_count++;
	}

	idx.global_block_start = idx.scalar_block_start + scalar_count;
	idx.total_vars = idx.global_block_start + input.num_global_vars;
}

idx_t VarIndexer::OwnerOf(idx_t col) const {
	D_ASSERT(col < global_block_start); // the global block has no decide variable
	if (col < keyed_block_start) {
		// Row block, laid out row-major: every row holds one slot per row-scoped var (keyed_block_start is 0 with
		// none).
		return row_offset_var[col % num_row_vars];
	}
	if (col < scalar_block_start) {
		// Keyed block: bases ascend in keyed_block_var order, so the owner is the
		// last variable whose base does not exceed `col`.
		idx_t owner = DConstants::INVALID_INDEX;
		for (idx_t v : keyed_block_var) {
			if (keyed_var_base[v] > col) {
				break;
			}
			owner = v;
		}
		return owner;
	}
	return scalar_slot_var[col - scalar_block_start];
}

VarIndexer VarIndexer::Build(SolverInput &input) {
	VarIndexer idx;
	// The layout is their one reader from here, and outlives the input for the readback.
	idx.keyed_mappings = std::move(input.keyed_mappings);
	BuildVarIndexer(idx, input);
	return idx;
}

//! Whether each decision term of `ec` has solver columns of its own: no row repeats a column
//! (EvaluatedConstraint::repeats_columns) and each decision is row-scoped. Then no two (term, data row) pairs share a
//! column, and a row's terms are read without summing them.
static bool TermsHaveTheirOwnColumns(const EvaluatedConstraint &ec, const VarIndexer &indexer) {
	// Keyed and query-wide decisions both put many rows on one column.
	return !ec.repeats_columns && std::all_of(ec.variable_indices.begin(), ec.variable_indices.end(),
	                                          [&](idx_t v) { return indexer.var_scope[v] == DecideVarScope::ROW; });
}

const EmittedRows &ClauseEmittedRows(const EvaluatedConstraint &ec, const VarIndexer &indexer,
                                     const DecideClauseRef &written) {
	auto &out = ec.emitted_rows;
	if (!out.offsets.empty()) {
		return out;
	}
	auto &records = ec.records;
	const idx_t num_emitted = records.Count();
	// Terms with columns of their own are copied; any other row sums its terms per column first.
	bool own_columns = TermsHaveTheirOwnColumns(ec, indexer);
	SparseCoeffAccumulator accum;
	if (own_columns) {
		out.columns.reserve(records.variables.size());
		out.values.reserve(records.variables.size());
	} else {
		accum.BeginForDecisions(indexer, records.variables.size() / std::max<idx_t>(num_emitted, 1), written);
	}
	out.offsets.reserve(num_emitted + 1);
	out.offsets.push_back(0);
	for (idx_t e = 0; e < num_emitted; e++) {
		for (idx_t t = records.term_begin[e]; t < records.term_begin[e + 1]; t++) {
			const double coefficient = records.coefficients[t];
			if (coefficient == 0.0) {
				continue;
			}
			const auto column = static_cast<int>(indexer.InstanceColumn(records.variables[t], records.instances[t]));
			if (own_columns) {
				out.columns.push_back(column);
				out.values.push_back(coefficient);
			} else {
				accum.Add(column, coefficient);
			}
		}
		if (!own_columns) {
			accum.Flush(out.columns, out.values);
		}
		out.offsets.push_back(out.columns.size());
	}
	return out;
}

//! F2: the provenance a row of `eval_const` starts from, whether linear or quadratic, aggregate or per-row. Every row
//! names its clause (`source_clause_id`): an error about a number the model computes quotes it. Everything else is what
//! a diagnosis reads and is stamped only under DIAGNOSE (SolverInput::diagnose): an ordinary query leaves it at its
//! default. Under DIAGNOSE the row starts as a copy of the record the emitter filled (EvaluatedConstraint::provenance).
//! A site builds the row from it, which may remember a typed K (`remember_typed_k`), and then places the row
//! (StampConstraintProvenance).
static ConstraintProvenance ClauseProvenance(const SolverInput &input, const EvaluatedConstraint &eval_const) {
	if (input.diagnose) {
		return eval_const.provenance;
	}
	ConstraintProvenance provenance;
	provenance.origin.source_clause_id = eval_const.provenance.origin.source_clause_id;
	return provenance;
}

//! F2: what only the builder knows of a row it states, stamped only under DIAGNOSE: the group the row sits in and its
//! name, the classes it stands for, the constant of the LHS the site folded into the RHS, and the data row.
//! `group_key` defaults to INVALID_INDEX for an ungrouped aggregate row; pass the `when`/`by` group id for a grouped
//! aggregate row, or the row's group for a per-row row. `row` is the data row a per-row row was emitted at (DIAGNOSE's
//! `row`); a row that stands for no class set is a kept row, a class of its own (D28). The group's name is the
//! clause's for `group_key` (EvaluatedConstraint::NameOf), so no site names a group apart from placing a row in it.
//! `lhs_offset` is the constant part of the LHS that this site folded into the RHS
//! (`SUM(x + c) >= K` is emitted as `SUM(x) >= K - Sum(c)`). The reconstructed clause
//! label still renders the LHS the user wrote, constant included, so the RHS it is
//! quoted against has to be the user's `K` and not the folded one — otherwise the report
//! names a bound nobody typed. It rides on `rhs_mechanism_offset` alongside whatever a
//! lowering added, because the reader needs their sum and neither alone.
static void StampConstraintProvenance(const SolverInput &input, ConstraintProvenance &provenance,
                                      const EvaluatedConstraint &eval_const,
                                      idx_t group_key = DConstants::INVALID_INDEX, double lhs_offset = 0.0,
                                      idx_t row = DConstants::INVALID_INDEX) {
	if (!input.diagnose) {
		return;
	}
	provenance.rhs_mechanism_offset = -lhs_offset;
	provenance.group_key = group_key;
	provenance.group_name = eval_const.NameOf(group_key);
	// Reducer-placement metadata for the diagnosis renderer's SUM(...)-wrapping (Facet B).
	provenance.is_aggregate = eval_const.lhs_is_aggregate;
	// The class set group `group_key`'s rows stand for (D28); an ungrouped clause has the one group 0.
	idx_t group = group_key == DConstants::INVALID_INDEX ? 0 : group_key;
	provenance.class_set =
	    group < eval_const.group_class_set.size() ? eval_const.group_class_set[group] : DConstants::INVALID_INDEX;
	if (row != DConstants::INVALID_INDEX) {
		provenance.data_row = row;
		provenance.per_kept_row = provenance.class_set == DConstants::INVALID_INDEX;
	}
}

//! A row a lowering states in flat columns, or a native construct, arrives with its provenance, which states what an
//! EvaluatedConstraint's sites derive from their group columns, so a clause reads the same in a diagnosis whichever
//! shape its rows took. Under DIAGNOSE it is kept whole (the `<>` disjunction binary's column too, so the infeasible
//! removal dial groups the two rows of an aggregate `<>` as a per-row one's); an ordinary query's row names its clause
//! and carries nothing else, as ClauseProvenance has it.
static void KeepProvenance(const SolverInput &input, ConstraintProvenance &provenance) {
	if (input.diagnose) {
		return;
	}
	ConstraintProvenance clause_only;
	clause_only.origin.source_clause_id = provenance.origin.source_clause_id;
	provenance = std::move(clause_only);
}

//! The sense and bound `comparison` gives `row`, whose bound `rhs` has the constant `lhs_offset` of its left side
//! moved off it already. A strict `<` / `>` steps the bound to the next integer, which is exact because the bind-time
//! type gate (DecideShapeCheck::CheckIntegerStep) admits only an integer-valued left side; under DIAGNOSE the row
//! remembers the K the user typed, so a suggestion is quoted against `> K` and not the stepped bound.
template <class ROW>
static void ApplyComparison(const SolverInput &input, ROW &row, ExpressionType comparison, double rhs,
                            double lhs_offset) {
	bool strict = false;
	switch (comparison) {
	case ExpressionType::COMPARE_LESSTHAN:
		strict = true;
		DUCKDB_EXPLICIT_FALLTHROUGH;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		row.sense = '<';
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		strict = true;
		DUCKDB_EXPLICIT_FALLTHROUGH;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		row.sense = '>';
		break;
	case ExpressionType::COMPARE_EQUAL:
		row.sense = '=';
		break;
	default:
		throw InternalException("Unsupported comparison type in ILP model builder");
	}
	row.rhs = strict ? DecideStrictBound(row.sense == '>', rhs) : rhs;
	if (strict && input.diagnose) {
		row.provenance.strict = true;
		row.provenance.typed_k = rhs + lhs_offset;
	}
}

//! The model class this built model demands: the facts the build settled, in the shape the plan-time prediction
//! (DeriveDecideModelClass) fills, classified by the same function. SolveModel checks the two agree before handing the
//! model to a backend.
SolverModelClass SolverModel::ModelClass() const {
	DecideModelShape shape;
	shape.quadratic_constraints = !quadratic_constraints.empty();
	shape.quadratic_objective = has_quadratic_obj;
	shape.nonconvex_objective = nonconvex_quadratic;
	// Any integral column, auxiliaries included: a Big-M indicator makes the program mixed-integer just as a user's
	// `x(INT)` does.
	for (idx_t col = 0; col < num_vars; col++) {
		if (is_integer[col]) {
			shape.integral_column = true;
			break;
		}
	}
	// An off-diagonal entry means two decision variables are coupled in the objective, which makes Q rank-deficient.
	for (idx_t k = 0; k < q_vals.size(); k++) {
		if (q_rows[k] != q_cols[k]) {
			shape.coupled_objective = true;
			break;
		}
	}
	return ClassifyDecideModel(shape);
}

bool SolverModel::ProvesInfeasible() const {
	for (const auto &row : constraints) {
		if ((row.indices.empty() && row.constant_fails) || IsUnreachableBound(row.sense, row.rhs)) {
			return true;
		}
	}
	for (const auto &row : quadratic_constraints) {
		if (IsUnreachableBound(row.sense, row.rhs)) {
			return true;
		}
	}
	for (idx_t col = 0; col < num_vars; col++) {
		if (col_lower[col] > col_upper[col]) {
			return true;
		}
	}
	return false;
}

void SolverModel::ClearObjective() {
	obj_coeffs.assign(obj_coeffs.size(), 0.0);
	obj_constant = 0.0;
	q_rows.clear();
	q_cols.clear();
	q_vals.clear();
	has_quadratic_obj = false;
	nonconvex_quadratic = false;
}

//! Build, step 1: the columns. Every decision instance's box and type from the input's boxes, then the global
//! auxiliaries' after them.
static void BuildModelColumns(SolverModel &model, SolverInput &input, const VarIndexer &indexer) {
	idx_t num_decide_vars = input.num_decide_vars;
	idx_t total_vars = indexer.total_vars;

	//===--------------------------------------------------------------------===//
	// 1. Set up variables with bounds and types
	//===--------------------------------------------------------------------===//

	// Determine per-variable types and default bounds
	vector<double> per_var_lower(num_decide_vars);
	vector<double> per_var_upper(num_decide_vars);
	vector<bool> per_var_binary(num_decide_vars, false);

	for (idx_t var = 0; var < num_decide_vars; var++) {
		auto logical_type = input.variable_types[var];

		if (logical_type == LogicalType::DOUBLE) {
			per_var_binary[var] = false;
			per_var_lower[var] = 0.0;
			per_var_upper[var] = DECIDE_SOLVER_INFINITY;
		} else if (logical_type == LogicalType::BOOLEAN) {
			per_var_binary[var] = true;
			per_var_lower[var] = 0.0;
			per_var_upper[var] = 1.0;
		} else {
			// INTEGER / BIGINT or default
			per_var_lower[var] = 0.0;
			per_var_upper[var] = DECIDE_SOLVER_INFINITY;
		}
	}

	// Merge with explicit bounds from input. The physical operator already
	// resolved each variable's lower bound (defaulting unconstrained variables to
	// 0 and honoring explicit negative bounds), so it is authoritative: take it
	// directly rather than std::max-ing with the type default, which would clamp a
	// legitimate negative bound back to 0. The upper bound is still intersected
	// (the type default is a true ceiling, e.g. 1.0 for BOOLEAN).
	for (idx_t var = 0; var < num_decide_vars; var++) {
		if (var < input.lower_bounds.size()) {
			per_var_lower[var] = input.lower_bounds[var];
		}
		if (var < input.upper_bounds.size()) {
			per_var_upper[var] = std::min(per_var_upper[var], input.upper_bounds[var]);
		}
	}

	// Expand per-variable config to all solver variables
	model.col_lower.resize(total_vars);
	model.col_upper.resize(total_vars);
	model.is_integer.resize(total_vars, false);
	model.is_binary.resize(total_vars, false);

	for (idx_t var = 0; var < num_decide_vars; var++) {
		idx_t num_instances = indexer.NumInstances(var);
		for (idx_t inst = 0; inst < num_instances; inst++) {
			if (!indexer.HasColumn(var, inst)) {
				continue;
			}
			idx_t var_idx = indexer.InstanceColumn(var, inst);
			// The instance's box: the decision's, with the floor raised by the refinement where the default floor of 0
			// still holds (a constant lower bound under a `when` replaces the default floor on the instances it keeps
			// alone), and a helper's ends at its own group's reach.
			model.col_lower[var_idx] = input.InstanceBoxLower(var, inst, per_var_lower);
			model.col_upper[var_idx] = input.InstanceBoxUpper(var, inst, per_var_upper);
			model.is_integer[var_idx] = input.variable_types[var] != LogicalType::DOUBLE;
			model.is_binary[var_idx] = per_var_binary[var];
		}
		// An auxiliary's instance every row of which reads it with no value (SolverInput::absent_instances) is fixed at
		// 0: no row reads it, so no bound folded into its box binds it (D88). A keyed decision's absent class has no
		// column (VarIndexer::instance_slot).
		for (idx_t inst = 0; var < input.absent_instances.size() && inst < input.absent_instances[var].size(); inst++) {
			if (input.absent_instances[var][inst] && indexer.HasColumn(var, inst)) {
				idx_t var_idx = indexer.InstanceColumn(var, inst);
				model.col_lower[var_idx] = 0.0;
				model.col_upper[var_idx] = 0.0;
			}
		}
	}

	// Append global auxiliary variables after row+keyed blocks.
	//
	// Stage 05 derives every auxiliary's box from the expression it stands for, so an
	// infinite bound here means one of two things: the expression genuinely reaches an
	// unbounded decision variable (flagged, and legitimate), or a creation site skipped
	// the derivation. The second is a silent performance cliff rather than a wrong
	// answer — a free continuous column leaves the root simplex without a box and it
	// crawls — so it is asserted (after the sanity checks) rather than left to a benchmark to notice.
	for (idx_t g = 0; g < input.num_global_vars; g++) {
		idx_t var_idx = indexer.global_block_start + g;
		auto gtype = input.global_variable_types[g];
		model.col_lower[var_idx] = input.global_lower_bounds[g];
		model.col_upper[var_idx] = input.global_upper_bounds[g];
		model.is_integer[var_idx] = gtype != LogicalType::DOUBLE;
		model.is_binary[var_idx] = (gtype == LogicalType::BOOLEAN);
	}
}

//! Build, step 2: the objective, linear and quadratic.
static void BuildModelObjective(SolverModel &model, SolverInput &input, const VarIndexer &indexer,
                                DecideProfileScope &phase) {
	idx_t num_rows = input.num_rows;
	idx_t total_vars = indexer.total_vars;

	//===--------------------------------------------------------------------===//
	// 2. Set up objective function
	//===--------------------------------------------------------------------===//

	phase.Next("model.linear_objective");
	model.obj_coeffs.resize(total_vars, 0.0);

	// Set objective coefficients for global variables
	for (idx_t g = 0; g < input.num_global_vars; g++) {
		idx_t var_idx = indexer.global_block_start + g;
		model.obj_coeffs[var_idx] = input.global_obj_coeffs[g];
	}
	model.obj_constant = input.objective_constant;

	// Its linear part, a coefficient per instance (SolverInput::objective_coefficients), each instance a column.
	for (idx_t var = 0; var < input.objective_coefficients.size(); var++) {
		auto &instances = input.objective_coefficients[var];
		for (idx_t inst = 0; inst < instances.size(); inst++) {
			if (instances[inst] != 0.0) {
				model.obj_coeffs[indexer.InstanceColumn(var, inst)] = instances[inst];
			}
		}
	}

	//===--------------------------------------------------------------------===//
	// 2b. Build quadratic objective (Q matrix) if present
	//===--------------------------------------------------------------------===//

	phase.Next("model.quadratic_objective");
	if (!input.objective_product_groups.empty()) {
		// Q accumulates in a map: (var_i, var_j) -> value, lower triangle only (var_i >= var_j).
		std::unordered_map<uint64_t, double> q_map;
		model.has_quadratic_obj = true;
		for (auto &group : input.objective_product_groups) {
			// Shared with stage 05's plan-time prediction, so the refusal there and the fact recorded here cannot
			// disagree.
			model.nonconvex_quadratic =
			    model.nonconvex_quadratic || IsNonconvexProductGroup(group.square, group.weight_sign);
			for (idx_t row = 0; row < num_rows; row++) {
				AddProductGroupRow(
				    group, row, row, indexer, q_map,
				    [&](int flat_idx, double value) {
					    AddCoefficient(model.obj_coeffs[flat_idx], value, input.objective_written);
				    },
				    model.obj_constant, input.objective_written);
			}
		}

		// Convert map to COO vectors
		model.q_rows.reserve(q_map.size());
		model.q_cols.reserve(q_map.size());
		model.q_vals.reserve(q_map.size());
		for (auto &entry : q_map) {
			if (entry.second == 0.0)
				continue;
			model.q_rows.push_back(static_cast<int>(entry.first >> 32));
			model.q_cols.push_back(static_cast<int>(entry.first & 0xFFFFFFFF));
			model.q_vals.push_back(entry.second);
		}
	}
}

// Handle constraints whose LHS reduced to 0 (all coefficients cancelled or
// all variables folded into column bounds). A tautology is dropped; a
// violated constant constraint (`0 <= -1`) is KEPT as a coefficient-free row,
// so the whole model still gets built and retained.
// Keeping the row is what lets the infeasible-diagnosis engine name and relax
// the offending clause — it stays a USER_PARAMETER row with its own source
// provenance, and the elastic weighting already guards an all-zero row. No
// backend ever sees it: SolverModel::ProvesInfeasible answers the model first.
static void PushNormalizedConstraint(SolverModel &model, ModelConstraint &&constr) {
	// A coefficient-free row that fails is kept, with its provenance, so ProvesInfeasible
	// answers the model and diagnosis can relax the row; one that holds is a tautology.
	constr.constant_fails = constr.indices.empty() && ConstantRowFails(constr.sense, constr.rhs);
	if (!constr.indices.empty() || constr.constant_fails) {
		model.constraints.push_back(std::move(constr));
	}
}

//! Build, step 3: the raw rows.
static void BuildModelRawRows(SolverModel &model, SolverInput &input, DecideProfileScope &phase) {
	// Raw rows: the helpers' definitions, ABS and bilinear links, `<>` disjunctions. They stand before the clauses that
	// read what they define (a helper's rows before the clause that reads it, an inner helper's before an outer's), the
	// order the solver reads them in: the same model with a clause's row ahead of its helper's envelope rows solves
	// two orders of magnitude slower.
	phase.Next("model.global_rows");
	for (auto &row : input.global_constraints) {
		KeepProvenance(input, row.provenance);
		PushNormalizedConstraint(model, std::move(row));
	}
}

//! Build, step 4: the clause rows: the linear rows, the quadratic rows, then the conditional and native rows the
//! backend states itself.
static void BuildModelClauseRows(SolverModel &model, SolverInput &input, const VarIndexer &indexer,
                                 DecideProfileScope &phase) {
	idx_t num_rows = input.num_rows;

	// A per-row bound on one decision (`x <= cap`, `2 * x >= lo`, `0.1 * x <= c`) is a fact about one column, so it
	// goes into that column's box rather than becoming a model row, as a literal bound already does
	// (decide_singleton_row_folding.cpp). A literal is the same for every row and folds once in the optimizer; a
	// data-valued bound differs per row and is known only here, as the values it admits on each row (stage 08 reads
	// them from the classifier both paths share). The bound narrows the box it finds, as the row did: it never
	// replaces the default floor of 0 itself (D41). A constant floor under a `when` has set the floor of
	// the instances its rows keep before this runs (ResolveLiftedFloors), and its row then sets the exact value. An
	// infinite end on the side it binds (`x >= c` with c = +inf) admits no value and keeps the row, and a box the
	// folds invert stays inverted: ProvesInfeasible answers both, as it would the row.
	//
	// HiGHS's QP solver does not turn such rows back into bounds: a 9,600-row box-bounded QP ran past 60 s as rows
	// and solves in 0.4 s as bounds. Under diagnosis the rows stay, because the elastic engine relaxes rows and
	// quotes the clause from their provenance (`x <= cap_col + delta`).
	//
	// A coefficient of 0 on a row (`cap * x <= -1e-10` where `cap` is 0) leaves the comparison of a constant, which
	// holds for every value or for none. DuckDB's comparison says which (the admitted ends are all or nothing), not
	// the builder's tolerance, under diagnosis too; a row that fails is kept for ProvesInfeasible.
	auto TryFoldPerRowBound = [&](const EvaluatedConstraint &ec, idx_t e, ModelConstraint &constr) -> bool {
		if (ec.records.admitted_lower.empty()) {
			return false;
		}
		double lower = ec.records.admitted_lower[e];
		double upper = ec.records.admitted_upper[e];
		if (constr.indices.empty()) {
			constr.constant_fails = lower > upper;
			if (constr.constant_fails) {
				model.constraints.push_back(std::move(constr));
			}
			return true;
		}
		if (input.diagnose) {
			return false;
		}
		D_ASSERT(constr.indices.size() == 1); // a bound on one decision: one term, merged by the canonicalizer
		if (lower == std::numeric_limits<double>::infinity() || upper == -std::numeric_limits<double>::infinity()) {
			return false;
		}
		// The column's box is finite, so an open end (±infinity) leaves it as it is.
		idx_t col = (idx_t)constr.indices[0];
		model.col_lower[col] = std::max(model.col_lower[col], lower);
		model.col_upper[col] = std::min(model.col_upper[col], upper);
		return true;
	};

	phase.Next("model.clause_rows");
	for (auto &eval_const : input.constraints) {
		const auto written = DecideWrittenClause(input, eval_const.provenance.origin.source_clause_id);
		auto &records = eval_const.records;

		// The rows the clause emits, read once (ClauseEmittedRows). The builder is their last reader, so it takes them
		// from the clause, and they are freed with this iteration.
		ClauseEmittedRows(eval_const, indexer, written);
		auto emitted = std::move(eval_const.emitted_rows);
		for (idx_t e = 0; e < records.Count(); e++) {
			ModelConstraint constr;
			constr.provenance = ClauseProvenance(input, eval_const);
			auto begin = NumericCast<int64_t>(emitted.offsets[e]);
			auto end = NumericCast<int64_t>(emitted.offsets[e + 1]);
			constr.indices.assign(emitted.columns.begin() + begin, emitted.columns.begin() + end);
			constr.coefficients.assign(emitted.values.begin() + begin, emitted.values.begin() + end);
			// What the left side holds beside its decisions moves to the bound, not dropped: its terms that read no
			// decision (the `+ 3` of `x + 3 <= K`, the `c` of `SUM(x + c) >= K`) and the constants of the helpers
			// written into the row.
			double lhs_offset = records.constants[e];
			ApplyComparison(input, constr, eval_const.comparison_type, records.bounds[e] - lhs_offset, lhs_offset);
			// F2: the group is stamped so a diagnosis over a grouped clause names the group its failure sits in
			// (CollectUnreachableClauses reports one finding per clause, group and row). A per-row row stands for its
			// data row: a scoped easy MIN/MAX's member row for its `by` group's classes, a `per K:` clause's row for
			// its class, any other for the row itself.
			StampConstraintProvenance(input, constr.provenance, eval_const, records.groups[e], lhs_offset,
			                          eval_const.lhs_is_aggregate ? DConstants::INVALID_INDEX : records.data_rows[e]);
			if (!eval_const.lhs_is_aggregate && TryFoldPerRowBound(eval_const, e, constr)) {
				continue;
			}
			PushNormalizedConstraint(model, std::move(constr));
		}
	}

	// Build quadratic constraints from product groups. A single QuadraticConstraint holds linear terms and every
	// group's Q entries, all accumulated together.

	// Helper: build QuadraticConstraint from one set of rows (aggregate or per-row).
	// lhs_constant: out, the constant part of the LHS folded into the bound (the linear
	// path's `lhs_offset`), for the stamp call. written: the clause, for a sum past a DOUBLE.
	auto BuildQuadraticConstraint = [&](const QuadraticClause &clause, idx_t e, double &lhs_constant,
	                                    const DecideClauseRef &written) -> SolverModel::QuadraticConstraint {
		const auto &ec = clause.row;
		SolverModel::QuadraticConstraint qc;
		qc.provenance = ClauseProvenance(input, ec);
		std::unordered_map<int, double> linear_accum;
		std::unordered_map<uint64_t, double> q_accum;
		auto &records = ec.records;
		double rhs_adjustment = records.constants[e];

		// Linear terms from the constraint LHS
		for (idx_t t = records.term_begin[e]; t < records.term_begin[e + 1]; t++) {
			int var_idx = static_cast<int>(indexer.InstanceColumn(records.variables[t], records.instances[t]));
			AddCoefficient(linear_accum[var_idx], records.coefficients[t], written);
		}

		// Product groups: each adds its rank-one update to Q, its cross terms to the linear part,
		// and its constant to the RHS (AddProductGroupRow). GRBaddqconstr reads x^T Q x with no
		// 1/2 factor, which is the monomial convention Q is held in.
		for (auto &group : clause.products) {
			for (idx_t k = clause.row_begin[e]; k < clause.row_begin[e + 1]; k++) {
				AddProductGroupRow(
				    group, clause.rows[k], clause.data_rows[clause.rows[k]], indexer, q_accum,
				    [&](int flat_idx, double value) { AddCoefficient(linear_accum[flat_idx], value, written); },
				    rhs_adjustment, written);
			}
		}

		// Flush linear terms
		for (auto &pair : linear_accum) {
			if (pair.second != 0.0) {
				qc.linear_indices.push_back(pair.first);
				qc.linear_coefficients.push_back(pair.second);
			}
		}
		// Flush Q terms
		for (auto &entry : q_accum) {
			if (entry.second != 0.0) {
				qc.q_rows.push_back(static_cast<int>(entry.first >> 32));
				qc.q_cols.push_back(static_cast<int>(entry.first & 0xFFFFFFFF));
				qc.q_coefficients.push_back(entry.second);
			}
		}

		// RHS and sense (move constant terms to RHS). A strict `< K` / `> K` steps the bound as the linear path does.
		lhs_constant = rhs_adjustment;
		ApplyComparison(input, qc, ec.comparison_type, records.bounds[e] - rhs_adjustment, rhs_adjustment);
		return qc;
	};

	phase.Next("model.quadratic_constraints");
	for (auto &quadratic : input.quadratics) {
		auto &eval_const = quadratic.row;
		const auto written = DecideWrittenClause(input, eval_const.provenance.origin.source_clause_id);
		// One QuadraticConstraint per row the clause emits, stamped as a linear row is.
		for (idx_t e = 0; e < eval_const.records.Count(); e++) {
			double lhs_constant;
			auto qc = BuildQuadraticConstraint(quadratic, e, lhs_constant, written);
			StampConstraintProvenance(input, qc.provenance, eval_const, eval_const.records.groups[e], lhs_constant,
			                          eval_const.lhs_is_aggregate ? DConstants::INVALID_INDEX
			                                                      : eval_const.records.data_rows[e]);
			model.quadratic_constraints.push_back(std::move(qc));
		}
	}

	// The conditional rows and the constructs the chosen backend states itself. Whatever `LowerDecideConstructs` did
	// not lower arrives here still conditioned; everything it did lower is an ordinary row in `global_constraints`
	// above. A model holds one or the other for a given clause, never both.
	phase.Next("model.general_constraints");
	model.indicator_constraints = std::move(input.indicator_constraints);
	for (auto &row : model.indicator_constraints) {
		KeepProvenance(input, row.provenance);
	}
	model.general_constraints = std::move(input.general_constraints);
	for (auto &construct : model.general_constraints) {
		KeepProvenance(input, construct.provenance);
	}
}

SolverModel SolverModel::Build(SolverInput &input, const VarIndexer &indexer) {
	DecideProfileScope profile("model.build");
	DecideProfileScope phase("model.variables_and_bounds");
	SolverModel model;
	model.class_sets = std::move(input.class_sets);
	model.key_values = std::move(input.key_values);

	idx_t num_rows = input.num_rows;

	// VarIndexer is built once in PhysicalDecide::Finalize() and threaded in.
	idx_t total_vars = indexer.total_vars;

	model.num_vars = total_vars;

	BuildModelColumns(model, input, indexer);
	BuildModelObjective(model, input, indexer, phase);

	//===--------------------------------------------------------------------===//
	// 3. Build constraints
	//===--------------------------------------------------------------------===//

	phase.Next("model.constraints");

	// The rows the clauses emit, and the raw rows appended after them, reserved once.
	{
		idx_t est_rows = input.global_constraints.size();
		for (auto &ec : input.constraints) {
			est_rows += ec.records.Count();
		}
		model.constraints.reserve(model.constraints.size() + est_rows);
	}

	BuildModelRawRows(model, input, phase);
	BuildModelClauseRows(model, input, indexer, phase);

	// A box inverted by rounding alone is one point. Boxes are derived in floating point from several sources -- a
	// bound divided by its coefficient (`3 * x <= 0.3` is `x <= 0.0999...` beside `x >= 0.1`), a helper's reach, a
	// per-row bound folded in above -- and a floor above its top by no more than DECIDE_ROUNDED_BOX_RELATIVE_TOLERANCE
	// is, in exact arithmetic, equal to it. Left inverted it reads as an infeasible model; a wider inversion still is
	// one. Applied to every column once its box is final, whatever formed or narrowed it.
	for (idx_t col = 0; col < total_vars; col++) {
		double &lower = model.col_lower[col];
		const double upper = model.col_upper[col];
		if (lower > upper &&
		    lower - upper <= DECIDE_ROUNDED_BOX_RELATIVE_TOLERANCE * MaxValue<double>(1.0, std::abs(upper))) {
			lower = upper;
		}
	}

	//===--------------------------------------------------------------------===//
	// 4. Sanity checks
	//===--------------------------------------------------------------------===//

	phase.Next("model.sanity_checks");

	// Every number below was read finite or merged by AddCoefficient, so one that is not finite is a reach or a
	// Big-M DeciDB derived past a DOUBLE: the user's error names the clause it belongs to. A column belongs to the
	// first row that reads it, else to the objective.
	auto too_large = [&](idx_t source_clause_id) {
		return DecideTooLargeError(DecideWrittenClause(input, source_clause_id));
	};
	auto column_clause = [&](idx_t col) {
		for (auto &constr : model.constraints) {
			if (std::find(constr.indices.begin(), constr.indices.end(), (int)col) != constr.indices.end()) {
				return constr.provenance.origin.source_clause_id;
			}
		}
		return DConstants::INVALID_INDEX;
	};
	for (idx_t i = 0; i < total_vars; i++) {
		if (!std::isfinite(model.col_lower[i]) || !std::isfinite(model.col_upper[i])) {
			throw too_large(column_clause(i));
		}
		if (!std::isfinite(model.obj_coeffs[i])) {
			throw too_large(DConstants::INVALID_INDEX);
		}
	}

	for (auto &constr : model.constraints) {
		for (auto coefficient : constr.coefficients) {
			if (!std::isfinite(coefficient)) {
				throw too_large(constr.provenance.origin.source_clause_id);
			}
		}
	}

	// An auxiliary's box is open and says so, or it is derived. A reach that overflowed a DOUBLE is neither: it is the
	// user's error raised above, which this check must not pre-empt.
	for (idx_t g = 0; g < input.num_global_vars; g++) {
		D_ASSERT(g >= input.global_bounds_unbounded.size() || input.global_bounds_unbounded[g] ||
		         (input.global_lower_bounds[g] > -DECIDE_EFFECTIVE_INFINITY &&
		          input.global_upper_bounds[g] < DECIDE_EFFECTIVE_INFINITY));
	}

	// Moved last: every row above names its clause from them (DecideWrittenClause).
	model.constraint_sources = std::move(input.constraint_sources);
	model.source_text = std::move(input.source_text);
	return model;
}

vector<ColumnProvenance> BuildColumnProvenance(const VarIndexer &indexer, const vector<string> &var_labels,
                                               const vector<bool> &var_is_aux,
                                               const vector<string> &global_var_labels) {
	// Every flat column defaults to an unnamed global-block auxiliary; the row /
	// keyed blocks are then overwritten below. Columns in [global_block_start,
	// total_vars) (internal global auxiliaries) keep the default unless named via
	// global_var_labels (an aggregate `<>` indicator — see below).
	vector<ColumnProvenance> provenance(indexer.total_vars);

	idx_t num_decide_vars = var_labels.size();
	for (idx_t var = 0; var < num_decide_vars; var++) {
		ColumnKind kind = var_is_aux[var] ? ColumnKind::AUX : ColumnKind::USER;
		// Each instance with a column: a row for a row-scoped variable, a class for a keyed one, the one instance of a
		// query-wide one.
		for (idx_t inst = 0; inst < indexer.NumInstances(var); inst++) {
			if (!indexer.HasColumn(var, inst)) {
				continue;
			}
			ColumnProvenance &slot = provenance[indexer.InstanceColumn(var, inst)];
			slot.kind = kind;
			slot.label = var_labels[var];
			slot.decide_var_idx = var;
			slot.instance = inst;
		}
	}
	// Name any labeled global-block columns (today: aggregate `<>` indicators).
	// global_var_labels is parallel to the global block at [global_block_start, ...);
	// empty entries leave the default unnamed GLOBAL_AUX. Kind stays GLOBAL_AUX —
	// only the label is consumed (the removal dial reads columns[indicator_col].label).
	for (idx_t g = 0; g < global_var_labels.size(); g++) {
		if (global_var_labels[g].empty()) {
			continue;
		}
		provenance[indexer.global_block_start + g].label = global_var_labels[g];
	}
	return provenance;
}

//===----------------------------------------------------------------------===//
// SparseCoeffAccumulator
//===----------------------------------------------------------------------===//

void SparseCoeffAccumulator::BeginDense(idx_t total_vars, const DecideClauseRef &written_p) {
	written = &written_p;
	use_dense = true;
	if (dense.size() < total_vars) {
		dense.assign(total_vars, 0.0);
	}
	// Cells previously written were already zeroed in Flush(); no full re-zero needed.
	touched.clear();
	pairs.clear();
}

void SparseCoeffAccumulator::BeginSparse(idx_t expected_size, const DecideClauseRef &written_p) {
	written = &written_p;
	use_dense = false;
	pairs.clear();
	if (expected_size > pairs.capacity()) {
		pairs.reserve(expected_size);
	}
	touched.clear();
}

void SparseCoeffAccumulator::BeginForDecisions(const VarIndexer &indexer, idx_t expected_size,
                                               const DecideClauseRef &written_p) {
	constexpr idx_t DENSE_CAP = 1u << 20;
	if (indexer.global_block_start <= DENSE_CAP) {
		BeginDense(indexer.global_block_start, written_p);
	} else {
		BeginSparse(expected_size, written_p);
	}
}

void SparseCoeffAccumulator::Flush(vector<int> &out_indices, vector<double> &out_coefficients) {
	if (use_dense) {
		for (int idx : touched) {
			double v = dense[idx];
			if (v != 0.0) {
				out_indices.push_back(idx);
				out_coefficients.push_back(v);
			}
			dense[idx] = 0.0;
		}
		touched.clear();
	} else {
		if (pairs.empty()) {
			return;
		}
		std::sort(pairs.begin(), pairs.end(),
		          [](const std::pair<int, double> &a, const std::pair<int, double> &b) { return a.first < b.first; });
		// Merge consecutive equal indices, drop zeros.
		idx_t n = pairs.size();
		idx_t i = 0;
		while (i < n) {
			int idx = pairs[i].first;
			double sum = pairs[i].second;
			idx_t j = i + 1;
			while (j < n && pairs[j].first == idx) {
				AddCoefficient(sum, pairs[j].second, *written);
				j++;
			}
			if (sum != 0.0) {
				out_indices.push_back(idx);
				out_coefficients.push_back(sum);
			}
			i = j;
		}
		pairs.clear();
	}
}

} // namespace duckdb
