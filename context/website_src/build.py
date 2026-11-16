#!/usr/bin/env python3
"""Build the DeciDB website (context/website) from the sources in this folder.

    python3 context/website_src/build.py                      # from the repository root, after `make release`
    DECIDB_BIN=/path/to/decidb SITE_OUT=/tmp/site python3 context/website_src/build.py

The sources live outside context/website because that folder is published to GitHub Pages as it is.
Edit pages/*.html, examples.py and context/website/assets/, never the generated *.html pages. Pages are body
fragments; the build adds the shared header and footer. Every SQL block marked with run= or check= is executed
on the DeciDB binary, and the build fails if one errors, so no query on the site is untested. A result table
under a query is that run's own output.

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

BIN = os.environ.get("DECIDB_BIN", os.path.join(REPO, "build", "release", "decidb"))
OUT = os.environ.get("SITE_OUT", SITE)
if not os.path.exists(BIN):
    sys.exit(f"no DeciDB binary at {BIN}: run `make release` or set DECIDB_BIN")

GITHUB = "https://github.com/huda-lab/decidb"
FAILURES = []
RUNS = 0

# ---------------------------------------------------------------- icons

ICON = {
    "github": '<svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M12 .5a11.5 11.5 0 0 0-3.64 22.41c.58.1.79-.25.79-.56v-2c-3.2.7-3.87-1.37-3.87-1.37-.53-1.33-1.28-1.69-1.28-1.69-1.05-.71.08-.7.08-.7 1.16.08 1.77 1.19 1.77 1.19 1.03 1.77 2.7 1.26 3.36.96.1-.75.4-1.26.73-1.55-2.56-.29-5.24-1.28-5.24-5.69 0-1.26.45-2.29 1.19-3.1-.12-.29-.52-1.46.11-3.05 0 0 .97-.31 3.17 1.18a11 11 0 0 1 5.78 0c2.2-1.49 3.17-1.18 3.17-1.18.63 1.59.23 2.76.11 3.05.74.81 1.19 1.84 1.19 3.1 0 4.42-2.69 5.39-5.25 5.68.41.36.78 1.06.78 2.14v3.17c0 .31.21.67.8.56A11.5 11.5 0 0 0 12 .5Z"/></svg>',
    "arrow": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 12h14M13 6l6 6-6 6"/></svg>',
    "sun": '<svg class="sun" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="12" r="4.5"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>',
    "moon": '<svg class="moon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8Z"/></svg>',
    "menu": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><path d="M4 7h16M4 12h16M4 17h16"/></svg>',
    "db": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><ellipse cx="12" cy="5.5" rx="8" ry="3"/><path d="M4 5.5v13c0 1.7 3.6 3 8 3s8-1.3 8-3v-13"/><path d="M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3"/></svg>',
    "zap": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M13 2 4 14h7l-1 8 9-12h-7l1-8Z"/></svg>',
    "layers": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m12 3 9 5-9 5-9-5 9-5Z"/><path d="m3 13 9 5 9-5"/></svg>',
    "search": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/></svg>',
    "cpu": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/></svg>',
    "target": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5"/><circle cx="12" cy="12" r="1"/></svg>',
}

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


def direct_rule(setup, query):
    ok, out, err = run_sql(setup + "\nEXPLAIN " + query.rstrip().rstrip(";") + ";", json_mode=False)
    if not ok:
        return None
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "Direct solve rule:" in line and i + 1 < len(lines):
            m = re.search(r"([A-Z0-9_]{4,})", lines[i + 1])
            if m:
                return m.group(1)
    return None


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
        body = '<div class="result-head"><b>Result</b><span>0 rows</span></div>'
        return f'<div class="result">{body}</div>'
    cols = list(rows[0].keys())
    head = "".join(
        f'<th class="{"dec " if c in dec else ""}{"num" if isinstance(rows[0][c], (int, float)) and not isinstance(rows[0][c], bool) else ""}">{html.escape(c)}</th>'
        for c in cols
    )
    trs = []
    for r in rows[:max_rows]:
        cls = ""
        if on is not None and r.get(on) not in (None, 0, 0.0, False):
            cls = ' class="on"'
        tds = []
        for c in cols:
            text, kind = fmt_value(r[c])
            classes = " ".join(x for x in [kind, "dec" if c in dec else ""] if x)
            tds.append(
                f'<td class="{classes}">{html.escape(text)}</td>' if classes else f"<td>{html.escape(text)}</td>"
            )
        trs.append(f"<tr{cls}>{''.join(tds)}</tr>")
    n = len(rows)
    right = f"{n} row{'s' if n != 1 else ''}" + (f" · {html.escape(note)}" if note else "")
    return (
        f'<div class="result"><div class="result-head"><b>Result</b><span>{right}</span></div>'
        f'<div class="result-scroll"><table><thead><tr>{head}</tr></thead><tbody>{"".join(trs)}</tbody></table></div></div>'
    )


# ---------------------------------------------------------------- code blocks


def code_block(code, lang="sql", title=None, status=None):
    code = code.strip("\n")
    label = title or {"sql": "SQL", "bash": "Shell", "python": "Python", "text": "Output"}.get(lang, lang)
    dot = ""
    if status == "ok":
        dot = '<span class="dot ok" title="Runs on DeciDB as written"></span>'
    cls = f"language-{lang}" if lang != "text" else "language-none"
    return (
        f'<div class="code"><div class="code-head"><span class="code-title">{dot}{html.escape(label)}</span>'
        f'<button class="copy-btn" type="button">Copy</button></div>'
        f'<pre><code class="{cls}">{html.escape(code)}</code></pre></div>'
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
    if has_flag(attrs, "frag"):
        query = fx["template"].replace("{CLAUSES}", code.strip())
    else:
        query = code
    sql = setup + "\n" + query.rstrip().rstrip(";") + ";\n"
    ok, out, err = run_sql(sql)
    if expect is not None:
        msg = err + out
        if ok or expect not in msg:
            FAILURES.append(f"{where(page, code)}\n    expected an error with {expect!r}, got ok={ok}: {msg[:300]}")
            return code_block(code, "sql", title)
        if has_flag(attrs, "show"):
            first = msg.strip().splitlines()[0]
            return (
                f'<div class="query-result">{code_block(code, "sql", title)}'
                f'<div class="result"><div class="result-head"><b>Error</b><span>refused before solving</span></div>'
                f'<pre class="err">{html.escape(first)}</pre></div></div>'
            )
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
        return f'<div class="query-result">{code_block(code, "sql", title, "ok")}{table}</div>'
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


def expand(fragment, page):
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
    text = text.replace("{{GITHUB}}", GITHUB)
    for k, v in ICON.items():
        text = text.replace("{{icon:" + k + "}}", v)
    return text


# ---------------------------------------------------------------- layout

NAV = [
    ("documentation.html", "Docs"),
    ("problems.html", "Problems"),
    ("examples.html", "Examples"),
    ("getting-started.html", "Get started"),
    ("about.html", "About"),
]


def layout(title, description, body, active, root=""):
    links = "".join(
        f'<a href="{root}{href}"{" aria-current=\"page\"" if href == active else ""}>{label}</a>' for href, label in NAV
    )
    mobile = "".join(f'<a href="{root}{href}">{label}</a>' for href, label in NAV)
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<meta name="description" content="{html.escape(description)}">
<meta name="keywords" content="DeciDB, DuckDB, SQL, constrained optimization, linear programming, mixed-integer programming, quadratic programming, DECIDE, solver, HUDA Lab">
<link rel="icon" type="image/svg+xml" href="{root}assets/images/mark.svg">
<script>(function(){{try{{var t=localStorage.getItem('decidb-theme');if(t)document.documentElement.setAttribute('data-theme',t)}}catch(e){{}}}})();</script>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&amp;family=JetBrains+Mono:wght@400;500;600;700&amp;display=swap" rel="stylesheet">
<link rel="stylesheet" href="{root}assets/css/site.css">
<script>window.Prism = window.Prism || {{}}; window.Prism.manual = true;</script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/prism/1.29.0/prism.min.js" defer></script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/prism/1.29.0/components/prism-sql.min.js" defer></script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/prism/1.29.0/components/prism-bash.min.js" defer></script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/prism/1.29.0/components/prism-python.min.js" defer></script>
<script src="{root}assets/js/site.js" defer></script>
</head>
<body>
<a class="skip-link" href="#main">Skip to content</a>
<header class="site-header">
  <div class="container">
    <a class="brand" href="{root}index.html" aria-label="DeciDB home">
      <img class="logo-light" src="{root}assets/images/logo.svg" alt="DeciDB" width="100" height="30">
      <img class="logo-dark" src="{root}assets/images/logo-dark.svg" alt="DeciDB" width="100" height="30">
      <span class="by">HUDA Lab<br>NYU Abu Dhabi &amp; UMass Amherst</span>
    </a>
    <nav class="nav" aria-label="Main">{links}</nav>
    <div class="header-actions">
      <a class="btn btn-ghost btn-sm" href="{GITHUB}" target="_blank" rel="noopener noreferrer">{ICON["github"]} GitHub</a>
      <button class="icon-btn theme-toggle" type="button" aria-label="Switch light or dark theme">{ICON["sun"]}{ICON["moon"]}</button>
      <button class="icon-btn menu-btn" type="button" aria-label="Open menu" aria-expanded="false">{ICON["menu"]}</button>
    </div>
  </div>
  <nav class="mobile-nav" aria-label="Mobile">{mobile}<a href="{GITHUB}" target="_blank" rel="noopener noreferrer">GitHub</a></nav>
</header>
<main id="main">
{body}
</main>
<footer class="site-footer">
  <div class="container">
    <div class="footer-grid">
      <div class="footer-brand">
        <img class="logo-light" src="{root}assets/images/logo.svg" alt="DeciDB" width="94" height="28">
        <img class="logo-dark" src="{root}assets/images/logo-dark.svg" alt="DeciDB" width="94" height="28">
        <p>Constrained optimization, native in SQL. Built on DuckDB by the HUDA Lab at NYU Abu Dhabi and UMass Amherst.</p>
      </div>
      <div>
        <h4>Learn</h4>
        <ul>
          <li><a href="{root}getting-started.html">Get started</a></li>
          <li><a href="{root}documentation.html">Language reference</a></li>
          <li><a href="{root}problems.html">Problems you can express</a></li>
          <li><a href="{root}examples.html">Examples</a></li>
        </ul>
      </div>
      <div>
        <h4>Project</h4>
        <ul>
          <li><a href="{GITHUB}" target="_blank" rel="noopener noreferrer">GitHub repository</a></li>
          <li><a href="{GITHUB}/issues" target="_blank" rel="noopener noreferrer">Issue tracker</a></li>
          <li><a href="{root}about.html#research">Research papers</a></li>
          <li><a href="https://huda-lab.github.io/" target="_blank" rel="noopener noreferrer">HUDA Lab</a></li>
        </ul>
      </div>
      <div>
        <h4>Contact</h4>
        <ul>
          <li><a href="mailto:support@decidb.com">support@decidb.com</a></li>
          <li class="subtle">NYU Abu Dhabi &amp; UMass Amherst</li>
        </ul>
      </div>
    </div>
    <div class="footer-bottom">
      <span>&copy; 2026 HUDA Lab &middot; NYU Abu Dhabi &amp; UMass Amherst</span>
      <span>Built on DuckDB &middot; Solvers: Gurobi when available, HiGHS bundled</span>
    </div>
  </div>
</footer>
</body>
</html>
"""


def page_head(title, lede, crumbs=None):
    c = ""
    if crumbs:
        c = '<div class="crumbs">' + "<span>/</span>".join(crumbs) + "</div>"
    return f'<section class="page-head"><div class="container">{c}<h1>{title}</h1><p>{lede}</p></div></section>'


# ---------------------------------------------------------------- examples


def example_page(ex, prev_ex, next_ex):
    setup = ex["setup"].strip()
    query = ex["query"].strip()
    sql = setup + "\n" + query.rstrip().rstrip(";") + ";\n"
    ok, out, err = run_sql(sql)
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

    tags = "".join(f'<span class="badge">{html.escape(t)}</span>' for t in ex["features"])
    if rule:
        tags = f'<span class="badge green">Direct solve: {rule}</span>' + tags
    explain = "".join(f"<li><code>{html.escape(c)}</code><div>{d}</div></li>" for c, d in ex["explain"])
    result = result_table(rows, dec=tuple(ex.get("dec", ())), on=ex.get("on"), note=ex.get("result_note"))
    extra = expand(ex.get("after", ""), "examples/" + ex["slug"])
    nav = '<div class="ex-nav">'
    nav += (
        f'<a href="{prev_ex["slug"]}.html"><small>Previous</small>{html.escape(prev_ex["title"])}</a>'
        if prev_ex
        else "<span></span>"
    )
    nav += (
        f'<a href="{next_ex["slug"]}.html" style="text-align:right"><small>Next</small>{html.escape(next_ex["title"])}</a>'
        if next_ex
        else "<span></span>"
    )
    nav += "</div>"
    solver_line = (
        f"Answered by <strong>direct solve</strong> ({rule}): DuckDB operators compute the optimum, no solver is called."
        if rule
        else f"Problem class: <strong>{html.escape(ex['class'])}</strong>."
    )
    aside_features = "".join(f"<li>{html.escape(t)}</li>" for t in ex["features"])
    body = f"""
{page_head(html.escape(ex["title"]), ex["summary"], ['<a href="../index.html">Home</a>', '<a href="../examples.html">Examples</a>', html.escape(ex["group"])])}
<div class="container">
  <div class="ex-layout">
    <div class="prose" style="max-width:none">
      <div class="meta" style="display:flex;gap:6px;flex-wrap:wrap;margin-bottom:22px">{tags}</div>
      <h2 style="font-size:1.35rem">The problem</h2>
      {ex["story"]}
      <h2 style="font-size:1.35rem;margin-top:36px">The data</h2>
      {code_block(setup, "sql", "Setup", "ok")}
      <h2 style="font-size:1.35rem;margin-top:36px">The query</h2>
      <div class="query-result">{code_block(query, "sql", ex.get("query_title", "DECIDE query"), "ok")}{result}</div>
      <p class="muted" style="font-size:14.5px">{solver_line}</p>
      <h2 style="font-size:1.35rem;margin-top:36px">How it reads</h2>
      <ul class="explain-list">{explain}</ul>
      <h2 style="font-size:1.35rem;margin-top:36px">The answer</h2>
      {ex["answer"]}
      {extra}
      {nav}
    </div>
    <aside class="ex-aside">
      <div class="card">
        <h4>Shows</h4>
        <ul>{aside_features}</ul>
      </div>
      <div class="card" style="margin-top:14px">
        <h4>Try it</h4>
        <p style="font-size:14px">Paste the setup, then the query, into <code>decidb</code>. The result table above is the output of this exact SQL.</p>
        <a class="btn btn-ghost btn-sm" href="../getting-started.html">Install DeciDB {ICON["arrow"]}</a>
      </div>
    </aside>
  </div>
</div>
"""
    return layout(f"{ex['title']} · DeciDB example", ex["summary_plain"], body, "examples.html", root="../")


def examples_index():
    groups = []
    for gname, gdesc in EX.GROUPS:
        cards = []
        for ex in EX.EXAMPLES:
            if ex["group"] != gname:
                continue
            badges = []
            if ex.get("direct"):
                badges.append('<span class="badge green">Direct solve</span>')
            badges.append(f'<span class="badge blue">{html.escape(ex["class"])}</span>')
            for t in ex["card_tags"]:
                badges.append(f'<span class="badge">{html.escape(t)}</span>')
            tags = " ".join(ex["filters"])
            cards.append(
                f'<a class="card ex-card" href="examples/{ex["slug"]}.html" data-tags="{tags}">'
                f'<h3>{html.escape(ex["title"])}</h3><p>{ex["summary"]}</p>'
                f'<div class="card-code">{html.escape(ex["snippet"])}</div>'
                f'<div class="meta">{"".join(badges)}</div></a>'
            )
        groups.append(
            f'<section class="ex-group"><h2>{html.escape(gname)}</h2><p>{gdesc}</p>'
            f'<div class="grid grid-2">{"".join(cards)}</div></section>'
        )
    filters = [("all", "All")] + EX.FILTERS
    bar = "".join(
        f'<button class="tab" type="button" data-filter="{k}" aria-selected="{"true" if k == "all" else "false"}">{v}</button>'
        for k, v in filters
    )
    body = (
        page_head(
            "Examples",
            f"{len(EX.EXAMPLES)} complete problems, each with its data, its query and the answer DeciDB returns. "
            "Every query on these pages is run on DeciDB when the site is built.",
        )
        + f'<div class="container" style="padding-top:40px;padding-bottom:80px"><div class="filter-bar" role="group" aria-label="Filter examples">{bar}</div>'
        + "".join(groups)
        + "</div>"
    )
    return layout(
        "Examples · DeciDB",
        "Worked DeciDB examples: knapsack, assignment, facility location, fair allocation, matching and more, in DECIDE SQL.",
        body,
        "examples.html",
    )


# ---------------------------------------------------------------- pages

PAGES = [
    (
        "index.html",
        "DeciDB · Constrained optimization, native in SQL",
        "DeciDB extends DuckDB with a DECIDE clause: declare decisions over your rows, state constraints and an objective, and get the optimal rows back in one SQL query.",
        "",
    ),
    (
        "getting-started.html",
        "Get started · DeciDB",
        "Install DeciDB and write your first DECIDE query: decisions, constraints, objectives and the three scope words per, when and by.",
        "getting-started.html",
    ),
    (
        "documentation.html",
        "Language reference · DeciDB",
        "The DECIDE language reference: decisions, keyed and query-wide declarations, per/when/by scopes, reducers, objectives, NULL rules, EXPLAIN and DIAGNOSE.",
        "documentation.html",
    ),
    (
        "problems.html",
        "Problems you can express · DeciDB",
        "The problem classes DeciDB solves in SQL: LP, MILP, QP, QCQP, bilinear, min-max and max-min, norms, and the classes answered by direct solve without a solver.",
        "problems.html",
    ),
    (
        "about.html",
        "About · DeciDB",
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
    for name, title, desc, active in PAGES:
        frag = open(os.path.join(HERE, "pages", name)).read()
        body = expand(frag, name)
        write_page(os.path.join(OUT, name), layout(title, desc, body, active))
    write_page(os.path.join(OUT, "examples.html"), examples_index())
    exs = EX.EXAMPLES
    for i, ex in enumerate(exs):
        prev_ex = exs[i - 1] if i > 0 else None
        next_ex = exs[i + 1] if i + 1 < len(exs) else None
        write_page(os.path.join(OUT, "examples", ex["slug"] + ".html"), example_page(ex, prev_ex, next_ex))
    print(f"built {len(PAGES) + 1 + len(exs)} pages, ran {RUNS} queries on {BIN}")
    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURES:")
        for f_ in FAILURES:
            print(" -", f_)
        sys.exit(1)


if __name__ == "__main__":
    main()
