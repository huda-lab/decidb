"""Find the removed DECIDE spellings in SQL text (syntax_reference.md §10.3): postfix
`WHEN` / `PER`, `T.x(TYPE)` and `scalar x(TYPE)` declarations, and `D.x` references to a
decision. Token-aware, so the scope spellings (`when c:`, `per K:`, `by (...)`) and ordinary
SQL (`CASE WHEN`, a relation's column) are not flagged.

`scan_file` reads a Python test (its string literals and f-strings, not docstrings,
assertion messages or regex patterns), a C++ test (its string literals), or a SQL / sqllogic
/ shell file (split at `;`). A deliberate case of the removed syntax is skipped: a sqllogic
block whose comment or text carries "removed spelling" or whose `statement error` expects
a syntax error, and a C++ literal whose line or the two lines above it carries "removed
spelling".
"""

from __future__ import annotations

import ast
import pathlib
import re

TYPES = {"INT", "INTEGER", "BOOL", "BOOLEAN", "REAL"}
# tokens after which a `when` starts a scope (new spelling) rather than trailing a clause (old)
SCOPE_START = {"THAT", "AND", "MAXIMIZE", "MINIMIZE", "(", ":"}
TOK = re.compile(r"""\s+|--[^\n]*|/\*.*?\*/|'(?:[^']|'')*'|"(?:[^"]|"")*"|\{[^{}]*\}|[A-Za-z_][A-Za-z_0-9]*|\d+(?:\.\d+)?|<=|>=|<>|!=|::|[^\s]""", re.S)

def tokens(sql):
    out = []
    for m in TOK.finditer(sql):
        t = m.group(0)
        if t.isspace() or t.startswith("--") or t.startswith("/*"):
            continue
        if t.startswith("{"):
            t = "_placeholder_"          # f-string / str.format slot: a name
        out.append(t)
    return out

def is_name(t):
    return bool(re.match(r'[A-Za-z_"]', t))

STOP = {"MAXIMIZE", "MINIMIZE", "ORDER", "LIMIT", "SUCH", ";"}
def reaches_colon(up, q):
    """True when a ':' at this depth comes before the clause (or the next clause) ends."""
    depth = 0
    n = len(up)
    while q < n:
        t = up[q]
        if t == "(":
            depth += 1
        elif t == ")":
            if depth == 0:
                return False
            depth -= 1
        elif depth == 0:
            if t == ":":
                return True
            if t in STOP:
                return False
            if t == "AND" and q + 1 < n and up[q + 1] in ("WHEN", "PER"):
                return False          # the next clause starts with its own scope
        q += 1
    return False

def scan_sql(sql):
    toks = tokens(sql)
    up = [t if t.startswith(("'", '"')) else t.upper() for t in toks]
    n = len(up)
    kinds = set()
    i = 0
    while i < n:
        if up[i] != "DECIDE":
            i += 1
            continue
        # ---- one DECIDE clause: its nesting depth is that of the DECIDE keyword
        depth = 0
        j = i + 1
        names = set()
        # decide list: until SUCH THAT / FROM (split order) at depth 0
        in_key = False
        k = j
        while k < n and not (depth == 0 and up[k] in ("SUCH", "FROM", "MAXIMIZE", "MINIMIZE")):
            t = up[k]
            if t == "(":
                depth += 1
            elif t == ")":
                depth -= 1
                if depth < 0:
                    break
            elif depth == 0:
                prev = up[k - 1]
                if t == "PER":
                    in_key = True
                elif t == ":" and in_key:
                    in_key = False
                elif t == "SCALAR" and prev in ("DECIDE", ",") and k + 1 < n and is_name(up[k + 1]):
                    kinds.add("scalar decl")
                elif (not in_key and prev in ("DECIDE", ",", "SCALAR") and is_name(t) and k + 2 < n
                      and up[k + 1] == "." and is_name(up[k + 2])):
                    kinds.add("T.x decl")
                if not in_key and is_name(t) and k + 2 < n and up[k + 1] == "(" and up[k + 2] in TYPES:
                    names.add(t)
            k += 1
        if k >= n or up[k] != "SUCH":
            # split order: DECIDE list ... FROM ... SUCH THAT; find SUCH at depth 0
            depth = 0
            while k < n and not (depth == 0 and up[k] == "SUCH"):
                if up[k] == "(":
                    depth += 1
                elif up[k] == ")":
                    depth -= 1
                    if depth < 0:
                        break
                k += 1
        # ---- SUCH THAT ... objective, until the clause's own depth closes or the statement ends
        depth = 0
        case_depth = []
        m = k
        while m < n:
            t = up[m]
            if t == "(":
                depth += 1
            elif t == ")":
                depth -= 1
                if depth < 0:
                    break
            elif t == "SELECT" and up[m - 1] == "(":
                # an ordinary subquery inside the clause: skip to its closing paren
                d = 1
                m += 1
                while m < n and d:
                    if up[m] == "(":
                        d += 1
                    elif up[m] == ")":
                        d -= 1
                    m += 1
                depth -= 1
                continue
            elif t == "CASE":
                case_depth.append(depth)
            elif t == "END" and case_depth and case_depth[-1] == depth:
                case_depth.pop()
            elif case_depth:
                pass
            elif t == "WHEN":
                # new: opens a clause, an objective or a reducer argument; anything else
                # trails an expression (old), unless it is a misplaced scope that still
                # reaches its ':' (a syntax-error case, not an old spelling)
                if up[m - 1] not in SCOPE_START and not reaches_colon(up, m + 1):
                    kinds.add("postfix WHEN")
            elif t == "PER":
                if not reaches_colon(up, m + 1):
                    kinds.add("postfix PER")
            elif t == "." and m >= 1 and m + 1 < n and up[m + 1] in names and is_name(up[m - 1]):
                kinds.add("D.x ref")
            m += 1
        i = m if m > i else i + 1
    return kinds

DECL_RX = re.compile(r"^\s*(per\s[^:]*:\s*)?(scalar\s+)?[A-Za-z_\"][\w\"]*(\.[A-Za-z_\"][\w\"]*)?\s*\(\s*(INT|BOOL|REAL|INTEGER|BOOLEAN)\s*\)", re.I)
BACKTICK = re.compile("`")
CLAUSE_RX = re.compile(r"(<=|>=|<>|!=|[^-<>=!]=|<|>)|^\s*(MAXIMIZE|MINIMIZE)\b", re.I)
def scan_fragment(text):
    """A clause or declaration fragment spliced into a template (no DECIDE keyword)."""
    text = BACKTICK.sub(" ", text)
    if re.search(r"\bDECIDE\b", text, re.I) or len(text) > 2000:
        return set()
    if DECL_RX.search(text):
        return scan_sql("DECIDE " + text + " SUCH THAT x <= 1")
    if CLAUSE_RX.search(text) and re.search(r"\b(PER|WHEN)\b", text, re.I):
        if re.match(r"\s*(MAXIMIZE|MINIMIZE)\b", text, re.I):
            return scan_sql("DECIDE x(INT) SUCH THAT x <= 1 " + text)
        return scan_sql("DECIDE x(INT) SUCH THAT " + text)
    return set()

def looks_sql(text):
    return bool(re.search(r"\bDECIDE\b", text, re.I)) and bool(re.search(r"\bSUCH\s+THAT\b|\(\s*(INT|BOOL|REAL|INTEGER|BOOLEAN)\s*\)", text, re.I))

def py_strings(path):
    src = path.read_text()
    tree = ast.parse(src)
    docs = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Module)):
            b = getattr(node, "body", [])
            if b and isinstance(b[0], ast.Expr) and isinstance(b[0].value, ast.Constant):
                docs.add(id(b[0].value))
    spans = [(nd.lineno, nd.end_lineno, nd.name) for nd in ast.walk(tree) if isinstance(nd, (ast.FunctionDef, ast.AsyncFunctionDef))]
    def enc(l):
        best = None
        for a, b, nm in spans:
            if a <= l <= b and (best is None or a > best[0]):
                best = (a, b, nm)
        return best
    inner = set()
    prose = set()   # assertion messages and error-match patterns: never SQL
    for nd in ast.walk(tree):
        if isinstance(nd, ast.Assert) and nd.msg is not None:
            for sub in ast.walk(nd.msg):
                prose.add(id(sub))
        if isinstance(nd, ast.keyword) and nd.arg in ("match", "reason", "msg"):
            for sub in ast.walk(nd.value):
                prose.add(id(sub))
        if isinstance(nd, ast.Call) and getattr(nd.func, "attr", getattr(nd.func, "id", "")) in ("fail", "skip", "xfail", "approx", "compile", "search", "match", "fullmatch"):
            for sub in ast.walk(nd):
                prose.add(id(sub))
    for nd in ast.walk(tree):
        if isinstance(nd, ast.JoinedStr):
            for v in nd.values:
                inner.add(id(v))
    for nd in ast.walk(tree):
        if id(nd) in prose:
            continue
        if isinstance(nd, ast.Constant) and isinstance(nd.value, str) and id(nd) not in docs and id(nd) not in inner:
            yield nd.lineno, nd.value, enc(nd.lineno)
        elif isinstance(nd, ast.JoinedStr):
            txt = "".join(v.value if isinstance(v, ast.Constant) else "{x}" for v in nd.values)
            yield nd.lineno, txt, enc(nd.lineno)

def c_strings(text):
    # concatenate adjacent C string literals into statements
    for m in re.finditer(r'((?:"(?:[^"\\]|\\.)*"\s*)+)', text):
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
        s = "".join(parts).replace('\\"', '"')
        yield text.count("\n", 0, m.start()) + 1, s


def sqllogic_statements(text):
    """The SQL of each `statement` / `query` block of a sqllogic file that is not a
    deliberate case: one whose comment or text says "removed spelling", or a `statement
    error` that expects a syntax error (a removed spelling after a scope is one)."""
    lines = text.split("\n")
    i = 0
    comment = []
    while i < len(lines):
        stripped = lines[i].strip()
        if stripped.startswith("#"):
            comment.append(stripped)
            i += 1
            continue
        if not (stripped.startswith("statement") or stripped.startswith("query")):
            comment = []
            i += 1
            continue
        directive = stripped
        start = i + 1
        sql, expected, in_expected = [], [], False
        i += 1
        while i < len(lines) and lines[i].strip():
            if lines[i].strip() == "----":
                in_expected = True
            elif in_expected:
                expected.append(lines[i])
            else:
                sql.append(lines[i])
            i += 1
        block = "\n".join(comment + sql + expected)
        comment = []
        if "removed spelling" in block:
            continue
        if directive.startswith("statement error") and any("syntax error" in e for e in expected):
            continue
        statement = "\n".join(sql)
        if looks_sql(statement):
            yield start + 1, statement


def scan_file(path: pathlib.Path) -> list[tuple[int, str, str]]:
    """Every old spelling in ``path``: (line, kinds, snippet)."""
    hits = []
    if path.suffix == ".py":
        for line, text, _ in py_strings(path):
            kinds = scan_sql(text) if looks_sql(text) else scan_fragment(text)
            if kinds:
                hits.append((line, kinds, text))
    elif path.suffix in (".cpp", ".hpp"):
        source = path.read_text()
        lines = source.split("\n")
        for line, text in c_strings(source):
            # a literal whose line, or the comment line above it, says "removed spelling"
            # is a deliberate case of the removed syntax
            if any("removed spelling" in lines[i] for i in range(max(0, line - 3), min(len(lines), line))):
                continue
            if looks_sql(text) and scan_sql(text):
                hits.append((line, scan_sql(text), text))
    elif path.suffix == ".test":
        for line, statement in sqllogic_statements(path.read_text()):
            kinds = scan_sql(statement)
            if kinds:
                hits.append((line, kinds, statement))
    else:
        text = path.read_text()
        pos = 0
        for statement in text.split(";"):
            line = text.count("\n", 0, pos) + 1 + (len(statement) - len(statement.lstrip("\n")))
            pos += len(statement) + 1
            if "removed spelling" in statement or not looks_sql(statement):
                continue
            kinds = scan_sql(statement)
            if kinds:
                hits.append((line, kinds, statement))
    return [(line, ",".join(sorted(kinds)), re.sub(r"\s+", " ", re.sub(r"--[^\n]*", "", text)).strip()[:140])
            for line, kinds, text in dict(((line, text), (line, kinds, text)) for line, kinds, text in hits).values()]
