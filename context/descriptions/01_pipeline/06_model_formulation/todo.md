# Stage 06 — Model formulation: open work

## 1. Inlining a helper per instance, not per helper

**Why.** `InlineReducerValueLinks` (D83, `done.md` §9, "Inlining a helper") writes a helper
into its readers as a whole, because its instances share one variable whose columns
`VarIndexer` lays out once. So a MIN or MAX helper with one group of one member among
groups of several keeps a column for that group, defined by an equality (deck p. 112
replaces `c2` by `x3` outright), and a SUM helper whose large groups would grow the model
keeps the columns of its small groups too.

**What it needs.** A variable whose instances are laid out sparsely: `VarIndexer` giving an
instance no column, `Get` resolving a row to a column or to the instance's members, and
the builder's expansion (`AddInlinedHelperColumns`) reading per instance. The cost rule
then applies instance by instance.

**Test.** F1_max's model (`test_anr_nested.py`) loses c2's column and equality: nine
columns, seven rows.

## 2. A bound on a helper alone keeps its column

**Why.** A clause that bounds a helper alone (`sum(from previous to previous else 0: x)
over (t.p) <= 5`) is folded into the helper's column box by bound absorption, so the
linear form leaves the helper a column (`MarkInlinableHelpers`): the box would be lost with
it. The model then has a column and a definition per window where an inlined window would
have its bound row alone.

**What it needs.** The census reading an absorbed bound as a reader that can take the
members -- the bound restated as a row over them, per instance, with the clause's
provenance (its repair rows already come from the clause, `08_execution/done.md`) -- or
bound absorption leaving such a clause as rows when its helper could otherwise be inlined.

**Test.** `test_anr_inlining.py::TestKept`'s `a-bound-on-the-helper` case builds six
columns and six rows.
