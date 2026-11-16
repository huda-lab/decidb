/* DeciDB website behavior: theme, mobile menu, copy buttons, tabs, docs TOC, DECIDE highlighting. */
(function () {
  "use strict";

  /* ---------- DECIDE-aware SQL highlighting ---------- */
  function highlight() {
    if (!window.Prism || !Prism.languages || !Prism.languages.sql) return;
    Prism.languages.insertBefore("sql", "keyword", {
      decide: { pattern: /\b(?:DECIDE|SUCH\s+THAT|MAXIMIZE|MINIMIZE|DIAGNOSE|per|when|by)\b/i, greedy: true },
      dtype: { pattern: /\b(?:BOOL|INT|REAL)\b(?=\s*\))/, greedy: true },
    });
    Prism.languages.insertBefore("sql", "function", {
      function: /\b(?:norm|power|abs|sum|avg|min|max|count|round|coalesce)(?=\s*\()/i,
    });
    Prism.highlightAll();
  }

  /* ---------- theme ---------- */
  function currentTheme() {
    var set = document.documentElement.getAttribute("data-theme");
    if (set) return set;
    return window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }
  function initTheme() {
    document.querySelectorAll(".theme-toggle").forEach(function (btn) {
      btn.addEventListener("click", function () {
        var next = currentTheme() === "dark" ? "light" : "dark";
        document.documentElement.setAttribute("data-theme", next);
        try { localStorage.setItem("decidb-theme", next); } catch (e) { /* storage unavailable */ }
      });
    });
  }

  /* ---------- mobile menu ---------- */
  function initMenu() {
    var btn = document.querySelector(".menu-btn");
    var nav = document.querySelector(".mobile-nav");
    if (!btn || !nav) return;
    btn.addEventListener("click", function () {
      var open = nav.classList.toggle("open");
      btn.setAttribute("aria-expanded", open ? "true" : "false");
      document.body.style.overflow = open ? "hidden" : "";
    });
    nav.querySelectorAll("a").forEach(function (a) {
      a.addEventListener("click", function () {
        nav.classList.remove("open");
        btn.setAttribute("aria-expanded", "false");
        document.body.style.overflow = "";
      });
    });
  }

  /* ---------- copy buttons ---------- */
  function initCopy() {
    document.querySelectorAll(".copy-btn").forEach(function (btn) {
      btn.addEventListener("click", function () {
        var block = btn.closest(".code");
        var code = block ? block.querySelector("pre code") : null;
        if (!code) return;
        var text = code.innerText;
        var done = function () {
          var old = btn.textContent;
          btn.textContent = "Copied";
          setTimeout(function () { btn.textContent = old; }, 1400);
        };
        if (navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(text).then(done, function () {});
        } else {
          var ta = document.createElement("textarea");
          ta.value = text;
          document.body.appendChild(ta);
          ta.select();
          try { document.execCommand("copy"); done(); } catch (e) { /* ignore */ }
          document.body.removeChild(ta);
        }
      });
    });
  }

  /* ---------- tabs ---------- */
  function initTabs() {
    document.querySelectorAll("[data-tabs]").forEach(function (root) {
      var tabs = root.querySelectorAll(".tab");
      var panels = root.querySelectorAll(".tab-panel");
      tabs.forEach(function (tab, i) {
        tab.addEventListener("click", function () {
          tabs.forEach(function (t, j) {
            t.setAttribute("aria-selected", i === j ? "true" : "false");
            t.tabIndex = i === j ? 0 : -1;
          });
          panels.forEach(function (p, j) { p.hidden = i !== j; });
        });
        tab.addEventListener("keydown", function (e) {
          var k = e.key === "ArrowRight" ? 1 : e.key === "ArrowLeft" ? -1 : 0;
          if (!k) return;
          var next = tabs[(i + k + tabs.length) % tabs.length];
          next.focus();
          next.click();
        });
      });
    });
  }

  /* ---------- docs TOC scrollspy ---------- */
  function initToc() {
    var links = Array.prototype.slice.call(document.querySelectorAll(".toc a[href^='#']"));
    if (!links.length || !("IntersectionObserver" in window)) return;
    var byId = {};
    links.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
    var targets = Object.keys(byId).map(function (id) { return document.getElementById(id); }).filter(Boolean);
    var visible = {};
    var obs = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) { visible[e.target.id] = e.isIntersecting; });
      var first = null;
      for (var i = 0; i < targets.length; i++) {
        if (visible[targets[i].id]) { first = targets[i].id; break; }
      }
      if (!first) return;
      links.forEach(function (a) { a.classList.remove("active"); });
      byId[first].classList.add("active");
    }, { rootMargin: "-72px 0px -65% 0px" });
    targets.forEach(function (t) { obs.observe(t); });
  }

  /* ---------- example filter ---------- */
  function initFilter() {
    var bar = document.querySelector(".filter-bar");
    if (!bar) return;
    var buttons = bar.querySelectorAll(".tab");
    buttons.forEach(function (b) {
      b.addEventListener("click", function () {
        var f = b.getAttribute("data-filter");
        buttons.forEach(function (o) { o.setAttribute("aria-selected", o === b ? "true" : "false"); });
        document.querySelectorAll("[data-tags]").forEach(function (card) {
          var show = f === "all" || card.getAttribute("data-tags").split(" ").indexOf(f) >= 0;
          card.hidden = !show;
        });
        document.querySelectorAll(".ex-group").forEach(function (g) {
          var any = g.querySelector("[data-tags]:not([hidden])");
          g.hidden = !any;
        });
      });
    });
  }

  function init() {
    initTheme();
    initMenu();
    initCopy();
    initTabs();
    initToc();
    initFilter();
    highlight();
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
