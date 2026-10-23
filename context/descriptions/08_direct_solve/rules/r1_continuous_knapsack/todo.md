# R1 — Continuous (fractional) knapsack — open work

R1 splits one shared budget among `REAL` decisions by filling the best value per unit of budget first. The class is in
[definition.md](definition.md); what R1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task. The goal is a plan near the floor that `done.md`
§ Performance measures, with the answers and errors R1 gives today.

## R1-30 — Bring two decisions within twice one decision

- **Problem.** `done.md` § Performance: `two_decisions` at 5,000,000 rows runs 2.52 s against 0.60 s for one decision
  (4.2 times; 21.2 s of CPU against 4.9 s), where the target was 2 times. Every one of the four reads computes both
  decisions' item values, though stages 2 to 4 need only some of them, and the sort holds 10,000,000 items.
- **Change.** Profile `two_decisions` with `benchmark/decide/profile_r1_plan.py` (the profiler is honest now) and cut
  the work each read does to the columns its stage reads. Answers and errors stay the same.
- **Done when.** The five checks pass; `two_decisions` at 5,000,000 rows runs within 2 times `le_positive`.
