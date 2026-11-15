//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_absence.cpp
//
// The one "no value" rule. See decide_absence.hpp.
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_absence.hpp"

#include "duckdb/decidb/formulation/solver_input.hpp"
#include "duckdb/execution/operator/decide/decide_frame_map.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"

#include <algorithm>

namespace duckdb {

DecideAbsence::DecideAbsence(const DecideTree &tree) : tree(tree) {
}

void DecideAbsence::ReadClasses(const vector<KeyedMapping> &classes_p, const vector<vector<bool>> &auxiliaries_p) {
	classes = &classes_p;
	auxiliaries = &auxiliaries_p;
}

void DecideAbsence::ReadFrames(const vector<DecideFrameMap> &frames_p) {
	frames = &frames_p;
}

bool DecideAbsence::At(idx_t variable, idx_t row) const {
	// A term of no decision, a constant term of the objective, reads none.
	if (variable == DConstants::INVALID_INDEX) {
		return false;
	}
	if (auxiliaries && !(*auxiliaries)[variable].empty() && (*auxiliaries)[variable][row]) {
		return true;
	}
	// A product's auxiliary has none where either factor has none.
	for (auto function : tree.function_nodes) {
		const auto &product = *tree.nodes[function];
		if (product.function_kind == DecideFunctionKind::BOOLEAN_PRODUCT && product.value_var == variable &&
		    (At(product.product_bool_var, row) || At(product.product_other_var, row))) {
			return true;
		}
	}
	auto &scope = tree.variable_scopes[variable];
	return scope.IsKeyed() && (*classes)[scope.keyed_scope_idx].IsAbsent(row);
}

bool DecideAbsence::TermAt(const DecideTerm &term, idx_t row) const {
	if (term.variable_index == DConstants::INVALID_INDEX || term.frame_read == DConstants::INVALID_INDEX) {
		return At(term.variable_index, row);
	}
	return DecideFrameReadsAbsent((*frames)[term.frame_read], row,
	                              [&](idx_t at) { return At(term.variable_index, at); });
}

bool DecideAbsence::MayBe(idx_t variable) const {
	if (variable == DConstants::INVALID_INDEX) {
		return false;
	}
	if (auxiliaries && !(*auxiliaries)[variable].empty()) {
		return true;
	}
	for (auto function : tree.function_nodes) {
		const auto &product = *tree.nodes[function];
		if (product.function_kind == DecideFunctionKind::BOOLEAN_PRODUCT && product.value_var == variable &&
		    (MayBe(product.product_bool_var) || MayBe(product.product_other_var))) {
			return true;
		}
	}
	return tree.variable_scopes[variable].IsKeyed();
}

bool DecideAbsence::MayBe(const DecideTerm &term) const {
	// A frame reads nothing at an anchor with no position, whatever its decision.
	return term.variable_index != DConstants::INVALID_INDEX &&
	       (term.frame_read != DConstants::INVALID_INDEX || MayBe(term.variable_index));
}

void DecideAbsence::SkipEmptyReducers(const vector<const TermFilterState *> &filters, idx_t num_rows,
                                      vector<idx_t> &row_group_ids, idx_t &num_groups) {
	auto filtered = [](const TermFilterState *filter) {
		return filter->has_filter;
	};
	if (std::none_of(filters.begin(), filters.end(), filtered)) {
		return;
	}
	if (row_group_ids.empty()) {
		row_group_ids.assign(num_rows, 0);
		num_groups = 1;
	}
	vector<bool> skipped(num_groups, false);
	for (auto filter : filters) {
		if (!filter->has_filter) {
			continue;
		}
		vector<bool> read(num_groups, false);
		for (idx_t row = 0; row < num_rows; row++) {
			if (row_group_ids[row] != DConstants::INVALID_INDEX && filter->mask[row]) {
				read[row_group_ids[row]] = true;
			}
		}
		for (idx_t group = 0; group < num_groups; group++) {
			skipped[group] = skipped[group] || !read[group];
		}
	}
	for (auto &group : row_group_ids) {
		if (group != DConstants::INVALID_INDEX && skipped[group]) {
			group = DConstants::INVALID_INDEX;
		}
	}
}

void DecideAbsence::SkipNullInstances(const vector<bool> &null_rows, bool grouped, idx_t num_rows,
                                      vector<idx_t> &row_group_ids, idx_t &num_groups) {
	if (std::find(null_rows.begin(), null_rows.end(), true) == null_rows.end()) {
		return;
	}
	if (row_group_ids.empty()) {
		row_group_ids.assign(num_rows, 0);
		num_groups = 1;
	}
	vector<bool> skipped(num_groups, false);
	for (idx_t row = 0; row < num_rows; row++) {
		if (null_rows[row] && row_group_ids[row] != DConstants::INVALID_INDEX) {
			if (grouped) {
				skipped[row_group_ids[row]] = true;
			} else {
				row_group_ids[row] = DConstants::INVALID_INDEX;
			}
		}
	}
	for (auto &group : row_group_ids) {
		if (group != DConstants::INVALID_INDEX && skipped[group]) {
			group = DConstants::INVALID_INDEX;
		}
	}
}

} // namespace duckdb
