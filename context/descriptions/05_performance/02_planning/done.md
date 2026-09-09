# Parsing and planning

Opt-in spans cover parsing, SELECT binding, comparison/objective canonicalization, backend selection, DECIDE rewrite families and physical DECIDE planning. DuckDB detailed JSON supplies the broader planner/optimizer timings. Statement markers exclude setup/validation from the target query's custom profile.

The first Gurobi availability check loads the library and probes an environment/license during planning; `gurobi.availability_probe` is distinct from actual model loading.

