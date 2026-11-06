# N1 (Euclidean ball) — open work

N1 keeps one sum of squares inside a limit by scaling the direction or the target onto the ball. The class is in
[definition.md](definition.md); what N1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task.

## N1-5 — Guide §10 teaches N1's shape

- **Problem.** The N1 bullet in `language/guide.md` § 10 says "decisions" (N1 takes one), does not say that any other
  bound loses the rule even when it never binds (`x <= 50`), leaves `by` and `per` out of the grouping sentence, does not
  mention a constant factor or `norm(x, 2)`, and does not say a square objective must be a convex `MINIMIZE`. It does not
  name the rule as `EXPLAIN` prints it.
- **Change.** A short example (scores 3 and 4 under `<= 25`), one query that keeps the rule and one that loses it, the
  rule id `N1_EUCLIDEAN_BALL` beside the bullet. One sentence that a direct answer is exact where the solver's carries its
  tolerance (3 and 4 against 3.0005 and 3.9996).
- **Done when.** Every query in the section runs as the text says under `require` (or declines where it says so).

## N1-6 — Benchmark sizes past `sf=1`

- **Problem.** `benchmark/decide/direct_solve/run.py --queries n1 n1_groups` has run up to `sf=1` only.
- **Change.** Run the larger sizes and record them in `done.md` § Performance. The owner starts these runs.
- **Done when.** The table in `done.md` holds the larger sizes with their agreement check.
