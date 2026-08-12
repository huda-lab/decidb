//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/query_node/bound_select_node.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/planner/bound_tableref.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/planner/expression_binder/select_bind_state.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"

namespace duckdb {

class BoundGroupByNode {
public:
	//! The total set of all group expressions
	vector<unique_ptr<Expression>> group_expressions;
	//! The different grouping sets as they map to the group expressions
	vector<GroupingSet> grouping_sets;
};

struct BoundUnnestNode {
	//! The index of the UNNEST node
	idx_t index;
	//! The set of expressions
	vector<unique_ptr<Expression>> expressions;
};

//! Bound equivalent of SelectNode
class BoundSelectNode : public BoundQueryNode {
public:
	static constexpr const QueryNodeType TYPE = QueryNodeType::SELECT_NODE;

public:
	BoundSelectNode() : BoundQueryNode(QueryNodeType::SELECT_NODE) {
	}

	//! Bind information
	SelectBindState bind_state;
	//! The projection list
	vector<unique_ptr<Expression>> select_list;
	//! The FROM clause
	unique_ptr<BoundTableRef> from_table;
	//! The WHERE clause
	unique_ptr<Expression> where_clause;
    //! The DECIDE clause
    vector<unique_ptr<Expression>> decide_variables;
    unique_ptr<Expression> decide_constraints;
    DecideSense decide_sense;
    unique_ptr<Expression> decide_objective;
    //! Additive constant peeled from the parsed objective body during
    //! SimplifyDecideObjective (e.g. the `3` in `MAXIMIZE SUM(x) + 3`). The
    //! solver doesn't need this to compute `argmax`/`argmin`, but it's kept
    //! here so callers that ever want to report the actual objective value
    //! can add it back. Zero when nothing was peeled. Transferred to
    //! LogicalDecide at plan time.
    double objective_constant_offset = 0.0;
    //! Number of auxiliary variables (e.g. from IN domain rewrite) at the end of decide_variables
    idx_t num_auxiliary_vars = 0;
    //! Per-variable boolean flag (true if declared IS BOOLEAN)
    vector<bool> is_boolean_var;
    //! Table-scoped variable metadata (populated during binding, transferred to LogicalDecide)
    vector<EntityScopeInfo> entity_scopes;
    vector<DecideVarScopeInfo> variable_scopes;
    //! BoundColumnRefExpressions for entity_key columns (one per key per scope, flattened in scope order).
    //! Transferred to LogicalDecide; see logical_decide.hpp for details on why these exist.
    vector<unique_ptr<Expression>> entity_key_expressions;
    //! MIN/MAX indicator links, objective type, and PER types are now populated by
    //! DecideOptimizer::RewriteMinMax directly on LogicalDecide (post-binding)
	//! list of groups
	BoundGroupByNode groups;
	//! HAVING clause
	unique_ptr<Expression> having;
	//! QUALIFY clause
	unique_ptr<Expression> qualify;
	//! SAMPLE clause
	unique_ptr<SampleOptions> sample_options;

	//! The amount of columns in the final result
	idx_t column_count;
	//! The amount of bound columns in the select list
	idx_t bound_column_count = 0;

	//! Index used by the LogicalProjection
	idx_t projection_index;

	//! Group index used by the LogicalAggregate (only used if HasAggregation is true)
	idx_t group_index;
	//! Table index for the projection child of the group op
	idx_t group_projection_index;
	//! Aggregate index used by the LogicalAggregate (only used if HasAggregation is true)
	idx_t aggregate_index;
	//! Index used for GROUPINGS column references
	idx_t groupings_index;
    //! The table index for storing decide_variables
    idx_t decide_index;

	//! Aggregate functions to compute (only used if HasAggregation is true)
	vector<unique_ptr<Expression>> aggregates;

	//! GROUPING function calls
	vector<unsafe_vector<idx_t>> grouping_functions;

	//! Map from aggregate function to aggregate index (used to eliminate duplicate aggregates)
	expression_map_t<idx_t> aggregate_map;

	//! Window index used by the LogicalWindow (only used if HasWindow is true)
	idx_t window_index;
	//! Window functions to compute (only used if HasWindow is true)
	vector<unique_ptr<Expression>> windows;

	//! Unnest expression
	unordered_map<idx_t, BoundUnnestNode> unnests;

	//! Index of pruned node
	idx_t prune_index;
	bool need_prune = false;

public:
	idx_t GetRootIndex() override {
		return need_prune ? prune_index : projection_index;
	}
    bool HasDecideClause() const {
        return !decide_variables.empty();
    }
};
} // namespace duckdb
