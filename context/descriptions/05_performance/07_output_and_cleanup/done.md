# Output and cleanup

Backend result extraction is separate from optimization. DECIDE's output accumulator measures complete solution-to-row mapping and integer conversion. CTAS forces full output without terminal formatting millions of rows; it is a different consumer from the original CLI benchmark.

The facade times postprocessing and cleanup separately. Gurobi model/environment destruction is instrumented. Finalize self time includes remaining local destruction; process wall/resource measurements additionally include validation and shutdown.

