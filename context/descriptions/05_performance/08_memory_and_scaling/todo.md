# Memory and scaling

Find the point where each workload and backend stops fitting comfortably, and which stage holds the memory when it does. Relate peak RSS to model size — variables, rows and nonzeros are recorded alongside it in the same CSV row. Large-database runs are unmeasured; the medium sweep comes first.

## A model row carries its full provenance

**Why.** Every `ModelConstraint` holds a `ConstraintProvenance` -- ids, kinds, two strings,
two vectors, the display fields -- of a few hundred bytes, beside its terms, so a model of
millions of short rows is mostly provenance. `benchmark/decide/anr_scaling.py` shows it: a
MAX over windows of five (`range-max`, six million rows at 10⁶ input rows) builds with a
growth exponent of 1.07 between 10⁵ and 10⁶, the excess in emitting the rows (the
extremum links and `model.global_rows`), where the workloads of one row per input row grow
at 1.0 (`../04_model_construction/done.md`, "How the ANR workloads grow").

**What it needs.** Provenance shared per clause and group, a row holding an index into a
table of them, so a row is its terms; the diagnosis reads a row's provenance through it.
`ConstraintProvenance` is read wherever a diagnosis names a row
(`07_query_diagnostics/`), so the contract changes there too.

**Test.** `anr_scaling.py --only range-max --max 1000000` at an exponent near 1.0, and the
diagnosis suites unchanged.
