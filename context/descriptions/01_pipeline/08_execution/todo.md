# Stage 08 — Execution: open work

- The general path (a term reading rows outside its instance: `BY` keyed differently
  from `PER`, a reducer beside a per-row term, a frame) supports SUM/AVG of linear terms,
  and an easy MIN/MAX while each row is its own instance; other MIN/MAX, `<>`, ABS,
  quadratic and bilinear bodies are refused there.
- `AVG` range frames need their denominator taken over the navigated rows.
- An `IF` guard on an easy MIN/MAX compared with a bound-side reducer (`PER k IF b:
  MAX(x) BY (k) <= MIN(cap) BY (k)`) reaches the general-path refusal: its instances
  are keyed, and the general path reads a direct term at one representative row.
