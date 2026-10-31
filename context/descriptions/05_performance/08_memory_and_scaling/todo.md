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

## A query-wide auxiliary's rows are stated once per row

**Why.** An ABS of a query-wide argument beside a clause's reducers has one query-wide
auxiliary (`05_optimizer/done.md`, "The auxiliary's scope"), but its two envelope rows are
per-row helper rows (`ScopeHelperRow`), so every kept row states the same two rows: over
1000 rows `sum(x) + abs(s - 1) >= 8` builds 1000 identical ABS constraints natively and 4001
rows lowered, and at 200k rows takes 1.98 s against 0.16 s with `s` in the ABS's place. It is
the shape of the ANR log's A12 (a query-wide decision bounded by a data reducer, one row per
input row): a row over query-wide values alone is the same on every row.

**What it needs.** A helper row whose every term is query-wide stated once -- one class, as
under `per ()` -- with its clause's `when` read as "some row is kept", and the expanded
DIAGNOSE scope still one repair per kept row where the clause's own rows are (A12's
constraint).

**Test.** The dump of that query at 1000 rows shows the envelope's two rows once; the answer
is unchanged on both solvers and lowered.
