//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_shape_check.hpp
//
// The one place the algebra of a DECIDE clause is judged.
//===----------------------------------------------------------------------===//
//
// DecideShapeCheck runs once, in PlanSelectNode, right after canonicalization: every
// clause and the objective are in the one spelling (decide_product_form.hpp), so a
// shape has one reading here, whatever way the user wrote it. It is the semantic-analysis
// pass of a compiler front end, after the parser and the binder have refused only what
// they alone can see. It refuses, for each clause as written:
//   - a function or operator over a decision outside the model's vocabulary (+, -, *,
//     a division by data, a whole-exponent POWER, ABS, the reducers and the markers);
//   - a POWER over a decision whose exponent is not a whole number known before the
//     query runs;
//   - a division by a decision;
//   - a CASE over a decision (a CASE over data is a per-row coefficient);
//   - a term of degree above 2, which no backend takes.
// Each refusal quotes the clause as written and carries the term's source location.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;
struct DecideSourceNames;

class DecideShapeCheck {
public:
	DecideShapeCheck(ClientContext &context, idx_t decide_index, const DecideSourceNames &names);

	//! Judge every comparison of the canonical constraint tree.
	void CheckConstraints(const Expression &constraints, const vector<ConstraintSourceInfo> &sources) const;
	//! Judge the canonical objective; `written` is the objective as written.
	void CheckObjective(const Expression &objective, const string &written) const;

private:
	//! One side of a comparison, or the objective: its vocabulary, then the degree of each term.
	void CheckSide(const Expression &side, const string &clause) const;
	//! Refuse the first decision-bearing node under `expr` the model has no reading for.
	void CheckVocabulary(const Expression &expr, const string &clause) const;
	[[noreturn]] void Refuse(const Expression &term, const string &problem, const string &clause) const;

	ClientContext &context;
	idx_t decide_index;
	const DecideSourceNames &names;
};

} // namespace duckdb
