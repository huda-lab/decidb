# SQL Functions & Expressions — Planned Features

- Range frames reducing with `MIN`, `MAX` or `AVG` (`MIN(FROM 3 PREVIOUS TO PREVIOUS: x)
  OVER (...)`). Today a range frame reduces with `SUM` only and `AT` reads one position;
  the extremum rewrites assume group semantics and the `AVG` denominator is taken over
  the group rather than the navigated rows.
