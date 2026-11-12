# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.

## S1-26 — S1 builds its plan with the ranked-prefix builder

- **Problem.** S1 and S3 build one plan shape twice (`../../framework/todo.md` § FRM-18). S1 computes a group bound's
  aggregates on a second read of the source, with a hash aggregate joined to every row (`BuildGroupBounds`,
  `DirectJoinClassAnswers`); S3 computes a group's limit in the window that ranks the rows, over one read
  (`DirectWindowSink`; S3's W1). **Ruling (owner, 2026-11-12).** S1 reads its group limits as S3 does, in the rank
  window over one read; the builder reads group limits one way for both rules.
- **Change.** S1 ranks, takes its prefix, reads its group limits and checks its rows with the builder of FRM-18; S1's
  own window, filter and group-bound plan (`BuildGroupBounds`, the second read and the join) go. S1's answers, errors
  and goldens stay; its plan cost, measured against Gurobi on S1's benchmark cases, stays within the noise.
- **Needs.** FRM-18.
- **Done when.** `build/release/test/unittest "[decidb]"`;
  `./test/decide/run_tests.sh -n 2 -k "direct_s1 or rule_conformance"`.

## S1-28 — S1 builds its projections with the shared stage

- **Problem.** S1 lays out its score projection's slots in its constructor and appends at each (`append_at`), a third
  way to build a projection beside `DirectProjectionStage` and R1's name map (`../r1_continuous_knapsack/todo.md`
  § R1-20).
  **Ruling (owner, 2026-11-11).** Every rule builds its projections with `DirectProjectionStage` as it is, keeping
  the bindings in structs (`DirectCarried`), as A1, A2, N1, O1 and S3 do; the stage gains no named columns.
- **Change.** S1 builds its projections with `DirectProjectionStage`; the slot layout and `append_at` go. Behavior
  stays as it is.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s1"`.
