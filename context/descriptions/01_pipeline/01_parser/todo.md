# Stage 01 — Parser: open work

---

## A DECIDE statement does not survive `ToString()` → re-parse

**Pointers**: `SelectNode::ToString` in `src/parser/query_node/select_node.cpp`
(the `HasDecideClause()` branch); the type markers in `src/common/constants.cpp`
(`DECIDE_VARIABLE_TYPES`); the tag constants in
`src/include/duckdb/common/enums/decide.hpp` (`WHEN_CONSTRAINT_TAG`,
`PER_CONSTRAINT_TAG`, `QUALIFIED_REDUCER_TAG`).

`SelectNode::ToString` renders the DECIDE clause by calling `ToString()` on the
parsed expressions it holds, and those expressions carry DECIDE syntax in an
internal encoding that the generic renderer prints literally. A declaration is a
comparison against a marker string; `WHEN` and `PER` are tag operators. So:

```sql
SELECT name, take FROM items DECIDE take(BOOL) SUCH THAT SUM(wt*take) <= 6
```

prints as

```sql
SELECT "name", take FROM items DECIDE (take = 'bool_variable') SUCH THAT (sum((wt * take)) <= 6)
```

which is not valid DECIDE syntax — re-parsing it is a syntax error at the `(`. A
`PER` clause prints as `__per_constraint__`. Any path that re-prints a DECIDE query
and reads it back silently corrupts it.

The stage owns this: layer 1 "retains source structure", and printing a statement
back as the statement that was written is part of that.

**Where it bites today**: `PRAGMA verify_serializer` (and `PRAGMA
enable_verification`) enable DuckDB's whole verification battery, which includes an
unconditional re-parse check (`StatementVerifier::Create(VerificationType::PARSED,
...)` in `src/main/client_verify.cpp`, added with no gate of its own). Every DECIDE
query fails it with `INTERNAL Error: Parsed statement verification failed`. That in
turn blocks the plan-serialization guard described in
[`../03_logical_plan/done.md`](../03_logical_plan/done.md) §5, whose harness switch
(`DECIDB_VERIFY_SERIALIZER=1`) is wired and working but cannot be used until this is
fixed.

**Decision needed**: whether the renderer reproduces the full surface — declarations
(`x(INT)`, `T.x(BOOL)`, `scalar x(REAL)`), `WHEN`, `PER`, qualified reducers
(`SUM(D, T: e)`), `NORM`, `IN`, the `DIAGNOSE` prefix — or whether the parsed tree
should instead keep the written spelling alongside the encoding, the way
`LogicalDecide::source_fragments` does one layer down. The second is less code to
keep in sync with the grammar, but it puts display state on the parsed tree.

Note the smaller alternative, which does not fix the bug: gating the PARSED verifier
on `query_verification_enabled` in `client_verify.cpp`, as every sibling verifier in
that function already is. That would unblock the serialization guard in one line, at
the cost of the four stock DuckDB tests that use bare `PRAGMA verify_serializer`
losing their re-parse coverage.

**Test**: for each DECIDE syntax form, assert
`Parser().ParseQuery(stmt->ToString())` round-trips to an equal statement. Then
turn the guard on:

```bash
DECIDB_VERIFY_SERIALIZER=1 test/decide/.venv/bin/python3 -m pytest test/decide/tests
```

**Done file**: `done.md` — record that a parsed DECIDE statement re-parses from its
own `ToString()`, and drop the blocker note from
[`../03_logical_plan/done.md`](../03_logical_plan/done.md) §5.
