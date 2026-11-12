# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan near the floor that `done.md`
§ Performance measures, with the answers and errors R1 gives today.

## R1-20 — R1 builds its projections with the shared stage

- **Problem.** R1's first stages forward every column through every projection and find each by name in a
  `std::map<string, Column>` (`Project`), so a wrong name shows only at run time (`columns.at`). It is a third way to
  build a projection, beside `DirectProjectionStage` and S1's slot layout (`../s1_uniform_matroid/todo.md` § S1-28).
  **Ruling (owner, 2026-11-11).** Every rule builds its projections with `DirectProjectionStage` as it is, keeping
  the bindings in structs (`DirectCarried`), as A1, A2, N1, O1 and S3 do; the stage gains no named columns.
- **Change.** R1 builds those projections with `DirectProjectionStage`; `Project` and the name map go. Behavior stays
  as it is.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_r1"`.
