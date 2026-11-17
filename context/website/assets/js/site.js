// DeciDB reading pages: copy buttons on code blocks and the filters on the examples index.
// The kit's system.js runs the nav; nothing here moves on scroll. Both controls stay hidden without script.
(() => {
  document.querySelectorAll('.code__copy').forEach(btn => {
    if (!navigator.clipboard) return;
    btn.hidden = false;
    btn.addEventListener('click', async () => {
      const code = btn.closest('.code').querySelector('pre code');
      try {
        await navigator.clipboard.writeText(code.textContent);
        btn.textContent = 'Copied';
      } catch {
        btn.textContent = 'Select and copy';
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
