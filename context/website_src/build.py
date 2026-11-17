#!/usr/bin/env python3
"""Build the DeciDB website (context/website) from the sources in this folder.

    python3 context/website_src/build.py                      # from the repository root, after `make release`
    DECIDB_BIN=/path/to/decidb SITE_OUT=/tmp/site python3 context/website_src/build.py

The sources live outside context/website because that folder is published to GitHub Pages as it is.
Edit pages/*.html, home.py, examples.py and context/website/assets/css|js, never the generated *.html pages. The
other folders under assets/ are unchanged copies of the taste-skill kit (system.css, system.js, motion/, ui/,
layout/, backgrounds/, icons/, diagrams/, effects/, moments/); the home page is built on it.

Pages are body fragments; the build adds the shared header and footer. Every SQL block marked with run= or check=
is executed on the DeciDB binary, and the build fails if one errors, so no query on the site is untested. A result
table under a query is that run's own output. The home page (home.py) runs its queries the same way.

Macros inside a fragment:
  <!--sql title="..." run="FIXTURE" dec="x,y" on="x"-->QUERY<!--/sql-->   run, show the result under it
  <!--sql check="FIXTURE"-->QUERY<!--/sql-->                              run, show only the query
  <!--sql check="FIXTURE" frag-->CONSTRAINTS<!--/sql-->                   splice into the fixture's template
  <!--sql check="FIXTURE" expect="text" show-->QUERY<!--/sql-->          must fail with that text; show the error
  <!--sql run="FIXTURE" direct="RULE"-->QUERY<!--/sql-->                 EXPLAIN must name this direct-solve rule
  <!--explain check="FIXTURE"-->QUERY<!--/explain-->                     the query, then its EXPLAIN DECIDE node
  <!--sql-->QUERY<!--/sql-->                                              shown only (grammar sketches)
  <!--bash-->, <!--python-->, <!--text-->                                  other code blocks
Fixtures (named databases) are in fixtures.py; every example's setup is one too.
"""

import html
import json
import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SITE = os.path.join(REPO, "context", "website")
sys.path.insert(0, HERE)

import examples as EX  # noqa: E402
import fixtures as FX  # noqa: E402
import home  # noqa: E402

BIN = os.environ.get("DECIDB_BIN", os.path.join(REPO, "build", "release", "decidb"))
OUT = os.environ.get("SITE_OUT", SITE)
if not os.path.exists(BIN):
    sys.exit(f"no DeciDB binary at {BIN}: run `make release` or set DECIDB_BIN")

GITHUB = "https://github.com/huda-lab/decidb"
YEAR = 2026
FAILURES = []
RUNS = 0
esc = html.escape

# ---------------------------------------------------------------- running SQL


def run_sql(sql, json_mode=True):
    """Run SQL on a fresh in-memory database. Returns (ok, stdout, stderr)."""
    global RUNS
    RUNS += 1
    args = [BIN, ":memory:"]
    if json_mode:
        args.append("-json")
    p = subprocess.run(args, input=sql, capture_output=True, text=True, timeout=300)
    err = p.stderr.strip()
    ok = p.returncode == 0 and not re.search(r"\b(Error|error):", err) and "Error:" not in p.stdout
    return ok, p.stdout, err


def parse_json_rows(out):
    """The CLI prints one JSON array per statement that returns rows; take the last one."""
    out = out.strip()
    if not out:
        return []
    arrays = re.findall(r"\[\{.*?\}\]", out, flags=re.S)
    if not arrays:
        return []
    return json.loads(arrays[-1])


def explain_text(setup, query):
    ok, out, err = run_sql(setup + "\nEXPLAIN " + query.rstrip().rstrip(";") + ";", json_mode=False)
    return out if ok else ""


def explain_fields(setup, query):
    """The direct-solve lines of EXPLAIN's plan box, joined back into one value each."""
    fields, key, inside = {}, None, False
    for line in explain_text(setup, query).splitlines():
        cell = line.strip().strip("│").strip()
        if cell.startswith("Direct solve") and cell.endswith(":"):
            key, inside = cell[:-1], True
            fields[key] = ""
            continue
        if inside:
            if not cell or cell.startswith("~"):
                inside = False
                continue
            fields[key] = (fields[key] + " " + cell).strip()
    return fields


def direct_rule(setup, query):
    rule = explain_fields(setup, query).get("Direct solve rule", "")
    m = re.search(r"([A-Z0-9_]{4,})", rule)
    return m.group(1) if m else None


def rows_of(page, setup, query):
    """Run a query that must succeed; the rows it returns."""
    ok, out, err = run_sql(setup + "\n" + query.rstrip().rstrip(";") + ";\n")
    if not ok:
        FAILURES.append(f"{page}: {query.strip().splitlines()[0][:90]}\n    {err[:400] or out[:400]}")
        return []
    return parse_json_rows(out)


def error_of(page, setup, query, expect):
    """Run a query that must fail with `expect` in its message; the message's first line."""
    ok, out, err = run_sql(setup + "\n" + query.rstrip().rstrip(";") + ";\n")
    msg = (err + out).strip()
    if ok or expect not in msg:
        FAILURES.append(f"{page}: expected an error with {expect!r}, got ok={ok}: {msg[:300]}")
        return ""
    return msg.splitlines()[0]


# ---------------------------------------------------------------- highlighting (done here, so pages need no script)

SQL_KEYWORDS = set(
    """SELECT FROM WHERE DECIDE SUCH THAT AND OR NOT MAXIMIZE MINIMIZE AS JOIN ON USING GROUP BY ORDER LIMIT
    INSERT INTO VALUES CREATE TABLE WITH IN IS NULL CASE WHEN THEN ELSE END DIAGNOSE EXPLAIN FORMAT DISTINCT
    LEFT RIGHT INNER OUTER CROSS UNION ALL HAVING SET PRAGMA BETWEEN ASC DESC OVER PARTITION BOOL BOOLEAN INT
    INTEGER BIGINT REAL DOUBLE VARCHAR DATE TRUE FALSE""".split()
)
SQL_SCOPE = {"per", "when", "by"}
SQL_FUNCS = set("""SUM MIN MAX AVG COUNT ABS ROUND POWER NORM LEAST GREATEST COALESCE SQRT LN EXP FLOOR CEIL""".split())
SQL_TOKEN = re.compile(r"(--[^\n]*)|('(?:[^']|'')*')|(\b\d+(?:\.\d+)?\b)|([A-Za-z_][A-Za-z_0-9]*)(?=(\s*\()?)|([\s\S])")


def hl_sql(code):
    out = []
    for m in SQL_TOKEN.finditer(code):
        com, s, num, ident, paren, other = m.groups()
        if com:
            out.append(f'<span class="tk-c">{esc(com)}</span>')
        elif s:
            out.append(f'<span class="tk-s">{esc(s)}</span>')
        elif num:
            out.append(f'<span class="tk-n">{num}</span>')
        elif ident:
            if ident in SQL_SCOPE:
                out.append(f'<span class="tk-p">{ident}</span>')
            elif ident.isupper() and ident in SQL_KEYWORDS:
                out.append(f'<span class="tk-k">{ident}</span>')
            elif paren and ident.upper() in SQL_FUNCS:
                out.append(f'<span class="tk-f">{ident}</span>')
            else:
                out.append(esc(ident))
        else:
            out.append(esc(other))
    return "".join(out)


SH_TOKEN = re.compile(r"(#[^\n]*)|(\"[^\"\n]*\"|'[^'\n]*')|([\s\S])")
PY_KEYWORDS = {"import", "from", "as", "def", "return", "for", "in", "if", "else", "with", "None", "True", "False"}
PY_TOKEN = re.compile(r"(#[^\n]*)|(\"\"\"[\s\S]*?\"\"\"|\"[^\"\n]*\"|'[^'\n]*')|([A-Za-z_][A-Za-z_0-9]*)|([\s\S])")


def hl_shell(code):
    out = []
    for m in SH_TOKEN.finditer(code):
        com, s, other = m.groups()
        if com and (m.start() == 0 or code[m.start() - 1] in " \n"):
            out.append(f'<span class="tk-c">{esc(com)}</span>')
        elif com:
            out.append(esc(com))
        elif s:
            out.append(f'<span class="tk-s">{esc(s)}</span>')
        else:
            out.append(esc(other))
    return "".join(out)


def hl_python(code):
    out = []
    for m in PY_TOKEN.finditer(code):
        com, s, ident, other = m.groups()
        if com:
            out.append(f'<span class="tk-c">{esc(com)}</span>')
        elif s:
            out.append(f'<span class="tk-s">{esc(s)}</span>')
        elif ident:
            out.append(f'<span class="tk-k">{ident}</span>' if ident in PY_KEYWORDS else esc(ident))
        else:
            out.append(esc(other))
    return "".join(out)


def highlight(code, lang):
    return {"sql": hl_sql, "bash": hl_shell, "python": hl_python}.get(lang, esc)(code)


# ---------------------------------------------------------------- code blocks and results


def code_block(code, lang="sql", title=None, status=None):
    code = code.strip("\n")
    label = title or {"sql": "SQL", "bash": "Shell", "python": "Python", "text": "Output"}.get(lang, lang)
    ok = '<span class="code__ok">Runs as shown</span>' if status == "ok" else ""
    return (
        f'<div class="code" data-lang="{lang}"><div class="code__head"><span class="code__title">{esc(label)}</span>{ok}'
        f'<button class="code__copy" type="button" hidden>Copy</button></div>'
        f"<pre><code>{highlight(code, lang)}</code></pre></div>"
    )


def fmt_value(v):
    if v is None:
        return "NULL", "null"
    if isinstance(v, bool):
        return ("true" if v else "false"), ""
    if isinstance(v, (int, float)):
        if isinstance(v, float) and v.is_integer() and abs(v) < 1e15:
            return f"{v:.1f}", "num"
        return str(v), "num"
    return str(v), ""


def result_table(rows, dec=(), on=None, note=None, max_rows=40):
    if not rows:
        return '<div class="output"><div class="output__head"><span>Result</span><span>0 rows</span></div></div>'
    cols = list(rows[0].keys())

    def is_num(c):
        return isinstance(rows[0][c], (int, float)) and not isinstance(rows[0][c], bool)

    head = "".join(
        f'<th class="{" ".join(x for x in ["num" if is_num(c) else "", "dec" if c in dec else ""] if x)}">{esc(c)}</th>'
        for c in cols
    )
    trs = []
    for r in rows[:max_rows]:
        cls = ' class="on"' if on is not None and r.get(on) not in (None, 0, 0.0, False) else ""
        tds = []
        for c in cols:
            text, kind = fmt_value(r[c])
            classes = " ".join(x for x in [kind, "dec" if c in dec else ""] if x)
            tds.append(f'<td class="{classes}">{esc(text)}</td>' if classes else f"<td>{esc(text)}</td>")
        trs.append(f"<tr{cls}>{''.join(tds)}</tr>")
    n = len(rows)
    right = f"{n} row{'s' if n != 1 else ''}" + (f" · {esc(note)}" if note else "")
    return (
        f'<div class="output"><div class="output__head"><span>Result</span><span>{right}</span></div>'
        f'<div class="output__scroll"><table><thead><tr>{head}</tr></thead><tbody>{"".join(trs)}</tbody></table></div></div>'
    )


def error_box(message):
    return (
        '<div class="output output--error"><div class="output__head"><span>Error</span><span>refused before solving</span></div>'
        f"<pre>{esc(message)}</pre></div>"
    )


def attr(attrs, name, default=None):
    m = re.search(name + r'="([^"]*)"', attrs)
    return m.group(1) if m else default


def has_flag(attrs, name):
    return re.search(r"(^|\s)" + name + r"(\s|$)", attrs) is not None


def where(page, code):
    return f"{page}: {code.strip().splitlines()[0][:90]}"


def expand_sql(attrs, code, page):
    code = code.strip("\n")
    title = attr(attrs, "title")
    run_fx = attr(attrs, "run")
    check_fx = attr(attrs, "check")
    expect = attr(attrs, "expect")
    fx_name = run_fx or check_fx
    if not fx_name:
        return code_block(code, "sql", title)
    fx = FX.FIXTURES[fx_name]
    setup = fx["setup"]
    query = fx["template"].replace("{CLAUSES}", code.strip()) if has_flag(attrs, "frag") else code
    sql = setup + "\n" + query.rstrip().rstrip(";") + ";\n"
    ok, out, err = run_sql(sql)
    if expect is not None:
        msg = err + out
        if ok or expect not in msg:
            FAILURES.append(f"{where(page, code)}\n    expected an error with {expect!r}, got ok={ok}: {msg[:300]}")
            return code_block(code, "sql", title)
        if has_flag(attrs, "show"):
            first = msg.strip().splitlines()[0]
            return f'<div class="qr">{code_block(code, "sql", title)}{error_box(first)}</div>'
        return code_block(code, "sql", title)
    if not ok:
        FAILURES.append(f"{where(page, code)}\n    {err[:400] or out[:400]}")
        return code_block(code, "sql", title)
    want_rule = attr(attrs, "direct")
    if want_rule:
        got = direct_rule(setup, query)
        if got != want_rule:
            FAILURES.append(f"{where(page, code)}\n    expected direct solve {want_rule}, got {got}")
    if run_fx:
        rows = parse_json_rows(out)
        dec = tuple(x for x in (attr(attrs, "dec", "") or "").split(",") if x)
        table = result_table(rows, dec=dec, on=attr(attrs, "on"), note=attr(attrs, "note"))
        return f'<div class="qr">{code_block(code, "sql", title, "ok")}{table}</div>'
    return code_block(code, "sql", title, "ok")


MACRO = re.compile(r'<!--(sql|bash|python|text|explain)(\s(?:[^>"]|"[^"]*")*)?-->(.*?)<!--/\1-->', re.S)


def expand_explain(attrs, code, page):
    """Show a query, then the DECIDE node of its EXPLAIN (FORMAT JSON), as EXPLAIN prints it."""
    fx = FX.FIXTURES[attr(attrs, "check")]
    query = code.strip().rstrip(";")
    ok, out, err = run_sql(fx["setup"] + "\nEXPLAIN (FORMAT JSON) " + query + ";", json_mode=False)
    node = None
    if ok:
        try:
            start = out.index("[")
            data = json.loads(out[start : out.rindex("]") + 1])
        except ValueError:
            data = []

        def walk(n):
            nonlocal node
            if n.get("name") == "DECIDE" and node is None:
                node = n.get("extra_info")
            for c in n.get("children", []):
                walk(c)

        for n in data:
            walk(n)
    if not node:
        FAILURES.append(f"{where(page, code)}\n    EXPLAIN gave no DECIDE node: {err[:300] or out[:300]}")
        return code_block(code, "sql", attr(attrs, "title"))
    lines = ["DECIDE", f"  Variables: {node.get('Variables', '')}"]
    if node.get("Objective"):
        lines.append(f"  Objective: {node['Objective']}")
    cons = node.get("Constraints", [])
    if isinstance(cons, str):
        cons = [cons]
    if cons:
        lines.append("  Constraints:")
        lines += ["    " + c for c in cons]
    return code_block(code, "sql", attr(attrs, "title"), "ok") + code_block(
        "\n".join(lines), "text", "EXPLAIN: the DECIDE node"
    )


def expand(fragment, page, root=""):
    def sub(m):
        kind, attrs, code = m.group(1), (m.group(2) or ""), m.group(3)
        if kind == "sql":
            return expand_sql(attrs, code, page)
        if kind == "explain":
            return expand_explain(attrs, code, page)
        return code_block(code, kind, attr(attrs, "title"))

    text = MACRO.sub(sub, fragment)
    left = re.search(r"<!--/?(sql|bash|python|text|explain)\b", text)
    if left:
        FAILURES.append(f"{page}: unexpanded macro near {text[left.start():left.start() + 120]!r}")
    return text.replace("{{GITHUB}}", GITHUB)


# ---------------------------------------------------------------- the shared frame (taste-skill system, dealt once)

# The deal (taste-skill, recorded in the home page's opening comment): software page, palette white-link-blue, type
# geist, radius sharp, buttons pill, lit ground sage, nav-mega, footer-statement, close-word, rhythm space.
TOKENS = """:root {
  --p-bg: #ffffff; --p-surface: #f4f4f4; --p-ink: #111111; --p-muted: #6b6b6b;
  --p-accent: #1a4be0; --p-accent-ink: #ffffff; --p-action: #1a4be0; --p-action-ink: #ffffff;
  --p-inv-bg: #111111; --p-inv-surface: #1e1e1e; --p-inv-ink: #ffffff; --p-inv-muted: #9a9a9a;
  --p-inv-accent: #ffffff; --p-inv-accent-ink: #111111; --p-inv-action: #ffffff; --p-inv-action-ink: #111111;
  --font-display: "Geist", system-ui, sans-serif;
  --font-text: "Geist", system-ui, sans-serif;
  --font-mono: "Geist Mono", ui-monospace, "SFMono-Regular", Menlo, Consolas, monospace;
  --display-weight: 500;
  --display-tracking: -0.035em;
  --display-tracking-xl: -0.045em;
}"""

CHEV = (
    '<svg class="chev" viewBox="0 0 12 12" aria-hidden="true"><path d="M3 4.5l3 3 3-3" fill="none" '
    'stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>'
)
ARROW = (
    '<svg class="m-arrow" viewBox="0 0 12 12" aria-hidden="true"><path class="m-arrow__stem" d="M1 6h8"/>'
    '<path class="m-arrow__head" d="M4.5 2.5 8 6l-3.5 3.5"/></svg>'
)
NAV_WORDS = [
    ("documentation.html", "Guide"),
    ("problems.html", "Problems"),
    ("examples.html", "Examples"),
    ("about.html", "About"),
]
MENU_COLS = [
    (
        "First problems",
        [
            ("knapsack", "Knapsack", "Pick items under a weight limit"),
            ("production", "Production", "Meet demand at lowest cost"),
            ("diet", "Diet", "Cheapest meals that meet targets"),
        ],
    ),
    (
        "Groups and keys",
        [
            ("shift-coverage", "Shift coverage", "Staff every shift"),
            ("facility-location", "Depots", "Open depots, then ship"),
            ("keyed-decisions", "Keyed decisions", "One decision per nurse"),
        ],
    ),
    (
        "Without a solver",
        [
            ("fair-allocation", "Fair allocation", "Max-min shares of a budget"),
            ("matching", "Matching", "Drivers to pickups"),
            ("shortlist", "Shortlist", "The best two per genre"),
        ],
    ),
]


def head(title, description, root, extra=""):
    return f"""<!doctype html>
<html lang="en" data-type="software" data-radius="sharp" data-ground="sage" data-buttons="pill" data-motion>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>{esc(title)}</title>
<meta name="description" content="{esc(description)}">
<meta property="og:type" content="website">
<meta property="og:title" content="{esc(title)}">
<meta property="og:description" content="{esc(description)}">
<meta property="og:image" content="{root}assets/images/share.jpg">
<meta name="twitter:card" content="summary_large_image">
<link rel="icon" href="{root}assets/images/mark.svg" type="image/svg+xml">
<meta name="theme-color" content="#ffffff">
<!-- <link rel="canonical" href="https://huda-lab.github.io/decidb/"> once the published address is confirmed -->
<meta name="site-kind" content="software">
<meta name="page-type" content="software">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Geist:wght@400..700&amp;family=Geist+Mono:wght@400..500&amp;display=swap" rel="stylesheet">
<link rel="stylesheet" href="{root}assets/system.css">
<link rel="stylesheet" href="{root}assets/ui/cards.css">
<link rel="stylesheet" href="{root}assets/motion/motion.css">
<link rel="stylesheet" href="{root}assets/backgrounds/backgrounds.css">
<link rel="stylesheet" href="{root}assets/layout/layout.css">
<link rel="stylesheet" href="{root}assets/icons/icon.css">
<style>
{TOKENS}
</style>
<link rel="stylesheet" href="{root}assets/css/site.css">
{extra}</head>"""


def header(root, active):
    words = []
    for href, word in NAV_WORDS:
        cur = ' aria-current="page"' if href == active else ""
        if href != "examples.html":
            words.append(f'<div class="nav__item"><a class="nav__link" href="{root}{href}"{cur}>{word}</a></div>')
            continue
        cols = "".join(
            f'<div class="menu-col span-3"><p class="t-label">{esc(g)}</p>'
            + "".join(
                f'<a href="{root}examples/{slug}.html">{esc(name)}<span>{esc(line)}</span></a>'
                for slug, name, line in items
            )
            + "</div>"
            for g, items in MENU_COLS
        )
        card = (
            f'<a class="menu-card span-3" href="{root}problems.html#direct-solve">'
            '<div class="panel"><div class="stage stage--fade-br"><div class="at-corner">'
            '<div class="sq sq--menu" aria-hidden="true"><span class="sq__k">Direct solve rule:</span>'
            "<span>R4_MAX_MIN_FAIRNESS</span></div></div></div></div>"
            '<span class="t-card">Direct solve</span><span class="t-small muted">Nine classes, no solver called</span></a>'
        )
        words.append(
            f'<div class="nav__item"><a class="nav__link" href="{root}{href}"{cur}>{word} {CHEV}</a>'
            f'<div class="nav__menu"><div class="wrap grid">{cols}{card}</div></div></div>'
        )
    return f"""<header class="nav">
  <div class="wrap nav__bar">
    <a class="skip-link" href="#top">Skip to content</a>
    <a class="nav__logo" href="{root}index.html"><picture><source media="(max-width: 379px)" srcset="{root}assets/images/mark.svg"><img src="{root}assets/images/logo.svg" alt="DeciDB" width="140" height="42"></picture></a>
    <nav class="nav__links" aria-label="Main">
      {"".join(words)}
    </nav>
    <div class="nav__actions">
      <a class="nav__link" href="{GITHUB}">GitHub</a>
      <a class="btn btn--primary" href="{root}getting-started.html#install">Install DeciDB</a>
    </div>
    <button class="nav__toggle" type="button" aria-expanded="false" aria-label="Menu">
      <svg width="18" height="18" viewBox="0 0 18 18" aria-hidden="true"><path d="M2 5h14M2 13h14" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/></svg>
    </button>
  </div>
</header>"""


def footer_links(root):
    def col(title, links):
        items = "".join(f'<li><a href="{href}">{esc(text)}</a></li>' for href, text in links)
        return f'<div class="span-2 footer__col"><h3>{esc(title)}</h3><ul>{items}</ul></div>'

    return f"""<div class="grid">
        <div class="span-4 stack-4">
          <a class="nav__logo" href="{root}index.html"><img src="{root}assets/images/logo.svg" alt="DeciDB" width="140" height="42" loading="lazy" decoding="async"></a>
          <p class="footer__lede">Constrained optimization in SQL, built on DuckDB.</p>
        </div>
        {col("Learn", [(f"{root}getting-started.html", "Install"), (f"{root}documentation.html", "Language guide"), (f"{root}problems.html", "Problems"), (f"{root}examples.html", "Examples")])}
        {col("Examples", [(f"{root}examples/knapsack.html", "Knapsack"), (f"{root}examples/facility-location.html", "Depots"), (f"{root}examples/fair-allocation.html", "Fair allocation"), (f"{root}examples/diagnose.html", "DIAGNOSE")])}
        {col("Project", [(f"{root}about.html", "About"), (f"{root}about.html#research", "Papers"), (GITHUB, "GitHub"), ("https://huda-lab.github.io/", "HUDA Lab")])}
        {col("Contact", [("mailto:support@decidb.com", "Email the team"), (f"{GITHUB}/issues", "Report an issue")])}
      </div>
      <div class="footer__legal">
        <span>&copy; {YEAR} HUDA Lab, NYU Abu Dhabi and UMass Amherst</span>
        <span>Product names and logos are trademarks of their owners.</span>
      </div>"""


def scripts(root, extra=""):
    return (
        f'<script type="module" src="{root}assets/system.js"></script>\n'
        f'<script type="module" src="{root}assets/motion/motion.js"></script>\n'
        f"{extra}"
        f'<script src="{root}assets/js/site.js" defer></script>'
    )


def page(title, description, body, active, root=""):
    return f"""{head(title, description, root)}
<body class="doc-body">
{header(root, active)}
<main id="top">
{body}
</main>
<footer class="footer footer--statement">
  <div class="wrap">
      {footer_links(root)}
  </div>
</footer>
{scripts(root)}
</body>
</html>
"""


def page_head(title, lede, crumbs=None, tags=""):
    c = ""
    if crumbs:
        c = '<nav class="crumbs" aria-label="Breadcrumb">' + '<span aria-hidden="true">/</span>'.join(crumbs) + "</nav>"
    t = f'<div class="page-head__tags">{tags}</div>' if tags else ""
    return f'<div class="page-head"><div class="container">{c}<h1>{title}</h1><p>{lede}</p>{t}</div></div>'


# ---------------------------------------------------------------- examples


def example_page(ex, prev_ex, next_ex):
    setup = ex["setup"].strip()
    query = ex["query"].strip()
    ok, out, err = run_sql(setup + "\n" + query.rstrip().rstrip(";") + ";\n")
    if not ok:
        FAILURES.append(f"example {ex['slug']}\n    {err[:400] or out[:400]}")
        rows = []
    else:
        rows = parse_json_rows(out)
    rule = direct_rule(setup, ex.get("explain_query", query))
    if ex.get("direct") and rule != ex["direct"]:
        FAILURES.append(f"example {ex['slug']}: expected direct solve {ex['direct']}, got {rule}")
    if not ex.get("direct") and rule:
        FAILURES.append(f"example {ex['slug']}: unexpectedly answered by direct solve {rule}")

    tags = f'<span class="tag">{esc(ex["class"])}</span>'
    if rule:
        tags = f'<span class="tag tag--direct">Direct solve {esc(rule.split("_")[0])}</span>' + tags
    explain = "".join(f"<dt><code>{esc(c)}</code></dt><dd>{d}</dd>" for c, d in ex["explain"])
    result = result_table(rows, dec=tuple(ex.get("dec", ())), on=ex.get("on"), note=ex.get("result_note"))
    extra = expand(ex.get("after", ""), "examples/" + ex["slug"])
    nav = '<nav class="pager" aria-label="More examples">'
    nav += (
        f'<a href="{prev_ex["slug"]}.html"><small>Previous</small>{esc(prev_ex["title"])}</a>'
        if prev_ex
        else "<span></span>"
    )
    nav += (
        f'<a class="pager__next" href="{next_ex["slug"]}.html"><small>Next</small>{esc(next_ex["title"])}</a>'
        if next_ex
        else "<span></span>"
    )
    nav += "</nav>"
    solver_line = (
        f"Answered by direct solve ({esc(rule)}): DuckDB operators compute the optimum, no solver is called."
        if rule
        else f"Problem class: {esc(ex['class'])}, solved by Gurobi or HiGHS."
    )
    shows = "".join(f"<li>{esc(t)}</li>" for t in ex["features"])
    body = f"""
{page_head(esc(ex["title"]), ex["summary"], ['<a href="../index.html">Home</a>', '<a href="../examples.html">Examples</a>', esc(ex["group"])], tags)}
<div class="container">
  <div class="doc-layout doc-layout--example">
    <article class="prose">
      <h2>The problem</h2>
      {ex["story"]}
      <h2>The data</h2>
      {code_block(setup, "sql", "Setup", "ok")}
      <h2>The query</h2>
      <div class="qr">{code_block(query, "sql", ex.get("query_title", "DECIDE query"), "ok")}{result}</div>
      <p class="solver-line">{solver_line}</p>
      <h2>How it reads</h2>
      <dl class="reads">{explain}</dl>
      <h2>The answer</h2>
      {ex["answer"]}
      {extra}
      {nav}
    </article>
    <aside class="side">
      <h2 class="side__title">This example shows</h2>
      <ul class="side__list">{shows}</ul>
      <h2 class="side__title">Run it yourself</h2>
      <p>Paste the setup, then the query, into <code>decidb</code>. The table above is the output of this exact SQL.</p>
      <p><a class="link" href="../getting-started.html#install">Install DeciDB</a></p>
    </aside>
  </div>
</div>
"""
    return page(f"{ex['title']}: a DeciDB example", ex["summary_plain"], body, "examples.html", root="../")


def examples_index():
    groups = []
    for gname, gdesc in EX.GROUPS:
        rows = []
        for ex in EX.EXAMPLES:
            if ex["group"] != gname:
                continue
            how = (
                f'<span class="tag tag--direct">Direct solve {esc(ex["direct"].split("_")[0])}</span>'
                if ex.get("direct")
                else f'<span class="tag">{esc(ex["class"])}</span>'
            )
            rows.append(
                f'<li data-tags="{" ".join(ex["filters"])}"><a class="ex-row" href="examples/{ex["slug"]}.html">'
                f'<span class="ex-row__title">{esc(ex["title"])}</span><span class="ex-row__sum">{ex["summary"]}</span>'
                f'<code class="ex-row__code">{hl_sql(ex["snippet"])}</code><span class="ex-row__how">{how}</span></a></li>'
            )
        groups.append(
            f'<section class="ex-group" aria-labelledby="g-{len(groups)}"><h2 id="g-{len(groups)}">{esc(gname)}</h2>'
            f'<p>{gdesc}</p><ul class="ex-list">{"".join(rows)}</ul></section>'
        )
    filters = [("all", "All")] + EX.FILTERS
    bar = "".join(
        f'<button class="filter" type="button" data-filter="{k}" aria-pressed="{"true" if k == "all" else "false"}">{v}</button>'
        for k, v in filters
    )
    body = (
        page_head(
            "Examples",
            f"{len(EX.EXAMPLES)} complete problems, each with its data, its query and the table DeciDB returns for it.",
            ['<a href="index.html">Home</a>', "Examples"],
        )
        + f'<div class="container ex-index"><div class="filters" role="group" aria-label="Show examples that use" hidden>{bar}</div>'
        + "".join(groups)
        + "</div>"
    )
    return page(
        "Examples: DeciDB queries for 20 decision problems",
        "Worked DeciDB examples: knapsack, assignment, facility location, fair allocation, matching and more, in DECIDE SQL.",
        body,
        "examples.html",
    )


# ---------------------------------------------------------------- pages

PAGES = [
    (
        "getting-started.html",
        "Install DeciDB: first DECIDE query in DuckDB",
        "Install DeciDB and write your first DECIDE query: decisions, constraints, objectives and the scope words per, when and by.",
        "",
    ),
    (
        "documentation.html",
        "DECIDE language guide: DeciDB",
        "The DECIDE language guide: decisions, keyed and query-wide declarations, per/when/by scopes, reducers, objectives, NULL rules, EXPLAIN and DIAGNOSE.",
        "documentation.html",
    ),
    (
        "problems.html",
        "Problem classes DeciDB solves in SQL",
        "The problem classes DeciDB solves in SQL: LP, MILP, QP, QCQP, bilinear, min-max and max-min, norms, and the classes answered by direct solve without a solver.",
        "problems.html",
    ),
    (
        "about.html",
        "About DeciDB: in-database optimization research",
        "DeciDB is a research project of the HUDA Lab at NYU Abu Dhabi and UMass Amherst on in-database constrained optimization.",
        "about.html",
    ),
]


def write_page(path, text):
    """Write a generated page with no trailing whitespace, so the repository's diff check stays clean."""
    with open(path, "w") as f:
        f.write("\n".join(line.rstrip() for line in text.split("\n")))


def copy_assets():
    """The assets live in the site itself; copy them only when building somewhere else."""
    import shutil

    if os.path.realpath(OUT) == os.path.realpath(SITE):
        return
    for dirpath, _, files in os.walk(os.path.join(SITE, "assets")):
        rel = os.path.relpath(dirpath, SITE)
        os.makedirs(os.path.join(OUT, rel), exist_ok=True)
        for name in files:
            shutil.copyfile(os.path.join(dirpath, name), os.path.join(OUT, rel, name))


def main():
    os.makedirs(os.path.join(OUT, "examples"), exist_ok=True)
    copy_assets()
    write_page(os.path.join(OUT, "index.html"), home.render(sys.modules[__name__]))
    for name, title, desc, active in PAGES:
        frag = open(os.path.join(HERE, "pages", name)).read()
        write_page(os.path.join(OUT, name), page(title, desc, expand(frag, name), active))
    write_page(os.path.join(OUT, "examples.html"), examples_index())
    exs = EX.EXAMPLES
    for i, ex in enumerate(exs):
        prev_ex = exs[i - 1] if i > 0 else None
        next_ex = exs[i + 1] if i + 1 < len(exs) else None
        write_page(os.path.join(OUT, "examples", ex["slug"] + ".html"), example_page(ex, prev_ex, next_ex))
    print(f"built {len(PAGES) + 2 + len(exs)} pages, ran {RUNS} queries on {BIN}")
    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURES:")
        for f_ in FAILURES:
            print(" -", f_)
        sys.exit(1)


if __name__ == "__main__":
    main()
