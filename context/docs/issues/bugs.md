# Known bugs

Each entry: short title, location, what's wrong, why it matters, and when it was found. Each of these fails a test of
DuckDB's own suite (`build/release/test/unittest "test/sql/*"`).

## COMPAT-1 — DeciDB cannot open a database file whose views DuckDB stored

Location: `src/include/duckdb/storage/serialization/query_node.json`, `SelectNode` members 211 (`decide_sense`) and 213
(`decide_text`), generated as `WriteProperty`/`ReadProperty` in `src/storage/serialization/serialize_query_node.cpp`.
Every stored `SELECT` carries them and every read requires them, so a view DuckDB stored, which has neither, does not
load: `Serialization Error: Failed to deserialize: field id mismatch, expected: 211, got: 65535`. The DECIDE members
beside them (209, 210, 212) are written with a default, which is how DuckDB adds a member to a stored node. Fix: give
both members the default a `SELECT` without DECIDE holds, so they are written only when set, and regenerate
(`scripts/generate_serialization.py`); check with `DECIDB_VERIFY_SERIALIZER=1 make decide-test`. Fails
`test/sql/storage_version/storage_version.test_slow`, `test/sql/storage/bc/test_view_v092.test` and
`test/sql/attach/attach_huggingface_index.test:12`. Found 2026-11-09 in H-52's gate.

## COMPAT-2 — the `BOOL` keyword quotes DuckDB's names in messages

Location: `third_party/libpg_query/grammar/keywords/unreserved_keywords.list` (`BOOL_P`, for a decision written
`x(BOOL)`). DuckDB quotes any keyword when it writes a name (`KeywordHelper::RequiresQuotes`,
`src/parser/keyword_helper.cpp`), so the member `bool` of `UNION(string VARCHAR, bool BOOLEAN)` prints as `"bool"`, and
a user's column or member named `bool` is quoted in every message and rendered query. Fix direction: the grammar reads a
decision's type as a name, and the transformer checks it, as DuckDB reads a type name, so DECIDE adds no keyword; if
another DECIDE keyword is a name DuckDB prints, the same fix covers it. Fails
`test/sql/index/art/types/test_art_union.test:29`. Found 2026-11-09 in H-52's gate.

## COMPAT-3 — a list comprehension over subscripted names reaches the binder

Location: the grammar (not yet traced). `select [a for a[1], b[2] in [1,2,3,4]]` raises `Binder Error: Invalid lambda
parameters!` where DuckDB raises `Parser Error: syntax error at or near "["`. First find the grammar change that admits
it (`git log -p -- third_party/libpg_query/grammar`), then decide whether it is DuckDB's or DeciDB's. Fails
`test/sql/function/list/lambdas/list_comprehension.test:93`. Found 2026-11-09 in H-52's gate (also failing before it).

## COMPAT-4 — `concurrent_append_metadata_queries` fails intermittently

Location: `DependencyManager::Scan` (`src/catalog/dependency_manager.cpp`) dereferences an unset `optional_ptr` in
`CatalogSet::Scan` while appends run beside `duckdb_dependencies()`; the database is then invalidated. It failed at line
22 in one run and at lines 54 to 78 in another. Whether DeciDB causes it is unknown: run the test repeatedly on a build
of the DuckDB commit DeciDB forked from; if it fails there it is a limitation inside DuckDB (recorded below), otherwise
bisect. Fails `test/sql/parallelism/interquery/concurrent_append_metadata_queries.test_slow`. Found 2026-11-09 in
H-52's gate.

## Limitations (not bugs)

- **Gurobi's non-convex tolerance.** On `SUM(POWER(x, 2)) BY (k) <= 25 MAXIMIZE SUM(POWER(x, 2))`, with optimum 50,
  Gurobi returns 50.0000001 and reports it optimal at tolerance 0. Inside Gurobi's spatial branching.
- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.
- **Gurobi's barrier on many limits on squares.** With a redundant bound on the decision, a feasible, well-scaled query
  with one `SUM(POWER(x, 2))` limit per group can stop with Gurobi's numerical-trouble status and no solution, at
  Gurobi's default settings. With `x <= 50` and a limit of 25, 7 of 20 random inputs fail on 2,000 rows in 100 groups
  and 9 of 20 on 20,000 rows in 1,000 groups. No Gurobi setting tried recovers them. The query raises `DECIDE
  optimization failed: Gurobi could not solve this model (numerical trouble).` Inside Gurobi.
- **Gurobi at an objective constant of about 1e200 and above.** `SELECT id, x FROM (VALUES (1, 1e100)) t(id, v) DECIDE
  x(REAL) SUCH THAT x BETWEEN 0 AND 2e100 MINIMIZE SUM(POWER(x - v, 2))` prints the number-range warning, then
  `DECIDE optimization is unbounded`, on a model whose every decision is bounded. Gurobi returns an unbounded status
  for the expanded square's constant `v^2`; HiGHS refuses to load it ("numbers too far apart in size"). Inside the
  backends; direct solve answers `x = v`.
- **DuckDB moves a constant across a comparison and drops an overflow.** `SELECT i FROM t WHERE i + 127::TINYINT > 0`
  over a TINYINT `i = 1` returns the row, where `SELECT i + 127::TINYINT FROM t` raises `Overflow in addition of INT8`.
  DuckDB's `MoveConstantsRule` (expression rewriter) rewrites the filter to `i > -127`, which has no addition to
  overflow; with `SET disabled_optimizers = 'expression_rewriter'` the filter raises. Inside DuckDB, for integer
  comparisons only. No DECIDE check depends on an integer overflow in a filter: they compute in DOUBLE.

Resolved behavior is documented by its owning `done.md`.
