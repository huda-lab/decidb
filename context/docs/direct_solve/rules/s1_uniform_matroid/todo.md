# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.

## S1-28 — S1 builds its projections with the shared stage

- **Problem.** S1 lays out its score projection's slots in its constructor and appends at each (`append_at`), a third
  way to build a projection beside `DirectProjectionStage` and R1's name map (`../r1_continuous_knapsack/todo.md`
  § R1-20).
  **Ruling (owner, 2026-11-11).** Every rule builds its projections with `DirectProjectionStage` as it is, keeping
  the bindings in structs (`DirectCarried`), as A1, A2, N1, O1 and S3 do; the stage gains no named columns.
- **Change.** S1 builds its projections with `DirectProjectionStage`; the slot layout and `append_at` go. The score
  projection lays out a group bound's inputs with `DirectLayoutGroupInputs`, as S1's first read does. Behavior stays as
  it is.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s1"`.
