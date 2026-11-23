#include "duckdb/planner/expression/bound_decide_reducer_expression.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/function/function_serialization.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

BoundDecideReducerExpression::BoundDecideReducerExpression(LogicalType return_type,
                                                           unique_ptr<AggregateFunction> function_p,
                                                           vector<unique_ptr<Expression>> children_p,
                                                           unique_ptr<Expression> filter_p,
                                                           unique_ptr<FunctionData> bind_info_p)
    : Expression(ExpressionType::BOUND_DECIDE_REDUCER, ExpressionClass::BOUND_DECIDE_REDUCER, std::move(return_type)),
      function(std::move(function_p)), children(std::move(children_p)), bind_info(std::move(bind_info_p)),
      filter(std::move(filter_p)) {
}

static BoundAggregateExpression &Aggregate(unique_ptr<Expression> &aggregate) {
	return aggregate->Cast<BoundAggregateExpression>();
}

BoundDecideReducerExpression::BoundDecideReducerExpression(unique_ptr<Expression> aggregate)
    : BoundDecideReducerExpression(aggregate->return_type, make_uniq<AggregateFunction>(Aggregate(aggregate).function),
                                   std::move(Aggregate(aggregate).children), std::move(Aggregate(aggregate).filter),
                                   std::move(Aggregate(aggregate).bind_info)) {
	alias = aggregate->alias;
	query_location = aggregate->query_location;
}

unique_ptr<Expression> BoundDecideReducerExpression::ToAggregate() const {
	vector<unique_ptr<Expression>> arguments;
	for (auto &child : children) {
		arguments.push_back(child->Copy());
	}
	auto result =
	    make_uniq<BoundAggregateExpression>(*function, std::move(arguments), filter ? filter->Copy() : nullptr,
	                                        bind_info ? bind_info->Copy() : nullptr, AggregateType::NON_DISTINCT);
	result->alias = alias;
	result->return_type = return_type;
	result->query_location = query_location;
	return std::move(result);
}

bool BoundDecideReducerExpression::PropagatesNullValues() const {
	return function && function->null_handling == FunctionNullHandling::SPECIAL_HANDLING
	           ? false
	           : Expression::PropagatesNullValues();
}

void BoundDecideReducerExpression::CopyFacts(const BoundDecideReducerExpression &other) {
	by = other.by;
	per = other.per;
	norm = other.norm;
	written_name = other.written_name;
	count_scale = other.count_scale;
	holds_reducer = other.holds_reducer;
	retained = other.retained;
	frame = other.frame;
	frame_aggregate = other.frame_aggregate;
}

bool BoundDecideReducerExpression::FactsEqual(const BoundDecideReducerExpression &other) const {
	return by == other.by && per == other.per && norm == other.norm && count_scale == other.count_scale &&
	       holds_reducer == other.holds_reducer && retained == other.retained && frame == other.frame &&
	       frame_aggregate == other.frame_aggregate;
}

string DecideNorm::Written() const {
	switch (order) {
	case Order::NONE:
		return string();
	case Order::L1:
		return "1";
	case Order::L2:
		return "2";
	case Order::INF:
		return "'inf'";
	default:
		return bound == 0 ? "0" : "0, " + StringUtil::Format("%.17g", bound);
	}
}

string BoundDecideReducerExpression::ToString() const {
	if (!function) {
		return "norm(" + children[0]->ToString() + ", " + norm.Written() + ")";
	}
	return FunctionExpression::ToString<BoundDecideReducerExpression, Expression, BoundOrderModifier>(
	    *this, string(), string(), WrittenName(), false, false, filter.get());
}

hash_t BoundDecideReducerExpression::Hash() const {
	return function ? CombineHash(Expression::Hash(), function->Hash()) : Expression::Hash();
}

bool BoundDecideReducerExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundDecideReducerExpression>();
	// A norm with no function is the norm it names; the norm of a definition is its spelling.
	if (function ? !other.function || !(*other.function == *function) : other.function || !(other.norm == norm)) {
		return false;
	}
	return Expression::ListEquals(children, other.children) && Expression::Equals(other.filter, filter) &&
	       FunctionData::Equals(bind_info.get(), other.bind_info.get());
}

unique_ptr<Expression> BoundDecideReducerExpression::Copy() const {
	vector<unique_ptr<Expression>> new_children;
	for (auto &child : children) {
		new_children.push_back(child->Copy());
	}
	auto copy = make_uniq<BoundDecideReducerExpression>(
	    return_type, function ? make_uniq<AggregateFunction>(*function) : nullptr, std::move(new_children),
	    filter ? filter->Copy() : nullptr, bind_info ? bind_info->Copy() : nullptr);
	copy->CopyProperties(*this);
	copy->CopyFacts(*this);
	return std::move(copy);
}

void BoundDecideReducerExpression::Serialize(Serializer &serializer) const {
	Expression::Serialize(serializer);
	serializer.WriteProperty(200, "return_type", return_type);
	serializer.WriteProperty(201, "children", children);
	serializer.WriteProperty(202, "has_function", function != nullptr);
	if (function) {
		FunctionSerializer::Serialize(serializer, *function, bind_info.get());
	}
	serializer.WritePropertyWithDefault(204, "filter", filter, unique_ptr<Expression>());
	serializer.WritePropertyWithDefault(205, "by", by, DConstants::INVALID_INDEX);
	serializer.WritePropertyWithDefault(206, "per", per, DConstants::INVALID_INDEX);
	serializer.WritePropertyWithDefault(207, "norm_order", static_cast<uint8_t>(norm.order), uint8_t(0));
	serializer.WritePropertyWithDefault(208, "norm_bound", norm.bound, 0.0);
	serializer.WritePropertyWithDefault(209, "written_name", written_name);
	serializer.WritePropertyWithDefault(210, "count_scale", count_scale, false);
	serializer.WritePropertyWithDefault(211, "holds_reducer", holds_reducer, false);
	serializer.WritePropertyWithDefault(212, "retained", retained, DConstants::INVALID_INDEX);
	serializer.WritePropertyWithDefault(213, "frame", frame, DConstants::INVALID_INDEX);
	serializer.WritePropertyWithDefault(214, "frame_aggregate", frame_aggregate, DConstants::INVALID_INDEX);
}

unique_ptr<Expression> BoundDecideReducerExpression::Deserialize(Deserializer &deserializer) {
	auto return_type = deserializer.ReadProperty<LogicalType>(200, "return_type");
	auto children = deserializer.ReadProperty<vector<unique_ptr<Expression>>>(201, "children");
	unique_ptr<AggregateFunction> function;
	unique_ptr<FunctionData> bind_info;
	if (deserializer.ReadProperty<bool>(202, "has_function")) {
		auto entry = FunctionSerializer::Deserialize<AggregateFunction, AggregateFunctionCatalogEntry>(
		    deserializer, CatalogType::AGGREGATE_FUNCTION_ENTRY, children, return_type);
		function = make_uniq<AggregateFunction>(std::move(entry.first));
		bind_info = std::move(entry.second);
	}
	auto filter =
	    deserializer.ReadPropertyWithExplicitDefault<unique_ptr<Expression>>(204, "filter", unique_ptr<Expression>());
	auto result = make_uniq<BoundDecideReducerExpression>(std::move(return_type), std::move(function),
	                                                      std::move(children), std::move(filter), std::move(bind_info));
	deserializer.ReadPropertyWithExplicitDefault(205, "by", result->by, DConstants::INVALID_INDEX);
	deserializer.ReadPropertyWithExplicitDefault(206, "per", result->per, DConstants::INVALID_INDEX);
	result->norm.order = static_cast<DecideNorm::Order>(
	    deserializer.ReadPropertyWithExplicitDefault<uint8_t>(207, "norm_order", uint8_t(0)));
	deserializer.ReadPropertyWithExplicitDefault(208, "norm_bound", result->norm.bound, 0.0);
	deserializer.ReadPropertyWithDefault(209, "written_name", result->written_name);
	deserializer.ReadPropertyWithExplicitDefault(210, "count_scale", result->count_scale, false);
	deserializer.ReadPropertyWithExplicitDefault(211, "holds_reducer", result->holds_reducer, false);
	deserializer.ReadPropertyWithExplicitDefault(212, "retained", result->retained, DConstants::INVALID_INDEX);
	deserializer.ReadPropertyWithExplicitDefault(213, "frame", result->frame, DConstants::INVALID_INDEX);
	deserializer.ReadPropertyWithExplicitDefault(214, "frame_aggregate", result->frame_aggregate,
	                                             DConstants::INVALID_INDEX);
	return std::move(result);
}

} // namespace duckdb
