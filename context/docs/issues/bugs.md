# Known bugs

## BUG-21 — A number DeciDB derives from the user's values says COEFFICIENT

- **Observed:** with b = 1e308, a merged coefficient raises COEFFICIENT quoting text the user never wrote: like terms
  merged in SQL (`DecideMergeLikeCoefficients`, `decide_term_split.cpp:~40`; `b*y + b*y <= b` says `b + b`), the S1 and
  A1 like-term sums (`s1_uniform_matroid_rule.cpp:~797`, `a1_separable_box_rule.cpp:~685`) and a reciprocal scale
  (`physical_decide.cpp:~2610`, `SUM(y)/(SELECT 1e-310)`). Every input is finite; DeciDB's own arithmetic overflowed.
  `MAXIMIZE SUM(2e308*y)` says COEFFICIENT "1e1000" under direct auto and require, TOO_LARGE with direct solve off:
  `DirectCoefficientText` returns text only, so the direct objective, score and value-text sites cannot pick the role.
- **Fix (owner, 2026-10-29):** classify on the failure path only. A derived coefficient's check stays one `isfinite`
  on its result; a row where it fails reads the inputs, and the first non-finite one a column holds raises COEFFICIENT
  quoting that input, else TOO_LARGE naming the clause. One helper builds the check for every derivation site. The
  direct text helper returns the role with the text.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
