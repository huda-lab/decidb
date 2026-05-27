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
