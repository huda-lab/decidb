# Known bugs

## BUG-24 — Direct solve sums an objective constant the solver path drops

- **Observed:** `MAXIMIZE SUM(y + b + b)` over two rows with b = 1e308, `y` in [0, 5]: direct solve off returns y = 5;
  `require` and `auto` raise "MAXIMIZE sum(y + b + b) computes a number too large for a DOUBLE". The per-row constant
  cannot change the answer; the solver path never sums it, direct solve does.
- **Fix (owner's delegate, to confirm):** direct solve computes no constant that no answer reads, as the solver path
  does; the overflow rule then has nothing to refuse. Locate where the A1 constant sum is built and what reads it.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
