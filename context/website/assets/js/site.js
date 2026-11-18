// DeciDB pages: the theme switch, the header's keyboard and focus rules, copy buttons on code blocks and the filters
// on the examples index. The kit's system.js runs the nav; nothing here moves on scroll. The switch, the copy buttons
// and the filters stay hidden without script.
(() => {
  // the evening grade: the head script has set data-theme from the stored choice or the visitor's setting. The kit
  // reads the page tone (system.js) and the grounds' mode (backgrounds.js) once on load, so both are set again here.
  const root = document.documentElement;
  const dark = matchMedia('(prefers-color-scheme: dark)');
  const stored = () => { try { return localStorage.getItem('decidb-theme'); } catch { return null; } };
  const toggles = [...document.querySelectorAll('[data-theme-toggle]')];
  const meta = document.querySelector('meta[name="theme-color"]');
  const sync = () => {
    const t = root.dataset.theme === 'dark' ? 'dark' : 'light';
    root.dataset.tone = t;
    root.removeAttribute('data-mode');
    window.tasteBackgrounds?.detect();
    toggles.forEach(b => b.setAttribute('aria-pressed', String(t === 'dark')));
    if (meta) meta.content = getComputedStyle(root).getPropertyValue('--p-bg').trim() || meta.content;
  };
  toggles.forEach(b => {
    b.hidden = false;
    b.addEventListener('click', () => {
      root.dataset.theme = root.dataset.theme === 'dark' ? 'light' : 'dark';
      try { localStorage.setItem('decidb-theme', root.dataset.theme); } catch { /* private mode: this page only */ }
      sync();
    });
  });
  // with nothing chosen, the page follows the visitor's setting as it changes
  dark.addEventListener('change', () => { if (!stored()) { root.dataset.theme = dark.matches ? 'dark' : 'light'; sync(); } });
  sync();

  // the phone and tablet sheet: while it is open the page behind it is inert, focus starts on its first link, and
  // Tab goes round the sheet and its close button (the kit's Escape closes it and returns focus to the button)
  const nav = document.querySelector('.nav');
  const navToggle = nav?.querySelector('.nav__toggle');
  if (navToggle) {
    const behind = [...document.querySelectorAll('body > main, body > footer')];
    const isOpen = () => navToggle.getAttribute('aria-expanded') === 'true';
    const stops = () => [...nav.querySelectorAll('.nav__links :is(a, button), .nav__actions :is(a, button)')]
      .filter(el => !el.hidden && el.getClientRects().length);
    new MutationObserver(() => {
      behind.forEach(el => { el.inert = isOpen(); });
      if (isOpen()) stops()[0]?.focus();
    }).observe(navToggle, { attributes: true, attributeFilter: ['aria-expanded'] });
    nav.addEventListener('keydown', e => {
      if (e.key !== 'Tab' || !isOpen()) return;
      const list = stops();
      if (!list.length) return;
      const at = document.activeElement, first = list[0], last = list[list.length - 1];
      const to = !e.shiftKey && (at === navToggle) ? first
        : !e.shiftKey && at === last ? navToggle
        : e.shiftKey && at === first ? navToggle
        : e.shiftKey && at === navToggle ? last : null;
      if (to) { e.preventDefault(); to.focus(); }
    });
  }

  // a desktop menu panel opened by hover or focus shuts on Escape and focus stays where it was (the kit would blur
  // it); the next hover, or focus coming back to the item, opens it again
  const wide = matchMedia('(min-width: 1024px)');
  const items = [...document.querySelectorAll('.nav__item')].filter(i => i.querySelector('.nav__menu'));
  window.addEventListener('keydown', e => {
    if (e.key !== 'Escape' || !wide.matches) return;
    const open = items.find(i => !i.classList.contains('is-shut') && (i.matches(':hover') || i.matches(':focus-within')));
    if (!open) return;
    open.classList.add('is-shut');
    e.stopPropagation();
  }, true);
  items.forEach(i => {
    i.addEventListener('pointerenter', () => i.classList.remove('is-shut'));
    i.addEventListener('focusout', e => { if (!i.contains(e.relatedTarget)) i.classList.remove('is-shut'); });
  });

  // one polite live region says what a copy button did, for screen readers
  let said = null;
  const say = text => {
    if (!said) { said = document.createElement('p'); said.className = 'sr-only'; said.setAttribute('aria-live', 'polite'); document.body.append(said); }
    said.textContent = '';
    setTimeout(() => { said.textContent = text; }, 50);
  };
  // each copy button is named by its block and the heading it sits under ("Copy SQL: Keyed decisions", "... 2")
  if (navigator.clipboard) {
    let heading = '', count = 0;
    document.querySelectorAll('main :is(h1, h2, h3, .code)').forEach(el => {
      if (!el.classList.contains('code')) { heading = el.textContent.replace(/\s+/g, ' ').trim(); count = 0; return; }
      const btn = el.querySelector('.code__copy');
      if (!btn) return;
      const title = el.querySelector('.code__title')?.textContent || 'code';
      count += 1;
      const label = `${title}${heading ? `: ${heading}` : ''}${count > 1 ? ` ${count}` : ''}`;
      btn.hidden = false;
      btn.setAttribute('aria-label', `Copy ${label}`);
      btn.addEventListener('click', async () => {
        try {
          await navigator.clipboard.writeText(el.querySelector('pre code').textContent);
          btn.textContent = 'Copied';
          say(`${label} copied to the clipboard`);
        } catch {
          btn.textContent = 'Select and copy';
          say('Copy failed: select the code and copy it');
        }
        setTimeout(() => { btn.textContent = 'Copy'; }, 1600);
      });
    });
  }

  const bar = document.querySelector('.filters');
  if (!bar) return;
  bar.hidden = false;
  const buttons = [...bar.querySelectorAll('.filter')];
  const rows = [...document.querySelectorAll('.ex-list > li')];
  buttons.forEach(btn => btn.addEventListener('click', () => {
    const key = btn.dataset.filter;
    buttons.forEach(b => b.setAttribute('aria-pressed', String(b === btn)));
    rows.forEach(li => { li.hidden = key !== 'all' && !li.dataset.tags.split(' ').includes(key); });
    document.querySelectorAll('.ex-group').forEach(g => { g.hidden = !g.querySelector('.ex-list > li:not([hidden])'); });
  }));
})();
