// DeciDB reading pages: copy buttons on code blocks and the filters on the examples index.
// The kit's system.js runs the nav; nothing here moves on scroll. Both controls stay hidden without script.
(() => {
  // the evening grade: the visitor's setting, or the nav's switch (remembered). The kit reads the page tone
  // (system.js) and the grounds' mode (backgrounds.js) once on load, so both are set again on every change.
  const root = document.documentElement;
  const dark = matchMedia('(prefers-color-scheme: dark)');
  const theme = () => root.dataset.theme || (dark.matches ? 'dark' : 'light');
  const toggles = [...document.querySelectorAll('[data-theme-toggle]')];
  const sync = () => {
    const t = theme();
    root.dataset.tone = t;
    root.removeAttribute('data-mode');
    window.tasteBackgrounds?.detect();
    toggles.forEach(b => b.setAttribute('aria-pressed', String(t === 'dark')));
  };
  toggles.forEach(b => {
    b.hidden = false;
    b.addEventListener('click', () => {
      root.dataset.theme = theme() === 'dark' ? 'light' : 'dark';
      try { localStorage.setItem('decidb-theme', root.dataset.theme); } catch { /* private mode: this page only */ }
      sync();
    });
  });
  dark.addEventListener('change', () => { if (!root.dataset.theme) sync(); });
  sync();

  // one polite live region says what a copy button did, for screen readers
  let said = null;
  const say = text => {
    if (!said) { said = document.createElement('p'); said.className = 'sr-only'; said.setAttribute('aria-live', 'polite'); document.body.append(said); }
    said.textContent = '';
    setTimeout(() => { said.textContent = text; }, 50);
  };
  document.querySelectorAll('.code__copy').forEach(btn => {
    if (!navigator.clipboard) return;
    const label = btn.closest('.code').querySelector('.code__title')?.textContent || 'code';
    btn.hidden = false;
    btn.setAttribute('aria-label', `Copy ${label}`);
    btn.addEventListener('click', async () => {
      const code = btn.closest('.code').querySelector('pre code');
      try {
        await navigator.clipboard.writeText(code.textContent);
        btn.textContent = 'Copied';
        say(`${label} copied to the clipboard`);
      } catch {
        btn.textContent = 'Select and copy';
        say('Copy failed: select the code and copy it');
      }
      setTimeout(() => { btn.textContent = 'Copy'; }, 1600);
    });
  });

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
