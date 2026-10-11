// Periodic refresh that never gets in the way: a page that redraws itself while the visitor is clicking turns the click
// into a text drag (the pressed element is replaced before the button comes up), and a redrawn input loses its focus.
//   <script src="https://site.common/live.js"></script>
//   Live.every(5000, load);   // calls load() every 5 s, but waits while the visitor is busy
// "Busy": a mouse button is down or was released less than half a second ago, a text field has the focus, or text is
// selected.
window.Live = (() => {
  let down = false, lastPointer = 0;
  addEventListener('pointerdown', () => { down = true; lastPointer = Date.now(); }, true);
  addEventListener('pointerup', () => { down = false; lastPointer = Date.now(); }, true);
  addEventListener('pointercancel', () => { down = false; }, true);

  function busy() {
    if (down || Date.now() - lastPointer < 500) return true;
    const a = document.activeElement;
    if (a && (a.tagName === 'INPUT' || a.tagName === 'TEXTAREA' || a.tagName === 'SELECT' || a.isContentEditable)) return true;
    const sel = getSelection();
    return !!(sel && !sel.isCollapsed);
  }

  // Runs fn every `ms`; a turn that falls while busy is retried every 250 ms until the visitor is idle.
  function every(ms, fn) {
    let due = Date.now() + ms;
    setInterval(() => {
      if (Date.now() < due || busy()) return;
      due = Date.now() + ms;
      fn();
    }, 250);
  }

  return { busy, every };
})();
