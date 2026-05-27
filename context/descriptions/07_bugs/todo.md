# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

---

## System Catalog Access Breaks With `Parser Error: syntax error at or near "then"`

**Priority: Medium** (user DECIDE queries unaffected; blocks metadata introspection and any tool that scans the catalog)

### Symptom

Any query that causes DuckDB's built-in default views to be materialized fails at bind time with:

```
Parser Error: syntax error at or near "then"
```

The caret in the error points into the *outer* query text even though that text contains no `then` token — the error is coming from the lazy parse of a default-view SQL body during catalog binding.

### Reproduction

```sql
SELECT * FROM duckdb_tables();          -- FAIL
SELECT * FROM duckdb_views();           -- FAIL
SELECT * FROM sqlite_master;            -- FAIL
SELECT * FROM information_schema.tables;-- FAIL
SELECT * FROM pg_catalog.pg_class;      -- FAIL
SHOW TABLES;                            -- FAIL (rewrites to sqlite_schema query)
.tables                                 -- FAIL (same)

SELECT 1 FROM duckdb_tables() LIMIT 0;  -- OK (LIMIT 0 short-circuits before bind)
SELECT * FROM duckdb_schemas();         -- OK (does not trigger default-view scan)
SELECT * FROM duckdb_indexes();         -- OK
```

User DECIDE queries and regular user-table queries are unaffected.

### What is known

- The parser error message shows the outer query text, but the offending token is inside one of the default-view bodies registered in `src/catalog/default/default_views.cpp`. Those bodies are parsed lazily by `CreateViewInfo::FromSelect` the first time a scan walks over them.
- The trigger is any path that calls `Catalog::GetAllSchemas` + `schema.Scan(CatalogType::TABLE_ENTRY, ...)` with execution (not `LIMIT 0`), because that walk materializes default view catalog entries.
- Default-view SQL bodies that fail to parse when fed directly to the parser: `sqlite_master`, `duckdb_tables`, `duckdb_views`, `duckdb_columns`, `duckdb_constraints`, `pg_attribute`, `pg_attrdef`, `pg_class`, `pg_constraint`, `pg_description`, `pg_proc`, `pg_tables`, `pg_type`, `pg_views`, `information_schema.columns`, `information_schema.tables`, `referential_constraints`, `key_column_usage`, and more. (Many of these fail transitively because they reference `duckdb_tables` / `duckdb_views` views, which themselves fail.)
- Simplified standalone snippets extracted from the view bodies — e.g. `SELECT CASE WHEN temporary THEN 't' ELSE 'p' END relpersistence FROM (VALUES (true)) v(temporary);` — parse fine. The failure only manifests in the full view body, suggesting a token-level interaction specific to a construct inside one of the longer bodies rather than `CASE WHEN ... THEN` in isolation.

### Ruled out

- **`STRICT` keyword**: fully removed by commit `8395945b08` (diff on `third_party/libpg_query/grammar/keywords/func_name_keywords.list` shows `-STRICT_P`). No `STRICT`/`STRICT_P` remains in any keyword list.
- **Stale compiled parser**: `make grammar` produced a zero-byte diff against the committed `third_party/libpg_query/src_backend_parser_gram.cpp`. The grammar sources and compiled parser are already in sync.
- **`type` as a bareword column reference**: works fine (`SELECT 'x' AS type`, `SELECT type FROM (VALUES (...)) v(type)`).
- **`sql` as a column reference**: works (e.g. `SELECT sql FROM (VALUES ('x')) v(sql)`).
- **`CASE WHEN ... THEN ... END alias`** in isolation: works.

### Where to look next

1. Binary-search one of the failing view bodies (start with `pg_class` or `information_schema.tables` in `src/catalog/default/default_views.cpp`) to isolate the minimal sub-expression that fails under the current grammar.
2. Once the construct is identified, check `third_party/libpg_query/grammar/statements/select.y` for rules added during DecidB development that may shadow or conflict with it. Prime suspects:
   - The postfix `a_expr WHEN b_expr` / `a_expr WHEN b_expr PER columnref` rules added for DECIDE constraints/objectives (`select.y` around lines 232–335 and 2914).
   - Anything that altered operator precedence for `%prec POSTFIXOP` in combination with `WHEN`/`PER`.
3. The "near 'then'" token name in the error is a Bison artifact: the offending bareword is being lexed into a token that shares a yacc rule with `THEN`. That's the same signature as the `type`/`COUNT` class of keyword-bucket mistakes seen in prior commits (e.g. `5375579` "removed COUNT from decidb keywords").

### Impact

- Blocks `SHOW TABLES`, `DESCRIBE`, `.tables`, `information_schema.*`, `pg_catalog.*`, `sqlite_master`, any ORM/tool that introspects schema via catalog views.
- Does **not** block user DECIDE queries or normal user-table SELECTs. The stress-test plan can proceed using a user-created schema that does not rely on catalog introspection.

---

## `gurobi.env` TimeLimit Is Silently Overridden By Hard-Coded 300s — Breaks `test_time_limit_surfaces_friendly_error`

**Priority: Medium** (one test in the suite fails; user-supplied Gurobi tuning via `gurobi.env` is partially neutralized)

### Symptom

`make decide-test` fails one test:

```
FAILED tests/test_error_time_limit.py::test_time_limit_surfaces_friendly_error
subprocess.TimeoutExpired: Command '[...decidb ... MINIMIZE SUM(POWER(qty - 2, 2))]'
  timed out after 30 seconds
```

The test drops a `gurobi.env` with `TimeLimit 1` in `cwd` and expects the symmetric MIQP to terminate within ~1s with the friendly "exceeded time limit" message. Instead the decidb subprocess keeps running past the test's 30s `subprocess.run(timeout=30)` cap, gets SIGKILL'd, and produces no output to match against.

### Reproduction

```bash
mkdir -p /tmp/gtest && printf 'TimeLimit 1\n' > /tmp/gtest/gurobi.env
cd /tmp/gtest && DECIDB_FORCE_SOLVER=gurobi \
  /Users/Muneeb1/Desktop/decidb/build/release/decidb \
  /Users/Muneeb1/Desktop/decidb/decidb.db -readonly -c \
  "SELECT l_orderkey, l_linenumber, qty FROM lineitem WHERE l_orderkey < 100
   DECIDE qty IS INTEGER
   SUCH THAT qty <= 10 AND SUM(qty) = 30
   MINIMIZE SUM(POWER(qty - 2, 2))"
```

Process runs for ≫30s (observed >65s in monitor and still going); never trips the TimeLimit=1 from `gurobi.env`.

### What is known

- `src/decidb/gurobi/gurobi_solver.cpp:67` unconditionally calls
  `api.setdblparam(guard.env, "TimeLimit", 300.0);` *after* `emptyenv_internal` (which is the call that auto-reads `gurobi.env`). So any `TimeLimit` from the env file is silently clobbered by the hard-coded 300s ceiling.
- The test relies on Gurobi's documented auto-loading of `gurobi.env` to inject a 1s limit without a SQL-level knob; that contract is violated by the DecidB override.
- The convex MIQP (`MINIMIZE SUM(POWER(qty-2, 2))` over 105 INTEGER vars with `SUM(qty)=30`) does not actually complete within 30s under default Gurobi presolve — the LP relaxation gives a fractional solution (qty=2 for all rows ⇒ SUM=210, infeasible) and the integer branching over the symmetric tree is non-trivial. So the test's "guaranteed status 9 in <1s" premise only held back when `gurobi.env` was being honored.
- The friendly-error branch itself (`status == GRB_TIME_LIMIT` → "DECIDE optimization exceeded time limit.") is correctly wired at `gurobi_solver.cpp:213`; the issue is purely that the limit never fires in the 30s window because it's pinned to 300s.

### Ruled out

- Not a Gurobi-status-constant drift (the original bug those constants caused was fixed; see `done.md`). The constants are correct in `gurobi_loader.hpp` now.
- Not a "no incumbent" issue — the throw path triggers regardless of `SolCount`. The current code intentionally never relabels TIME_LIMIT to OPTIMAL (`gurobi_solver.cpp:189-193` comment "never relabel a non-OPTIMAL termination to OPTIMAL"). That is a behavior change relative to the verification claim in `done.md` ("when SolCount > 0, status is rewritten to GRB_OPTIMAL"), but is internally consistent with the current throw-message expectation in the test.

### Where to look next

Two possible fixes — either is reasonable, pick one:

1. **Honor `gurobi.env`** (matches Gurobi's documented contract). In `gurobi_solver.cpp`, only call `setdblparam("TimeLimit", 300.0)` if the user hasn't already set one — e.g. read the current value with `GRBgetdblparam` and skip the override if it's non-default (i.e. < the Gurobi default of `INFINITY`/1e100). This also unblocks user-side Gurobi tuning more generally.
2. **Expose a SQL-level / env-var TimeLimit knob** (e.g. `DECIDB_TIME_LIMIT` or a `PRAGMA`) and have the test set that instead of dropping `gurobi.env`. Then the unconditional 300s default is fine.

Either way, update `tests/test_error_time_limit.py` to drive the limit through whichever mechanism is chosen, and re-verify the symmetric MIQP actually returns status 9 within the new limit.

### Impact

- One test failure in `make decide-test` (547 passed, 1 failed, 1 skipped as of this writing).
- Any user dropping a `gurobi.env` with a tighter `TimeLimit` or other limit-style parameters that DecidB also re-sets (currently only `OutputFlag`, `TimeLimit`, `NonConvex`) silently has their override discarded — minor footgun for power users.

---

## `PER table.column` (Qualified Column Reference) Fails To Parse

**Priority: Low** (workaround: drop the qualifier; only matters inside JOIN-based DECIDE queries)

### Symptom

The `PER` clause rejects table-qualified column references with a parser error pointing at the dot:

```
Parser Error: syntax error at or near "."

LINE 9:     SUM(x) <= 3 PER r.resource_id AND
                             ^
```

Unqualified column names parse and execute correctly, even in JOIN queries where the column lives on a specific side.

### Reproduction

```sql
CREATE TABLE Resources (resource_id INTEGER, name VARCHAR, available_day VARCHAR);
CREATE TABLE Slots (slot_id INTEGER, day VARCHAR);
INSERT INTO Resources VALUES (1, 'Alice', 'Mon'), (2, 'Bob', 'Mon');
INSERT INTO Slots VALUES (10, 'Mon'), (11, 'Mon');

-- FAILS:
SELECT r.resource_id, s.slot_id, x
FROM Resources r JOIN Slots s ON r.available_day = s.day
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) <= 3 PER r.resource_id AND
          SUM(x) = 1 PER s.slot_id;

-- WORKS (same semantics, no table qualifiers):
SELECT r.resource_id, s.slot_id, x
FROM Resources r JOIN Slots s ON r.available_day = s.day
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) <= 3 PER resource_id AND
          SUM(x) = 1 PER slot_id;
```

### What is known

- The PER rule in the grammar accepts only a bare identifier (or parenthesized list of bare identifiers), not a full `ColumnRef`/qualified-name production. The error is at parse time, not bind time — the parser never builds an AST node for `PER r.resource_id`.
- This is inconsistent with the rest of SQL: `SELECT r.resource_id FROM ...`, `GROUP BY r.resource_id`, `ORDER BY r.resource_id`, and `WHERE r.resource_id = 1` all accept the qualified form.
- The `advanced-scheduling` website example originally used `PER r.resource_id` and `PER s.slot_id` and was unrunnable until rewritten with unqualified names.

### Ruled out

- Not an ambiguity issue: in the failing repro the join column names `resource_id` and `slot_id` are unambiguous (each lives on exactly one side). The parser fails before any name resolution runs.
- Not a `(col1, col2)` issue: the parenthesized multi-column form has the same restriction. `PER (r.resource_id, s.slot_id)` also fails to parse.

### Where to look next

1. `third_party/libpg_query/grammar/statements/select.y`: locate the `PER` rule (referenced in the catalog-views bug entry above as around lines 232–335 and 2914) and replace the bare-identifier production with a `ColumnRef` production (or whichever non-terminal `GROUP BY` uses for its column list — that's the obvious analogue).
2. Regenerate the parser (`make grammar-build`) and verify both single-column and parenthesized multi-column qualified forms parse.
3. Add a `PER table.column` test under `test/decide/` (the `PER` test directory).

### Impact

- JOIN-heavy DECIDE queries are the natural place to want qualifier disambiguation, and they're exactly where users hit this. Workaround is a 5-second fix once you know the rule, but the parser error message (`syntax error at or near "."`) doesn't suggest the fix.
- Documentation impact: the syntax reference (`context/descriptions/00_project_overview/syntax_reference.md` §7) implies "any column name" works with PER without stating the qualifier restriction.
