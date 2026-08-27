#include "duckdb/execution/operator/decide/physical_decide_diagnose.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class DecideDiagnoseGlobalSinkState : public GlobalSinkState {
public:
	DecideDiagnostic diag;
};

SinkResultType PhysicalDecideDiagnose::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	// The query's own rows are not the answer. Drop them.
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType PhysicalDecideDiagnose::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                  OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<DecideDiagnoseGlobalSinkState>();
	gstate.diag = TakeDecideDiagnostic(context);
	if (!gstate.diag.valid) {
		// The child solved and handed off nothing, so there is nothing wrong to report.
		// A feasible query is not a separate output path — it is one finding that says so.
		gstate.diag = BuildFeasibleDiagnostic();
	}
	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> PhysicalDecideDiagnose::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DecideDiagnoseGlobalSinkState>();
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class DecideDiagnoseGlobalSourceState : public GlobalSourceState {
public:
	idx_t offset = 0;

	idx_t MaxThreads() override {
		return 1;
	}
};

unique_ptr<GlobalSourceState> PhysicalDecideDiagnose::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<DecideDiagnoseGlobalSourceState>();
}

SourceResultType PhysicalDecideDiagnose::GetData(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<DecideDiagnoseGlobalSinkState>();
	auto &source_state = input.global_state.Cast<DecideDiagnoseGlobalSourceState>();

	RenderDecideDiagnostic(gstate.diag, source_state.offset, chunk);
	return source_state.offset >= gstate.diag.findings.size() ? SourceResultType::FINISHED
	                                                          : SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
