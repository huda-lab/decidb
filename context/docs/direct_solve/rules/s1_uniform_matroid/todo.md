# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## Read a `by` key again (counts per group)

The language groups a count on the reducer (`sum(x) by (g) <= K`, or one clause per class with
`per g: sum(x) by (g) <= K`), and S1 declines both. Each group of one or more source columns should get its own
count interval. The per-group code is kept and reached only through IR edits
(`PerKeyOnConstraint` in `test/common/test_decidb_direct_decline.cpp`); `DirectIRScope::per_key_slots` and
`DirectFrontEnd::SourceSlot` are kept with nothing filling them (`direct_ir.cpp`, `ReadReducer` declines on
`TryParseGroupKeyTag`).

The per-group plan kept in the code does the following, which is the spec:

- A count bound per group: a constant, or a source value read on each row of the group. The bound-side window rewrite
  now reduces a data bound per group and raises an `=` bound that disagrees, so S1 reads one value per group.
- A bound read from the group: `COUNT(*)`, `COUNT`, `MIN`, `MAX`, `SUM`, `AVG` over the group's rows, alone or in
  arithmetic (`sum(x) by (warehouse) <= min(cap) by (warehouse)`; the top fraction of each group,
  `sum(x) by (g) <= count(*) by (g) * 0.1`).
  `COUNT(*)` counted the rows the clause covers in each group: a clause-level `when` narrowed it, a `when` inside the
  left-hand sum did not.
- The grouped rows of the Performance table in `done.md` measure this plan (up to 98x over Gurobi at 100 groups).

Where the language differs from that plan:

- A NULL key is in no group (D5), as in the kept plan: a NULL-key row bypasses every bound of that key and still obeys
  its fixed value.
- A group the reducer's own `when` leaves empty is an error that quotes the group (D8: `sum(when c: x) has no rows for
  g = 'c'.`), where the kept plan raises the empty-aggregate error. The keyed check belongs to the row checks
  (`AddRowChecks`; `../../framework/todo.md` § H-04).
- `per g:` in front of a clause does not group its reducers (D6): `per g: sum(x) <= K` bounds the whole total once
  per class. Only a `by` key groups.
- Nothing is computed on a row a `when` drops (syntax reference §6.3).

Needs: `../../framework/todo.md` § H-04 and § H-05.

Tests: `test_direct_source_values.py` marks `s1_limit_per_group` and `s1_limit_per_key_is_null` `xfail(strict=True)`;
remove the marks. The grouped cases of `test_direct_s1_uniform_matroid.py` run on the solver path alone
(`direct_path`); run them on both paths, and give the decline-grid and differential cases that now only assert "`by` declines" their own grouped branch again. The direct-solve benchmark's `s1.sql`
(`benchmark/decide/direct_solve/queries/`) fails in `direct` mode; run it again, and once R1 reads it too, drop the
sentence about it in `../../../performance/benchmarking/direct_solve.md`.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
