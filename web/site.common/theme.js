// Light / dark theme for the browser's sites, as on the real web: it follows the system (Windows) setting until the
// visitor picks one with the site's toggle, which this site remembers (localStorage).
// Load it in <head> (no flash of the wrong theme):  <script src="https://site.common/theme.js"></script>
// The page styles both themes with html[data-theme="dark"]; any element with data-theme-toggle becomes the toggle.
(() => {
  const root = document.documentElement;
  const media = matchMedia('(prefers-color-scheme: dark)');
  const read = () => { try { return localStorage.getItem('theme'); } catch { return null; } };
  const write = v => { try { v ? localStorage.setItem('theme', v) : localStorage.removeItem('theme'); } catch { /* storage off */ } };
  const apply = () => {
    const chosen = read();
    const dark = chosen ? chosen === 'dark' : media.matches;
    root.dataset.theme = dark ? 'dark' : 'light';
    root.style.colorScheme = dark ? 'dark' : 'light';
    document.querySelectorAll('[data-theme-toggle]').forEach(b => {
      b.title = dark ? '切換為淺色模式' : '切換為深色模式';
      b.setAttribute('aria-label', b.title);
    });
  };
  window.Theme = {
    get dark() { return root.dataset.theme === 'dark'; },
    toggle() {
      const next = Theme.dark ? 'light' : 'dark';
      // Picking what the system already says goes back to following the system.
      write(next === (media.matches ? 'dark' : 'light') ? null : next);
      apply();
    },
  };
  media.addEventListener('change', apply);
  document.addEventListener('click', e => {
    if (e.target.closest('[data-theme-toggle]')) Theme.toggle();
  });
  document.addEventListener('DOMContentLoaded', apply);
  // A default look for the toggle (sites can restyle .theme-toggle).
  const style = document.createElement('style');
  style.textContent = `
.theme-toggle { width: 36px; height: 36px; border-radius: 50%; border: 1px solid currentColor; background: transparent; color: inherit;
  display: inline-grid; place-items: center; cursor: pointer; opacity: .85; padding: 0; flex: none; }
.theme-toggle:hover { opacity: 1; }
.theme-toggle::before { content: ''; width: 16px; height: 16px; background: currentColor;
  -webkit-mask: var(--theme-icon) center / contain no-repeat; mask: var(--theme-icon) center / contain no-repeat; }
html { --theme-icon: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath d='M21 14.5A8.5 8.5 0 0 1 9.5 3 8.5 8.5 0 1 0 21 14.5z'/%3E%3C/svg%3E"); }
html[data-theme="dark"] { --theme-icon: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Ccircle cx='12' cy='12' r='5'/%3E%3Cg stroke='black' stroke-width='2' stroke-linecap='round'%3E%3Cpath d='M12 1v3M12 20v3M1 12h3M20 12h3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M4.2 19.8l2.1-2.1M17.7 6.3l2.1-2.1'/%3E%3C/g%3E%3C/svg%3E"); }`;
  document.head.appendChild(style);
  apply();
})();
