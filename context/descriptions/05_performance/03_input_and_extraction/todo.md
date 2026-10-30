# Input and coefficient extraction

Rank repeated scans and coefficient/RHS work by workload and clause. Compare Q8's keyed-decision mapping, Q1's low-cardinality `per` classes and P4's order-key classes. Propose batching/caching only where supported by measured cost.

**Measured 2026-10-30 (medium, Q8):** model construction is 527 ms of a 0.68 s query, and
`extraction.group_ids` is 412 ms of it -- `execution.entity_mapping` alone 289 ms, grouping
500,000 joined rows by the nine stored columns `per o` stands for into 124,923 orders. DuckDB
itself groups the same rows by the same columns, join and scan included, in about 50 ms
(`SELECT DISTINCT o.* ...`). `BuildGroupIds` (`physical_decide.cpp`) hashes a chunk by vector,
then on every hash hit compares the key column by column through `Vector::GetValue`, a boxed
`Value` per column per row (and a copy into `rep_keys` per new group). Comparing one sort key
per row instead (`CreateSortKeyHelpers::CreateSortKey` over the key chunk, as the frame map
already orders timelines), hashed and compared as bytes, would leave Values only for the
representatives a message asks for (`out_rep_keys`). Check first that sort-key equality is
the grouping SQL's GROUP BY gives (NULL a value, D5; -0.0 and 0.0; NaN), and pin it with a
model dump on Q1, Q4, Q8 and P4.

`profile_pipeline.py` puts `execution.scope_checks` and `execution.evaluate_value_links` in no stage column, so their time shows only inside `non_solver_ms`. The scope checks (`syntax_reference.md` §7.3) run before constraint evaluation on every query with a `when` or a `per` key, one pass over the kept rows per check. Add both spans to the `extraction` stage (each stage names parent spans only, so no time is counted twice), then rank the checks against constraint evaluation on Q1, Q6, Q8 and P4. Record the result in `done.md`.

