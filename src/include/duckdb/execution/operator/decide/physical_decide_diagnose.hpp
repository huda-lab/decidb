//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/execution/operator/decide/physical_decide_diagnose.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/decide_diagnostic.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

//! Stage 08's half of `DIAGNOSE <select>`. It sinks the decision query's rows and
//! throws them away — DIAGNOSE reports on the run, it does not return the run's
//! output — and then emits the diagnosis the DECIDE operator below it left on the
//! connection, one row per finding.
//!
//! It formulates nothing and decides nothing: the engines produced typed findings
//! during the child's Finalize, and this operator writes them into a chunk.
class PhysicalDecideDiagnose : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::DECIDE_DIAGNOSE;

public:
	PhysicalDecideDiagnose(vector<LogicalType> types, idx_t estimated_cardinality)
	    : PhysicalOperator(PhysicalOperatorType::DECIDE_DIAGNOSE, std::move(types),
	                       estimated_cardinality) {
	}

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk,
	                         OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}
};

} // namespace duckdb
