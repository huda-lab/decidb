# Stage 01 — Parser: open work

Open work in the DECIDE grammar and the text it keeps. Finished work is in [done.md](done.md). How to work and the
checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these
todo files.

## PARSE-2 — A removed spelling is an ordinary syntax error

- **Problem.** `decide_removed_spellings.cpp` (909 lines) rebuilds what the old grammar would have parsed, to print
  the new spelling from the user's text. It keeps a second token (`WHEN_DECIDE_OBJECTIVE`) and lexer state, error-only
  grammar productions, the parser markers `POSTFIX_WHEN_TAG` and `POSTFIX_PER_TAG`, a walk over every `SelectNode`,
  and tests (`test_scope_removed_syntax.py`, `_old_spellings.py` and its readers).
- **Change.** By the owner's ruling (2026-11-18, `context/opa_rewrite/decisions.md` R13): the grammar has only the
  current syntax, and an old spelling fails as any other syntax error does. The rewriter, the token, the lexer state,
  the error-only productions, the markers and the tests that reach only them go. `language/errors.md` § 3 goes; the
  guide keeps no list of old forms. Needs `make grammar-build`.
- **Done when.** `grep -rn "RemovedDecideSpellings\|WHEN_DECIDE_OBJECTIVE\|POSTFIX_WHEN_TAG" src third_party` finds
  nothing; `./test/decide/run_tests.sh -n 2 -k "parser or spelling or scope"` passes.

## PARSE-3 — Parse-time messages state the kind and where

- **Problem.** The parse-time rows of `language/errors.md` §1 tell the user what to write (`write per a, b: without
  parentheses`, `add SUCH THAT with at least one constraint`).
- **Change.** By the owner's rulings (2026-11-18, `context/opa_rewrite/decisions.md` R12, R19): each says what is wrong
  and points at it, and stops. `errors.md` quotes the new text.
- **Needs.** PARSE-2; TEST-15 (the tests the rewording breaks move to its helper).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "parser"` passes.
