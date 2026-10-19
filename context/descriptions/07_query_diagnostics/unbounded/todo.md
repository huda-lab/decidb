# Query Diagnostics — Unbounded — open work

## 1. A NULL slice prescribes a cap that matches no row

**Why.** A categorical slice over a column holding NULL labels its value with the text
`NULL` (`UnboundedCandidateProvider::BuildRowGrouping` in `physical_decide.cpp`), and
both the `group` text (`decide_diagnostic.cpp`, `c = 'v'`) and the scoped remedy
(`PrescribeCap`, which quotes the value as a SQL string) treat it as the string
`'NULL'`. Over `(VALUES (1,NULL),(2,NULL),(3,'a'),(4,'a'),(5,'b'),(6,'b')) t(id,g)` with
`when g is not null: x <= 4 MAXIMIZE SUM(x)`, the finding's `group` reads `g = 'NULL'`
and its `suggested_change` reads `when g = 'NULL': x <= <cap>`. Pasted back, that cap
keeps no row ("when g = 'NULL' matches no rows"), so the advice does not work. A NULL
value has to render as an `IS NULL` test, in both columns.

**Decision needed.** None for the remedy: it is wrong as printed. How the `group`
column labels a NULL value is the same decision as the infeasible `group` label
(`../infeasible/todo.md` §2), and the two should agree.

**Test.** In `test_query_diagnostics_escaping_instances.py`, both backends: the query
above reports the slice and the remedy as an `IS NULL` test on `g`; pasting the remedy
with a finite cap makes the query solve.

**done.md.** "Scoping the prescribed cap" and "Escape characterization — which
scope-instances escape".
