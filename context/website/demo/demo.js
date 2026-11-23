// The live demo (demo.html): DeciDB runs in a worker (worker.js); each step's editor runs its query there, and the
// festival board draws the acts the last query booked. The page is built by context/website_src/build.py, which
// runs every query here on the native binary first; this script only runs them again, in the browser.

const data = JSON.parse(document.getElementById("demo-data").textContent);
const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
const escH = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const fmtInt = (n) => Math.round(n).toLocaleString("en-US");
const fmtMs = (ms) => (ms < 10 ? ms.toFixed(1) : fmtInt(ms)) + " ms";

// ---------------------------------------------------------------- SQL colors (build.py's hl_sql, word for word)
const W = data.words;
const KEYWORDS = new Set(W.keywords), DECIDE = new Set(W.decide), SCOPE = new Set(W.scope);
const TYPES = new Set(W.types), FUNCS = new Set(W.funcs);
const TOKEN = /(--[^\n]*)|('(?:[^']|'')*')|(\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)|([A-Za-z_][A-Za-z_0-9]*)(?=(\s*\()?)|([\s\S])/g;
const DECLARED = new RegExp(String.raw`\b([A-Za-z_]\w*)\s*\(\s*(?:${[...TYPES].sort().join("|")})\s*\)`, "g");

function decisionNames(sql) {
  const names = new Set();
  for (const m of sql.matchAll(/\bDECIDE\b([\s\S]*?)(?=\bSUCH\s+THAT\b|\bFROM\b|\bMAXIMIZE\b|\bMINIMIZE\b|;|$)/g)) {
    for (const d of m[1].matchAll(DECLARED)) names.add(d[1]);
  }
  return names;
}

function hlSql(code) {
  const names = decisionNames(code);
  const span = (cls, t) => `<span class="${cls}">${escH(t)}</span>`;
  let out = "";
  for (const [, com, str, num, ident, paren, other] of code.matchAll(TOKEN)) {
    if (com) out += span("tk-c", com);
    else if (str) out += span("tk-s", str);
    else if (num) out += span("tk-n", num);
    else if (ident) {
      if (names.has(ident)) out += span("tk-v", ident);
      else if (SCOPE.has(ident) || DECIDE.has(ident)) out += span("tk-d", ident);
      else if (TYPES.has(ident)) out += span("tk-t", ident);
      else if (ident === ident.toUpperCase() && KEYWORDS.has(ident)) out += span("tk-k", ident);
      else if (paren && FUNCS.has(ident.toUpperCase())) out += span("tk-f", ident);
      else out += escH(ident);
    } else out += escH(other);
  }
  return out;
}

// ---------------------------------------------------------------- the engine
class Engine {
  constructor() {
    this.seq = 0;
    this.pending = new Map();
    this.log = []; // the data changes since the setup, replayed when a stopped query restarts the worker
    this.handlers = {};
    this.start();
  }
  on(name, f) { (this.handlers[name] ||= []).push(f); }
  emit(name, x) { (this.handlers[name] || []).forEach((f) => f(x)); }
  start() {
    this.state = "loading";
    this.ready = new Promise((resolve, reject) => { this.resolve = resolve; this.reject = reject; });
    this.ready.catch(() => {});
    try {
      this.worker = new Worker(new URL("worker.js", import.meta.url), { type: "module" });
    } catch (err) {
      return this.fail(err.message || err);
    }
    this.worker.onmessage = (e) => this.message(e.data);
    this.worker.onerror = (e) => this.fail(e.message || "the worker stopped");
  }
  message(m) {
    if (m.type === "progress") this.emit("progress", m);
    else if (m.type === "failed") this.fail(m.error);
    else if (m.type === "ready") {
      this.info = m;
      this.load().then(() => { this.state = "ready"; this.resolve(); this.emit("ready", m); }, (err) => this.fail(err.message || err));
    } else if (m.type === "result") {
      const done = this.pending.get(m.id);
      this.pending.delete(m.id);
      if (done) done(m);
    }
  }
  fail(why) {
    this.state = "failed";
    this.reject(why);
    this.emit("failed", String(why));
  }
  send(sql) {
    return new Promise((done) => {
      const id = ++this.seq;
      this.pending.set(id, done);
      this.worker.postMessage({ id, sql });
    });
  }
  async load() {
    for (const sql of [data.setup, data.big.replace("{N}", String(data.bigN)), ...this.log]) {
      const r = await this.send(sql);
      if (!r.ok) throw new Error(r.error);
    }
  }
  async run(sql) {
    await this.ready;
    return this.send(sql);
  }
  async change(sql) {
    const r = await this.run(sql);
    if (r.ok) this.log.push(sql);
    this.emit("data");
    return r;
  }
  async restore() {
    this.log = [];
    const r = await this.run("DROP TABLE IF EXISTS Acts;\n" + data.setup);
    this.emit("data");
    return r;
  }
  restart() {
    this.worker.terminate();
    for (const done of this.pending.values()) done({ ok: false, stopped: true, error: "Stopped." });
    this.pending.clear();
    this.emit("restart");
    this.start();
  }
}
const engine = new Engine();

// ---------------------------------------------------------------- results
const NUMERIC = /INT|DOUBLE|FLOAT|DECIMAL|REAL|NUMERIC/;
const truthy = (v) => v !== null && v !== "false" && (Number.isNaN(Number(v)) || Number(v) !== 0);

function outputHead(left, right) {
  return `<div class="output__head"><span>${left}</span><span>${right}</span></div>`;
}

function pathWords(path) {
  if (!path) return "";
  return path.rule ? `rewritten, rule ${path.rule.split("_")[0]}, no solver` : "solved with HiGHS";
}

function resultTable(r, dec, path) {
  const cols = r.columns;
  const on = cols.findIndex((c) => dec.has(c));
  const num = r.types.map((t) => NUMERIC.test(t));
  const shown = r.rows.slice(0, 40);
  const head = cols
    .map((c, i) => `<th scope="col" class="${[num[i] ? "num" : "", dec.has(c) ? "dec" : ""].join(" ").trim()}">${escH(c)}</th>`)
    .join("");
  const body = shown
    .map((row) => {
      const tds = row
        .map((v, i) => {
          const cls = [v === null ? "null" : num[i] ? "num" : "", dec.has(cols[i]) ? "dec" : ""].join(" ").trim();
          return `<td${cls ? ` class="${cls}"` : ""}>${v === null ? "NULL" : escH(v)}</td>`;
        })
        .join("");
      return `<tr${on >= 0 && truthy(row[on]) ? ' class="on"' : ""}>${tds}</tr>`;
    })
    .join("");
  const n = r.rows.length;
  const right = [
    n > shown.length ? `the first ${shown.length} of ${fmtInt(n)} rows` : `${n} row${n === 1 ? "" : "s"}`,
    fmtMs(r.ms),
    pathWords(path),
  ].filter(Boolean).join(" · ");
  return `<div class="output">${outputHead("Result", right)}<div class="output__scroll"><table><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table></div></div>`;
}

function errorBox(message, status) {
  return `<div class="output output--error">${outputHead("Error", status)}<pre>${escH(message)}</pre></div>`;
}

function errorStatus(message) {
  if (/infeasible/i.test(message)) return "no answer exists";
  if (/unbounded/i.test(message)) return "no best answer";
  if (/time limit/i.test(message)) return "out of time";
  return "refused";
}

// The rule EXPLAIN names, or none: the solver runs. Asked only of a single DECIDE statement.
async function findPath(sql) {
  const q = sql.trim().replace(/;\s*$/, "");
  if (!/\bDECIDE\b/.test(q) || /^(DIAGNOSE|EXPLAIN)\b/i.test(q) || q.includes(";")) return null;
  const r = await engine.run("EXPLAIN " + q);
  if (!r.ok) return null;
  const text = r.rows.flat().join("\n").replace(/[│┌┐└┘─┬┴├┤]/g, " ").replace(/\s+/g, " ");
  const m = text.match(/Direct solve rule: ([A-Z][A-Z0-9_]+)/);
  return m ? { rule: m[1] } : { solver: true };
}

// ---------------------------------------------------------------- the editors
const lives = new Map();
let pressed = null; // the editor whose run the board shows

class Live {
  constructor(el) {
    this.el = el;
    this.id = el.dataset.live;
    this.extraDec = (el.dataset.dec || "").split(",").filter(Boolean);
    this.ta = $("textarea", el);
    this.code = $(".live__edit pre", el);
    this.runBtn = $(".live__run", el);
    this.resetBtn = $(".live__reset", el);
    this.status = $(".live__status", el);
    this.out = $(".live__out", el);
    this.original = this.ta.value;
    this.controls = $$(`.live__controls[data-for="${this.id}"] input[data-sub]`);
    this.ta.addEventListener("input", () => this.changed());
    this.ta.addEventListener("scroll", () => { this.code.scrollLeft = this.ta.scrollLeft; });
    this.ta.addEventListener("keydown", (e) => {
      if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) { e.preventDefault(); this.run(); }
    });
    this.runBtn.addEventListener("click", () => (this.running ? engine.restart() : this.run()));
    this.resetBtn.addEventListener("click", () => { this.set(this.original); this.run(); });
    for (const input of this.controls) {
      input.addEventListener("input", () => this.slide(input));
    }
    this.changed();
  }
  set(code) {
    this.ta.value = code;
    this.changed();
  }
  changed() {
    const code = this.ta.value;
    $("code", this.code).innerHTML = hlSql(code) + "\n";
    this.ta.rows = code.split("\n").length;
    this.resetBtn.hidden = code === this.original;
    for (const input of this.controls) {
      const m = code.match(new RegExp(input.dataset.sub));
      input.disabled = !m;
      if (m) { input.value = m[2]; this.label(input); }
    }
    // a result under a query that has since changed fades until Run runs the new query
    const stale = this.shown !== undefined && code.trim() !== this.shown;
    this.el.classList.toggle("is-stale", stale);
    if (!this.running) this.status.textContent = stale ? "Edited, press Run" : "";
  }
  label(input) {
    const out = input.parentElement.querySelector("output");
    if (out) out.textContent = input.dataset.unit === "k" ? `$${input.value}k` : input.value;
  }
  // a slider writes its number into the query; Run runs it
  slide(input) {
    const re = new RegExp(input.dataset.sub);
    this.set(this.ta.value.replace(re, (_, before) => before + input.value));
  }
  async run() {
    if (this.running) return;
    const sql = this.ta.value.trim();
    if (!sql) return;
    if (engine.state === "failed") return this.recorded();
    this.running = true;
    this.el.classList.add("is-running");
    this.status.textContent = engine.state === "ready" ? "Running…" : "Waiting for DeciDB…";
    this.runBtn.disabled = true;
    this.runBtn.textContent = "Running…";
    const stop = setTimeout(() => {
      if (this.running) { this.runBtn.disabled = false; this.runBtn.textContent = "Stop"; }
    }, 1500);
    let r;
    let path = null;
    try {
      r = await engine.run(sql);
      if (r.ok) path = await findPath(sql);
    } catch (err) {
      r = { ok: false, error: String(err) };
    }
    clearTimeout(stop);
    this.running = false;
    this.el.classList.remove("is-running");
    this.runBtn.disabled = false;
    this.runBtn.textContent = "Run";
    this.show(r, path, sql);
    this.shown = sql;
    this.changed();
    if (r.ok) this.el.closest(".demo-step")?.classList.add("is-done");
    return r;
  }
  show(r, path, sql) {
    if (r.stopped) {
      this.out.innerHTML = errorBox("You stopped the query. DeciDB restarted with the same tables.", "stopped");
      return;
    }
    if (!r.ok) {
      const helped = [...lives.values()].some((l) => l.el.dataset.target === this.id);
      const fix = /DIAGNOSE/.test(r.error) && !/^\s*DIAGNOSE\b/i.test(sql) && !helped
        ? `<div class="live__fix"><button class="pill" type="button" data-diagnose>Run it with DIAGNOSE</button></div>` : "";
      this.out.innerHTML = errorBox(r.error, errorStatus(r.error)) + fix;
      $("[data-diagnose]", this.out)?.addEventListener("click", () => this.diagnose(sql));
      return;
    }
    const dec = new Set([...decisionNames(sql), ...this.extraDec]);
    if (r.columns.length === 2 && r.columns[0] === "explain_key") {
      const plan = r.rows.map((row) => row[1]).join("\n");
      this.out.innerHTML = `<div class="output">${outputHead("Plan", fmtMs(r.ms))}<pre class="live__plan">${escH(plan)}</pre></div>`;
      return;
    }
    this.out.innerHTML = resultTable(r, dec, path) + (r.columns.includes("suggested_change") ? this.suggestions(r) : "");
    for (const b of $$("[data-apply]", this.out)) b.addEventListener("click", () => this.apply(b.dataset.from, b.dataset.to));
    board.show(this, r, dec, path, sql);
  }
  suggestions(r) {
    const col = (name) => r.columns.indexOf(name);
    const items = [];
    let best = null;
    for (const row of r.rows) {
      const clause = row[col("clause")], to = row[col("suggested_change")];
      if (clause && to) {
        items.push(`<li><code>${hlSql(clause)}</code><span aria-hidden="true">→</span><code>${hlSql(to)}</code>`
          + `<button class="pill" type="button" data-apply data-from="${escH(clause)}" data-to="${escH(to)}">Apply</button></li>`);
      } else if (row[col("edit_source")] === "achievable_objective" && row[col("amount")] !== null) {
        best = Number(row[col("amount")]);
      }
    }
    if (!items.length) return "";
    const after = best === null ? "" : `<p>With that change, the objective reaches ${fmtInt(best)}.</p>`;
    return `<div class="live__fix"><p>DIAGNOSE suggests</p><ul>${items.join("")}</ul>${after}</div>`;
  }
  // put a suggestion into the query it diagnoses (this editor's target, or this editor without its DIAGNOSE)
  apply(from, to) {
    const target = lives.get(this.el.dataset.target) || this;
    let text = target.ta.value;
    if (target === this) text = text.replace(/^\s*DIAGNOSE\s*/i, "");
    const loose = new RegExp(from.replace(/[.*+?^${}()|[\]\\]/g, "\\$&").replace(/\s+/g, "\\s*"));
    if (!loose.test(text)) return;
    target.set(text.replace(loose, to));
    if (target !== this) target.el.scrollIntoView({ behavior: "smooth", block: "center" });
    target.run();
  }
  diagnose(sql) {
    this.set("DIAGNOSE\n" + sql);
    this.run();
  }
  recorded() {
    const t = $("template.live__recorded", this.el);
    const edited = this.ta.value !== this.original ? " It is the result of the query as first written." : "";
    this.out.innerHTML = (t ? t.innerHTML : "")
      + `<p class="live__note">This browser could not start DeciDB, so this is the result the site recorded when it was built.${edited}</p>`;
  }
}

// ---------------------------------------------------------------- the festival board
const GENRES = ["Rock", "Jazz", "Electronic", "Folk"];
const SVG = "http://www.w3.org/2000/svg";
const board = {
  plot: $("[data-board-plot]"),
  acts: [],
  booked: null,
  budget: null,
  async refresh() {
    const r = await engine.run("SELECT act, genre, fee, crowd FROM Acts ORDER BY genre, act");
    if (!r.ok) return;
    this.acts = r.rows.map(([act, genre, fee, crowd]) => ({ act, genre, fee: Number(fee), crowd: Number(crowd) }));
    if (this.booked) for (const a of [...this.booked]) if (!this.acts.some((x) => x.act === a)) this.booked.delete(a);
    this.draw();
  },
  show(live, r, dec, path, sql) {
    const act = r.columns.indexOf("act");
    const d = r.columns.findIndex((c) => dec.has(c) && decisionNames(sql).has(c));
    if (act < 0) return;
    this.booked = d >= 0 ? new Set(r.rows.filter((row) => truthy(row[d])).map((row) => row[act])) : null;
    const name = d >= 0 ? r.columns[d] : "";
    const m = name && sql.match(new RegExp(String.raw`SUM\(\s*${name}\s*\*\s*fee\s*\)\s*<=\s*(\d+(?:\.\d+)?)`));
    this.budget = m ? Number(m[1]) : null;
    const step = live.el.closest(".demo-step");
    const n = step ? $(".demo-step__n", step)?.textContent : "";
    const how = d < 0 ? "the acts as they are" : path?.rule ? `rule ${path.rule.split("_")[0]}, no solver` : "HiGHS";
    $("[data-board-path]").textContent = [n, how, fmtMs(r.ms)].filter(Boolean).join(" · ");
    this.draw();
  },
  draw() {
    const acts = this.acts;
    const P = { l: 46, r: 350, t: 26, b: 226 };
    const maxFee = Math.max(45, ...acts.map((a) => a.fee + 5));
    const maxCrowd = Math.ceil(Math.max(12000, ...acts.map((a) => a.crowd)) / 4000) * 4000;
    const x = (v) => P.l + (v / maxFee) * (P.r - P.l);
    const y = (v) => P.b - (v / maxCrowd) * (P.b - P.t);
    let grid = "";
    for (let f = 0; f <= maxFee; f += 10) {
      grid += `<line class="board__grid" x1="${x(f)}" x2="${x(f)}" y1="${P.t}" y2="${P.b}"/><text class="board__tick" x="${x(f)}" y="${P.b + 15}" text-anchor="middle">${f}</text>`;
    }
    for (let c = 0; c <= maxCrowd; c += 4000) {
      grid += `<line class="board__grid" x1="${P.l}" x2="${P.r}" y1="${y(c)}" y2="${y(c)}"/><text class="board__tick" x="${P.l - 6}" y="${y(c) + 4}" text-anchor="end">${c ? c / 1000 + "k" : 0}</text>`;
    }
    grid += `<text class="board__axis" x="${P.r}" y="${P.b + 32}" text-anchor="end">fee, $ thousands</text>`
      + `<text class="board__axis" x="${P.l - 34}" y="${P.t - 14}">crowd</text>`;
    const dots = acts
      .map((a) => {
        const on = this.booked?.has(a.act);
        const g = GENRES.includes(a.genre) ? a.genre.toLowerCase() : "other";
        return `<circle class="board__dot g-${g}${on ? " is-on" : ""}" cx="${x(a.fee)}" cy="${y(a.crowd)}" r="${on ? 7 : 5}">`
          + `<title>${escH(a.act)}, ${escH(a.genre)}: $${a.fee}k, ${fmtInt(a.crowd)} people${on ? ", booked" : ""}</title></circle>`;
      })
      .join("");
    this.plot.innerHTML = `<desc id="board-desc">Each act drawn by its fee and its expected crowd; booked acts are filled.</desc>${grid}${dots}`;
    const genres = [...new Set([...GENRES.filter((g) => acts.some((a) => a.genre === g)), ...acts.map((a) => a.genre)])];
    $("[data-board-legend]").innerHTML = genres
      .map((g) => `<span class="board__key g-${GENRES.includes(g) ? g.toLowerCase() : "other"}">${escH(g)}</span>`)
      .join("");
    const on = acts.filter((a) => this.booked?.has(a.act));
    const fees = on.reduce((s, a) => s + a.fee, 0);
    $("[data-board-fees]").textContent = this.booked ? `$${fees}k${this.budget !== null ? ` of $${this.budget}k` : ""}` : "–";
    $("[data-board-crowd]").textContent = this.booked ? fmtInt(on.reduce((s, a) => s + a.crowd, 0)) : "–";
    $("[data-board-count]").textContent = this.booked ? `${on.length} of ${acts.length}` : "–";
    const meter = $("[data-board-meter]");
    meter.hidden = !this.booked || this.budget === null;
    if (!meter.hidden) meter.firstElementChild.style.width = `${Math.min(100, (fees / this.budget) * 100)}%`;
    $("[data-board-booked]").innerHTML = on
      .map((a) => `<li><span class="board__key g-${GENRES.includes(a.genre) ? a.genre.toLowerCase() : "other"}"></span>${escH(a.act)}<span>$${a.fee}k · ${fmtInt(a.crowd)}</span></li>`)
      .join("");
  },
};

// ---------------------------------------------------------------- the steps' other controls
function wireActions() {
  for (const box of $$(".live__actions[data-for]")) {
    const live = lives.get(box.dataset.for);
    for (const b of $$("button", box)) {
      b.addEventListener("click", async () => {
        if (b.hasAttribute("data-restore")) {
          await engine.restore();
          $$("button[data-sql]", box).forEach((x) => { x.disabled = false; });
        } else {
          const r = await engine.change(b.dataset.sql);
          if (!r.ok) return;
          b.disabled = true;
        }
        await board.refresh();
        live?.run();
      });
    }
  }
  for (const b of $$("[data-idea]")) {
    b.addEventListener("click", () => {
      const live = lives.get(b.dataset.idea);
      if (!live) return;
      live.set(b.dataset.code);
      live.run();
    });
  }
}

// the speed step: one table of N acts, the same query rewritten and on the solver
function wireRace() {
  const box = $("[data-race]");
  if (!box) return;
  const live = lives.get(box.dataset.race);
  const bars = $(".race__bars", box);
  const go = $("[data-race-go]", box);
  let n = Number($('[aria-pressed="true"]', box)?.dataset.n || data.bigN);
  for (const b of $$("[data-n]", box)) {
    b.addEventListener("click", () => {
      $$("[data-n]", box).forEach((x) => x.setAttribute("aria-pressed", String(x === b)));
      n = Number(b.dataset.n);
    });
  }
  const row = (label, cls) => `<div class="race__row ${cls}"><span>${label}</span><div class="race__track"><i></i></div><b>…</b></div>`;
  let racing = false;
  go.addEventListener("click", async () => {
    if (racing) return engine.restart();
    racing = true;
    go.textContent = "Stop";
    const sql = live.ta.value.trim();
    bars.innerHTML = `<p>Making ${fmtInt(n)} acts…</p>`;
    const made = await engine.change(data.big.replace("{N}", String(n)));
    if (!made.ok) {
      bars.innerHTML = errorBox(made.error, made.stopped ? "stopped" : "refused");
      racing = false;
      go.textContent = "Race";
      return;
    }
    bars.innerHTML = row("Query rewriting", "race__row--fast") + row("Solver (HiGHS)", "race__row--slow");
    const [fast, slow] = $$(".race__row", bars);
    const tick = (el, t0) => setInterval(() => { $("b", el).textContent = fmtMs(performance.now() - t0) + "…"; }, 100);
    const timed = async (el, mode) => {
      await engine.run(`SET decide_direct_solve = '${mode}'`);
      const t = tick(el, performance.now());
      const r = await engine.run(sql);
      clearInterval(t);
      $("b", el).textContent = r.ok ? fmtMs(r.ms) : r.stopped ? "stopped" : "error";
      return r;
    };
    const a = await timed(fast, "auto");
    if (a.ok) live.show(a, null, sql);
    const b = a.stopped ? { ok: false, stopped: true } : await timed(slow, "off");
    if (!b.stopped) await engine.run("SET decide_direct_solve = 'auto'");
    const top = Math.max(a.ms || 0, b.ms || 0) || 1;
    $(".race__track i", fast).style.width = `${Math.max(0.6, (a.ms / top) * 100)}%`;
    $(".race__track i", slow).style.width = `${Math.max(0.6, ((b.ms || 0) / top) * 100)}%`;
    if (a.ok && b.ok) {
      const same = JSON.stringify(a.rows) === JSON.stringify(b.rows);
      bars.insertAdjacentHTML("beforeend", `<p>Rewriting was ${fmtInt(b.ms / a.ms)} times faster on ${fmtInt(n)} acts. ${same ? "Both returned the same answer." : "The two answers differ: please tell us."}</p>`);
    } else if (b.stopped || a.stopped) {
      bars.insertAdjacentHTML("beforeend", "<p>You stopped the race. DeciDB restarted with the same tables.</p>");
    } else if (!b.ok) {
      bars.insertAdjacentHTML("beforeend", errorBox(b.error, errorStatus(b.error)));
    }
    racing = false;
    go.textContent = "Race";
  });
}

// the steps column: the step in view is marked, and a step whose query ran is ticked
function wireRail() {
  const links = new Map($$(".demo__rail a").map((a) => [a.getAttribute("href").slice(1), a]));
  const seen = new IntersectionObserver(
    (entries) => {
      for (const e of entries) {
        if (!e.isIntersecting) continue;
        links.forEach((a) => a.removeAttribute("aria-current"));
        links.get(e.target.id)?.setAttribute("aria-current", "step");
      }
    },
    { rootMargin: "-30% 0px -60% 0px" }
  );
  $$(".demo-step").forEach((s) => seen.observe(s));
  new MutationObserver(() => {
    $$(".demo-step").forEach((s) => links.get(s.id)?.parentElement.classList.toggle("is-done", s.classList.contains("is-done")));
  }).observe($(".demo__story"), { subtree: true, attributes: true, attributeFilter: ["class"] });
}

// ---------------------------------------------------------------- the engine's state, in the hero and the rail
function wireEngine() {
  const el = (k) => $(`[data-engine-${k}]`);
  const size = Number($("[data-engine]")?.dataset.engineSize) || 0;
  const mb = (b) => (b / 1e6).toFixed(1);
  const rail = $("[data-engine-state]");
  const set = (k, v) => { const e = el(k); if (e) e.textContent = v; };
  const tables = async () => {
    const r = await engine.run("SELECT (SELECT COUNT(*) FROM Acts), (SELECT COUNT(*) FROM Big)");
    if (r.ok) set("tables", `Acts, ${fmtInt(r.rows[0][0])} rows; Big, ${fmtInt(r.rows[0][1])} rows`);
  };
  set("status", "Downloading");
  rail.textContent = "Downloading DeciDB…";
  engine.on("progress", ({ loaded, total }) => {
    const all = total || size;
    set("loaded", all ? `${mb(loaded)} of ${mb(all)} MB` : `${mb(loaded)} MB`);
    const bar = el("bar");
    if (bar && all) bar.style.width = `${Math.min(100, (loaded / all) * 100)}%`;
    if (all && loaded >= all) { set("status", "Starting"); rail.textContent = "Starting DeciDB…"; }
  });
  engine.on("ready", (info) => {
    set("status", `Ready in ${(info.ms / 1000).toFixed(1)} s`);
    set("version", `DeciDB ${info.version}`);
    el("bar")?.style.setProperty("width", "100%");
    $("[data-engine]")?.classList.add("is-ready");
    rail.textContent = "DeciDB is running in this page.";
    rail.classList.add("is-ready");
    $$(".live__run").forEach((b) => { b.disabled = false; });
    tables();
    board.refresh();
  });
  engine.on("data", tables);
  engine.on("restart", () => { rail.textContent = "Restarting DeciDB…"; rail.classList.remove("is-ready"); });
  engine.on("failed", (why) => {
    set("status", "Could not start");
    set("loaded", why);
    $("[data-engine]")?.classList.add("is-failed");
    rail.textContent = "DeciDB could not start in this browser. Run shows the result recorded when the site was built.";
    rail.classList.add("is-failed");
    $$(".live__run").forEach((b) => { b.disabled = false; });
  });
}

for (const el of $$(".live[data-live]")) lives.set(el.dataset.live, new Live(el));
wireEngine();
wireActions();
wireRace();
wireRail();
board.draw();
