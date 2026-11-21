//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_term_split.hpp
//
// The single answer to "what are the terms of this decision expression".
//===----------------------------------------------------------------------===//
//
// A READER OF THE ONE SPELLING. Canonicalization writes every product of decisions as
// a coefficient times decision factors raised to whole powers (decide_product_form.hpp),
// and the shape check refuses whatever is outside that vocabulary or above degree 2.
// This reads one atom's body back as terms. It opens no product of its own: a product
// is read through ReadDecideProduct, and a coefficient on a single factor is
// distributed over that factor's terms (`K * (1 - pick)` is `K - K * pick`), which is
// an identity. It never refuses: a decision-bearing node it does not open (ABS, a
// reducer, a marker a rewrite left) is an opaque term, which each reader takes or
// declines.
//
// WHO READS IT. The solver path's prepared linear form (stage 05), singleton folding and
// direct solve's semantic facts. All see the same terms, so a shape one of them admits
// cannot mean something else to another.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;
class BoundFunctionExpression;
class LogicalDecide;

//! One Boolean product helper allocated while the optimizer annotates a written product. The tree turns this
//! specification into a BOOLEAN_PRODUCT F node and owns the executable BilinearLinkSpec record.
struct DecideBilinearFunctionProduct {
	idx_t aux_idx = DConstants::INVALID_INDEX;
	idx_t first_var_idx = DConstants::INVALID_INDEX;
	idx_t second_var_idx = DConstants::INVALID_INDEX;
	DecideRowOrigin origin;
};

//! An intact written product plus the expression the term splitter reads. `expansion` replaces Boolean products by
//! their plan-time helper columns while leaving non-Boolean products as terms of degree 2.
struct DecideBilinearFunctionGroup {
	unique_ptr<Expression> expansion;
	vector<DecideBilinearFunctionProduct> products;
};

struct DecideSplitFactor;

//! One additive term: `sign * coefficient * ...`, where what follows the coefficient is its degree's part.
struct DecideSplitTerm {
	int sign = 1;
	//! Decision-free; null means 1. A term of degree 0 is its coefficient.
	unique_ptr<Expression> coefficient;
	//! The term's degree in the decisions. An opaque term has the degree of its node.
	idx_t degree = 0;
	//! Degree 1: the decision the term multiplies.
	idx_t var_a = DConstants::INVALID_INDEX;
	//! Degree 2: one linear form squared, or two linear forms multiplied.
	vector<DecideSplitFactor> factors;
	//! A decision-bearing node the reader does not open: `coefficient * opaque`. For ABS, `inner` holds its argument's
	//! terms; only a tree RewriteAbs has not lowered holds one.
	unique_ptr<Expression> opaque;
	vector<DecideSplitTerm> inner;

	//! A frame selector over decisions (language/guide.md §5.6): `coefficient * at(F: payload) over (...)`. The term is
	//! opaque (its marker), so a reader that does not read frames declines it; `inner` holds the payload's terms, each
	//! read at the row the frame `frame` selects. INVALID_INDEX for every other term. Only the solver path reads one
	//! (it reads the frame's fallback off LogicalDecide::frames).
	idx_t frame = DConstants::INVALID_INDEX;
	const Expression *marker = nullptr;

	bool IsConstant() const {
		return degree == 0 && !opaque;
	}
	bool IsLinear() const {
		return degree == 1 && !opaque;
	}
	bool IsFrame() const {
		return frame != DConstants::INVALID_INDEX;
	}
	bool IsAbs() const;
	bool IsSquare() const;
	bool IsProduct() const;
};

//! A factor of a degree-2 term: a linear form (terms of degree 0 and 1) raised to `power`.
struct DecideSplitFactor {
	vector<DecideSplitTerm> form;
	idx_t power = 1;
};

inline bool DecideSplitTerm::IsSquare() const {
	return !opaque && factors.size() == 1 && factors[0].power == 2;
}
inline bool DecideSplitTerm::IsProduct() const {
	return !opaque && factors.size() == 2;
}

//! Bind `name` for exactly these children through FunctionBinder. Rebuilding a bound function by reusing another
//! node's signature over different children silently reinterprets their physical types; this is the only rebuild
//! that stays correct, and every rebuilt coefficient goes through it. `name` is a function's primary name (`**`, `@`);
//! `written_name` is what a plan prints for it (`power`, `abs`), empty for the primary itself.
unique_ptr<Expression> DecideRebindOperator(ClientContext &context, const string &name,
                                            vector<unique_ptr<Expression>> children,
                                            const string &written_name = string());

//! The coefficient of two like terms summed into one: `sign_first * (first ± next)`, taking `-` exactly when the terms'
//! signs differ. The solver's like-term collection and the direct-solve plans both build their merged coefficient here,
//! so a NULL message quotes the same expression on both paths.
unique_ptr<Expression> DecideMergeLikeCoefficients(ClientContext &context, int first_sign, unique_ptr<Expression> first,
                                                   int next_sign, unique_ptr<Expression> next);

class DecideTermSplitter {
public:
	//! Reads the node's decisions and the product groups its optimizer annotated, so no caller can read a group's
	//! function as opaque.
	DecideTermSplitter(ClientContext &context, const LogicalDecide &decide);

	//! Read a reducer body or a row-level decision expression into terms, appending to `out` in reading order with
	//! `sign` applied.
	void Split(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const;
	//! Whether the expression becomes linear after optimizer-annotated product groups and function helpers are read.
	bool IsLinear(const Expression &expr) const;
	//! Replaces every function the optimizer gave a helper decision (an ABS, a definition) with its helper, cast to the
	//! function's type: the casts around the function were bound to that type, and a helper's is its decision's.
	void ReplaceTaggedFunctions(unique_ptr<Expression> &expression) const;
	//! Copy and expand any optimizer-annotated product groups in an expression; false leaves `expanded` unset.
	bool ExpandBilinearGroups(const Expression &expr, unique_ptr<Expression> &expanded) const;
	//! The index of the first decision variable referenced anywhere in `expr`, or INVALID_INDEX.
	idx_t FindVariable(const Expression &expr) const;

private:
	//! Split without the location rule below.
	void SplitNode(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const;
	void ReadProduct(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const;
	void ReadQuotient(const BoundFunctionExpression &quotient, int sign, vector<DecideSplitTerm> &out) const;
	void AddOpaque(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const;

	ClientContext &context;
	idx_t decide_index;
	column_binding_map_t<idx_t> variable_indexes;
	const vector<unique_ptr<Expression>> &decide_variables;
	const vector<DecideBilinearFunctionGroup> &bilinear_groups;
};

} // namespace duckdb
