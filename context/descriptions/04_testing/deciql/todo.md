# DeciQL Surface Test Coverage — Remaining

- **Lexing depth.** `test_deciql_lexing_errors.py` pins the DECIDE words as
  columns, aliases, keys, order keys and decision names, `t.per` / `"within"`,
  `WITHIN row` versus `PER ROW`, `CASE WHEN` in the select list, `JOIN ON` and
  `WHERE`, nested DECIDE in both clause orders and through a CTE, and a
  three-statement script. Still unpinned: DECIDE nested to four levels, a
  reserved word (`all`, `inner`) as a CTE or column name refused by DuckDB.
- **EXPLAIN, DIAGNOSE and the serializer** over the combination matrix: the
  suite runs under `DECIDB_VERIFY_SERIALIZER=1` but no test pins DIAGNOSE's
  `clause` column for prefixed clauses or `k IS NULL` for a NULL key.
- **SQL interplay** (USING, LEFT JOIN NULL keys, GROUP BY beside DECIDE, views,
  PREPARE, UNION, DECIDE as a scalar subquery of another DECIDE).
- **Domains × guards on both backends** — needs a Gurobi host; see the
  `decidb_cli_gurobi` fixture.
- **Shapes refused by name that could be formulated**, each pinned only as a
  refusal today: an `IF` guard over an `IN` list, a `MIN`/`MAX` compared with a
  frame, an `IF` guard on an easy `MIN`/`MAX` against a bound-side reducer, a sum
  of `MIN`/`MAX` terms under `WHEN`/`PER`. When one is implemented, replace its
  catalogue entry in `test_deciql_lexing_errors.py` with an oracle-backed test.
- **File size.** `test_deciql_frames_matrix.py`, `test_deciql_deck_conformance.py`,
  `test_deciql_prefix_grid.py` and `test_deciql_combinations.py` run 800 to 1,050
  lines, above the ~600-line guideline; each has a clean split point at a section
  header.
