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

TECH_LIMIT = """SELECT item, category, x
FROM Items
DECIDE x(BOOL)
SUCH THAT SUM(x * weight) <= 50
  AND when category = 'tech': SUM(x) <= 1
MAXIMIZE SUM(x * value);"""

PER_DEPOT = "per D.depotID: ship <= 10"

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


def grid(cols, rows, mark=None, steps=False, cls=""):
    """A result as the window draws it: a header row and one row per tuple, aligned on a CSS grid."""
    style = f' style="--tpl: minmax(0, 1.4fr) repeat({len(cols) - 1}, minmax(0, 1fr))"'
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
        how = f'direct solve {ex["direct"].split("_")[0]}' if ex.get("direct") else ex["class"]
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
        <p class="pill"><span>New in DeciDB</span><span class="muted">Direct solve</span></p>
        <h1 id="hero-h">Optimization in DuckDB</h1>
        <p class="hero__lead">Add DECIDE to a SQL query and get the best values back as rows.</p>
        <div class="btn-row"><a class="btn btn--primary" href="getting-started.html#install">Install DeciDB</a><a class="btn btn--secondary" href="documentation.html">Read the guide</a></div>
      </div>
      <div class="hero__window" data-fx="grain-gradient" data-fx-seed="7" role="img" aria-label="{esc(label)}">
        <div class="ly-piece ui ui-hw hw" aria-hidden="true">
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
SKY = '{"bg":"#c6d3df","surface":"#d6e0e9","ink":"#25303a","accent":"#3f6a92"}'
STILL = '{"drift":0,"speed":0}'


def card(title, line, seed, palette, params, label, inner):
    return f"""
          <article class="card card--row card--open span-6 m-nolift">
            <h3>{title}</h3>
            <p>{line}</p>
            <div class="panel" data-fx="painterly-landscape" data-fx-seed="{seed}" data-fx-palette='{palette}' data-fx-params='{params}'>
              <div class="stage stage--fade-b" role="img" aria-label="{esc(label)}"><div class="at-rise fit" style="--fit-max:420px;--ax:max(24px, 50% - 210px);--ay:44px;--ay-sm:20px">
                {inner}
              </div></div>
            </div>
          </article>"""


def language(b, ex_by_slug, fx):
    keyed = ex_by_slug["keyed-decisions"]
    krows = b.rows_of("index.html language", keyed["setup"], keyed["query"])
    seen, pairs = set(), []
    for r in krows:
        if r["name"] not in seen:
            seen.add(r["name"])
            pairs.append({"name": r["name"], "keep": r["keep"]})
    kept = [p["name"] for p in pairs if p["keep"]]
    key_sql = "DECIDE per n: keep(BOOL)\nSUCH THAT SUM(keep * s.hours) <= 60\nMAXIMIZE SUM(per n: keep * n.skill_score)"
    c1 = card(
        "Declares one decision per key",
        "<code>per n: keep(BOOL)</code> gives each nurse one decision, shared by every shift row the join returns.",
        21,
        PAINT,
        PAINT_PARAMS,
        f"A keyed DECIDE query over nurses and their shifts keeps {', '.join(kept)}: one value per nurse.",
        f'<div class="ui ui-sq sq" aria-hidden="true"><pre class="sq__code"><code>{b.hl_sql(key_sql)}</code></pre>'
        f'<div class="sq__out" data-ly-steps="520">{grid(["name", "keep"], pairs, mark="keep", steps=True)}</div></div>',
    )

    trows = b.rows_of("index.html language", fx["items"]["setup"], TECH_LIMIT)
    when_sql = "SUCH THAT SUM(x * weight) <= 50\n  AND when category = 'tech': SUM(x) <= 1"
    c2 = card(
        "Limits a constraint to the rows it names",
        "<code>when</code> picks the rows a constraint reads. Here the bag takes at most one tech item.",
        22,
        SAGE,
        STILL,
        "With at most one tech item, DeciDB takes " + ", ".join(r["item"] for r in trows if r["x"]) + ".",
        f'<div class="ui ui-sq sq" aria-hidden="true"><pre class="sq__code"><code>{b.hl_sql(when_sql)}</code></pre>'
        f'<div class="sq__out">{grid(["item", "category", "x"], trows, mark="x")}</div></div>',
    )

    deck = fx["deck"]
    message = b.error_of(
        "index.html language", deck["setup"], deck["template"].replace("{CLAUSES}", PER_DEPOT), "does not determine"
    )
    message = message.replace("Invalid Input Error: ", "")
    c3 = card(
        "Refuses a constraint the data cannot decide",
        "A depot ships several rows, so a cap per depot on a decision per row stops before solving.",
        23,
        SKY,
        STILL,
        "The query stops with: " + message,
        f'<div class="ui ui-sq sq" aria-hidden="true"><pre class="sq__code"><code>{b.hl_sql("DECIDE ship(INT)")}\n'
        f'<span class="tk-k">SUCH THAT</span> ... <span class="tk-k">AND</span> {b.hl_sql(PER_DEPOT)}</code></pre>'
        f'<div class="sq__err"><b>Error</b> {esc(message)}</div></div>',
    )

    drows = b.rows_of("index.html language", ex_by_slug["diagnose"]["setup"], DIAGNOSE)
    fix = [r for r in drows if r.get("clause")]
    first = fix[0] if fix else {"clause": "", "suggested_change": "", "amount": 0}
    target = [r for r in drows if r.get("edit_source") == "achievable_objective"]
    objective = num(target[0]["amount"]) if target else ""
    shown = [{"suggested_change": r["suggested_change"], "amount": r["amount"]} for r in fix]
    limit = re.search(r"(\d+(?:\.\d+)?)\s*$", first["suggested_change"] or "")
    c4 = card(
        "Explains a query with no answer",
        "<code>DIAGNOSE</code> returns the clause to change and by how much: here, a weight limit of "
        f"{limit.group(1) if limit else '?'} instead of 40.",
        24,
        SAGE,
        STILL,
        f"DIAGNOSE suggests changing {first['clause']} to {first['suggested_change']}, which reaches an objective of {objective}.",
        f'<div class="ui ui-sq sq" aria-hidden="true"><pre class="sq__code"><code>{b.hl_sql(DIAGNOSE)}</code></pre>'
        f'<div class="sq__out">{grid(["suggested_change", "amount"], shown) if shown else ""}</div></div>',
    )
    return f"""
  <section class="section g2 g2m" id="language" aria-labelledby="language-h">
    <div class="wrap">
      <div class="group">
        <div class="section-head">
          <h2 id="language-h">Write the model in SQL. <span class="muted">Decisions, rules and the goal sit in one DECIDE query.</span></h2>
        </div>
        <div class="grid">{c1}{c2}
        </div>
      </div>
      <div class="group">
        <div class="grid">{c3}{c4}
        </div>
        <p class="group__end"><a class="link" href="documentation.html#scopes">Read about scopes{b.ARROW}</a></p>
      </div>
    </div>
  </section>"""


def direct(b, ex_by_slug):
    ex = ex_by_slug["fair-allocation"]
    fields = b.explain_fields(ex["setup"], FAIR)
    rule = fields.get("Direct solve rule", "")
    if not rule.startswith("R4"):
        b.FAILURES.append(f"index.html direct: expected direct solve R4, got {rule!r}")
    rows = b.rows_of("index.html direct", ex["setup"], FAIR)
    shown = [{"program": r["program"], "alloc": round(r["alloc"], 2)} for r in rows]
    explain = "".join(
        f'<div class="sq__kv"><span>{esc(k)}</span><b>{esc(v)}</b></div>'
        for k, v in (
            ("Direct solve rule", rule),
            ("Direct solve condition", fields.get("Direct solve condition", "")),
        )
    )
    return f"""
  <section class="section" id="direct" aria-labelledby="direct-h">
    <div class="wrap">
      <div class="grid grid--top fill">
        <div class="span-5 split-col">
          <h2 id="direct-h">9 problem classes skip the solver</h2>
          <p class="t-lead">When a query matches a class with an exact answer, DeciDB computes it with DuckDB's own sorting and aggregation. The query stays the same, and <code>EXPLAIN</code> names the rule.</p>
          <ul class="facts">
            <li>Fair allocation over 800,000 decisions: 0.026 s, where Gurobi took 2,158 s</li>
            <li>Faster than Gurobi in all nine classes, at every size measured</li>
            <li><code>SET decide_direct_solve = 'off'</code> sends a query back to the solver</li>
          </ul>
          <p class="split-col__end"><a class="link" href="problems.html#direct-solve">See all 9 classes{b.ARROW}</a></p>
        </div>
        <div class="frame frame--split frame--lit span-6 start-7">
          <div class="stage stage--fade-br" role="img" aria-label="EXPLAIN on a fair-allocation query names the direct-solve rule {esc(rule)}, and the query returns each program's share.">
            <div class="at-corner to-edge">
              <div class="ui ui-sq sq sq--wide" aria-hidden="true">
                <div class="sq__bar"><span class="hw__dots"><i></i><i></i><i></i></span><span><b>decidb</b> · fair.sql</span></div>
                <pre class="sq__code"><code><span class="tk-k">EXPLAIN</span> {b.hl_sql(FAIR)}</code></pre>
                <div class="sq__explain">{explain}</div>
                <div class="sq__out">{grid(["program", "alloc"], shown)}</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </section>"""


def statement(b):
    parts = [
        ("DECIDE x(BOOL)", "The decisions: one per row, per key or per query."),
        ("SUCH THAT ...", "The constraints, on each row or on sums over groups."),
        ("MAXIMIZE ...", "The goal; leave it out to ask for any answer that fits."),
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
  <section class="section" id="solvers" aria-labelledby="solvers-h">
    <div class="wrap">
      <div class="grid grid--top fill">
        <div class="span-5 split-col">
          <h2 id="solvers-h">Built on DuckDB, solved by Gurobi or HiGHS</h2>
          <p class="t-lead">DeciDB is DuckDB with the DECIDE clause added, so it reads the same tables and files. HiGHS ships in every build; Gurobi is used when its license is present.</p>
          <ul class="facts">
            <li>Python: <code>pip install decidb</code>, with DuckDB's API</li>
            <li>A command-line shell for macOS, Linux and Windows</li>
            <li>LP, MILP and convex QP on HiGHS; non-convex QP, MIQP and QCQP need Gurobi</li>
          </ul>
          <p class="split-col__end"><a class="link" href="problems.html#classes">See solvers by class{b.ARROW}</a></p>
        </div>
        <div class="frame frame--split frame--lit span-6 start-7 dg-frame">
          <div class="dg" data-diagram data-dg-still><script type="application/json">{json.dumps(spec)}</script></div>
        </div>
      </div>
    </div>
  </section>"""


FAQ = [
    (
        "Do I need a Gurobi license?",
        "No. HiGHS is bundled and solves LP, MILP and convex QP. With a Gurobi license, DeciDB also takes "
        "non-convex and mixed-integer quadratic models.",
    ),
    (
        "Does ordinary DuckDB SQL still work?",
        "Yes. DeciDB is DuckDB with one more clause; a query without DECIDE runs as it does in DuckDB.",
    ),
    (
        "What happens when no answer meets every constraint?",
        "The query stops and says so. Put DIAGNOSE in front of it to see which clause to change and by how much.",
    ),
    (
        "Can I see the optimization model it builds?",
        "Yes. EXPLAIN shows the DECIDE node with its variables, constraints and objective, and the direct-solve rule "
        "when one applies.",
    ),
    (
        "Does release 0.2 run the queries on this site?",
        "No. Release 0.2 uses the earlier spellings, such as a postfix PER. The guide's migration table lists each "
        "change.",
    ),
]


def faq(b):
    def items(qs):
        return "".join(f'<div class="faq__item"><h3>{esc(q)}</h3><p>{esc(a)}</p></div>' for q, a in qs)

    return f"""
  <section class="section bg-band" id="faq" aria-labelledby="faq-h">
    <div class="wrap">
      <div class="grid grid--top">
        <div class="span-5 stack-4">
          <h2 id="faq-h">Questions</h2>
          <p class="t-lead">On solvers, releases and what happens when a model has no answer.</p>
          <p><a class="link" href="{b.GITHUB}/issues">Ask on GitHub{b.ARROW}</a></p>
        </div>
        <div class="span-6 start-7 faq">{items(FAQ[:3])}</div>
      </div>
      <div class="grid grid--top">
        <div class="span-6 start-7 faq faq--more">{items(FAQ[3:])}</div>
      </div>
    </div>
  </section>"""


def closing(b):
    return f"""<footer class="footer closing footer--statement g2m" id="contact" aria-labelledby="close-h">
  <div class="wrap">
    <div class="closing__cta close--word">
      <div class="section-head--split">
        <div><h2 id="close-h">Run your first DECIDE query</h2><p class="t-lead">Python wheels and command-line builds for macOS, Linux and Windows.</p></div>
        <a class="btn btn--primary" href="getting-started.html#install">Install DeciDB{b.ARROW}</a>
      </div>
      <p class="close__word" aria-hidden="true">DeciDB</p>
    </div>
    <div class="closing__links">
      {b.footer_links("")}
    </div>
  </div>
</footer>"""


PLAN = """<!-- type: software (a developer tool: DuckDB with a DECIDE clause)
     deal: hero centred, palette white-link-blue (a white sheet and a biro blue, the blue of DeciDB's own
     logo, for a tool whose answers are tables of numbers), type geist, radius sharp, nav mega (nav-cards
     dealt first; it needs two to four products and DeciDB is one), close word, footer statement, rhythm
     space, buttons pill, mark: DeciDB's own logo, lit ground sage, grounds statement bg-mist and faq bg-band,
     effect grain-gradient in the hero frame, card ground painterly-landscape
     moments: liquid-type in #statement (the one sentence that sums DeciDB up answers the pointer)
     motion: hero entrance (the name first, then what to do); the window's result rows arrive (a query
     returning its rows); the first card's rows arrive; hover on every link and button -->"""


def render(b):
    ex_by_slug = {ex["slug"]: ex for ex in b.EX.EXAMPLES}
    fx = b.FX.FIXTURES
    head = b.head(
        "DeciDB: optimization queries in DuckDB",
        "DeciDB adds a DECIDE clause to DuckDB: state decisions, constraints and a goal in SQL and get the best "
        "values back as rows. Nine problem classes are answered without a solver; the rest go to Gurobi or HiGHS.",
        "",
        extra=(
            "<script>document.documentElement.classList.add('m-enter');"
            "setTimeout(()=>document.documentElement.classList.add('m-enter-done'),700)</script>\n"
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
        hero(b, ex_by_slug) + language(b, ex_by_slug, fx) + direct(b, ex_by_slug) + statement(b) + solvers(b) + faq(b)
    )
    return f"""{head}
<body class="home">
{PLAN}
{b.header("", "")}
<main id="top">{main}
</main>
{closing(b)}
{b.scripts("", '<script type="module" src="assets/layout/layout.js"></script>' + chr(10) + '<script type="module" src="assets/diagrams/mount.js"></script>' + chr(10))}
</body>
</html>
"""
