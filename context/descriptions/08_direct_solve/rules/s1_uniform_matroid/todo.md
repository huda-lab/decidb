# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## Read a `by` key again (counts per group)

Before the Sami-Active merge S1 read `SUM(x) <= K PER g`: one count interval per group of one or more source columns.
The merge moved the grouping onto the reducer (`sum(x) by (g) <= K`, or one clause per class with
`per g: sum(x) by (g) <= K`), and S1 declines both. The per-group code is kept and reached only through IR edits
(`PerKeyOnConstraint` in `test/common/test_decidb_direct_decline.cpp`); `DirectIRScope::per_key_slots` and
`DirectFrontEnd::SourceSlot` are kept with nothing filling them (`direct_ir.cpp`, `ReadReducer` declines on
`TryParseGroupKeyTag`).

What the old reading did, as the spec to restore:

- A count bound per group: a constant, or a source value read on each row of the group. The bound-side window rewrite
  now reduces a data bound per group and raises an `=` bound that disagrees, so S1 reads one value per group.
- A bound read from the group: `COUNT(*)`, `COUNT`, `MIN`, `MAX`, `SUM`, `AVG` over the group's rows, alone or in
  arithmetic (`SUM(x) <= MIN(cap) PER warehouse`; the top fraction of each group, `SUM(x) <= COUNT(*) * 0.1 PER g`).
  `COUNT(*)` counted the rows the clause covers in each group: a clause-level `when` narrowed it, a `when` inside the
  left-hand sum did not.
- The grouped rows of the Performance table in `done.md` measure this reading (up to 98x over Gurobi at 100 groups).

What changed in the language, and must change in the reading:

- A NULL key is its own group (D5). The old reading let a NULL-key row bypass every bound (it still obeyed its fixed
  value), and read 0 for it, so a NULL-key group could not raise.
- A group the reducer's own `when` leaves empty is an error that quotes the group (D8: `sum(when c: x) has no rows for
  g = 'c'.`), where the old reading raised the empty-aggregate error. The keyed check belongs to the row checks
  (`AddRowChecks`; `../../framework/todo.md` § H-04).
- `per g:` in front of a clause does not group its reducers (D6): `per g: sum(x) <= K` bounds the whole total once
  per class. Only a `by` key groups.
- Nothing is computed on a row a `when` drops (syntax reference §6.3).

Needs: `../../framework/todo.md` § H-04 and § H-05.

Tests: `test_direct_source_values.py` marks `s1_limit_per_group` and `s1_limit_per_key_is_null` `xfail(strict=True)`;
remove the marks. The grouped cases of `test_direct_s1_uniform_matroid.py` run on the solver path alone
(`direct_modes`); run them on both paths, and restore the SQL-spelled decline-grid and differential cases that the
merge turned into "`by` declines", each reaching its own grouped branch again. The direct-solve benchmark's `s1.sql`
(`benchmark/decide/direct_solve/queries/`) fails in `direct` mode; run it again, and once R1 reads it too, drop the
sentence about it in `05_performance/benchmarking/direct_solve.md`.

## Read a folded affine bound

The solver folds an affine bound on `x` into its box (`1*x >= 2`, `x >= 1+1`, `2*x >= 1`, D41). The IR marks it
`variable_bound` but not `plain`, and S1 declines it. Read the folded box, or the scaled and offset bound, as A1 must
(`../a1_separable_box/todo.md`).

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
