#include "duckdb/planner/expression/bound_decide_reducer_expression.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/function/function_serialization.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/decide/decide_tree_nodes.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

BoundDecideReducerExpression::BoundDecideReducerExpression(AggregateFunction function_p,
                                                           vector<unique_ptr<Expression>> children_p,
                                                           unique_ptr<Expression> filter_p,
                                                           unique_ptr<FunctionData> bind_info_p)
    : Expression(ExpressionType::BOUND_DECIDE_REDUCER, ExpressionClass::BOUND_DECIDE_REDUCER, function_p.return_type),
      function(std::move(function_p)), children(std::move(children_p)), bind_info(std::move(bind_info_p)),
      filter(std::move(filter_p)) {
}

static BoundAggregateExpression &Aggregate(unique_ptr<Expression> &aggregate) {
	return aggregate->Cast<BoundAggregateExpression>();
}

BoundDecideReducerExpression::BoundDecideReducerExpression(unique_ptr<Expression> aggregate)
    : BoundDecideReducerExpression(Aggregate(aggregate).function, std::move(Aggregate(aggregate).children),
                                   std::move(Aggregate(aggregate).filter), std::move(Aggregate(aggregate).bind_info)) {
	alias = aggregate->alias;
	return_type = aggregate->return_type;
	query_location = aggregate->query_location;
}

unique_ptr<Expression> BoundDecideReducerExpression::ToAggregate() const {
	vector<unique_ptr<Expression>> arguments;
	for (auto &child : children) {
		arguments.push_back(child->Copy());
	}
	auto result =
	    make_uniq<BoundAggregateExpression>(function, std::move(arguments), filter ? filter->Copy() : nullptr,
	                                        bind_info ? bind_info->Copy() : nullptr, AggregateType::NON_DISTINCT);
	result->alias = alias;
	result->return_type = return_type;
	result->query_location = query_location;
	return std::move(result);
}

bool BoundDecideReducerExpression::PropagatesNullValues() const {
	return function.null_handling == FunctionNullHandling::SPECIAL_HANDLING ? false
	                                                                        : Expression::PropagatesNullValues();
}

string BoundDecideReducerExpression::ToString() const {
	return FunctionExpression::ToString<BoundDecideReducerExpression, Expression, BoundOrderModifier>(
	    *this, string(), string(), DecideWrittenName(*this), false, false, filter.get());
}

hash_t BoundDecideReducerExpression::Hash() const {
	return CombineHash(Expression::Hash(), function.Hash());
}

bool BoundDecideReducerExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundDecideReducerExpression>();
	return other.function == function && Expression::ListEquals(children, other.children) &&
	       Expression::Equals(other.filter, filter) && FunctionData::Equals(bind_info.get(), other.bind_info.get());
}

unique_ptr<Expression> BoundDecideReducerExpression::Copy() const {
	vector<unique_ptr<Expression>> new_children;
	for (auto &child : children) {
		new_children.push_back(child->Copy());
	}
	auto copy = make_uniq<BoundDecideReducerExpression>(
	    function, std::move(new_children), filter ? filter->Copy() : nullptr, bind_info ? bind_info->Copy() : nullptr);
	copy->CopyProperties(*this);
	return std::move(copy);
}

void BoundDecideReducerExpression::Serialize(Serializer &serializer) const {
	Expression::Serialize(serializer);
	serializer.WriteProperty(200, "return_type", return_type);
	serializer.WriteProperty(201, "children", children);
	FunctionSerializer::Serialize(serializer, function, bind_info.get());
	serializer.WritePropertyWithDefault(204, "filter", filter, unique_ptr<Expression>());
}

unique_ptr<Expression> BoundDecideReducerExpression::Deserialize(Deserializer &deserializer) {
	auto return_type = deserializer.ReadProperty<LogicalType>(200, "return_type");
	auto children = deserializer.ReadProperty<vector<unique_ptr<Expression>>>(201, "children");
	auto entry = FunctionSerializer::Deserialize<AggregateFunction, AggregateFunctionCatalogEntry>(
	    deserializer, CatalogType::AGGREGATE_FUNCTION_ENTRY, children, return_type);
	auto filter =
	    deserializer.ReadPropertyWithExplicitDefault<unique_ptr<Expression>>(204, "filter", unique_ptr<Expression>());
	auto result = make_uniq<BoundDecideReducerExpression>(std::move(entry.first), std::move(children),
	                                                      std::move(filter), std::move(entry.second));
	result->return_type = std::move(return_type);
	return std::move(result);
}

} // namespace duckdb
