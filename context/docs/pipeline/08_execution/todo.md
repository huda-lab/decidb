# Stage 08 — Execution: open work

## 1. One key-grouping path (optional)

**Why.** Execution turns a key into row → class ids in two places, both over
`BuildGroupIds` and both with NULL a key value:

- `BuildEntityMappings` (§4) for the key scopes (`EntityScopeInfo`: a keyed decision's
  key, a reducer's own `per` or colon form, a helper decision's `by` key). Ids are
  first-seen over every row; a helper's scope reads only its clause's kept rows and puts
  every dropped row in one spare class. Nothing is cached.
- `LookupOrBuildPerGroupIds` (§5) for the `by` keys, a constraint's `per K:` classes,
  the scope checks' keys and the two-level objective's key. The ids over every row are
  cached in `PerGroupCache`; each call then renumbers the groups its filter keeps densely,
  in the order their rows first appear, and a clause whose `when` drops rows builds
  afresh over its kept rows.

A key used both ways — a keyed decision `per g: x(INT)` and a reducer's `by (g)` or a
constraint's `per g:` — is evaluated twice. One cache serving both would evaluate each
key once and give equal keys equal ids.

**Decision needed.** Whether to keep both numberings or re-capture the golden dumps. The
column order of keyed decisions and helpers follows the entity ids, and the row order of
grouped rows follows the dense ids; golden corpus entries 22, 23, 38, 39, 56, 61, 62, 65,
70 and 91–94 depend on them. A merge that keeps entity ids first-seen over every row and
the per-call dense renumbering changes no dump. If `entity_scopes` and `group_keys` also
merge into one key table, that part is stage 03's.

**Test.** The golden corpus dumps identically on Gurobi and HiGHS; a query with a
decision keyed by `g` and a `by (g)` reducer evaluates `g` once (a profile counter on
`extraction.group_ids`); the scope suites pass unchanged.

**done.md.** §4 "PHASE 1.5 — entity mappings" and §5 "`when`, `by` and `per` →
`row_group_ids`".

## 2. A NULL key is not a class (reverses D5)

**Ruling (owner, 2026-10-27).** "NULL should not be a key. That group should skip." Today D5 makes a NULL key one
more class (`../../language/guide.md` §7.2): with `sum(x) by (g) <= 2`, the rows whose `g` is NULL get a limit of 2 of
their own. Under the ruling those rows leave that clause, as `when g is not null` would, and keep every other clause
and the objective. The keyed direct plans already skip NULL-key rows (`../../direct_solve/framework/todo.md` H-05).

**Decision needed before coding** (the ruling names `by`; the rest is open):

1. `by (g)` on a reducer: rows with a NULL `g` are out of that reducer. Assumed yes.
2. `per g:` in front of a constraint: do NULL-`g` rows get no constraint at all?
3. `per` inside a reducer (`sum(per cap: cap)`): are NULL-`cap` rows left out of the sum?
4. A keyed decision (`per K:` declaration): what does a NULL-key row get? Its own decision, none (fixed 0 or NULL),
   or an error?
5. SQL's `GROUP BY` keeps NULL as one group; the docs must say why DECIDE departs from it.

**Work.** The window partitions and class numbering in `RewriteRhsAggregates` (`decide_rewrite_rhs_window.cpp`) and
the grouping here (§1); the scope checks and messages that quote `g = NULL` (`PhysicalDecide::RunScopeChecks`); the
removed-spelling NULL note (D62), which becomes unnecessary.

**Test.** Every D5 test turns over (for example `empty_all_keys_null` and the NULL-key cases of
`test_direct_s1_uniform_matroid.py`, `test_direct_r1_continuous_knapsack.py`, `test_scope_*.py`), and the golden
baselines with NULL keys. The keyed-plan C++ test ("S1 and R1 keyed plans answer as the solver does",
`test/common/test_decidb_direct_decline.cpp`) can take NULL keys again (`KEYED_ROWS`). Flip each D5 test in the same
change as the code, not before.

**Update.** `../../language/guide.md` §2.3, §3 and §7.2; `../../language/errors.md` §3; `../../language/decisions.md`
D5 and D62.
