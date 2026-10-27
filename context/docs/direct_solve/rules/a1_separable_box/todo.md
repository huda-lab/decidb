# A1 (separable box-constrained problem) — open work

What A1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task.

## Read a folded affine bound

The solver folds an affine constant bound on one decision into its box (`2*x >= -6`, `x + 1 >= -3`, a constant-folding
right side, an easy `min(x) >= K`; D41), and it replaces the default lower bound of 0. The IR marks such a clause
`variable_bound` but not `plain` (`DecideIsSingletonRowBound`), and A1 declines it. Read the scaled and offset box, or
read the box from the folding itself (`decide_singleton_row_folding.cpp`), so the rounding of a quotient is the
solver's.

Tests: `FOLDED_AFFINE_PENDING` in `test_direct_a1_separable_box.py` runs those cases on the solver path alone, and
`int_upper_quotient_just_below_a_whole_number` is `xfail(strict=True)`. Run them on both paths and remove the mark. S1
and R1 wait on the same reading (`../s1_uniform_matroid/todo.md`). The one-classifier task in
`../../../issues/code_quality.md` § "A bound on one decision" is classified three times covers this reading.
