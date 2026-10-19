# Input and coefficient extraction

Rank repeated scans and coefficient/RHS work by workload and clause. Compare Q8's keyed-decision mapping, Q1's low-cardinality `per` classes and P4's order-key classes. Propose batching/caching only where supported by measured cost.

`profile_pipeline.py` puts `execution.scope_checks` and `execution.evaluate_value_links` in no stage column, so their time shows only inside `non_solver_ms`. The scope checks (`syntax_reference.md` §7.3) run before constraint evaluation on every query with a `when` or a `per` key, one pass over the kept rows per check. Add both spans to the `extraction` stage (each stage names parent spans only, so no time is counted twice), then rank the checks against constraint evaluation on Q1, Q6, Q8 and P4. Record the result in `done.md`.

