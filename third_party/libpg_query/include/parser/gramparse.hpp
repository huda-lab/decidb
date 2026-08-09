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
	 * returns the DECIDE token; cleared by the decide_clause grammar action.
	 * While set, base_yylex() rewrites WHEN -> WHEN_DECIDE so the DECIDE WHEN
	 * is a distinct token that can't collide with CASE ... WHEN ... or pollute
	 * the global expression grammar.
	 */
	bool in_decide_clause;

	/*
	 * DecidB: CASE...END nesting depth while inside a DECIDE clause. WHEN is
	 * only rewritten to WHEN_DECIDE at depth 0; a WHEN belonging to a CASE
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

/* from parser.c */
int base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, core_yyscan_t yyscanner);

/* from gram.y */
void parser_init(base_yy_extra_type *yyext);
int base_yyparse(core_yyscan_t yyscanner);

}
