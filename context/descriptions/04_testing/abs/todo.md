# ABS Linearization Test Coverage — Todo

## Missing coverage

- Path-B + WHEN (verify the unconditional per-row Big-M is correct under WHEN-filtered aggregates) and Path-B + PER (per-group aggregates over Path-B-pinned auxes). Lower priority — the core Big-M formulations are now oracle-covered in `done.md`; these check the WHEN/PER composition on top.
