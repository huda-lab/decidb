/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *		Shared definitions for the "raw" parser (flex and bison phases only)
 *
 * NOTE: this file is only meant to be included in the core parsing files,
 * ie, parser.c, gram.y, scan.l, and src/common/keywords.c.
 * Definitions that are needed outside the core parser should be in parser.h.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development PGGroup
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/gramparse.h
 *
 *-------------------------------------------------------------------------
 */

#pragma once

#include "nodes/parsenodes.hpp"
#include "parser/scanner.hpp"

namespace duckdb_libpgquery {
#include "parser/gram.hpp"

/*
 * The YY_EXTRA data that a flex scanner allows us to pass around.  Private
 * state needed for raw parsing/lexing goes here.
 */
/*
 * DecidB: the lexer state a DECIDE clause owns. A DECIDE query may appear as a
 * subquery inside an outer DECIDE clause; the inner clause must restore this
 * state on its way out rather than clear it, or the outer clause's remaining
 * WHENs lex as ordinary SQL WHEN and fail to parse.
 */
typedef struct PGDecideLexState {
	bool in_decide_clause;
	bool in_decide_objective;
	int decide_case_depth;
	bool decide_declared_before_from;
	int decide_subquery_depth;
	bool decide_in_body;
} PGDecideLexState;

/* Nesting deeper than this keeps parsing; only the innermost levels' state is
 * restored exactly, which no realistic query reaches. */
#define PG_DECIDE_STATE_STACK_MAX 16

typedef struct base_yy_extra_type {
	/*
	 * Fields used by the core scanner.
	 */
	core_yy_extra_type core_yy_extra;

	/*
	 * State variables for base_yylex().
	 */
	bool have_lookahead;           /* is lookahead info valid? */
	int lookahead_token;           /* one-token lookahead */
	core_YYSTYPE lookahead_yylval; /* yylval for lookahead token */
	YYLTYPE lookahead_yylloc;      /* yylloc for lookahead token */
	char *lookahead_end;           /* end of current token */
	char lookahead_hold_char;      /* to be put back at *lookahead_end */

	/*
	 * DecidB: true while lexing inside a DECIDE clause. Set when base_yylex()
	 * returns the DECIDE or SUCH token; restored to the enclosing clause's value
	 * by the decide_clause / decide_declaration / decide_tail grammar actions.
	 * While set, base_yylex() rewrites depth-0 WHEN to a DECIDE-specific token
	 * so it can't collide with CASE ... WHEN ... or pollute the global
	 * expression grammar.
	 */
	bool in_decide_clause;

	/* DecidB: true after MAXIMIZE/MINIMIZE in a DECIDE clause. Objective WHEN
	 * gets a distinct token because its condition cannot steal a trailing
	 * constraint bound. */
	bool in_decide_objective;

	/*
	 * DecidB: CASE...END nesting depth while inside a DECIDE clause. WHEN is
	 * only rewritten to a DECIDE token at depth 0; a WHEN belonging to a CASE
	 * inside a DECIDE expression must stay a normal WHEN so the CASE parses
	 * (and is then rejected by the binder with a friendly error).
	 */
	int decide_case_depth;

	/*
	 * DecidB: true once a pre-FROM DECIDE declaration (decide_declaration)
	 * has been consumed. Lets decide_clause's action recognize a second
	 * DECIDE reached through decide_body and report "DECIDE appears twice"
	 * directly, instead of whatever error its own opt_decide_tail
	 * production would otherwise raise.
	 */
	bool decide_declared_before_from;

	/*
	 * DecidB: true when the previous token was ')'. Inside a DECIDE clause that
	 * is what tells a reducer's BY -- sum(x) BY (g) -- from GROUP BY, ORDER BY
	 * and PARTITION BY, whose BY always follows a keyword.
	 */
	bool decide_after_rparen;

	/*
	 * DeciDB: what a frame selector's tokens need (at(...) over (... within K)).
	 * `decide_paren_depth` counts open parentheses over the whole statement. An
	 * ordinary SQL subquery written in a clause opens at `decide_subquery_depth`
	 * (-1 for none) and lasts until the parenthesis that holds it closes; inside it
	 * AT, OVER and WITHIN stay SQL's, so a window function there still parses. A
	 * subquery opens at a SELECT, or at a WITH, VALUES, TABLE, PIVOT or UNPIVOT
	 * directly after '(', or at a FROM there unless a name comes before the '(' --
	 * `sum(from first ...)` is a range frame, `(FROM t ...)` a query.
	 * `decide_over_parens` has bit d set while the parenthesis at depth d (1..63)
	 * is a frame's OVER_DECIDE parenthesis, where WITHIN is the frame's; a nested
	 * one keeps the outer's bit. `decide_after_over` marks the token after
	 * OVER_DECIDE; `decide_last_token` and `decide_token_before_last` are the raw
	 * tokens before the current one. `decide_in_body` is false in a declaration list
	 * and true from SUCH THAT on: the frame words are the body's, so a decision may
	 * still be called `at`.
	 */
	int decide_paren_depth;
	int decide_subquery_depth;
	uint64_t decide_over_parens;
	bool decide_after_over;
	int decide_last_token;
	int decide_token_before_last;
	bool decide_in_body;

	/*
	 * DecidB: saved copies of the four clause fields above, one per DECIDE clause
	 * currently open. Pushed by base_yylex() when it arms a clause on DECIDE
	 * or SUCH; popped by the decide_clause, decide_declaration and decide_tail
	 * grammar actions, which is what makes a nested DECIDE restore its parent
	 * instead of disarming it.
	 */
	PGDecideLexState decide_state_stack[PG_DECIDE_STATE_STACK_MAX];
	int decide_state_depth;

	/*
	 * State variables that belong to the grammar.
	 */
	PGList *parsetree; /* final parse result is delivered here */
} base_yy_extra_type;

/*
 * In principle we should use yyget_extra() to fetch the yyextra field
 * from a yyscanner struct.  However, flex always puts that field first,
 * and this is sufficiently performance-critical to make it seem worth
 * cheating a bit to use an inline macro.
 */
#define pg_yyget_extra(yyscanner) (*((base_yy_extra_type **)(yyscanner)))

/* DecidB: save the enclosing DECIDE clause's lexer state before arming a new one. */
static inline void PGDecidePushLexState(base_yy_extra_type *yyextra) {
	if (yyextra->decide_state_depth >= 0 && yyextra->decide_state_depth < PG_DECIDE_STATE_STACK_MAX) {
		PGDecideLexState *slot = &yyextra->decide_state_stack[yyextra->decide_state_depth];
		slot->in_decide_clause = yyextra->in_decide_clause;
		slot->in_decide_objective = yyextra->in_decide_objective;
		slot->decide_case_depth = yyextra->decide_case_depth;
		slot->decide_declared_before_from = yyextra->decide_declared_before_from;
		slot->decide_subquery_depth = yyextra->decide_subquery_depth;
		slot->decide_in_body = yyextra->decide_in_body;
	}
	yyextra->decide_state_depth++;
	/* A clause nested in a subquery lexes its own frames. */
	yyextra->decide_subquery_depth = -1;
}

/* DecidB: restore the enclosing DECIDE clause's lexer state as this one closes.
 * With nothing pushed this leaves the clause disarmed, matching the plain
 * assignment these call sites used before nesting was supported. */
static inline void PGDecidePopLexState(base_yy_extra_type *yyextra) {
	if (yyextra->decide_state_depth <= 0) {
		yyextra->decide_state_depth = 0;
		yyextra->in_decide_clause = false;
		yyextra->in_decide_objective = false;
		yyextra->decide_case_depth = 0;
		yyextra->decide_subquery_depth = -1;
		yyextra->decide_in_body = false;
		return;
	}
	yyextra->decide_state_depth--;
	if (yyextra->decide_state_depth < PG_DECIDE_STATE_STACK_MAX) {
		PGDecideLexState *slot = &yyextra->decide_state_stack[yyextra->decide_state_depth];
		yyextra->in_decide_clause = slot->in_decide_clause;
		yyextra->in_decide_objective = slot->in_decide_objective;
		yyextra->decide_case_depth = slot->decide_case_depth;
		yyextra->decide_declared_before_from = slot->decide_declared_before_from;
		yyextra->decide_subquery_depth = slot->decide_subquery_depth;
		yyextra->decide_in_body = slot->decide_in_body;
	}
}

/* from parser.c */
int base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, core_yyscan_t yyscanner);

/* from gram.y */
void parser_init(base_yy_extra_type *yyext);
int base_yyparse(core_yyscan_t yyscanner);

}
