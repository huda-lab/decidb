# Benchmark methodology

Quantify measurement overhead: an instrumented run against an uninstrumented one, isolated from solver logs and stack sampling. None of the current sweep does this.

Decide whether to bring back a lightweight correctness check now that `validation_sql` is gone — even a per-query objective sanity bound would catch a fast wrong answer, which the row-count-only check cannot.

Run the same sweep at the large tier and repeat it against a second host, since every number so far comes from one machine.
