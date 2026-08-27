# Golden model dump

A characterization oracle over the model DeciDB *builds*, as opposed to the answer it
returns.

## Why it exists

Two different models routinely share an optimum. A refactor can change which rows are
emitted, how a Big-M is scaled, or how an auxiliary is boxed, and every test in the
pytest suite still passes — because every test asks what the answer was. The suite
cannot see the change at all.

This harness makes that visible. `DECIDB_DUMP_MODEL` writes the exact matrix handed to
the backend — columns with their boxes and integrality, rows with senses and
coefficients, general constraints, indicator constraints — for every query in
`corpus.sql`. Diffing that against a committed baseline turns "this refactor changes no
model" from a claim into a check.

## Layout

| File | What it is |
|---|---|
| `corpus.sql` | 91 queries, each present for its constraint *shape*, not its answer |
| `capture.sh --solver <gurobi\|highs>` | Writes `baseline.<solver>.dump` and `.results` |
| `check.sh` | Diffs today's model against both baselines; the read-only half |
| `baseline.<solver>.dump` | The committed reference, one per backend |
| `baseline.<solver>.dump.results` | Query output, diffed separately |

The fixture is four rows on purpose: shapes carrying a Big-M encoding stay instant.

## One baseline per backend

The backend is part of the answer, not an ambient fact about the host. Gurobi states
ABS, MIN/MAX and `<>` natively while HiGHS lowers all three, so the two build genuinely
different models from identical SQL — 90 against 87, since the three queries over an
open-ended range reach a model only where a construct is stated natively; on HiGHS they
are the refusals recorded in `baseline.highs.dump.results`.

An unqualified `baseline.dump` therefore meant "whatever backend the capturing host
happened to have", which is why the previous one drifted 310 lines out of date without
anyone able to tell drift from signal. `--solver` is required, and `capture.sh`
validates the name itself rather than trusting `DECIDB_FORCE_SOLVER` to complain — an
unrecognized name there falls through to auto-selection by design, which would record
the host default under a name promising otherwise.

## The configuration is pinned too, not just the backend

`DECIDB_NATIVE_CONSTRUCTS` is the A/B switch between stating a construct natively and
lowering it to Big-M. `capture.sh` pins it to `on` — the shipped default — for the same
reason it pins `DECIDB_FORCE_SOLVER`: a baseline records the model DeciDB ships, never
one arm of a comparison.

Left ambient, it made `DECIDB_NATIVE_CONSTRUCTS=force make decide-test` fail before
pytest ever ran. The check captured the forced arm and diffed it against the shipped
baseline, so eleven dumps reported `MODEL CHANGED` — each going `num_genconstrs: 0` to
`1`, gaining the MIN or MAX general constraint and shedding the Big-M rows and binaries
that had encoded it. That is the switch working, not the model moving. HiGHS was
identical throughout, because it declares no native MIN/MAX and the switch is a no-op
there.

The three queries whose *results* also differed are tied optima: `MAX(x) >= 3 ...
MINIMIZE SUM(x)` has four equally good answers and `MAX(SUM(x)) PER grp` has two, so
the two encodings break the tie differently while the objective is unchanged. Pinning
the switch keeps those ties out of the baseline, where they would have frozen an
arbitrary choice.

## Running it

`make decide-test` runs `check.sh` first and **fails on any difference**, before pytest.
`make decide-golden` runs it alone.

A failure is not automatically a bug; it means the built model moved and the move has to
be read. When it is intended, regenerate and commit the baselines with the change that
caused them:

```bash
test/decide/golden/capture.sh --solver gurobi
test/decide/golden/capture.sh --solver highs
```

A backend missing from the host is skipped — which solvers are installed is a fact about
the machine. A baseline that was never captured is a failure, not a skip: passing
silently on a missing reference is how the last one went stale.

## What the corpus covers

Per-row and aggregate constraint shapes, canonicalization spellings (side-swapped,
leading-negative, scaled reducers), `WHEN` and `PER`, subquery right-hand sides, ABS,
MIN/MAX in all four hard forms, `<>` including both range collapses, and — queries 86-88
— auxiliary boxes over a range open on one side, on both the constraint and the
objective path. That last group was added with the half-open box: before it, every
MIN/MAX query in the corpus carried a finite bound, so a model that discarded a
derivable bound diffed clean.
