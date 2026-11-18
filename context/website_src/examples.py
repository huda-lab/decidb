"""The examples on the website. Each one is run on DeciDB at build time; its result table is that run's output.

`direct` names the rewrite rule EXPLAIN must report (its "Direct solve rule" line: the engine's name for query
rewriting); an example without it must take the solver path.
"""

GROUPS = [
    ("First problems", "One decision per row, a budget or a target, and an objective. Start here."),
    (
        "Groups and keys",
        "<code>per</code>, <code>when</code> and <code>by</code> write one constraint per group; "
        "a keyed declaration shares one decision across the rows of a key.",
    ),
    (
        "Beyond a sum",
        "Make the worst case as good as possible, change as little as possible, or stay close to a target.",
    ),
    (
        "Answered by query rewriting",
        "These problems have an exact answer that DuckDB computes with sorting, ranking "
        "and aggregation. DeciDB recognizes them and rewrites the query to compute it, with no solver and no "
        "change to the SQL you write.",
    ),
    ("When there is no answer", "A query whose constraints cannot all hold tells you which clause to change."),
]

FILTERS = [
    ("basics", "Basics"),
    ("scopes", "per / when / by"),
    ("keyed", "Keyed decisions"),
    ("minmax", "Min-max &amp; fairness"),
    ("nonlinear", "Quadratic &amp; norms"),
    ("direct", "Query rewriting"),
    ("diagnose", "DIAGNOSE"),
]

EXAMPLES = [
    # ------------------------------------------------------------------ first problems
    {
        "slug": "knapsack",
        "title": "Pack the most valuable bag",
        "group": "First problems",
        "class": "ILP",
        "filters": ["basics"],
        "card_tags": ["BOOL"],
        "features": ["BOOL decision", "SUM constraint", "MAXIMIZE"],
        "summary": "Choose which items to carry so their total weight stays under a limit and their total value is as high as possible.",
        "summary_plain": "The 0/1 knapsack problem in one DeciDB query.",
        "snippet": "DECIDE x(BOOL)\nSUCH THAT SUM(x * weight) <= 50\nMAXIMIZE SUM(x * value)",
        "story": "<p>A photographer can carry 50 kg of gear. Each item has a value to the shoot and a weight. "
        "Which items go in the bag? Each row is a yes-or-no choice, so the decision is a <code>BOOL</code>: "
        "one 0/1 value per row.</p>",
        "setup": """CREATE TABLE Items (item VARCHAR, value INT, weight INT);
INSERT INTO Items VALUES
    ('Laptop', 100, 20), ('Camera', 60, 10), ('Drone', 120, 30),
    ('Tripod', 40, 15), ('Lens', 75, 12);""",
        "query": """SELECT item, value, weight, x AS take
FROM Items
DECIDE x(BOOL)
SUCH THAT SUM(x * weight) <= 50
MAXIMIZE SUM(x * value);""",
        "dec": ["take"],
        "on": "take",
        "explain": [
            ("DECIDE x(BOOL)", "One decision per row of <code>Items</code>, 0 or 1."),
            (
                "SUM(x * weight) <= 50",
                "The chosen items weigh at most 50 kg in total. <code>SUM</code> reads every row.",
            ),
            ("MAXIMIZE SUM(x * value)", "Among all choices that fit, return the one with the highest total value."),
            (
                "x AS take",
                "A decision is a column of the result like any other: select it, rename it, compute with it.",
            ),
        ],
        "answer": "<p>Laptop, camera and lens: 42 kg and a value of 235. The drone is worth more than any single item but "
        "costs 30 kg, and every set that includes it is worth less than 235. Every row comes back, with its "
        "decision filled in.</p>",
    },
    {
        "slug": "production",
        "title": "Meet demand at the lowest cost",
        "group": "First problems",
        "class": "ILP",
        "filters": ["basics"],
        "card_tags": ["INT"],
        "features": ["INT decision", "per-row bound", "MINIMIZE"],
        "summary": "Decide how many units each plant makes, within its capacity, so that total output meets demand at the lowest cost.",
        "summary_plain": "A production planning problem with integer quantities in DeciDB.",
        "snippet": "DECIDE units(INT)\nSUCH THAT units <= capacity\n      AND SUM(units) >= 500\nMINIMIZE SUM(units * unit_cost)",
        "story": "<p>Decisions are not only yes or no. An <code>INT</code> decision is a whole quantity, 0 or more. "
        "Four plants can make the same part at different costs; 500 parts are due.</p>",
        "setup": """CREATE TABLE Plants (plant VARCHAR, capacity INT, unit_cost DOUBLE);
INSERT INTO Plants VALUES
    ('Austin', 300, 12.5), ('Boston', 200, 10.0),
    ('Chicago', 250, 11.0), ('Denver', 150, 9.5);""",
        "query": """SELECT plant, capacity, unit_cost, units
FROM Plants
DECIDE units(INT)
SUCH THAT units <= capacity
      AND SUM(units) >= 500
MINIMIZE SUM(units * unit_cost);""",
        "dec": ["units"],
        "on": "units",
        "explain": [
            ("DECIDE units(INT)", "A whole number of units per plant, 0 or more by default."),
            (
                "units <= capacity",
                "A constraint with no reducer holds on every row: each plant stays within its own capacity.",
            ),
            ("SUM(units) >= 500", "Together the plants make at least 500 units."),
            ("MINIMIZE SUM(units * unit_cost)", "Spend as little as possible."),
        ],
        "answer": "<p>The cheapest plants fill first: Denver and Boston run at capacity, Chicago makes the remaining 150, "
        "and Austin, the most expensive, makes nothing. Total cost: 5,075.</p>",
    },
    {
        "slug": "diet",
        "title": "A cheap diet that meets nutrition targets",
        "group": "First problems",
        "class": "ILP",
        "filters": ["basics"],
        "card_tags": ["INT", "BETWEEN"],
        "features": ["INT decision", "BETWEEN", "several targets"],
        "summary": "Pick servings of each food so protein, calories and fat land within their targets at the lowest cost.",
        "summary_plain": "The diet problem in DeciDB: several nutrient constraints, minimum cost.",
        "snippet": "DECIDE servings(INT)\nSUCH THAT SUM(servings * protein) >= 90\n  AND SUM(servings * calories) BETWEEN 1800 AND 2200",
        "story": "<p>The classic diet problem: at most three servings of any food, at least 90 g of protein, between "
        "1,800 and 2,200 calories, and no more than 50 g of fat.</p>",
        "setup": """CREATE TABLE Foods (food VARCHAR, cost DOUBLE, protein INT, calories INT, fat INT);
INSERT INTO Foods VALUES
    ('Oatmeal', 0.60, 5, 150, 3), ('Chicken', 2.40, 31, 240, 6),
    ('Eggs', 0.90, 12, 155, 10), ('Rice', 0.40, 4, 205, 0),
    ('Lentils', 0.70, 18, 230, 1), ('Yogurt', 1.10, 10, 150, 4),
    ('Peanut butter', 0.50, 8, 190, 16);""",
        "query": """SELECT food, cost, servings
FROM Foods
DECIDE servings(INT)
SUCH THAT servings <= 3
      AND SUM(servings * protein) >= 90
      AND SUM(servings * calories) BETWEEN 1800 AND 2200
      AND SUM(servings * fat) <= 50
MINIMIZE SUM(servings * cost);""",
        "dec": ["servings"],
        "on": "servings",
        "explain": [
            ("servings <= 3", "Per row: no food more than three times."),
            ("SUM(servings * protein) >= 90", "A total over all foods."),
            (
                "BETWEEN 1800 AND 2200",
                "<code>BETWEEN</code> is two constraints, a floor and a ceiling, on the same sum.",
            ),
            ("MINIMIZE SUM(servings * cost)", "The cheapest plan that meets every target."),
        ],
        "answer": "<p>Three servings each of rice and lentils, two of peanut butter and one egg: 94 g of protein, "
        "1,840 calories and 45 g of fat for 5.20. Chicken has the most protein per serving but costs too much "
        "per gram.</p>",
    },
    {
        "slug": "category-rules",
        "title": "Category rules and a budget from a table",
        "group": "First problems",
        "class": "ILP",
        "filters": ["basics", "scopes"],
        "card_tags": ["when", "subquery"],
        "features": ["WHERE before DECIDE", "when c: constraint", "bound from a subquery"],
        "summary": "Rules that apply to one category at a time, and limits read from a settings table instead of written into the query.",
        "summary_plain": "Conditional constraints with when, and constraint bounds from scalar subqueries.",
        "snippet": "SUCH THAT SUM(pick * cost) <= (SELECT budget FROM Settings)\n  AND when category = 'A': SUM(pick) >= 2\n  AND when category = 'B': SUM(pick) <= 1",
        "story": "<p>A store picks products for a display. The budget and the item limit live in a <code>Settings</code> "
        "table so they can change without editing the query. At least two products must come from category A, "
        "at most one from category B, and out-of-stock products are not candidates at all.</p>",
        "setup": """CREATE TABLE Settings (budget INT, max_items INT);
INSERT INTO Settings VALUES (120, 5);

CREATE TABLE Products (name VARCHAR, category VARCHAR, value INT, cost INT, in_stock BOOLEAN);
INSERT INTO Products VALUES
    ('Desk lamp', 'A', 40, 25, true), ('Monitor arm', 'A', 55, 35, true),
    ('Webcam', 'A', 35, 30, true), ('Headset', 'B', 60, 45, true),
    ('Keyboard', 'B', 50, 30, true), ('Mouse', 'B', 30, 15, true),
    ('Cable kit', 'C', 20, 10, true), ('Dock', 'C', 70, 60, false),
    ('Chair mat', 'C', 25, 20, true);""",
        "query": """SELECT name, category, value, cost, pick
FROM Products
WHERE in_stock
DECIDE pick(BOOL)
SUCH THAT SUM(pick * cost) <= (SELECT budget FROM Settings)
      AND SUM(pick) <= (SELECT max_items FROM Settings)
      AND when category = 'A': SUM(pick) >= 2
      AND when category = 'B': SUM(pick) <= 1
MAXIMIZE SUM(pick * value);""",
        "dec": ["pick"],
        "on": "pick",
        "explain": [
            ("WHERE in_stock", "Ordinary SQL runs first. The dock is out of stock, so it never gets a decision."),
            (
                "<= (SELECT budget FROM Settings)",
                "A bound can be any scalar subquery. Change the table, rerun the query.",
            ),
            (
                "when category = 'A': SUM(pick) >= 2",
                "<code>when</code> in front of a constraint keeps only the rows where the "
                "condition holds. The sum reads category A alone.",
            ),
            ("when category = 'B': SUM(pick) <= 1", "One rule per category, each with its own condition."),
        ],
        "answer": "<p>Five products for exactly 120: the lamp and monitor arm from A, the keyboard from B, and both "
        "C items. The headset is the most valuable product, but B allows only one pick and the keyboard leaves "
        "more of the budget for the rest.</p>",
        "after": """<h3>A condition inside one sum</h3>
<p>A <code>when</code> can also sit inside a reducer, where it filters that reducer only. This caps the spend on
category C while the other sums still read every row:</p>
<!--sql check="category-rules" frag-->SUM(when category = 'C': pick * cost) <= 25<!--/sql-->""",
    },
    # ------------------------------------------------------------------ groups and keys
    {
        "slug": "shift-coverage",
        "title": "Cover every shift",
        "group": "Groups and keys",
        "class": "Feasibility",
        "filters": ["scopes"],
        "card_tags": ["per", "by", "no objective"],
        "features": ["per K: one constraint per group", "by (K): sum within the group", "feasibility (no objective)"],
        "summary": "Exactly one nurse per shift, at most one shift per nurse. There is nothing to optimize, only rules to satisfy.",
        "summary_plain": "A feasibility query with per-group constraints using per and by.",
        "snippet": "SUCH THAT per shift: SUM(on_duty) by (shift) = 1\n      AND per nurse: SUM(on_duty) by (nurse) <= 1",
        "story": "<p>Each row says a nurse is available for a shift. Every shift needs exactly one nurse, and nobody "
        "works twice. Leaving out <code>MAXIMIZE</code> and <code>MINIMIZE</code> asks for any assignment that "
        "satisfies the rules.</p>",
        "setup": """CREATE TABLE Availability (nurse VARCHAR, shift VARCHAR, weekend BOOLEAN);
INSERT INTO Availability VALUES
    ('Alice', 'Sat day', true), ('Alice', 'Mon day', false),
    ('Bob', 'Sat day', true), ('Bob', 'Sat night', true),
    ('Carol', 'Sat night', true), ('Carol', 'Mon night', false),
    ('Dan', 'Mon day', false), ('Dan', 'Mon night', false),
    ('Eve', 'Mon day', false);""",
        "query": """SELECT nurse, shift, on_duty
FROM Availability
DECIDE on_duty(BOOL)
SUCH THAT per shift: SUM(on_duty) by (shift) = 1
      AND per nurse: SUM(on_duty) by (nurse) <= 1
ORDER BY shift, nurse;""",
        "dec": ["on_duty"],
        "on": "on_duty",
        "explain": [
            ("per shift:", "Write this constraint once per distinct shift: four shifts, four constraints."),
            (
                "SUM(on_duty) by (shift) = 1",
                "<code>by (shift)</code> makes the sum read only the rows of the current shift.",
            ),
            ("per nurse: SUM(on_duty) by (nurse) <= 1", "The same pattern per nurse."),
            (
                "no objective",
                "A feasibility query: DeciDB returns one assignment that satisfies every constraint, or says none exists.",
            ),
        ],
        "answer": "<p>Each of the four shifts has exactly one nurse and nobody works twice; Bob is left free. Other valid "
        "rosters exist. To prefer one, add an objective, such as <code>MAXIMIZE SUM(on_duty * seniority)</code>.</p>",
        "after": """<h3>Why both <code>per</code> and <code>by</code>?</h3>
<p><code>per</code> sets <em>how many</em> constraints there are; <code>by</code> sets <em>which rows</em> a sum reads. They are
separate words because they are separate choices: <code>per shift: SUM(on_duty) &lt;= 3</code> would write one constraint per
shift, each bounding the total over <em>all</em> rows. Here, a weekend rule combines a <code>when</code> with a <code>per</code>:</p>
<!--sql check="shift-coverage" frag-->when weekend per nurse: SUM(on_duty) by (nurse) <= 1<!--/sql-->""",
    },
    {
        "slug": "assignment",
        "title": "Assign people to tasks",
        "group": "Groups and keys",
        "class": "ILP",
        "filters": ["scopes"],
        "card_tags": ["per", "by", "subquery"],
        "features": ["per / by on two keys", "DECIDE inside a subquery", "filter the answer"],
        "summary": "Every task gets exactly one person, nobody gets two tasks, and the total fit score is as high as possible.",
        "summary_plain": "The assignment problem in DeciDB, filtered to the chosen pairs.",
        "snippet": "SUCH THAT per worker: SUM(assign) by (worker) <= 1\n      AND per task: SUM(assign) by (task) = 1\nMAXIMIZE SUM(assign * score)",
        "story": "<p>Each row is a person–task pair with a fit score. There are four people and three tasks. A DECIDE query "
        "is a query like any other, so it can sit in a subquery and its answer can be filtered.</p>",
        "setup": """CREATE TABLE Fit (worker VARCHAR, task VARCHAR, score INT);
INSERT INTO Fit VALUES
    ('Ana', 'Backend', 9), ('Ana', 'Frontend', 6), ('Ana', 'Data', 7),
    ('Ben', 'Backend', 8), ('Ben', 'Frontend', 8), ('Ben', 'Data', 4),
    ('Cleo', 'Backend', 7), ('Cleo', 'Frontend', 5), ('Cleo', 'Data', 9),
    ('Dev', 'Backend', 5), ('Dev', 'Frontend', 7), ('Dev', 'Data', 6);""",
        "query": """SELECT worker, task, score FROM (
    SELECT worker, task, score, assign
    FROM Fit
    DECIDE assign(BOOL)
    SUCH THAT per worker: SUM(assign) by (worker) <= 1
          AND per task: SUM(assign) by (task) = 1
    MAXIMIZE SUM(assign * score)
) WHERE assign = 1
ORDER BY task;""",
        "explain_query": """SELECT worker, task, score, assign
FROM Fit
DECIDE assign(BOOL)
SUCH THAT per worker: SUM(assign) by (worker) <= 1
      AND per task: SUM(assign) by (task) = 1
MAXIMIZE SUM(assign * score)""",
        "explain": [
            ("per worker: ... by (worker) <= 1", "One constraint per person: at most one task."),
            ("per task: ... by (task) = 1", "One constraint per task: exactly one person."),
            ("MAXIMIZE SUM(assign * score)", "The best total fit."),
            ("WHERE assign = 1", "The outer query keeps only the chosen pairs."),
        ],
        "answer": "<p>Ana takes backend, Cleo data and Ben frontend, for a total score of 26. Dev is left out: his best "
        "score, 7, is lower than each chosen pair's.</p>",
    },
    {
        "slug": "diversification",
        "title": "A diversified portfolio",
        "group": "Groups and keys",
        "class": "ILP",
        "filters": ["scopes"],
        "card_tags": ["per", "by"],
        "features": ["per sector: … by (sector)", "a limit per group", "a total risk budget"],
        "summary": "Pick five stocks with the highest expected return, within a risk budget and at most two per sector.",
        "summary_plain": "A diversification limit per sector with per and by.",
        "snippet": "SUCH THAT SUM(pick) = 5\n      AND SUM(pick * risk) <= 30\n      AND per sector: SUM(pick) by (sector) <= 2",
        "story": "<p>The sector limit is the interesting part. The query does not list the sectors: <code>per sector</code> "
        "writes one constraint for every sector in the data, so a new sector needs no change to the query.</p>",
        "setup": """CREATE TABLE Stocks (ticker VARCHAR, sector VARCHAR, expected_return DOUBLE, risk INT);
INSERT INTO Stocks VALUES
    ('NOVA', 'Tech', 12.5, 9), ('BYTE', 'Tech', 11.0, 8), ('CHIP', 'Tech', 10.2, 7),
    ('MEDI', 'Health', 8.4, 5), ('CURE', 'Health', 7.9, 4), ('GENE', 'Health', 9.6, 7),
    ('BANK', 'Finance', 6.5, 3), ('LOAN', 'Finance', 7.2, 5),
    ('SOLR', 'Energy', 9.1, 6), ('GRID', 'Energy', 5.8, 2);""",
        "query": """SELECT ticker, sector, expected_return, risk, pick
FROM Stocks
DECIDE pick(BOOL)
SUCH THAT SUM(pick) = 5
      AND SUM(pick * risk) <= 30
      AND per sector: SUM(pick) by (sector) <= 2
MAXIMIZE SUM(pick * expected_return);""",
        "dec": ["pick"],
        "on": "pick",
        "explain": [
            ("SUM(pick) = 5", "Exactly five stocks."),
            ("SUM(pick * risk) <= 30", "Total risk points within the budget."),
            ("per sector: SUM(pick) by (sector) <= 2", "One constraint per sector, each counting that sector's picks."),
        ],
        "answer": "<p>The two best tech stocks, then SOLR, CURE and BANK: risk exactly 30, expected return 47.0. "
        "CHIP is the third-best stock but would be a third tech pick; GENE returns more than CURE but its risk "
        "does not fit.</p>",
    },
    {
        "slug": "keyed-decisions",
        "title": "One decision per nurse, not per shift",
        "group": "Groups and keys",
        "class": "ILP",
        "filters": ["keyed", "scopes"],
        "card_tags": ["per K: x(TYPE)", "SUM(per K: …)"],
        "features": ["keyed declaration per n: keep(BOOL)", "reducer with its own per", "joins"],
        "summary": "After a join each nurse appears once per shift, but the decision is about the nurse. A keyed declaration makes one decision per nurse.",
        "summary_plain": "Keyed decisions over a join with per K: x(TYPE).",
        "snippet": "DECIDE per n: keep(BOOL)\nSUCH THAT SUM(keep * s.hours) <= 60\nMAXIMIZE SUM(per n: keep * n.skill_score)",
        "story": "<p>A ward keeps at most 60 hours of the coming shifts and wants the most skilled team. Joining "
        "<code>nurses</code> with <code>shifts</code> gives one row per shift, but a nurse is kept or released as a "
        "whole. With a per-row decision the solver could keep Alice on Monday and drop her on Tuesday.</p>",
        "setup": """CREATE TABLE nurses (id INT, name VARCHAR, skill_score INT);
INSERT INTO nurses VALUES (1, 'Alice', 9), (2, 'Bob', 6), (3, 'Carol', 8), (4, 'Dan', 5);

CREATE TABLE shifts (nurse_id INT, shift_date DATE, hours INT);
INSERT INTO shifts VALUES
    (1, '2026-12-01', 12), (1, '2026-12-02', 12), (1, '2026-12-03', 12),
    (2, '2026-12-01', 8), (2, '2026-12-03', 8),
    (3, '2026-12-02', 12), (3, '2026-12-04', 10),
    (4, '2026-12-01', 6), (4, '2026-12-02', 6), (4, '2026-12-04', 6);""",
        "query": """SELECT n.name, s.shift_date, s.hours, keep
FROM nurses n JOIN shifts s ON n.id = s.nurse_id
DECIDE per n: keep(BOOL)
SUCH THAT SUM(keep * s.hours) <= 60
MAXIMIZE SUM(per n: keep * n.skill_score)
ORDER BY n.name, s.shift_date;""",
        "dec": ["keep"],
        "on": "keep",
        "explain": [
            (
                "DECIDE per n: keep(BOOL)",
                "One decision per nurse (per distinct row of <code>n</code>), shared by all of that "
                "nurse's shift rows. Four decisions, not ten.",
            ),
            (
                "SUM(keep * s.hours) <= 60",
                "A sum reads result rows, as SQL does over a join: each shift's hours count once.",
            ),
            (
                "SUM(per n: keep * n.skill_score)",
                "A reducer with its own <code>per</code> reads one value per nurse, so a "
                "nurse's skill counts once, not once per shift.",
            ),
        ],
        "answer": "<p>Bob, Carol and Dan: 56 hours and a combined skill of 19. Alice is the most skilled nurse, but her "
        "36 hours leave room for only one other nurse, and no such pair reaches 19.</p>",
        "after": """<h3>The colon form</h3>
<p><code>SUM(n: e)</code> is shorthand for <code>SUM(per n: e)</code>: one term per tuple of the named relation. This objective is
the same as the one above:</p>
<!--sql check="keyed-decisions"-->SELECT n.name, keep
FROM nurses n JOIN shifts s ON n.id = s.nurse_id
DECIDE per n: keep(BOOL)
SUCH THAT SUM(keep * s.hours) <= 60
MAXIMIZE SUM(n: keep * n.skill_score);<!--/sql-->""",
    },
    {
        "slug": "facility-location",
        "title": "Which depots to open",
        "group": "Groups and keys",
        "class": "MILP",
        "filters": ["keyed", "scopes"],
        "card_tags": ["per D: open(BOOL)", "colon form"],
        "features": [
            "two decisions with different keys",
            "per C / per D with by",
            "a fixed cost counted once per depot",
        ],
        "summary": "Open depots and route shipments so every customer's demand is met, paying a fixed cost for each open depot plus a cost per unit shipped.",
        "summary_plain": "Capacitated facility location in one DeciDB query with keyed decisions.",
        "snippet": "DECIDE per D: open(BOOL), ship(INT)\nSUCH THAT per C: SUM(ship) by (C) >= C.demand\n  AND per D: SUM(ship) by (D) <= D.capacity * open\nMINIMIZE SUM(R.unit_cost * ship) + SUM(D: D.opening_cost * open)",
        "story": "<p>Three depots can serve four customers along the routes listed in <code>Routes</code>. A depot ships "
        "nothing unless it is open, and opening it has a fixed cost. Two decisions with different keys: "
        "<code>open</code> is one per depot, <code>ship</code> one per route.</p>",
        "setup": """CREATE TABLE Depots (depotID VARCHAR, region VARCHAR, capacity INT, opening_cost INT);
INSERT INTO Depots VALUES ('D1', 'North', 120, 500), ('D2', 'North', 80, 300), ('D3', 'South', 150, 650);

CREATE TABLE Customers (customerID VARCHAR, demand INT);
INSERT INTO Customers VALUES ('C1', 40), ('C2', 55), ('C3', 30), ('C4', 45);

CREATE TABLE Routes (depotID VARCHAR, customerID VARCHAR, unit_cost DOUBLE);
INSERT INTO Routes VALUES
    ('D1', 'C1', 2), ('D1', 'C2', 4), ('D1', 'C3', 5),
    ('D2', 'C1', 3), ('D2', 'C2', 2), ('D2', 'C4', 6),
    ('D3', 'C2', 5), ('D3', 'C3', 1), ('D3', 'C4', 2);""",
        "query": """SELECT D.depotID, C.customerID, open, ship
FROM Routes R
JOIN Depots D USING (depotID)
JOIN Customers C USING (customerID)
DECIDE per D: open(BOOL), ship(INT)
SUCH THAT per C: SUM(ship) by (C) >= C.demand
      AND per D: SUM(ship) by (D) <= D.capacity * open
MINIMIZE SUM(R.unit_cost * ship) + SUM(D: D.opening_cost * open);""",
        "dec": ["open", "ship"],
        "on": "ship",
        "explain": [
            (
                "DECIDE per D: open(BOOL), ship(INT)",
                "Each declarator has its own key: <code>open</code> is one per depot, "
                "<code>ship</code> (no key) one per route row.",
            ),
            (
                "per C: SUM(ship) by (C) >= C.demand",
                "One constraint per customer: what reaches it covers its demand. "
                "A relation name as the key stands for all its columns.",
            ),
            (
                "per D: SUM(ship) by (D) <= D.capacity * open",
                "One constraint per depot. A closed depot has capacity 0.",
            ),
            (
                "SUM(D: D.opening_cost * open)",
                "The colon form: one term per depot, so each fixed cost counts once even "
                "though a depot appears on several routes.",
            ),
        ],
        "answer": "<p>Open D2 and D3, keep D1 closed. D2 serves C1 and most of C2; D3 serves the rest of C2, all of C3 and "
        "C4. D1 has cheap routes to C1, but its 500 opening cost outweighs the savings.</p>",
    },
    # ------------------------------------------------------------------ beyond a sum
    {
        "slug": "min-max-shortfall",
        "title": "Share a shortage fairly across regions",
        "group": "Beyond a sum",
        "class": "ILP",
        "filters": ["minmax", "keyed"],
        "card_tags": ["per (): x(TYPE)", "min-max"],
        "features": ["query-wide decision per (): x", "per g: … by (g)", "minimize the worst case"],
        "summary": "Supply is short. Ship so that no region falls short of its demand by more than necessary. This is the DeciDB paper's Example 1.",
        "summary_plain": "Min-max allocation with a query-wide decision, the DeciDB paper's Example 1.",
        "snippet": "DECIDE ship(INT), per (): max_shortfall(INT)\nSUCH THAT per g: g.demand - SUM(ship) by (g) <= max_shortfall\nMINIMIZE max_shortfall",
        "story": "<p>Regions need 48 units in total but the network can ship only 40. Minimizing total shortfall would "
        "let one region absorb all of it. Instead, minimize the <em>largest</em> regional shortfall. The trick is a "
        "single decision for the whole query, declared with <code>per ()</code>.</p>",
        "setup": """CREATE TABLE regions (regionID VARCHAR, demand INT);
INSERT INTO regions VALUES ('East', 18), ('West', 14), ('North', 16);

CREATE TABLE routes (routeID VARCHAR, regionID VARCHAR, capacity INT);
INSERT INTO routes VALUES
    ('R1', 'East', 10), ('R2', 'East', 6), ('R3', 'West', 9),
    ('R4', 'West', 8), ('R5', 'North', 7), ('R6', 'North', 5);""",
        "query": """SELECT r.routeID, r.regionID, g.demand, ship, max_shortfall
FROM routes r JOIN regions g USING (regionID)
DECIDE ship(INT), per (): max_shortfall(INT)
SUCH THAT ship <= r.capacity
      AND SUM(ship) <= 40
      AND per g: g.demand - SUM(ship) by (g) <= max_shortfall
MINIMIZE max_shortfall;""",
        "dec": ["ship", "max_shortfall"],
        "explain": [
            (
                "per (): max_shortfall(INT)",
                "<code>per ()</code> is the empty key: exactly one decision for the whole "
                "query. Its value repeats on every row.",
            ),
            ("SUM(ship) <= 40", "The network's total capacity."),
            (
                "per g: g.demand - SUM(ship) by (g) <= max_shortfall",
                "One constraint per region: its shortfall is at most " "the shared bound.",
            ),
            ("MINIMIZE max_shortfall", "Push the shared bound, and with it the worst region, as low as possible."),
        ],
        "answer": "<p>No region is short by more than 4. North's routes carry only 12 of its 16, so 4 is the best "
        "possible; East and West are each held within 4 as well, using the 40 units of network capacity.</p>",
        "after": """<h3>The same idea as a two-level objective</h3>
<p>The next example writes a min-max directly in the objective, with no helper decision.</p>""",
    },
    {
        "slug": "makespan",
        "title": "Balance work across machines",
        "group": "Beyond a sum",
        "class": "ILP",
        "filters": ["minmax", "scopes"],
        "card_tags": ["max(per k: sum(…) by (k))"],
        "features": ["two-level objective", "CROSS JOIN for all pairs", "one choice per job"],
        "summary": "Assign each job to one of two machines so the busier machine finishes as early as possible.",
        "summary_plain": "Makespan minimization with DeciDB's two-level objective.",
        "snippet": "SUCH THAT per J: SUM(run) by (J) = 1\nMINIMIZE max(per M.machine:\n    sum(run * J.hours) by (M.machine))",
        "story": "<p>Six jobs, two machines. Every job runs once, on either machine. The goal is the classic makespan: "
        "make the load of the busiest machine as small as possible. The objective reads like the sentence: the "
        "<em>max</em> over machines of each machine's <em>sum</em> of hours.</p>",
        "setup": """CREATE TABLE Jobs (job VARCHAR, hours INT);
INSERT INTO Jobs VALUES ('J1', 7), ('J2', 5), ('J3', 4), ('J4', 3), ('J5', 3), ('J6', 2);

CREATE TABLE Machines (machine VARCHAR);
INSERT INTO Machines VALUES ('M1'), ('M2');""",
        "query": """SELECT machine, job, hours FROM (
    SELECT M.machine, J.job, J.hours, run
    FROM Jobs J CROSS JOIN Machines M
    DECIDE run(BOOL)
    SUCH THAT per J: SUM(run) by (J) = 1
    MINIMIZE max(per M.machine: sum(run * J.hours) by (M.machine))
) WHERE run = 1
ORDER BY machine, job;""",
        "explain_query": """SELECT M.machine, J.job, J.hours, run
FROM Jobs J CROSS JOIN Machines M
DECIDE run(BOOL)
SUCH THAT per J: SUM(run) by (J) = 1
MINIMIZE max(per M.machine: sum(run * J.hours) by (M.machine))""",
        "explain": [
            ("FROM Jobs J CROSS JOIN Machines M", "Every job–machine pair is a row with its own decision."),
            ("per J: SUM(run) by (J) = 1", "Each job runs on exactly one machine."),
            (
                "max(per M.machine: sum(...) by (M.machine))",
                "The two-level objective: an inner <code>sum</code> per machine, "
                "then the <code>max</code> over those sums. <code>MINIMIZE</code> makes the busiest machine as light as possible.",
            ),
        ],
        "answer": "<p>The 24 hours split evenly: 12 hours per machine. M2 runs the two long jobs, M1 the four short ones.</p>",
    },
    {
        "slug": "price-changes",
        "title": "Raise revenue with as few price changes as possible",
        "group": "Beyond a sum",
        "class": "MILP",
        "filters": ["nonlinear"],
        "card_tags": ["norm(e, 'inf')", "norm(e, 0)"],
        "features": ["norm(e, 0): count of changes", "norm(e, 'inf'): largest change", "REAL decision"],
        "summary": "Reach a revenue target by changing prices, keeping the largest change small and touching at most two items.",
        "summary_plain": "L0 and L-infinity norms in DeciDB: sparse, small price changes.",
        "snippet": "SUCH THAT SUM(new_price * weekly_qty) >= 16500\n  AND norm(new_price - price, 0) <= 2\nMINIMIZE norm(new_price - price, 'inf')",
        "story": "<p>A café needs weekly revenue of 16,500; current prices bring in 15,662.50. Each price may move within a "
        "floor and a ceiling. Customers notice big changes, and the menu board is a hassle to reprint, so: keep the "
        "biggest change as small as possible, and change at most two prices.</p>",
        "setup": """CREATE TABLE Menu (item VARCHAR, price DOUBLE, weekly_qty INT, floor DOUBLE, ceiling DOUBLE);
INSERT INTO Menu VALUES
    ('Espresso', 3.00, 900, 2.50, 3.60), ('Latte', 4.50, 1200, 4.00, 5.40),
    ('Mocha', 5.00, 500, 4.50, 6.00), ('Tea', 2.50, 400, 2.00, 3.00),
    ('Muffin', 3.25, 700, 3.00, 3.90), ('Bagel', 2.75, 650, 2.50, 3.30);""",
        "query": """SELECT item, price, ROUND(new_price, 2) AS new_price
FROM Menu
DECIDE new_price(REAL)
SUCH THAT new_price BETWEEN floor AND ceiling
      AND SUM(new_price * weekly_qty) >= 16500
      AND norm(new_price - price, 0) <= 2
MINIMIZE norm(new_price - price, 'inf');""",
        "dec": ["new_price"],
        "explain": [
            ("new_price BETWEEN floor AND ceiling", "Per-row bounds read from the data."),
            (
                "norm(new_price - price, 0) <= 2",
                "The L0 norm counts the rows where the expression is nonzero: at most two " "prices change.",
            ),
            ("norm(new_price - price, 'inf')", "The L∞ norm is the largest absolute change."),
        ],
        "answer": "<p>Espresso and latte each go up by 0.40, the busiest items, and nothing else changes. Without the "
        "L0 limit, the smallest largest change spreads across all six items, each up by about 0.19, so six "
        "prices change instead of two:</p>",
        "after": """<!--sql title="Without the L0 limit" run="price-changes" dec="new_price"-->SELECT item, price, ROUND(new_price, 2) AS new_price
FROM Menu
DECIDE new_price(REAL)
SUCH THAT new_price BETWEEN floor AND ceiling
      AND SUM(new_price * weekly_qty) >= 16500
MINIMIZE norm(new_price - price, 'inf');<!--/sql-->
<p><code>norm(e, 1)</code> (sum of absolute changes) and <code>norm(e, 2)</code> (sum of squares) are the other two norms.</p>""",
    },
    {
        "slug": "rebalancing",
        "title": "Rebalance toward target weights",
        "group": "Beyond a sum",
        "class": "QP",
        "filters": ["nonlinear"],
        "card_tags": ["POWER(e, 2)", "convex QP"],
        "features": ["quadratic objective", "REAL decision", "least squares"],
        "summary": "The targets add up to more than 100% and one asset has a cap. Find the allowed weights closest to the targets.",
        "summary_plain": "A least-squares projection as a quadratic DeciDB query.",
        "snippet": "DECIDE w(REAL)\nSUCH THAT w <= max_weight AND SUM(w) = 1\nMINIMIZE SUM(POWER(w - target, 2))",
        "story": "<p>An advisor's target allocation adds up to 110%, and the stock target exceeds its 50% cap. The closest "
        "valid allocation, in the least-squares sense, minimizes the sum of squared differences: a convex "
        "quadratic program, which both bundled HiGHS and Gurobi solve.</p>",
        "setup": """CREATE TABLE Holdings (asset VARCHAR, target DOUBLE, max_weight DOUBLE);
INSERT INTO Holdings VALUES
    ('Stocks', 0.55, 0.50), ('Bonds', 0.30, 0.40),
    ('Gold', 0.10, 0.10), ('Cash', 0.15, 0.20);""",
        "query": """SELECT asset, target, max_weight, ROUND(w, 4) AS w
FROM Holdings
DECIDE w(REAL)
SUCH THAT w <= max_weight AND SUM(w) = 1
MINIMIZE SUM(POWER(w - target, 2));""",
        "dec": ["w"],
        "explain": [
            ("DECIDE w(REAL)", "A continuous weight per asset, 0 or more."),
            ("SUM(w) = 1", "Weights add up to 100%."),
            (
                "SUM(POWER(w - target, 2))",
                "The squared distance from the targets. <code>POWER(e, 2)</code>, <code>e ** 2</code> "
                "and <code>e * e</code> are the same square.",
            ),
        ],
        "answer": "<p>Stocks sit at their 50% cap. The remaining excess comes off the other three targets in equal "
        "amounts, about 1.67 percentage points each, which is what least squares does when no other bound is "
        "active.</p>",
    },
    # ------------------------------------------------------------------ query rewriting
    {
        "slug": "shortlist",
        "title": "A shortlist: the best two per genre",
        "group": "Answered by query rewriting",
        "class": "ILP",
        "direct": "S1_UNIFORM_MATROID",
        "filters": ["direct", "scopes"],
        "card_tags": ["S1"],
        "features": ["choose K rows by score", "a count limit per group", "query rewriting"],
        "summary": "Pick at most two films per genre with the highest total rating. DeciDB ranks rows instead of calling a solver.",
        "summary_plain": "Top-k per group, answered by DeciDB's query rewriting.",
        "snippet": "DECIDE pick(BOOL)\nSUCH THAT per genre: SUM(pick) by (genre) <= 2\nMAXIMIZE SUM(rating * pick)",
        "story": "<p>Choosing rows by score under a count limit has an exact answer: sort by score and take the top "
        "<em>k</em> in each group. DeciDB proves the query has that shape and computes it with DuckDB's own window "
        "operators.</p>",
        "setup": """CREATE TABLE Films (title VARCHAR, genre VARCHAR, rating DOUBLE);
INSERT INTO Films VALUES
    ('Northern Lights', 'Drama', 8.6), ('Paper Boats', 'Drama', 7.9),
    ('The Long Field', 'Drama', 8.1), ('Quiet Harbor', 'Drama', 7.2),
    ('Laugh Track', 'Comedy', 7.4), ('Second Helping', 'Comedy', 8.0),
    ('Odd Couples', 'Comedy', 6.9), ('Deep Orbit', 'Sci-fi', 8.8),
    ('Red Dust', 'Sci-fi', 7.7), ('Signal', 'Sci-fi', 8.3);""",
        "query": """SELECT title, genre, rating, pick
FROM Films
DECIDE pick(BOOL)
SUCH THAT per genre: SUM(pick) by (genre) <= 2
MAXIMIZE SUM(rating * pick)
ORDER BY genre, rating DESC;""",
        "dec": ["pick"],
        "on": "pick",
        "explain": [
            ("per genre: SUM(pick) by (genre) <= 2", "A count limit per group."),
            ("MAXIMIZE SUM(rating * pick)", "A linear score: the class DeciDB calls S1, choose rows by score."),
        ],
        "answer": "<p>The two highest-rated films of each genre. <code>EXPLAIN</code> on this query shows "
        "<code>Direct solve rule: S1_UNIFORM_MATROID</code>: the query was rewritten, and no solver was started.</p>",
    },
    {
        "slug": "nested-quotas",
        "title": "Hire under nested quotas",
        "group": "Answered by query rewriting",
        "class": "ILP",
        "direct": "S2_LAMINAR_MATROID",
        "filters": ["direct", "scopes"],
        "card_tags": ["S2", "by (a, b)"],
        "features": ["quotas that nest: team, division, total", "by with two columns", "query rewriting"],
        "summary": "At most one hire per team, two per division and four overall, maximizing total score.",
        "summary_plain": "Nested hiring quotas, answered by DeciDB's query rewriting.",
        "snippet": "SUCH THAT SUM(hire) by (division, team) <= 1\n      AND SUM(hire) by (division) <= 2\n      AND SUM(hire) <= 4",
        "story": "<p>The quotas nest: every team lies inside one division, and every division inside the company. Quotas "
        "that nest like this still have a greedy exact answer, so DeciDB rewrites the query and calls no solver.</p>",
        "setup": """CREATE TABLE Candidates (name VARCHAR, division VARCHAR, team VARCHAR, score DOUBLE);
INSERT INTO Candidates VALUES
    ('Ada', 'Product', 'Design', 9.1), ('Bo', 'Product', 'Design', 8.7),
    ('Cy', 'Product', 'Mobile', 8.9), ('Jo', 'Product', 'Web', 9.05),
    ('Ed', 'Platform', 'Data', 9.4), ('Fa', 'Platform', 'Data', 9.3),
    ('Gu', 'Platform', 'Infra', 6.5), ('Hu', 'Research', 'ML', 9.0),
    ('Io', 'Research', 'ML', 8.0);""",
        "query": """SELECT name, division, team, score, hire
FROM Candidates
DECIDE hire(BOOL)
SUCH THAT SUM(hire) by (division, team) <= 1
      AND SUM(hire) by (division) <= 2
      AND SUM(hire) <= 4
MAXIMIZE SUM(score * hire);""",
        "dec": ["hire"],
        "on": "hire",
        "explain": [
            (
                "SUM(hire) by (division, team) <= 1",
                "Without a <code>per</code>, a grouped sum bounds each row's own group: " "every team gets the limit.",
            ),
            ("SUM(hire) by (division) <= 2", "A coarser group containing the first."),
            ("SUM(hire) <= 4", "The whole company."),
        ],
        "answer": "<p>Ed, Ada, Jo and Hu. Fa has the second-highest score but shares the Data team with Ed. Cy is the "
        "best Mobile candidate, but Product already has its two hires, Ada and Jo, who score higher.</p>",
    },
    {
        "slug": "sprint-planning",
        "title": "Fit as many tickets as possible into a sprint",
        "group": "Answered by query rewriting",
        "class": "ILP",
        "direct": "S3_MAX_CARDINALITY_KNAPSACK",
        "filters": ["direct"],
        "card_tags": ["S3"],
        "features": ["maximize a count", "one budget", "query rewriting"],
        "summary": "With 20 story points of capacity, take the largest number of tickets.",
        "summary_plain": "Maximum-cardinality knapsack, answered by DeciDB's query rewriting.",
        "snippet": "DECIDE take(BOOL)\nSUCH THAT SUM(points * take) <= 20\nMAXIMIZE SUM(take)",
        "story": "<p>When every item is worth the same, a knapsack has an exact greedy answer: take the smallest items "
        "first. DeciDB recognizes the shape (a count objective and one weight budget) and sorts instead of "
        "searching.</p>",
        "setup": """CREATE TABLE Backlog (ticket VARCHAR, points INT);
INSERT INTO Backlog VALUES
    ('API-12', 5), ('API-15', 3), ('UI-4', 2), ('UI-9', 8),
    ('DB-2', 1), ('DB-7', 5), ('OPS-3', 3), ('OPS-8', 13);""",
        "query": """SELECT ticket, points, take
FROM Backlog
DECIDE take(BOOL)
SUCH THAT SUM(points * take) <= 20
MAXIMIZE SUM(take)
ORDER BY points;""",
        "dec": ["take"],
        "on": "take",
        "explain": [
            ("SUM(points * take) <= 20", "One budget."),
            ("MAXIMIZE SUM(take)", "Count the tickets: every ticket is worth one."),
        ],
        "answer": "<p>Six tickets for 19 points. The eight-point ticket would fit only by dropping two smaller ones.</p>",
    },
    {
        "slug": "fair-allocation",
        "title": "Allocate a budget fairly",
        "group": "Answered by query rewriting",
        "class": "LP",
        "direct": "R4_MAX_MIN_FAIRNESS",
        "filters": ["direct", "minmax"],
        "card_tags": ["R4", "MAXIMIZE MIN(…)"],
        "features": ["max-min fairness", "MIN in the objective", "query rewriting"],
        "summary": "Split 100 units among programs so that the worst-served program gets as large a share of its need as possible.",
        "summary_plain": "Weighted max-min fair allocation, answered by DeciDB's query rewriting.",
        "snippet": "DECIDE alloc(REAL)\nSUCH THAT alloc <= max_alloc AND SUM(alloc) <= 100\nMAXIMIZE MIN(alloc / need)",
        "story": "<p>Four programs need 110 units in total but only 100 are available, and each program can absorb only so "
        "much. Max-min fairness raises every program's share of its need together until the budget runs out. "
        "<code>MAXIMIZE MIN(...)</code> says exactly that.</p>",
        "setup": """CREATE TABLE Programs (program VARCHAR, need DOUBLE, max_alloc DOUBLE);
INSERT INTO Programs VALUES
    ('Clinic', 20, 40), ('Shelter', 30, 25), ('School', 50, 60), ('Pantry', 10, 30);""",
        "query": """SELECT program, need, max_alloc,
       ROUND(alloc, 2) AS alloc, ROUND(alloc / need, 3) AS share
FROM Programs
DECIDE alloc(REAL)
SUCH THAT alloc <= max_alloc AND SUM(alloc) <= 100
MAXIMIZE MIN(alloc / need);""",
        "dec": ["alloc", "share"],
        "explain": [
            ("alloc <= max_alloc", "A cap per program."),
            ("SUM(alloc) <= 100", "One shared budget."),
            ("MAXIMIZE MIN(alloc / need)", "Make the smallest share of need as large as possible."),
        ],
        "answer": "<p>Every program receives 83.3% of its need. Shelter's share is also its cap, 25 units. On the solver "
        "path this is a linear program; the rewritten query computes the level with DuckDB's own operators."
        "</p>",
    },
    {
        "slug": "matching",
        "title": "Match drivers to pickups",
        "group": "Answered by query rewriting",
        "class": "ILP",
        "direct": "O1_MONGE_ASSIGNMENT",
        "filters": ["direct", "scopes"],
        "card_tags": ["O1", "CROSS JOIN"],
        "features": ["one-to-one matching", "per / by on both sides", "query rewriting"],
        "summary": "Pair four drivers with four pickups along a road so the total distance driven is as small as possible.",
        "summary_plain": "One-to-one matching by position, answered by DeciDB's query rewriting.",
        "snippet": "SUCH THAT per d: SUM(x) by (d) = 1\n      AND per p: SUM(x) by (p) = 1\nMINIMIZE SUM(ABS(d.km - p.km) * x)",
        "story": "<p>When both sides sit on a line and the cost is the distance between them, the best one-to-one matching "
        "pairs them in sorted order. DeciDB recognizes the pattern, ranks both sides and joins on rank.</p>",
        "setup": """CREATE TABLE Drivers (driver VARCHAR, km DOUBLE NOT NULL);
INSERT INTO Drivers VALUES ('Ana', 1.0), ('Ben', 4.5), ('Cai', 7.0), ('Dee', 12.0);

CREATE TABLE Pickups (pickup VARCHAR, km DOUBLE NOT NULL);
INSERT INTO Pickups VALUES ('P1', 2.0), ('P2', 3.0), ('P3', 9.5), ('P4', 11.0);""",
        "query": """SELECT driver, pickup, driver_km, pickup_km FROM (
    SELECT d.driver, p.pickup, d.km AS driver_km, p.km AS pickup_km, x
    FROM Drivers d CROSS JOIN Pickups p
    DECIDE x(BOOL)
    SUCH THAT per d: SUM(x) by (d) = 1
          AND per p: SUM(x) by (p) = 1
    MINIMIZE SUM(ABS(d.km - p.km) * x)
) WHERE x = 1
ORDER BY driver;""",
        "explain_query": """SELECT d.driver, p.pickup, d.km AS driver_km, p.km AS pickup_km, x
FROM Drivers d CROSS JOIN Pickups p
DECIDE x(BOOL)
SUCH THAT per d: SUM(x) by (d) = 1
      AND per p: SUM(x) by (p) = 1
MINIMIZE SUM(ABS(d.km - p.km) * x)""",
        "explain": [
            ("CROSS JOIN", "Every driver–pickup pair is a candidate row."),
            (
                "per d: ... = 1 AND per p: ... = 1",
                "Each driver gets exactly one pickup and each pickup exactly one driver.",
            ),
            ("SUM(ABS(d.km - p.km) * x)", "Total distance. <code>ABS</code> of data is just a per-row number."),
        ],
        "answer": "<p>The first driver takes the first pickup, and so on down the road: 6 km in total.</p>",
    },
    {
        "slug": "depot-placement",
        "title": "Place one depot per highway",
        "group": "Answered by query rewriting",
        "class": "LP",
        "direct": "A2_SINGLE_FACILITY_LOCATION",
        "filters": ["direct", "keyed"],
        "card_tags": ["A2", "per K: x(REAL)"],
        "features": ["keyed REAL decision", "absolute-distance objective", "query rewriting"],
        "summary": "Choose a mile marker for one depot on each highway that minimizes the total distance to that highway's stores.",
        "summary_plain": "One-dimensional facility location per group, answered by DeciDB's query rewriting.",
        "snippet": "DECIDE per highway: depot(REAL)\nSUCH THAT depot <= 100\nMINIMIZE SUM(ABS(depot - mile))",
        "story": "<p>One shared value closest to many rows is a median (for absolute distance), a mean (for squared "
        "distance) or a midrange (for the largest distance). A keyed decision makes one such value per highway.</p>",
        "setup": """CREATE TABLE Stores (store VARCHAR, highway VARCHAR, mile DOUBLE);
INSERT INTO Stores VALUES
    ('S1', 'I-10', 3), ('S2', 'I-10', 8), ('S3', 'I-10', 9), ('S4', 'I-10', 20),
    ('S5', 'I-95', 2), ('S6', 'I-95', 6), ('S7', 'I-95', 40);""",
        "query": """SELECT highway, store, mile, depot
FROM Stores
DECIDE per highway: depot(REAL)
SUCH THAT depot <= 100
MINIMIZE SUM(ABS(depot - mile));""",
        "dec": ["depot"],
        "explain": [
            ("DECIDE per highway: depot(REAL)", "One depot position per highway, shared by that highway's rows."),
            ("SUM(ABS(depot - mile))", "Total distance to the stores. <code>ABS</code> over a decision is allowed."),
        ],
        "answer": "<p>A median per highway: mile 6 on I-95. On I-10, with four stores, any point from mile 8 to mile 9 "
        "is optimal and DeciDB returns 8. The I-95 store at mile 40 does not pull the depot toward it, as it "
        "would with squared distance.</p>",
    },
    # ------------------------------------------------------------------ diagnose
    {
        "slug": "diagnose",
        "title": "Find out why there is no answer",
        "group": "When there is no answer",
        "class": "ILP",
        "filters": ["diagnose"],
        "card_tags": ["DIAGNOSE"],
        "features": ["DIAGNOSE prefix", "smallest edit to a clause", "the result is a relation"],
        "summary": "Four items in a 40 kg bag is impossible. DIAGNOSE names the clause to change and by how much.",
        "summary_plain": "DIAGNOSE on an infeasible DeciDB query.",
        "snippet": "DIAGNOSE SELECT item, x FROM Items\nDECIDE x(BOOL)\nSUCH THAT SUM(x * weight) <= 40\n      AND SUM(x) >= 4",
        "story": "<p>The bag from the first example, with a lower weight limit and a new rule: take at least four items. "
        "Without the prefix, the query stops with an error:</p>"
        '<div class="code"><div class="code-head"><span class="code-title">Output</span></div><pre><code class="language-none">'
        "Invalid Input Error: DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see which clause to change."
        "</code></pre></div>",
        "setup": """CREATE TABLE Items (item VARCHAR, value INT, weight INT);
INSERT INTO Items VALUES
    ('Laptop', 100, 20), ('Camera', 60, 10), ('Drone', 120, 30),
    ('Tripod', 40, 15), ('Lens', 75, 12);""",
        "query": """DIAGNOSE
SELECT item, x
FROM Items
DECIDE x(BOOL)
SUCH THAT SUM(x * weight) <= 40
      AND SUM(x) >= 4
MAXIMIZE SUM(x * value);""",
        "explain_query": """SELECT item, x
FROM Items
DECIDE x(BOOL)
SUCH THAT SUM(x * weight) <= 40
      AND SUM(x) >= 4
MAXIMIZE SUM(x * value)""",
        "query_title": "DIAGNOSE query",
        "dec": ["suggested_change", "amount"],
        "explain": [
            (
                "DIAGNOSE",
                "Run the query and report on the run instead of returning its rows, as "
                "<code>EXPLAIN ANALYZE</code> does for an ordinary query.",
            ),
            (
                "clause / suggested_change",
                "The clause as you wrote it, and the smallest edit that makes the query feasible.",
            ),
            ("achievable_objective", "The objective you would get once the edit is applied."),
        ],
        "answer": "<p>The weight limit is the clause to move: the four lightest items weigh 57 kg, so the limit must rise "
        "by 17. With that change the best bag is worth 275. <code>DIAGNOSE</code> returns a relation, so you can "
        "query it:</p>",
        "after": """<!--sql check="diagnose"-->SELECT clause, suggested_change, amount
FROM (
    DIAGNOSE SELECT item, x FROM Items
    DECIDE x(BOOL)
    SUCH THAT SUM(x * weight) <= 40 AND SUM(x) >= 4
    MAXIMIZE SUM(x * value)
)
WHERE edit_source = 'source_literal';<!--/sql-->""",
    },
]
