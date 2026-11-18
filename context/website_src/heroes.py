"""The openings of the reading pages, in the home page's manner: a short hero with the words on the left (the
breadcrumb, the h1, one line, the page's actions) and on the right a drawn DeciDB window standing on the moving lit
ground, its result rows arriving as the home page's do.

Every query, result and EXPLAIN line in a window is run on the DeciDB binary by build.py when the site is built:
each function takes the build module (b) and uses its helpers, so a query that stops working fails the build.
"""

import html

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
    b.rows_of("documentation.html hero", {e["slug"]: e for e in b.EX.EXAMPLES}["knapsack"]["setup"], KNAPSACK)
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
    "documentation.html": documentation,
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
