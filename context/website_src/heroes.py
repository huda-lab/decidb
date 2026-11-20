"""The openings of the reading pages, in the home page's manner: a short hero with the words on the left (the
breadcrumb, the h1, one line, the page's actions) and on the right a drawn DeciDB window standing on the moving lit
ground, its result rows arriving as the home page's do.

Every query, result and EXPLAIN line in a window is run on the DeciDB binary by build.py when the site is built:
each function takes the build module (b) and uses its helpers, so a query that stops working fails the build.
"""

import html
import re

from home import DIAGNOSE, FAIR, KNAPSACK, grid, num

esc = html.escape


def window(title, body, label, cls=""):
    """A drawn DeciDB window: its title bar, then its body (a picture: the label is what a screen reader hears)."""
    return (
        f'<div class="ui ui-hw hw hw--page {cls}" role="img" aria-label="{esc(label)}">'
        f'<div class="hw__bar"><span class="hw__dots"><i></i><i></i><i></i></span><span><b>decidb</b> · {esc(title)}</span></div>'
        f'<div class="hw__page">{body}</div></div>'
    )


def hero(b, root, trail, title, lede, picture, seed, actions="", tags=""):
    """The opening: the words (span-5) and the picture on the moving ground (span-6 from column 7)."""
    btns = f'<div class="btn-row">{actions}</div>' if actions else ""
    tag_row = f'<div class="page-head__tags">{tags}</div>' if tags else ""
    return f"""<section class="hero hero--page" data-enter aria-labelledby="page-h">
  <div class="wrap"><div class="hero-text hero-split grid">
    <div class="hero-copy span-5">{b.crumbs(root, *trail)}<h1 id="page-h">{title}</h1><p class="hero__lead">{lede}</p>{tag_row}{btns}</div>
    <div class="hero-visual hero__window span-6 start-7" data-fx="grain-gradient" data-fx-seed="{seed}" data-ui-pause-host>{picture}</div>
  </div></div>
</section>"""


def btn(href, words, primary=False):
    return f'<a class="btn btn--{"primary" if primary else "secondary"}" href="{href}">{words}</a>'


def run_window(b, title, sql, rows, cols, mark=None, label="", before=""):
    """A query and its result, the rows arriving one by one."""
    body = (
        f'{before}<pre class="hw__sql"><code>{b.hl_sql(sql)}</code></pre>'
        f'<div class="hw__out" data-ly-steps="520">{grid(cols, rows, mark=mark, steps=True)}</div>'
    )
    return window(title, body, label)


# ---------------------------------------------------------------- the four written pages


def getting_started(b):
    ex = {e["slug"]: e for e in b.EX.EXAMPLES}["knapsack"]
    rows = b.rows_of("getting-started.html hero", ex["setup"], KNAPSACK)
    taken = [r["item"].lower() for r in rows if r["take"]]
    shell = (
        '<pre class="hw__cmd"><code><span class="hw__prompt">$</span> pip install decidb\n'
        '<span class="hw__prompt">$</span> ./decidb shop.db</code></pre>'
    )
    picture = run_window(
        b,
        "shop.db",
        KNAPSACK,
        rows,
        ["item", "value", "weight", "take"],
        mark="take",
        label=f"Install DeciDB, open the shell and run the first query: it takes the {', '.join(taken)}.",
        before=shell,
    )
    return hero(
        b,
        "",
        ["Get started"],
        "Get started",
        "Install DeciDB, run a first optimization query, and learn the parts every DECIDE query has.",
        picture,
        11,
        btn("#install", "Install DeciDB", True) + btn("#first-query", "Your first query"),
    )


def documentation(b):
    clauses = {"DECIDE": "decisions", "SUCH THAT": "constraints", "MAXIMIZE": "objective"}
    lines = []
    for line in KNAPSACK.splitlines():
        tag = next((t for k, t in clauses.items() if line.startswith(k)), "")
        mark = f'<span class="an__tag">{tag}</span>' if tag else ""
        lines.append(f'<div class="an__line{" is-clause" if tag else ""}"><code>{b.hl_sql(line)}</code>{mark}</div>')
    b.rows_of("guide.html hero", {e["slug"]: e for e in b.EX.EXAMPLES}["knapsack"]["setup"], KNAPSACK)
    picture = window(
        "anatomy.sql",
        f'<div class="an">{"".join(lines)}</div>',
        "The first query with its three clauses marked: DECIDE declares the decisions, SUCH THAT the constraints, "
        "MAXIMIZE the objective.",
        "hw--mid",
    )
    return hero(
        b,
        "",
        ["Guide"],
        "The DECIDE guide",
        "A DECIDE query is a SQL query with three more clauses. This guide takes them in the order you write them, and "
        "every example runs on DeciDB as written.",
        picture,
        12,
        btn("#decide", "Start with DECIDE", True) + btn("examples.html", "Browse examples"),
    )


def features(b):
    ex = {e["slug"]: e for e in b.EX.EXAMPLES}
    rule = b.explain_fields(ex["fair-allocation"]["setup"], FAIR).get("Direct solve rule", "")
    if not rule.startswith("R4"):
        b.FAILURES.append(f"features.html hero: expected the R4 rewrite, got {rule!r}")
    drows = b.rows_of("features.html hero", ex["diagnose"]["setup"], DIAGNOSE)
    fix = [{"suggested_change": r["suggested_change"], "amount": r["amount"]} for r in drows if r.get("clause")]
    rewrite = window(
        "fair.sql",
        f'<pre class="hw__sql"><code><span class="tk-k">EXPLAIN</span> {b.hl_sql(FAIR)}</code></pre>'
        f'<div class="sq__kv"><span>Direct solve rule</span><b>{esc(rule)}</b></div>',
        f"EXPLAIN on a fair-allocation query prints the rewrite rule {rule}.",
        "hw--a",
    )
    diagnose = window(
        "bag.sql",
        f'<pre class="hw__sql"><code>{b.hl_sql(DIAGNOSE)}</code></pre>'
        f'<div class="hw__out" data-ly-steps="520">{grid(["suggested_change", "amount"], fix, steps=True, lead="3fr")}</div>',
        "DIAGNOSE on a query with no answer suggests " + ", ".join(f["suggested_change"] for f in fix) + ".",
        "hw--b",
    )
    return hero(
        b,
        "",
        ["Novel features"],
        "Novel features",
        "Two things DeciDB does that a solver library does not. It rewrites a query that has an exact answer into "
        "ordinary SQL operations, and it explains a query that has no answer.",
        rewrite + diagnose,
        13,
        btn("#rewriting", "Query rewriting", True) + btn("#diagnose", "DIAGNOSE"),
    )


# the papers, as the About page lists them
PAPERS = [
    ("DeciDB: In-Database Processing of Decision Queries", "CIDR 2027, submitted"),
    ("Scaling Package Queries to a Billion Tuples", "VLDB 2024"),
    ("Scalable Computation of High-Order Optimization Queries", "ACM 2019"),
    ("Scalable Package Queries in Relational Database Systems", "VLDB 2016"),
]


def about(b):
    rows = "".join(f'<div class="hw__paper" data-step><b>{esc(t)}</b><span>{esc(v)}</span></div>' for t, v in PAPERS)
    picture = window(
        "research",
        f'<div class="hw__q">Papers</div><div class="hw__papers" data-ly-steps="520">{rows}</div>',
        "The four papers behind DeciDB, from Scalable Package Queries in 2016 to DeciDB itself.",
    )
    return hero(
        b,
        "",
        ["About"],
        "About DeciDB",
        "A research collaboration between the HUDA Lab at NYU Abu Dhabi and UMass Amherst on bringing constrained "
        "optimization into the database.",
        picture,
        14,
        btn("assets/DeciDB_Paper.pdf", "Read the paper", True) + btn("#team", "The team"),
    )


PAGES = {
    "getting-started.html": getting_started,
    "guide.html": documentation,
    "features.html": features,
    "about.html": about,
}


# ---------------------------------------------------------------- the examples


def examples_index(b):
    ex = {e["slug"]: e for e in b.EX.EXAMPLES}["production"]
    rows = b.rows_of("examples.html hero", ex["setup"], ex["query"])
    items = "".join(
        f'<div class="hw__row{" is-on" if g == ex["group"] else ""}"><b>{esc(g)}</b>'
        f'<span>{sum(1 for e in b.EX.EXAMPLES if e["group"] == g)}</span></div>'
        for g, _ in b.EX.GROUPS
    )
    body = (
        f'<div class="hw__list"><div class="hw__q">Examples</div>{items}</div>'
        f'<div class="hw__main"><pre class="hw__sql"><code>{b.hl_sql(ex["query"])}</code></pre>'
        f'<div class="hw__out" data-ly-steps="520">{grid(["plant", "units"], rows, mark="units", steps=True)}</div></div>'
    )
    made = ", ".join(f"{r['plant']} makes {num(r['units'])}" for r in rows)
    picture = (
        f'<div class="ui ui-hw hw hw--page hw--list" role="img" aria-label="{esc("The example groups, and the production example: " + made + ".")}">'
        '<div class="hw__bar"><span class="hw__dots"><i></i><i></i><i></i></span><span><b>decidb</b> · examples</span></div>'
        f'<div class="hw__body">{body}</div></div>'
    )
    return hero(
        b,
        "",
        ["Examples"],
        "Examples",
        f"{len(b.EX.EXAMPLES)} complete problems, each with its data, its query and the table DeciDB returns for it.",
        picture,
        15,
    )


def example(b, ex, rows, tags):
    """An example page's opening: the example's own query and the rows DeciDB returned for it."""
    if not rows:
        b.FAILURES.append(f"example {ex['slug']}: no rows to draw in its opening")
        return ""
    cols = list(rows[0].keys())
    dec = [c for c in ex.get("dec", []) if c in cols]
    picture = run_window(
        b,
        ex["slug"] + ".sql",
        ex["query"],
        rows,
        cols,
        mark=dec[0] if len(dec) == 1 and isinstance(rows[0][dec[0]], int) else None,
        label=f"The {ex['title'].lower()} query and the {len(rows)} rows it returns.",
    )
    return hero(
        b, "../", [("examples.html", "Examples"), ex["group"]], esc(ex["title"]), ex["summary"], picture, 16, tags=tags
    )


# ---------------------------------------------------------------- the documentation

# the DECIDE path as the docs draw it: the eight stages, and the two places a query leaves the solver's path
STAGES = [
    ("01", "Parser", "the grammar and the DECIDE keywords"),
    ("02", "Binder", "names, scopes and types"),
    ("03", "Logical plan", "the LogicalDecide operator"),
    ("04", "Canonicalizer", "one shape for every clause"),
    ("05", "Optimizer", "the formulation"),
    ("↳", "Query rewriting", "DuckDB operators, no solver"),
    ("06", "Model", "variables, coefficients, bounds"),
    ("07", "Solver", "Gurobi or HiGHS"),
    ("↳", "DIAGNOSE", "what to change when there is no answer"),
    ("08", "Execution", "the decisions, back as columns"),
]
# which rows each page lights: the stages it explains
DOC_LIT = {
    "index.html": {"01", "02", "03", "04", "05", "Query rewriting", "06", "07", "DIAGNOSE", "08"},
    "life-of-a-query.html": {"01", "02", "03", "04", "05", "06", "07", "08"},
    "front-end.html": {"01", "02", "03"},
    "canonical-form.html": {"04", "05"},
    "solving.html": {"06", "07", "08"},
    "rewriting.html": {"05", "Query rewriting"},
    "rewrite-rules.html": {"Query rewriting"},
    "diagnose.html": {"07", "DIAGNOSE"},
    "testing.html": {"01", "02", "03", "04", "05", "Query rewriting", "06", "07", "DIAGNOSE", "08"},
    "performance.html": {"Query rewriting", "06", "07", "08"},
}


def pipeline_window(slug):
    lit = DOC_LIT.get(slug, set())
    rows = []
    for num_, name, line in STAGES:
        on = num_ in lit or name in lit
        exit_ = num_ == "↳"
        cls = "hw__stage" + (" hw__stage--exit" if exit_ else "") + (" is-on" if on else "")
        rows.append(
            f'<div class="{cls}" data-step><span class="hw__num">{esc(num_)}</span><b>{esc(name)}</b>'
            f"<span>{esc(line)}</span></div>"
        )
    named = [n for k, n, _ in STAGES if k in lit or n in lit]
    label = "The DECIDE path, from the parser to execution" + (
        f"; this page covers {', '.join(named)}." if len(named) < 8 else "."
    )
    return window(
        "the DECIDE path",
        f'<div class="hw__stages" data-ly-steps="260">{"".join(rows)}</div>',
        label,
        "hw--mid",
    )


def rewriting_picture(b):
    """The same problem twice: rewritten into DuckDB operators, and on the solver (decide_direct_solve = 'off')."""
    ex = {e["slug"]: e for e in b.EX.EXAMPLES}["shortlist"]
    query = ex["query"].split("ORDER BY")[0].strip().rstrip(";")
    trees = []
    for fx, title, cls in (("shortlist", "rewritten", "hw--a"), ("rewrite-shortlist-solver", "solver", "hw--b")):
        nodes = b.plan_json(b.FX.FIXTURES[fx]["setup"], query)
        if not nodes:
            b.FAILURES.append(f"docs/rewriting.html hero: no plan for {fx}")
            return ""
        lines = b.plan_lines(nodes)
        trees.append((title, cls, lines))
    if not any("query rewriting" in line for line in trees[0][2]) or not any("DECIDE" in line for line in trees[1][2]):
        b.FAILURES.append("docs/rewriting.html hero: expected a rewritten plan and a DECIDE plan")
    cols, said = [], []
    for title, _, lines in trees:
        lines = [re.sub(r"\s+query rewriting, rule (\w\d)\w*", r"  rule \1", line) for line in lines]
        lines = [re.sub(r"(?<=\S)\s{3,}(?:decisions: )?", "  ", line) for line in lines]
        rows = "".join(f'<div class="hw__tree" data-step>{esc(line)}</div>' for line in lines)
        head = "rewritten" if title == "rewritten" else "on the solver"
        cols.append(
            f'<div><div class="hw__col">{head}</div><div class="hw__trees" data-ly-steps="160">{rows}</div></div>'
        )
        said.append(f"{head}: " + ", ".join(line.strip("└├│─ ") for line in lines))
    return window(
        "shortlist.sql · EXPLAIN",
        f'<div class="hw__split hw__split--plan">{"".join(cols)}</div>',
        "The shortlist query's plan, " + "; and ".join(said) + ".",
        "hw--mid",
    )


# the nine rules in words, as the novel features page names them
RULE_WORDS = {
    "S1": "the best rows by score",
    "S2": "the best rows under nested quotas",
    "S3": "as many rows as fit a budget",
    "A1": "each row on its own",
    "A2": "one value closest to every row",
    "R1": "fill a budget by value per unit",
    "R4": "a fair split of a budget",
    "N1": "stay inside one sum of squares",
    "O1": "match two tables one to one",
}


def rules_picture(b):
    """The rules the rules page proves, in its order: each name is a direct= the build checked on that page."""
    import os

    src = open(os.path.join(b.HERE, "pages", "docs", "rewrite-rules.html")).read()
    names = list(dict.fromkeys(re.findall(r'direct="([A-Z0-9_]+)"', src)))
    if len(names) < 9:
        b.FAILURES.append(f"docs/rewrite-rules.html hero: {len(names)} rules proved on the page, expected 9")
    rows = "".join(
        f'<div class="hw__rule" data-step><b>{esc(n[:2])}</b><code>{esc(n[3:])}</code><span>{esc(RULE_WORDS.get(n[:2], ""))}</span></div>'
        for n in names
    )
    return window(
        "EXPLAIN · the rule each example fires",
        f'<div class="hw__rules" data-ly-steps="200">{rows}</div>',
        "The nine rewrite rules: " + ", ".join(names) + ".",
        "hw--mid",
    )


# the router, as decide_router.cpp maps a solver status to an outcome under DIAGNOSE
ROUTES = [
    ("OPTIMAL, SUBOPTIMAL", "one row: state feasible"),
    ("INFEASIBLE", "elastic model: the smallest edit"),
    ("UNBOUNDED", "ray: the decision that needs a limit"),
    ("INF_OR_UNBD", "a probe with no objective, then one of the two"),
    ("TIME_LIMIT, ITERATION_LIMIT, OTHER", "undiagnosed: the plain error"),
]


def diagnose_picture(b):
    rows = "".join(
        f'<div class="hw__route" data-step><code>{esc(st)}</code><span aria-hidden="true">→</span><b>{esc(out)}</b></div>'
        for st, out in ROUTES
    )
    return window(
        "DIAGNOSE · the router",
        f'<div class="hw__q">The solver says</div><div class="hw__routes" data-ly-steps="300">{rows}</div>',
        "The DIAGNOSE router: " + "; ".join(f"{st} leads to {out}" for st, out in ROUTES) + ".",
        "hw--mid",
    )


# the direct-solve benchmark at 800,000 partsupp rows (Apple M3 Pro, 9 to 12 November 2026, median of 3), from the
# performance page's table: seconds rewritten, seconds on Gurobi
PERF = [
    ("A1", 0.028, 0.353),
    ("A2", 0.026, 0.077),
    ("A2 abs", 0.039, 12.322),
    ("N1", 0.032, 3.295),
    ("R1", 0.120, 1.386),
    ("S1", 0.044, 3.046),
    ("S3", 0.207, 22.978),
]


def performance_picture(b):
    import math

    lo, hi = math.log10(0.01), math.log10(100)

    def width(t):
        return f"{100 * (math.log10(t) - lo) / (hi - lo):.1f}%"

    rows = "".join(
        f'<div class="hw__perf" data-step><b>{esc(rule)}</b><div class="hw__bars">'
        f'<span class="hw__pline"><span class="hw__pbar hw__pbar--r" style="--w:{width(r)}"></span><i>{r:g} s</i></span>'
        f'<span class="hw__pline"><span class="hw__pbar hw__pbar--g" style="--w:{width(g)}"></span><i>{g:g} s</i></span>'
        "</div></div>"
        for rule, r, g in PERF
    )
    legend = '<div class="hw__legend"><span class="hw__key hw__key--r">rewritten</span><span class="hw__key hw__key--g">Gurobi</span><span>seconds, log scale</span></div>'
    return window(
        "benchmark · 800,000 rows",
        f'{legend}<div class="hw__perfs" data-ly-steps="220">{rows}</div>',
        "Seconds at 800,000 rows, rewritten against Gurobi: "
        + "; ".join(f"{rule} {r} s against {g} s" for rule, r, g in PERF)
        + ".",
        "hw--mid",
    )


def solving_picture(b):
    """The running example of the solving page and the model DeciDB builds for it (DECIDB_DUMP_MODEL)."""
    query = """SELECT item, x, open, cap
FROM Items
DECIDE x(BOOL),
  per category: open(BOOL),
  per (): cap(INT)
SUCH THAT x <= open
  AND SUM(x * weight) <= cap
  AND cap <= 40
MAXIMIZE SUM(x * value)
  - SUM(per category: 30 * open)
  - cap"""
    ok, out, err = b.run_sql(
        b.FX.FIXTURES["items"]["setup"] + "\n" + query + ";", json_mode=False, env={"DECIDB_DUMP_MODEL": "1"}
    )

    m = re.search(r"=== DECIDB MODEL DUMP ===\n(.*?)\n=== END MODEL DUMP ===", err, re.S)
    if not ok or not m:
        b.FAILURES.append(f"docs/solving.html hero: no model dump: {err[:200]}")
        return ""
    lines = []
    for line in m.group(1).splitlines():
        f = dict(re.findall(r"(\w+)=(\S+)", line))
        if line.startswith("col"):
            kind = "bin" if f.get("bin") == "1" else "int" if f.get("int") == "1" else "real"
            lines.append(f"{line.split(':')[0]} {kind:<4} {f['lb']} to {f['ub']}, obj {f['obj']}")
        elif line.startswith("row"):
            sense = {"<": "≤", ">": "≥"}.get(f.get("sense"), f.get("sense", ""))
            lines.append(f"{line.split(':')[0]} {line.split('|')[1].strip()} {sense} {f['rhs']}")
    rows = "".join(f'<div class="hw__dump" data-step>{esc(line)}</div>' for line in lines)
    return window(
        "DECIDB_DUMP_MODEL=1",
        '<div class="hw__split hw__split--model">'
        f'<div><div class="hw__col">the query</div><pre class="hw__sql"><code>{b.hl_sql(query)}</code></pre></div>'
        f'<div><div class="hw__col">the model</div><div class="hw__dumps" data-ly-steps="140">{rows}</div></div></div>',
        f"A query with per-row, per-category and query-wide decisions, and the model DeciDB builds for it: "
        f"{sum(1 for x in lines if x.startswith('col'))} columns and {sum(1 for x in lines if x.startswith('row'))} rows.",
        "hw--mid",
    )


# a correctness test, as test/decide/comparison/compare.py judges it: two statements of one problem must agree
TEST_LANES = [
    ("DeciDB", ["the DECIDE query", "the decidb binary", "its rows and objective"]),
    ("The oracle", ["the same rows, read by stock DuckDB", "a gurobipy model written by hand", "Gurobi's optimum"]),
]


def testing_picture(b):
    lanes = "".join(
        f'<div><div class="hw__col">{esc(name)}</div>'
        + "".join(f'<div class="hw__lane" data-step>{esc(step)}</div>' for step in steps)
        + "</div>"
        for name, steps in TEST_LANES
    )
    verdict = (
        '<div class="hw__verdict" data-step><code>compare_solutions</code><span>objective within 1e-4: '
        "<b>identical</b> when every decision matches, <b>optimal</b> when only the objective does</span></div>"
    )
    return window(
        "a correctness test",
        f'<div data-ly-steps="260"><div class="hw__split hw__split--lanes">{lanes}</div>{verdict}</div>',
        "A correctness test: DeciDB answers the query, a hand-written gurobipy model answers the same problem on the "
        "same rows, and compare_solutions requires the objectives to agree within 1e-4.",
        "hw--mid",
    )


DOC_PICTURES = {
    "testing.html": testing_picture,
    "rewriting.html": rewriting_picture,
    "rewrite-rules.html": rules_picture,
    "diagnose.html": diagnose_picture,
    "performance.html": performance_picture,
    "solving.html": solving_picture,
}


def docs(b, slug, label, meta):
    """A documentation page's opening: the page's words, and its picture: the DECIDE path with the stages it covers
    lit, or the page's own window drawn from what the build runs."""
    trail = ["Documentation"] if slug == "index.html" else [("docs/index.html", "Documentation"), label]
    draw = DOC_PICTURES.get(slug)
    picture = (draw(b) if draw else "") or pipeline_window(slug)
    return hero(b, "../", trail, meta.get("h1", label), meta.get("lede", ""), picture, 20 + len(slug) % 7)
