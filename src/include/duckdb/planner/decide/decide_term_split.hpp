//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_term_split.hpp
//
// The single answer to "what are the terms of this decision expression".
//===----------------------------------------------------------------------===//
//
// WHY THIS IS NOT THE CANONICALIZER. Canonicalization never opens a term: it splits
// a side additively, moves whole terms across the relation, and is total and pure
// because of it. Splitting a reducer body or a row-level expression into
// `coefficient * variable` pieces does open terms. It distributes `K * (1 - x)`,
// recognises squares and products, rebuilds coefficients, and can fail. So it lives
// beside the canonicalizer and reads what canonicalization produced: the additive
// atoms come from ReadCanonicalAtoms, and this splits one atom's body.
//
// WHO READS IT. The solver path's prepared linear form (stage 05) and direct
// solve's semantic facts. Both see the same terms, so a shape one of them admits
// cannot mean something else to the other.
//
// FAILURE IS A TERM, NOT A GUESS. A decision under a node this does not recognise
// becomes an UNKNOWN term carrying the reason. The solver path raises it as the
// error it always raised; the facts report it as an unknown fact. Nothing is read as
// a plain variable because a variable appears somewhere beneath it.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;

enum class DecideTermKind : uint8_t {
	//! A decision-free term: `coefficient` is the whole term.
	CONSTANT,
	//! `coefficient * var_a`.
	LINEAR,
	//! `coefficient * var_a * var_b`, two different decisions. A null coefficient means 1.
	PRODUCT,
	//! `square_scale * (inner)^2`, from POWER(e, 2), `e ** 2` or `(e) * (e)`. `inner` is linear.
	SQUARE,
	//! `coefficient * |inner|`. `inner` is linear; a null coefficient means 1. Only a tree that has not been
	//! lowered holds one: RewriteAbs replaces every ABS before the solver path reads terms.
	ABS,
	//! Not one of the above. `reason` says why; `user_error` says whether the query or the engine is at fault.
	UNKNOWN
};

//! One additive term, contributing `sign` times what its kind describes.
struct DecideSplitTerm {
	DecideTermKind kind = DecideTermKind::UNKNOWN;
	int sign = 1;
	unique_ptr<Expression> coefficient;
	double square_scale = 1.0;
	idx_t var_a = DConstants::INVALID_INDEX;
	idx_t var_b = DConstants::INVALID_INDEX;
	vector<DecideSplitTerm> inner;
	string reason;
	bool user_error = false;
};

//! Bind `name` for exactly these children through FunctionBinder. Rebuilding a bound function by reusing another
//! node's signature over different children silently reinterprets their physical types; this is the only rebuild
//! that stays correct, and every rebuilt coefficient goes through it.
unique_ptr<Expression> DecideRebindOperator(ClientContext &context, const string &name,
                                            vector<unique_ptr<Expression>> children);

class DecideTermSplitter {
public:
	DecideTermSplitter(ClientContext &context, idx_t decide_index,
	                   const vector<unique_ptr<Expression>> &decide_variables);

	//! Split a reducer body or a row-level decision expression into constant, linear, product, square and
	//! absolute-value terms, appending to `out` in reading order with `sign` applied.
	void Split(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const;
	//! Split an expression that must be linear: every term is CONSTANT or LINEAR, or UNKNOWN when it is not.
	void SplitLinear(const Expression &expr, vector<DecideSplitTerm> &out) const;
	//! The index of the first decision variable referenced anywhere in `expr`, or INVALID_INDEX.
	idx_t FindVariable(const Expression &expr) const;

private:
	struct QuadraticPattern {
		const Expression *inner_linear_expr = nullptr;
		double sign = 1.0;
		//! Set when the squared expression is not linear; names the shape for the error.
		const char *nonlinear_shape = nullptr;
	};
	struct ProductFactors {
		vector<const Expression *> coefficient_factors;
		vector<idx_t> decide_factors;
		//! A factor `ABS(e)`, when the product is a data coefficient times one.
		const Expression *abs_factor = nullptr;
	};

	bool ContainsVariable(const Expression &expr, idx_t var_idx) const;
	bool TryGetBareDecideFactor(const Expression &expr, idx_t &var_idx) const;
	//! False with `failure` filled when a factor cannot be classified.
	bool ClassifyProduct(const Expression &expr, ProductFactors &result, DecideSplitTerm &failure) const;
	unique_ptr<Expression> ExtractCoefficientWithoutVariable(const Expression &expr, idx_t var_idx) const;
	QuadraticPattern DetectQuadraticPattern(const Expression &expr) const;
	bool IsLinear(const Expression &expr) const;
	void AddAbs(const Expression &abs_function, unique_ptr<Expression> coefficient, int sign,
	            vector<DecideSplitTerm> &out) const;

	ClientContext &context;
	idx_t decide_index;
	column_binding_map_t<idx_t> variable_indexes;
	const vector<unique_ptr<Expression>> &decide_variables;
};

} // namespace duckdb
