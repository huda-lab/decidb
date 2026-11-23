//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/linearization_bilinear_abs.cpp
//
// Bilinear products and ABS lowering. See ilp_linearization.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

//===--------------------------------------------------------------------===//
// Bilinear products and ABS
//===--------------------------------------------------------------------===//

void DeriveBilinearAuxiliaryBounds(SolverInput &input, const FormulationBox &box, const vector<string> &var_names,
                                   bool refuse_when_unbounded) {
	for (const auto &link : input.bilinear_links) {
		double U = box.upper[link.other_var_idx];
		double L = box.lower[link.other_var_idx];
		if (DecideIsOpenAbove(U)) {
			if (!refuse_when_unbounded) {
				continue;
			}
			// A helper decision of the value rewrite holds a reducer's value, which the user cannot bound: name the
			// member of the reducer that has no bound.
			idx_t blame = link.other_var_idx;
			for (const auto &helper : input.reducer_value_links) {
				if (helper.value_var == link.other_var_idx) {
					idx_t member = FirstUnboundedMemberVariable(helper.members, box.lower, box.upper);
					blame = member == DConstants::INVALID_INDEX ? blame : member;
				}
			}
			// An ABS helper is named by the ABS the user wrote.
			string name = var_names[blame];
			for (const auto &abs : input.abs_links) {
				if (abs.aux_idx == blame) {
					name = abs.written;
				}
			}
			throw InvalidInputException(
			    StringUtil::Format("'%s' has no finite upper bound, which its product with a BOOL decision needs",
			                       name),
			    Exception::InitializeExtraInfo(DecideWrittenClause(input, link.origin.source_clause_id).place));
		}
		input.lower_bounds[link.aux_idx] = std::min(0.0, L);
		input.upper_bounds[link.aux_idx] = std::max(0.0, U);
	}
}

void FloorBilinearAuxiliaries(SolverInput &input) {
	for (const auto &link : input.bilinear_links) {
		auto &floor = input.lower_bounds[link.aux_idx];
		floor = std::max(floor, std::min(0.0, input.lower_bounds[link.other_var_idx]));
	}
}

void LinearizeBilinear(SolverInput &input, const VarIndexer &indexer, const FormulationBox &box) {
	// For (w = b * x) with b Boolean and x in [L, U] the exact linearization is:
	//   w <= U*b                  (1)
	//   w >= x - U*(1-b)          (2)
	//   w <= x - L*(1-b)          (3, upper corner)
	//   w >= L*b                  (4, lower corner)
	// w's column is floored at the product's range, min(0, L) over the instances, raised by a floor written on the
	// product (it has no default floor of 0: AddBooleanProduct). Where that floor is 0 or more, it implies the lower
	// corner, and (3) simplifies to the plain structural `w <= x` (w=0 at b=0 is enforced by (1)): two-plus-one
	// rows. Where it is negative, all four corners are emitted, an instance whose own L >= 0 included, so the product
	// can take the negative value of x when b=1 and is 0 when b=0.
	//
	// One block per INSTANCE of w, in flat columns, from the box of the instance of x it reads (the way a helper
	// decision of the value rewrite is defined): a query-wide product is one block, a keyed one a block per key,
	// a row-scoped one a block per row. A product of two Booleans is the AND, the same block of three rows.
	struct ProductInstance {
		int w, b, x;
		double U, L;
	};
	for (auto &link : input.bilinear_links) {
		const auto &mapping_scope = indexer.var_scope[link.aux_idx];
		const idx_t absent = mapping_scope == DecideVarScope::KEYED
		                         ? indexer.keyed_mappings[indexer.var_keyed_mapping_idx[link.aux_idx]].absent_class
		                         : DConstants::INVALID_INDEX;
		auto emit = [&](vector<int> indices, vector<double> coefficients, char sense, double rhs) {
			ModelConstraint row;
			row.indices = std::move(indices);
			row.coefficients = std::move(coefficients);
			row.sense = sense;
			row.rhs = rhs;
			row.provenance.kind = ConstraintKind::STRUCTURAL;
			row.provenance.origin = link.origin;
			input.global_constraints.push_back(std::move(row));
		};
		vector<ProductInstance> products;
		for (idx_t instance = 0; instance < indexer.NumInstances(link.aux_idx); instance++) {
			if (instance == absent) {
				continue; // the instance of a row that reads no key is fixed at 0
			}
			const idx_t bool_instance =
			    ProductFactorInstance(indexer, link.bool_var_idx, link.bool_instances, instance);
			const idx_t other_instance =
			    ProductFactorInstance(indexer, link.other_var_idx, link.other_instances, instance);
			// A factor with no value there leaves the product none (D88): no corner reads it.
			if (!indexer.HasColumn(link.aux_idx, instance) || !indexer.HasColumn(link.bool_var_idx, bool_instance) ||
			    !indexer.HasColumn(link.other_var_idx, other_instance)) {
				continue;
			}
			const int w = static_cast<int>(indexer.InstanceColumn(link.aux_idx, instance));
			const int b = static_cast<int>(indexer.InstanceColumn(link.bool_var_idx, bool_instance));
			const int x = static_cast<int>(indexer.InstanceColumn(link.other_var_idx, other_instance));
			if (input.variable_types[link.other_var_idx] == LogicalType::BOOLEAN) {
				// Two Booleans: the AND, w <= b, w <= x, w >= b + x - 1.
				emit({w, b}, {1.0, -1.0}, '<', 0.0);
				emit({w, x}, {1.0, -1.0}, '<', 0.0);
				emit({b, x, w}, {1.0, 1.0, -1.0}, '<', 1.0);
				continue;
			}
			const double U = box.InstanceUpper(link.other_var_idx, other_instance);
			const double L = box.InstanceLower(input, link.other_var_idx, other_instance);
			D_ASSERT(U < DECIDE_EFFECTIVE_INFINITY); // derived and validated before ABS descendants read the aux box
			products.push_back({w, b, x, U, L});
		}
		// A write to the box, not a read of it: this DECLARES the auxiliary's column, so it belongs on `input`
		// rather than on the derived-constant box above. The two coincide today because `box` is the solved model's
		// own box; they stop coinciding as soon as a second formulation runs against a widened box, and the floor
		// must still land on the column the model declares.
		double range_floor = 0.0;
		for (const auto &product : products) {
			range_floor = std::min(range_floor, product.L);
		}
		auto &floor = input.lower_bounds[link.aux_idx];
		floor = std::max(floor, range_floor);
		for (const auto &product : products) {
			const int w = product.w, b = product.b, x = product.x;
			const double U = product.U, L = product.L;
			emit({w, b}, {1.0, -U}, '<', 0.0);         // (1) w - U*b <= 0
			emit({w, x, b}, {1.0, -1.0, -U}, '>', -U); // (2) w - x - U*b >= -U
			if (floor < 0.0) {
				emit({w, x, b}, {1.0, -1.0, -L}, '<', -L); // (3) w - x - L*b <= -L
				emit({w, b}, {1.0, -L}, '>', 0.0);         // (4) w >= L*b
			} else {
				emit({w, x}, {1.0, -1.0}, '<', 0.0); // (3) w - x <= 0
			}
		}
	}
}

namespace {

//! The largest value some clause requires `aux` to reach, or 0 when nothing does.
//!
//! Every Big-M and every derived column ceiling in this file is sized from the decision
//! box as the query states it, and that is right for the solve. It is wrong for the
//! diagnosis that follows an infeasible one: the elastic engine repairs an infeasible
//! query by WIDENING a bound, and a ceiling baked in at the old width makes the widened
//! repair unrepresentable — so the engine reports a different, worse edit. It also
//! reports a DIFFERENT edit per backend, because a natively-stated construct bakes in
//! nothing (`x >= -1 AND x <= 1 AND ABS(x) >= 5` was advised to widen the box on Gurobi
//! and to weaken the ABS on HiGHS, one repair worth five times the other).
//!
//! So a clause that asks the auxiliary for more than the box can supply sizes the
//! ceiling itself. It fires when the demand EXCEEDS the box. The demand covers the worst
//! value the rest of the row's box allows, so on a row with other variable terms
//! (`ABS(x - 3) <= ABS(x - 9)`) it can widen a query that solves: the worst case is a
//! point another row may exclude, and it is kept so that a repair stays representable. A
//! looser M is valid in any case — it only ever slackens the deactivated
//! arm of a disjunction — whereas a too-small one cuts off legal answers.
static double DemandedAuxReach(const SolverInput &input, idx_t aux_idx) {
	double demanded = 0.0;
	// The common shape, and the one B3 was reported against: `ABS(x) >= 5` is a simple
	// `var OP const` comparison over the auxiliary, so stage 05 folds it into the
	// auxiliary's own box rather than leaving a row (`FoldSingletonRows`). The floor
	// it puts there IS the demand, and without this the ceiling below is set under it —
	// an empty column, which no widening of `x` can repair.
	demanded = MaxValue<double>(demanded, input.lower_bounds[aux_idx]);
	input.ForEachClauseRow([&](const EvaluatedConstraint &ec) {
		if (ec.defined_var == aux_idx) {
			return; // the definitional pair: it states what the auxiliary IS, not what was asked of it
		}
		if (std::find(ec.variable_indices.begin(), ec.variable_indices.end(), aux_idx) == ec.variable_indices.end()) {
			return;
		}
		bool pushes_up = ec.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
		                 ec.comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
		                 ec.comparison_type == ExpressionType::COMPARE_EQUAL;
		bool pushes_down = ec.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
		                   ec.comparison_type == ExpressionType::COMPARE_LESSTHAN ||
		                   ec.comparison_type == ExpressionType::COMPARE_EQUAL;
		auto &records = ec.records;
		for (idx_t e = 0; e < records.Count(); e++) {
			double k = records.EffectiveBound(e);
			if (!std::isfinite(k)) {
				continue;
			}
			// The row reads `a*aux + S <op> k` with `S` the rest of its variable terms.
			// The auxiliary has to cover the worst `S` the box allows, and only an
			// UPWARD demand matters here: this raises a ceiling, and an ABS auxiliary
			// is non-negative and floored by its own definitional pair.
			auto range = DecideRowSignedRange(ec, e, input.lower_bounds, input.upper_bounds, aux_idx);
			double lo = range.Lo();
			double hi = range.Hi();
			for (idx_t t = records.term_begin[e]; t < records.term_begin[e + 1]; t++) {
				double a = records.coefficients[t];
				if (records.variables[t] != aux_idx || std::abs(a) < 1e-15) {
					continue;
				}
				if (a > 0.0 && pushes_up && std::isfinite(lo)) {
					demanded = MaxValue<double>(demanded, (k - lo) / a);
				} else if (a < 0.0 && pushes_down && std::isfinite(hi)) {
					demanded = MaxValue<double>(demanded, (k - hi) / a);
				}
			}
		}
	});
	return demanded;
}

//! The expression inside the ABS on row `e` the operand emits (a row the `when` of its clause and its group keep), as
//! the operand row `aux + sum_{t != aux} c_t x_t >= k` states it:
//!   inner_r = k - sum_{t != aux} c_t[r] * x_t
//! `columns` and `coefficients` are the sum's terms, each column once (two terms may land on one column -- a frame
//! selecting the row itself (`abs(x - at(last: x) over (...))` at the last period) -- and a row states a column once,
//! or the solver refuses it), and `k` is the bound with the operand row's constant terms moved across (a frame's `else`
//! read as data is a term of no decision). Both arms read the argument here and only state it differently.
double ReadAbsArgument(const AbsLinkSpec &link, const VarIndexer &indexer, idx_t e, const DecideClauseRef &written,
                       SparseCoeffAccumulator &argument, vector<int> &columns, vector<double> &coefficients) {
	const auto &records = link.operand.records;
	argument.BeginSparse(records.term_begin[e + 1] - records.term_begin[e], written);
	for (idx_t t = records.term_begin[e]; t < records.term_begin[e + 1]; t++) {
		if (records.variables[t] != link.aux_idx) {
			argument.Add(static_cast<int>(indexer.InstanceColumn(records.variables[t], records.instances[t])),
			             records.coefficients[t]);
		}
	}
	argument.Flush(columns, coefficients);
	return records.EffectiveBound(e);
}

//! The row's provenance, which every row of an ABS carries as the clause it was written in does.
void StampAbsRow(ModelConstraint &row, const EvaluatedConstraint &operand, idx_t e) {
	row.provenance.kind = ConstraintKind::STRUCTURAL;
	row.provenance.origin = operand.provenance.origin;
	row.StampGroup(operand, operand.records.groups[e]);
}

//! The lowered form: a sign indicator `y` per instance of the auxiliary and two Big-M rows that pin `aux = |inner|`
//! from above, which the lower envelope (EmitAbsEnvelopes) pins from below:
//!   aux - inner <= A1 * (1 - y)     (y = 1 pins aux = inner)
//!   aux + inner <= A2 * y           (y = 0 pins aux = -inner)
//! The two sign rows relax by what the deactivated branch needs, each on its own: `y = 1` pins `aux = inner`, so the
//! row `aux + inner <= A2 * y` must admit `2 * inner` up to `2 * hi`; `y = 0` pins `aux = -inner`, so `aux - inner <=
//! A1 * (1 - y)` must admit `-2 * inner` up to `-2 * lo`. A side the interval never reaches needs nothing. The interval
//! was derived, and the auxiliary's box narrowed to it, by DeriveAbsAuxiliaryBounds, which runs before every
//! linearizer because they all read column boxes; one that is unbounded is stated natively or refused there.
void ExpandAbs(SolverInput &input, const VarIndexer &indexer, const AbsLinkSpec &link) {
	D_ASSERT(!link.range_unbounded);
	const auto &operand = link.operand;
	const auto written = DecideWrittenClause(input, operand.provenance.origin.source_clause_id);
	const double relax_neg = 2.0 * std::max(0.0, -link.inner_lo); // A1
	const double relax_pos = 2.0 * std::max(0.0, link.inner_hi);  // A2
	SparseCoeffAccumulator argument;
	for (idx_t e = 0; e < operand.records.Count(); e++) {
		vector<int> columns;
		vector<double> coefficients;
		const double k = ReadAbsArgument(link, indexer, e, written, argument, columns, coefficients);
		const int aux =
		    static_cast<int>(indexer.InstanceColumn(link.aux_idx, operand.records.InstanceOf(e, link.aux_idx)));
		const int sign = static_cast<int>(AddGlobalBinaryAux(input, indexer, 0.0, link.label));

		// `aux + sum c x + A1 * y <= k + A1`, which is `aux - inner <= A1 * (1 - y)`.
		ModelConstraint pin_inner;
		pin_inner.indices.push_back(aux);
		pin_inner.coefficients.push_back(1.0);
		pin_inner.indices.insert(pin_inner.indices.end(), columns.begin(), columns.end());
		pin_inner.coefficients.insert(pin_inner.coefficients.end(), coefficients.begin(), coefficients.end());
		pin_inner.indices.push_back(sign);
		pin_inner.coefficients.push_back(relax_neg);
		pin_inner.sense = '<';
		pin_inner.rhs = k + relax_neg;
		StampAbsRow(pin_inner, operand, e);
		input.global_constraints.push_back(std::move(pin_inner));

		// `aux - sum c x - A2 * y <= -k`, which is `aux + inner <= A2 * y`.
		ModelConstraint pin_negated;
		pin_negated.indices.push_back(aux);
		pin_negated.coefficients.push_back(1.0);
		for (idx_t t = 0; t < columns.size(); t++) {
			pin_negated.indices.push_back(columns[t]);
			pin_negated.coefficients.push_back(-coefficients[t]);
		}
		pin_negated.indices.push_back(sign);
		pin_negated.coefficients.push_back(-relax_pos);
		pin_negated.sense = '<';
		pin_negated.rhs = -k;
		StampAbsRow(pin_negated, operand, e);
		input.global_constraints.push_back(std::move(pin_negated));
	}
}

//! The native form: one column `t` per active row, an equality row `t = inner`, and a `GeneralConstraint` saying
//! `aux = |t|`. The general constraint relates two VARIABLES, so the linear half has to be a row either way. That is
//! the whole shape of a native construct here; it replaces the two Big-M rows and the sign indicator, which is the
//! point, since those are what needed a finite M. The lower envelope stays: it is exact (`aux >= inner`,
//! `aux >= -inner`), implied by `aux = |t|`, and it carries the clause provenance the elastic engine reads.
void StateAbsNatively(SolverInput &input, const VarIndexer &indexer, const InstanceBox &instances,
                      const AbsLinkSpec &link) {
	const auto &operand = link.operand;
	const auto written = DecideWrittenClause(input, operand.provenance.origin.source_clause_id);
	SparseCoeffAccumulator argument;
	for (idx_t e = 0; e < operand.records.Count(); e++) {
		// `t = inner`, boxed by THIS ROW's own reach -- the same per-row walk native MIN/MAX boxes its argument
		// columns with, rather than one table-wide magnitude reused for every row. The wider box is not free: a slack
		// continuous column costs the root LP relaxation, and the ABS interval is the cover over rows, so every row but
		// the extreme one was being given more room than it can reach.
		//
		// The bracket is read off the operand row exactly as the link row below states it, and negating the variable
		// walk swaps its two ends. Skipping the auxiliary is what makes this the range of the expression rather than
		// of the row.
		auto vars = DecideRowSignedRange(operand, e, input.lower_bounds, input.upper_bounds, link.aux_idx, &instances);
		vector<int> columns;
		vector<double> coefficients;
		const double k = ReadAbsArgument(link, indexer, e, written, argument, columns, coefficients);
		// Each end on its own. A row open on one side still contributes its closed side, and a row whose contributors
		// are all bounded gets a real box even when some other row's are not -- which the one interval could not
		// express, being one number for the whole link. Free only where nothing at all is derivable, which is
		// precisely the query this arm exists to answer and the only case that earns a free column.
		AuxRange inner_range;
		inner_range.CoverRowEnds(k - vars.hi, k - vars.lo, vars.hi_open, vars.lo_open, DConstants::INVALID_INDEX);
		// `ABS(x)` is the common shape, and there `t = x`: one column and one equality row per data row to restate a
		// column that already exists. The operand row spells the argument as `inner = k - sum c_t x_t`, so a renaming
		// is one non-auxiliary column with coefficient -1 and a zero bound. Anything else -- `ABS(x - target)`,
		// `ABS(2 * x)` -- is a genuine expression, and a general constraint relates columns, so it still earns one.
		bool renaming = columns.size() == 1 && coefficients[0] == -1.0 && k == 0.0;

		idx_t t_idx = DConstants::INVALID_INDEX;
		if (renaming) {
			t_idx = static_cast<idx_t>(columns[0]);
		} else {
			t_idx = AddGlobalContinuousAux(input, indexer, inner_range, 0.0);

			// `t - inner = 0` with `inner = k - sum c_t x_t` is `t + sum c_t x_t = k`.
			ModelConstraint link_row;
			link_row.indices.push_back(static_cast<int>(t_idx));
			link_row.coefficients.push_back(1.0);
			link_row.indices.insert(link_row.indices.end(), columns.begin(), columns.end());
			link_row.coefficients.insert(link_row.coefficients.end(), coefficients.begin(), coefficients.end());
			link_row.sense = '=';
			link_row.rhs = k;
			link_row.provenance.kind = ConstraintKind::STRUCTURAL;
			link_row.provenance.origin = operand.provenance.origin;
			input.global_constraints.push_back(std::move(link_row));
		}

		GeneralConstraint gc;
		gc.kind = GeneralConstraintKind::ABS;
		gc.result_column =
		    static_cast<int>(indexer.InstanceColumn(link.aux_idx, operand.records.InstanceOf(e, link.aux_idx)));
		gc.argument_columns.push_back(static_cast<int>(t_idx));
		gc.provenance.origin = operand.provenance.origin;
		input.general_constraints.push_back(std::move(gc));
	}
}

} // namespace

void EmitAbsEnvelopes(SolverInput &input) {
	input.constraints.reserve(input.constraints.size() + 2 * input.abs_links.size());
	for (auto &link : input.abs_links) {
		// Contract with stage 05: an envelope row holds no aggregate, because RewriteScopedReducers gives every reducer
		// a function reads a helper decision. Every pass below reads the rows per row.
		for (auto *row : {&link.operand, &link.negated}) {
			if (row->lhs_is_aggregate) {
				throw InternalException("DECIDE ABS envelope row for auxiliary %llu holds a raw aggregate: the value "
				                        "rewrite did not replace its reducers",
				                        link.aux_idx);
			}
			// The passes that skip the pair read it as the rows that define the auxiliary (DemandedAuxReach).
			if (row->defined_var != link.aux_idx) {
				throw InternalException("DECIDE ABS envelope row for auxiliary %llu does not define it", link.aux_idx);
			}
		}
		// `aux >= inner` stays on the link, which the rows of an exact ABS are read from.
		input.constraints.push_back(link.operand);
		input.constraints.push_back(std::move(link.negated));
	}
}

void DeriveAbsAuxiliaryBounds(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names,
                              bool refuse_when_unbounded) {
	if (input.abs_links.empty()) {
		return;
	}
	// A helper among the members is read at its own instance's reach, as the model builder boxes its column.
	const InstanceBox instances {input, indexer, /*reach=*/true};

	for (auto &link : input.abs_links) {
		const auto &c1 = link.operand;

		// The reach of the expression inside ABS, `inner = k_r - sum_{t != aux} c_t x_t`, over every
		// row: the signed interval [lo, hi], by the same walk `<>`, MIN/MAX and the native arm
		// use (`DecideRowSignedRange`). Derived afresh each time: a run that found a member open
		// may be followed by one that finds it boxed, once the value rewrite's helpers among the
		// members are boxed.
		link.range_unbounded = false;
		bool abs_unbounded = false;
		double lo = 0.0;
		double hi = 0.0;
		bool seen_row = false;
		const auto written = DecideWrittenClause(input, c1.provenance.origin.source_clause_id);
		for (idx_t e = 0; e < c1.records.Count(); e++) {
			// The link's bound carries the constant part of the expression the user wrote inside ABS, with C1's
			// constant terms moved across, where a frame's `else` read as data stands. It is finite: an infinity there
			// is a data problem, which defeats the native path too (|inf| is still inf), and RewriteAbs refuses it
			// where it reads the argument, whichever path follows; a constant term past a DOUBLE is refused as it is
			// moved (AddCoefficient).
			double link_rhs = c1.records.EffectiveBound(e);
			D_ASSERT(std::isfinite(link_rhs));
			auto vars = DecideRowSignedRange(c1, e, input.lower_bounds, input.upper_bounds, link.aux_idx, &instances);
			if (vars.lo_open || vars.hi_open) {
				abs_unbounded = true;
				continue;
			}
			// Negating the variable part swaps its two ends.
			double row_lo = link_rhs - vars.hi;
			double row_hi = link_rhs - vars.lo;
			lo = seen_row ? std::min(lo, row_lo) : row_lo;
			hi = seen_row ? std::max(hi, row_hi) : row_hi;
			seen_row = true;
		}

		if (abs_unbounded) {
			link.range_unbounded = true;
			if (!link.exact || !refuse_when_unbounded) {
				// An ABS that is not exact has no Big-M to size: nothing here needs its box, and a reader that does
				// (a product, a MIN or MAX, a `<>`) refuses the open column through its own check. An exact one on
				// the native path is about to hand Gurobi `aux = |t|`, which needs no Big-M and therefore no bound.
				// Leave the auxiliary's box open and carry on: this is exactly the query the capability buys back.
				continue;
			}
			// The row's own auxiliary is skipped: an offending member is named, and with none the reach overflowed a
			// DOUBLE.
			idx_t bad = FindUnboundedContributor(c1, input.lower_bounds, input.upper_bounds, link.aux_idx);
			ThrowUnboundedBigMNaming(bad != DConstants::INVALID_INDEX, bad, var_names, "ABS",
			                         DecideWrittenClause(input, c1.provenance.origin.source_clause_id));
		}

		// A clause may demand more of the auxiliary than the box can supply, and then
		// the box is not the reach the encoding has to cover: `ABS(x) >= 5` over
		// `x` in [-1, 1] needs `aux` to be ABLE to hold 5, or the only repair the
		// infeasible diagnosis can see is weakening the ABS itself. See
		// `DemandedAuxReach` — this can only widen, and only on a clause that cannot
		// be met as written. `aux = |inner|` holds a demand of D by `inner` at +D or -D,
		// so the interval grows to cover both.
		double reach = std::max(std::abs(lo), std::abs(hi));
		double demanded = DemandedAuxReach(input, link.aux_idx);
		if (demanded > reach) {
			lo = std::min(lo, -demanded);
			hi = std::max(hi, demanded);
			reach = demanded;
		}

		link.inner_lo = lo;
		link.inner_hi = hi;
		// The formulation that follows — Big-M rows or a native `aux = |t|` — pins
		// `aux = |inner|`, and `reach` is the largest |inner| any row can attain. So it is a
		// valid upper bound on the auxiliary's column, and the only one anything
		// derives: the pair `aux >= inner`, `aux >= -inner` bounds the auxiliary from
		// BELOW only, which is why it otherwise sits at +infinity. Narrow the box
		// here, before any other linearizer reads it, so an outer MIN/MAX or `<>` over
		// this auxiliary gets a tight Big-M instead of an unbounded column it must
		// refuse. Never widens: an implied bound already tighter than `reach` stands.
		//
		// This holds for an ABS that is not exact too, though it has no Big-M to size. Every reader of such an ABS
		// pushes it down, so its value is |inner| at any answer, which `reach` bounds; the box is a bound on that
		// value, not a constraint the reader needs to hold it.
		if (reach < input.upper_bounds[link.aux_idx]) {
			input.upper_bounds[link.aux_idx] = reach;
		}
	}
}

void EmitAbsConstraints(SolverInput &input, const VarIndexer &indexer, NativeConstructPolicy policy) {
	const InstanceBox instances {input, indexer, /*reach=*/true};
	for (auto &link : input.abs_links) {
		if (!link.exact) {
			continue;
		}
		// THE CHOICE, made once: the ABS is a pseudo-operation the backend either declares (legal: stated natively) or
		// not (expanded into its indicator and Big-M rows), as a legalization pass decides per operation. The same rule
		// as a MIN or MAX (NativeConstructPolicy). Native is the default, and ABS has no measurement that its lowering
		// is faster (Q10 on Gurobi: a tie), so a declared ABS is stated natively and the expansion serves a backend
		// that declares none.
		if (policy.Use(link.range_unbounded)) {
			StateAbsNatively(input, indexer, instances, link);
		} else {
			ExpandAbs(input, indexer, link);
		}
	}
}

} // namespace duckdb
