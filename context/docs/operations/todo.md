# Operations — open work

Upkeep of the repository as a whole: releases, packaging and the docs across areas.

## BLOAT-17 — Bloat in the direct-solve docs

- **Problem.** The docs in `context/docs/direct_solve/` (about 5,043 lines) have not been read for bloat since the code
  they describe grew.
- **Change.** Done by the orchestrator, since lanes do not edit docs. Remove what `.claude/CLAUDE.md` and the
  owner's doc rules call bloat: history and narrative, a fact written in two places (one home stays, the other links
  to it), a description of code that no longer exists, and prose that restates a table or the code. Consolidate;
  add nothing.
- **Done when.** Every fact the files held before is still in one place or is no longer true of the code, every
  internal link resolves, `git diff --check` passes, and the report gives the lines before and after.

## BLOAT-18 — Bloat in the pipeline docs

- **Problem.** The docs in `context/docs/pipeline/` (about 6,930 lines) have not been read for bloat since the code they
  describe grew.
- **Change.** Done by the orchestrator, since lanes do not edit docs. Remove what `.claude/CLAUDE.md` and the
  owner's doc rules call bloat: history and narrative, a fact written in two places (one home stays, the other links
  to it), a description of code that no longer exists, and prose that restates a table or the code. Consolidate;
  add nothing.
- **Done when.** Every fact the files held before is still in one place or is no longer true of the code, every
  internal link resolves, `git diff --check` passes, and the report gives the lines before and after.

## BLOAT-19 — Bloat in the other docs

- **Problem.** The docs in `context/docs/diagnostics/`, `language/`, `testing/`, `performance/`, `operations/` and
  `issues/` (about 8,564 lines) have not been read for bloat since the code they describe grew.
- **Change.** Done by the orchestrator, since lanes do not edit docs. Remove what `.claude/CLAUDE.md` and the
  owner's doc rules call bloat: history and narrative, a fact written in two places (one home stays, the other links
  to it), a description of code that no longer exists, and prose that restates a table or the code. Consolidate;
  add nothing.
- **Done when.** Every fact the files held before is still in one place or is no longer true of the code, every
  internal link resolves, `git diff --check` passes, and the report gives the lines before and after.
