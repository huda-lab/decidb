"""The DeciDB home page, built to the taste-skill page plan for a software page.

Every query, result, error and EXPLAIN line on it is run on the DeciDB binary by build.py when the site is built:
render() takes the build module (b) and uses its helpers, so a query that stops working fails the build.
"""

import html
import json
import re

esc = html.escape

KNAPSACK = """SELECT item, value, weight, x AS take
FROM Items
DECIDE x(BOOL)
SUCH THAT SUM(x * weight) <= 50
MAXIMIZE SUM(x * value);"""

DIAGNOSE = """DIAGNOSE
SELECT item, x FROM Items
DECIDE x(BOOL)
SUCH THAT SUM(x * weight) <= 40
  AND SUM(x) >= 4
MAXIMIZE SUM(x * value);"""

FAIR = """SELECT program, alloc
FROM Programs
DECIDE alloc(REAL)
SUCH THAT alloc <= max_alloc
  AND SUM(alloc) <= 100
MAXIMIZE MIN(alloc / need);"""

WINDOW_LIST = [
    ("knapsack", "Knapsack"),
    ("production", "Production plan"),
    ("facility-location", "Depots"),
    ("fair-allocation", "Fair allocation"),
    ("matching", "Matching"),
]


def num(v):
    if isinstance(v, float):
        return f"{v:g}"
    return str(v)


def grid(cols, rows, mark=None, steps=False, cls="", lead="1.4fr"):
    """A result as the window draws it: a header row and one row per tuple, aligned on a CSS grid (the first column
    takes `lead` to the others' 1fr)."""
    style = f' style="--tpl: minmax(0, {lead}) repeat({len(cols) - 1}, minmax(0, 1fr))"'
    head = "".join(f'<span class="{"r" if isinstance(rows[0][c], (int, float)) else ""}">{esc(c)}</span>' for c in cols)
    out = [f'<div class="sq__grid {cls}"{style}><div class="sq__tr sq__th">{head}</div>']
    for r in rows:
        on = " is-on" if mark and r.get(mark) not in (None, 0, False) else ""
        step = " data-step" if steps else ""
        cells = "".join(
            f'<span class="{"r" if isinstance(r[c], (int, float)) else ""}">{esc(num(r[c]) if r[c] is not None else "NULL")}</span>'
            for c in cols
        )
        out.append(f'<div class="sq__tr{on}"{step}>{cells}</div>')
    out.append("</div>")
    return "".join(out)


def hero(b, ex_by_slug):
    setup = ex_by_slug["knapsack"]["setup"]
    rows = b.rows_of("index.html hero", setup, KNAPSACK)
    taken = [r for r in rows if r["take"]]
    value = sum(r["value"] for r in taken)
    weight = sum(r["weight"] for r in taken)
    names = ", ".join(r["item"].lower() for r in taken[:-1]) + " and " + taken[-1]["item"].lower() if taken else ""
    label = (
        f"A DeciDB session: the knapsack query takes the {names}, worth {value} at {weight} kg of the 50 allowed. "
        "Beside it, a list of other example problems."
    )
    items = []
    for slug, name in WINDOW_LIST:
        ex = ex_by_slug[slug]
        how = "rewritten" if ex.get("direct") else ex["class"]
        on = " is-on" if slug == "knapsack" else ""
        items.append(f'<div class="hw__row{on}"><b>{esc(name)}</b><span>{esc(how)}</span></div>')
    status = (
        f'<div class="hw__status" data-step><i aria-hidden="true"></i>{len(taken)} of {len(rows)} items · value {value} '
        f"· {weight} of 50 kg</div>"
    )
    return f"""
  <section class="hero hero--centred" data-enter aria-labelledby="hero-h">
    <div class="wrap"><div class="hero-text">
      <div class="hero__c">
        <h1 id="hero-h">Optimization in DuckDB</h1>
        <p class="hero__lead">Add DECIDE to a SQL query and get the best values back as rows.</p>
        <div class="btn-row"><a class="btn btn--primary" href="getting-started.html#install">Install DeciDB</a><a class="btn btn--secondary" href="guide.html">Read the guide</a></div>
      </div>
      <div class="hero__window" data-fx="grain-gradient" data-fx-seed="7" data-ui-pause-host>
        <div class="ly-piece ui ui-hw hw" role="img" aria-label="{esc(label)}">
          <div class="hw__bar"><span class="hw__dots"><i></i><i></i><i></i></span><span><b>decidb</b> · knapsack.sql</span><span class="hw__sp"></span><span class="hw__meta">in-memory</span></div>
          <div class="hw__body">
            <div class="hw__list"><div class="hw__q">Examples</div>{"".join(items)}</div>
            <div class="hw__main">
              <pre class="hw__sql"><code>{b.hl_sql(KNAPSACK)}</code></pre>
              <div class="hw__out" data-ly-steps="520">{grid(["item", "value", "weight", "take"], rows, mark="take", steps=True)}{status}</div>
            </div>
          </div>
        </div>
      </div>
    </div></div>
  </section>"""


PAINT = '{"bg":"#a3b0b6","accent":"#c49a6c"}'
PAINT_PARAMS = '{"blur":1,"brush":0.35,"slope":0,"hillY":0.3,"horizon":0.62,"drift":0,"speed":0}'
SAGE = '{"bg":"#cdd6c6","surface":"#dce3d6","ink":"#262d24","accent":"#6c8e63"}'
SKY = '{"bg":"#c6d3df","surface":"#d6e0e9","ink":"#25303a","accent":"#7f9fbd"}'
STILL = '{"drift":0,"speed":0}'

# the same bag, solved the usual way: read the rows out, model them in a solver library, write the answer back
# (checked by hand with PuLP 2.9 on the Items table: it takes the laptop, camera and lens, as the DECIDE query does)
WITHOUT = """import duckdb, pulp

rows = duckdb.sql("SELECT * FROM Items").fetchall()

bag = pulp.LpProblem("bag", pulp.LpMaximize)
take = {i: pulp.LpVariable(i, cat="Binary") for i, _, _ in rows}
bag += pulp.lpSum(v * take[i] for i, v, _ in rows)
bag += pulp.lpSum(w * take[i] for i, _, w in rows) <= 50
bag.solve()

answer = [(i, round(take[i].value())) for i, _, _ in rows]
duckdb.sql("CREATE TABLE Answer (item VARCHAR, take INT)")
duckdb.executemany("INSERT INTO Answer VALUES (?, ?)", answer)"""

# the problems on the home page: one of each kind, each linking to its example page, with lines from its query and
# the columns of its answer that show the decision (a column repeated across a key's rows is shown once per key)
PROBLEMS = [
    (
        "knapsack",
        "Budget constraints",
        "Pick rows under a weight or cost limit.",
        "DECIDE x(BOOL)\nSUCH THAT SUM(x * weight) <= 50\nMAXIMIZE SUM(x * value)",
        ["item", "take"],
    ),
    (
        "production",
        "Production plans",
        "How much each plant makes.",
        "DECIDE units(INT)\nSUCH THAT units <= capacity\n  AND SUM(units) >= 500\nMINIMIZE SUM(units * unit_cost)",
        ["plant", "units"],
    ),
    (
        "assignment",
        "Assignment decisions",
        "One person for every task.",
        "DECIDE assign(BOOL)\nSUCH THAT per task:\n  SUM(assign) by (task) = 1\nMAXIMIZE SUM(assign * score)",
        ["task", "worker"],
    ),
    (
        "facility-location",
        "Depot decisions",
        "Which depots open, and what ships.",
        "DECIDE per D: open(BOOL), ...\nSUCH THAT per D: SUM(ship)\n  by (D) <= D.capacity * open\nMINIMIZE ... +\n  SUM(D: D.opening_cost * open)",
        ["depotID", "open"],
    ),
]


def card(title, line, seed, palette, params, label, inner, fit=420):
    """An open card: its title and line on the page, and under them a painted panel holding a drawn pane whole."""
    return f"""
          <article class="card card--row card--open span-6 m-nolift">
            <h3>{title}</h3>
            <p>{line}</p>
            <div class="panel" data-fx="painterly-landscape" data-fx-seed="{seed}" data-fx-palette='{palette}' data-fx-params='{params}' style="background-color: {json.loads(palette)['bg']}">
              <div class="stage" role="img" aria-label="{esc(label)}"><div class="at-rise fit" style="--fit-max:{fit}px;--ax:max(24px, 50% - {fit // 2}px);--ay:32px;--ay-sm:20px">
                {inner}
              </div></div>
            </div>
          </article>"""


def cards(name, heading, items, link=""):
    """A section of cards under one heading: side by side on a laptop (the groups dissolve into the section's
    grid), and on a phone the heading with the first half, then the rest, one screen each."""
    half = (len(items) + 1) // 2
    end = f'\n        <p class="group__end">{link}</p>' if link else ""
    return f"""
  <section class="section g2m pair" id="{name}" aria-labelledby="{name}-h">
    <div class="wrap grid">
      <div class="group">
        <div class="section-head"><h2 id="{name}-h">{heading}</h2></div>{"".join(items[:half])}
      </div>
      <div class="group">{"".join(items[half:])}{end}
      </div>
    </div>
  </section>"""


def why(b, ex_by_slug):
    rows = b.rows_of("index.html why", ex_by_slug["knapsack"]["setup"], KNAPSACK)
    taken = [r["item"] for r in rows if r["take"]]
    without = card(
        "Without DeciDB",
        "Read the rows out, rebuild the problem in a solver library, solve it, and load the answer back in.",
        31,
        SKY,
        STILL,
        "Thirteen lines of Python: read the items from DuckDB, build a PuLP model with one binary variable per item, "
        "solve it, and insert the answer into a new table.",
        f'<div class="ui ui-sq sq" aria-hidden="true"><pre class="sq__code"><code>{b.highlight(WITHOUT, "python")}</code></pre></div>',
        fit=520,
    )
    with_ = card(
        "With DeciDB",
        "One query over the table. The answer comes back as rows, ready to join with the rest of your data.",
        32,
        SAGE,
        STILL,
        f"One DECIDE query over the same table; it returns every item with take set to 1 for the {', '.join(taken)}.",
        f'<div class="ui ui-sq sq" aria-hidden="true"><pre class="sq__code"><code>{b.hl_sql(KNAPSACK)}</code></pre>'
        f'<div class="sq__out">{grid(["item", "value", "weight", "take"], rows, mark="take")}</div></div>',
        fit=520,
    )
    return cards("why", "Why optimization belongs in the database", [without, with_])


def problems(b, ex_by_slug):
    items = []
    for slug, title, line, lines, cols in PROBLEMS:
        ex = ex_by_slug[slug]
        shown, seen = [], set()
        for r in b.rows_of("index.html problems", ex["setup"], ex["query"]):
            row = {c: r[c] for c in cols}
            if tuple(row.values()) not in seen:
                seen.add(tuple(row.values()))
                shown.append(row)
        mark = cols[-1] if isinstance(shown[0][cols[-1]], int) else None
        items.append(
            f'<a class="prob span-3" href="examples/{slug}.html"><h3>{esc(title)}</h3><p>{esc(line)}</p>'
            f'<div class="ui ui-sq sq" aria-hidden="true"><pre class="sq__code"><code>{b.hl_sql(lines)}</code></pre>'
            f'<div class="sq__out">{grid(cols, shown, mark=mark)}</div></div></a>'
        )
    items = "".join(items)
    return f"""
  <section class="section" id="problems" aria-labelledby="problems-h">
    <div class="wrap">
      <div class="section-head">
        <h2 id="problems-h">Problems DeciDB solves. <span class="muted">Choosing and allocating over rows.</span></h2>
      </div>
      <div class="grid probs fill">{items}</div>
      <p class="group__end"><a class="link" href="examples.html">See all {len(b.EX.EXAMPLES)} examples{b.ARROW}</a></p>
    </div>
  </section>"""


def novel(b, ex_by_slug):
    ex = ex_by_slug["fair-allocation"]
    rule = b.explain_fields(ex["setup"], FAIR).get("Direct solve rule", "")
    if not rule.startswith("R4"):
        b.FAILURES.append(f"index.html novel: expected the R4 rewrite, got {rule!r}")
    b.rows_of("index.html novel", ex["setup"], FAIR)
    rewrite = card(
        "Query rewriting",
        "A query with an exact answer, such as a fair split of a budget, runs as sorting and aggregation in DuckDB, "
        "with no solver: 0.026 s at 800,000 decisions, where Gurobi took 2,158 s.",
        41,
        PAINT,
        PAINT_PARAMS,
        f"A fair-allocation query, and the line EXPLAIN prints for it: Direct solve rule {rule}.",
        f'<div class="ui ui-sq sq" aria-hidden="true"><pre class="sq__code"><code>{b.hl_sql(FAIR)}</code></pre>'
        f'<div class="sq__explain"><div class="sq__kv"><span>EXPLAIN</span><b>Direct solve rule: {esc(rule)}</b></div></div></div>',
    )

    drows = b.rows_of("index.html novel", ex_by_slug["diagnose"]["setup"], DIAGNOSE)
    fix = [r for r in drows if r.get("clause")]
    first = fix[0] if fix else {"clause": "", "suggested_change": ""}
    target = [r for r in drows if r.get("edit_source") == "achievable_objective"]
    objective = num(target[0]["amount"]) if target else ""
    shown = [{"suggested_change": r["suggested_change"], "amount": r["amount"]} for r in fix]
    limit = re.search(r"(\d+(?:\.\d+)?)\s*$", first["suggested_change"] or "")
    diagnose = card(
        "DIAGNOSE for a query with no answer",
        "Put it in front of the query and it returns the clause to change and by how much: here, a weight limit of "
        f"{limit.group(1) if limit else '?'} instead of 40.",
        42,
        SAGE,
        STILL,
        f"DIAGNOSE suggests changing {first['clause']} to {first['suggested_change']}, which reaches an objective of {objective}.",
        f'<div class="ui ui-sq sq" aria-hidden="true"><pre class="sq__code"><code>{b.hl_sql(DIAGNOSE)}</code></pre>'
        f'<div class="sq__out">{grid(["suggested_change", "amount"], shown, lead="3fr") if shown else ""}</div></div>',
    )
    return cards(
        "novel",
        'Novel features. <span class="muted">Two things a solver library does not do.</span>',
        [rewrite, diagnose],
        f'<a class="link" href="features.html">Read about query rewriting and DIAGNOSE{b.ARROW}</a>',
    )


def statement(b):
    parts = [
        ("DECIDE x(BOOL)", "The decisions: what DeciDB chooses, one per row, per key or per query."),
        ("SUCH THAT ...", "The constraints: the rules every answer follows, on each row or on totals."),
        ("MAXIMIZE ...", "The objective: what makes an answer best. Leave it out to ask for any answer that fits."),
    ]
    cols = "".join(f'<div class="span-4 part"><code>{b.hl_sql(c)}</code><p>{esc(t)}</p></div>' for c, t in parts)
    return f"""
  <section class="section bg-mist" id="statement" aria-labelledby="statement-h">
    <div class="wrap">
      <div data-moment="liquid-type" class="section-head statement">
        <h2 id="statement-h" class="mo-lt__type">The query is the model</h2>
        <p>Three clauses added to a SELECT. The rows stay in DuckDB from the scan to the answer.</p>
      </div>
      <div class="grid parts">{cols}</div>
      <p class="group__end"><a class="link" href="guide.html">Read the guide, clause by clause{b.ARROW}</a></p>
    </div>
  </section>"""


def solvers(b):
    spec = {
        "layout": "hub",
        "alt": "DeciDB at the centre, reading DuckDB tables and Parquet files, solving with Gurobi or HiGHS, called from Python or the command line.",
        "center": {"label": "DeciDB"},
        "motion": {"pulse": "none"},
        "spokes": [
            {"label": "DuckDB", "icon": "si:duckdb"},
            {"label": "Parquet", "icon": "si:apacheparquet"},
            {"label": "Gurobi", "icon": "si:gurobi"},
            {"label": "HiGHS", "icon": "ui:sigma"},
            {"label": "Python", "icon": "python"},
            {"label": "Command line", "icon": "ui:terminal"},
        ],
    }
    return f"""
  <section class="section split m2" id="solvers" aria-labelledby="solvers-h">
    <div class="wrap grid grid--top">
      <div class="span-5 split-col">
        <h2 id="solvers-h">Built on DuckDB, solved by Gurobi or HiGHS</h2>
        <p class="t-lead">DeciDB is DuckDB with the DECIDE clause added, so it reads the same tables and files. HiGHS ships in every build; Gurobi is used when its license is present.</p>
        <ul class="facts">
          <li>The data never leaves DuckDB: the model is built from the query's own rows</li>
          <li>Python: <code>pip install decidb</code>, with DuckDB's API</li>
          <li>A command-line shell for macOS, Linux and Windows</li>
          <li>You state the rules and the goal; DeciDB writes the model the solver reads</li>
        </ul>
        <p class="split-col__end"><a class="link" href="guide.html#solvers">See which solver takes what{b.ARROW}</a></p>
      </div>
      <div class="frame frame--split frame--lit span-6 start-7 dg-frame">
        <div class="dg" data-diagram data-dg-still><script type="application/json">{json.dumps(spec)}</script></div>
      </div>
    </div>
  </section>"""


PLAN = """<!-- type: software (a developer tool: DuckDB with a DECIDE clause)
     deal: hero centred, palette white-link-blue (a white sheet and a biro blue, the blue of DeciDB's own
     logo, for a tool whose answers are tables of numbers), type geist, radius sharp, nav mega (nav-cards
     dealt first; it needs two to four products and DeciDB is one), close word, footer statement, rhythm
     space, buttons pill, mark: DeciDB's own logo, lit ground sage, ground statement bg-mist,
     effect grain-gradient in the hero frame, card ground painterly-landscape
     moments: liquid-type in #statement (the one sentence that sums DeciDB up answers the pointer)
     motion: hero entrance (the name first, then what to do); the window's result rows arrive (a query
     returning its rows); hover on every link and button
     sections, in the order of the live site: hero, why in the database, the three clauses, problems, novel
     features (query rewriting, DIAGNOSE), built on DuckDB, get started -->"""


def render(b):
    ex_by_slug = {ex["slug"]: ex for ex in b.EX.EXAMPLES}
    head = b.head(
        "DeciDB: optimization queries in DuckDB",
        "DeciDB adds a DECIDE clause to DuckDB: state decisions, constraints and a goal in SQL and get the best "
        "values back as rows. Query rewriting answers nine problem classes without a solver; the rest go to Gurobi or "
        "HiGHS.",
        "",
        extra=(
            '<script src="assets/backgrounds/backgrounds.js"></script>\n'
            "<script>document.documentElement.classList.add('mo-js');"
            "setTimeout(()=>document.documentElement.classList.add('mo-safe'),2500)</script>\n"
            '<link rel="modulepreload" href="assets/moments/moments.js">\n'
            '<link rel="modulepreload" href="assets/moments/lib.js">\n'
            '<link rel="modulepreload" href="assets/moments/liquid-type/moment.js">\n'
            '<link rel="stylesheet" href="assets/diagrams/diagram.css">\n'
        ),
    )
    main = (
        hero(b, ex_by_slug)
        + why(b, ex_by_slug)
        + statement(b)
        + problems(b, ex_by_slug)
        + novel(b, ex_by_slug)
        + solvers(b)
    )
    return f"""{head}
<body class="home">
{PLAN}
{b.header("", "")}
<main id="top">{main}
</main>
{b.closing("")}
{b.scripts("", '<script type="module" src="assets/diagrams/mount.js"></script>' + chr(10))}
</body>
</html>
"""
