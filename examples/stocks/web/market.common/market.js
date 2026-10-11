// Shared stock market page: window.SITE = { key: 'lcn' | 'bawsaq', title, tagline }. Data from the stocks mod.
const characters = ['麥可', '富蘭克林', '崔佛'];
const money = n => '$' + Number(n).toLocaleString('en-US', { maximumFractionDigits: 0 });
const price = n => '$' + Number(n).toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 });
const pct = n => (n > 0 ? '+' : '') + n.toFixed(2) + '%';
const cls = n => (n > 0.0001 ? 'up' : n < -0.0001 ? 'down' : '');
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

document.title = SITE.title;
document.body.insertAdjacentHTML('afterbegin', `
<header class="top">
  <div class="logo">${SITE.title}<small>${SITE.tagline}</small></div>
  <div class="end"><div class="account"><span id="who"></span> <b id="cash"></b></div><button class="theme-toggle" data-theme-toggle></button></div>
</header>
<div class="tape"><div class="track" id="tape"></div></div>
<div class="closed" id="closed" hidden>目前無法交易：交易所沒有報價。</div>
<main>
  <div>
    <section class="card">
      <h2>上市公司 <small id="count"></small></h2>
      <div class="filters">
        <input id="search" type="search" placeholder="搜尋代號或公司名稱">
        <button id="fUp" data-f="up">上漲</button>
        <button id="fDown" data-f="down">下跌</button>
        <button id="fHeld" data-f="held">我的持股</button>
      </div>
      <table>
        <thead><tr><th data-s="ticker">代號 / 公司</th><th class="num" data-s="price">股價</th><th class="num" data-s="change">漲跌</th><th class="num">走勢</th></tr></thead>
        <tbody id="rows"></tbody>
      </table>
    </section>
  </div>
  <div>
    <section class="card" id="detailCard" hidden>
      <h2><span id="dTitle"></span> <small id="dTicker"></small></h2>
      <div class="detail">
        <div class="head"><div class="price" id="dPrice"></div><div id="dChange"></div></div>
        <svg id="dChart" viewBox="0 0 400 190" preserveAspectRatio="none"></svg>
        <div class="stats">
          <div><span class="sub">最高</span><b id="dHigh"></b></div>
          <div><span class="sub">最低</span><b id="dLow"></b></div>
          <div><span class="sub">持有</span><b id="dHeld"></b></div>
          <div><span class="sub">市值</span><b id="dValue"></b></div>
        </div>
        <div class="actions"><button class="primary" id="dBuy">買進</button><button class="sell" id="dSell">賣出</button></div>
      </div>
    </section>
    <section class="card" style="margin-top:22px">
      <h2>我的投資組合 <small id="slots"></small></h2>
      <div class="pf" id="portfolio"></div>
    </section>
    <section class="card" style="margin-top:22px">
      <h2>財經新聞</h2>
      <div class="newsbox" id="news"></div>
    </section>
  </div>
</main>
<div class="overlay" id="trade" hidden>
  <div class="panel">
    <h3 id="tTitle"></h3>
    <div class="sub" id="tSub"></div>
    <div class="qty"><button id="tMinus">−</button><input id="tShares" type="number" min="1" value="1"><button id="tPlus">＋</button></div>
    <div class="quick" id="tQuick"></div>
    <div class="sum">
      <span>股價</span><span id="tPrice"></span>
      <span id="tTotalLabel">總額</span><span class="big" id="tTotal"></span>
      <span>交易後現金</span><span id="tAfter"></span>
    </div>
    <div class="actions"><button class="sell" id="tCancel">取消</button><button class="primary" id="tGo">確認</button></div>
  </div>
</div>
<div class="toast" id="toast" hidden></div>`);

let data = { stocks: [], portfolio: [], news: [], cash: 0 };
let selected = null, sortKey = 'ticker', sortDir = 1, filter = '', trade = null;

function change(s) {
  const h = s.history.filter(v => v > 0);
  return { abs: s.change, pct: s.changePct, high: Math.max(s.price, ...h), low: Math.min(s.price, ...h) };
}

function spark(values, w, h, area, up) {
  const v = values.filter(x => x > 0);
  if (v.length < 2) return '';
  const lo = Math.min(...v), hi = Math.max(...v), span = hi - lo || 1;
  const pts = v.map((x, i) => [i / (v.length - 1) * w, h - 4 - (x - lo) / span * (h - 8)]);
  const line = pts.map(p => p.map(n => n.toFixed(1)).join(',')).join(' ');
  const color = up ? 'var(--up)' : 'var(--down)';
  const fill = area ? `<polygon points="0,${h} ${line} ${w},${h}" fill="${color}" opacity=".12"/>` : '';
  return `${fill}<polyline points="${line}" fill="none" stroke="${color}" stroke-width="${area ? 2.5 : 1.5}" vector-effect="non-scaling-stroke"/>`;
}

function when(hour) {
  if (hour == null) return '';
  const d = new Date(Date.UTC(2000, 0, 1) + hour * 3600000);
  return `${d.getUTCMonth() + 1} 月 ${d.getUTCDate()} 日 ${String(d.getUTCHours()).padStart(2, '0')}:00`;
}

async function load() {
  data = await game.call('stocks.list', SITE.key);
  $('who').textContent = characters[data.character] || '';
  $('cash').textContent = money(data.cash);
  $('closed').hidden = data.tradable;
  render();
}

function render() {
  const q = $('search').value.trim().toLowerCase();
  let list = data.stocks.map(s => ({ ...s, ch: change(s) }))
    .filter(s => !q || s.ticker.toLowerCase().includes(q) || s.name.toLowerCase().includes(q))
    .filter(s => filter === 'up' ? s.ch.pct > 0 : filter === 'down' ? s.ch.pct < 0 : filter === 'held' ? s.held > 0 : true);
  const key = { ticker: s => s.ticker, price: s => s.price, change: s => s.ch.pct }[sortKey];
  list.sort((a, b) => (key(a) > key(b) ? 1 : key(a) < key(b) ? -1 : 0) * sortDir);
  $('count').textContent = `共 ${list.length} 家`;
  $('rows').replaceChildren(...list.map(s => {
    const tr = document.createElement('tr');
    if (selected === s.id) tr.className = 'sel';
    tr.innerHTML = `<td><span class="tick">${esc(s.ticker)}</span>${s.held ? `<span class="held">${s.held} 股</span>` : ''}<div class="sub">${esc(s.name)}</div></td>
      <td class="num">${s.price > 0 ? price(s.price) : '—'}</td>
      <td class="num ${cls(s.ch.pct)}">${s.price > 0 ? pct(s.ch.pct) : ''}</td>
      <td class="num"><svg width="90" height="28" viewBox="0 0 90 28">${spark(s.history, 90, 28, false, s.ch.pct >= 0)}</svg></td>`;
    tr.onclick = () => { selected = s.id; render(); };
    return tr;
  }));
  $('tape').innerHTML = data.stocks.filter(s => s.price > 0).map(s => {
    const c = change(s);
    return `<span><b>${esc(s.ticker)}</b> ${price(s.price)} <span class="${cls(c.pct)}">${c.pct >= 0 ? '▲' : '▼'} ${Math.abs(c.pct).toFixed(2)}%</span></span>`;
  }).join('');
  renderDetail();
  renderPortfolio();
  renderNews();
}

function renderDetail() {
  const s = data.stocks.find(x => x.id === selected);
  $('detailCard').hidden = !s;
  if (!s) return;
  const c = change(s);
  $('dTitle').textContent = s.name || s.ticker;
  $('dTicker').textContent = s.ticker;
  $('dPrice').textContent = s.price > 0 ? price(s.price) : '—';
  $('dChange').innerHTML = s.price > 0 ? `<span class="${cls(c.pct)}">${c.abs >= 0 ? '+' : ''}${c.abs.toFixed(2)}（${pct(c.pct)}）</span><div class="sub">近期走勢</div>` : '';
  $('dChart').innerHTML = spark(s.history, 400, 190, true, c.pct >= 0);
  $('dHigh').textContent = s.price > 0 ? price(c.high) : '—';
  $('dLow').textContent = s.price > 0 ? price(c.low) : '—';
  $('dHeld').textContent = `${s.held} 股`;
  $('dValue').textContent = money(s.held * s.price);
  $('dBuy').disabled = !(s.price > 0) || data.character < 0;
  $('dSell').disabled = !(s.price > 0) || !s.held;
}

function renderPortfolio() {
  const rows = data.portfolio;
  $('slots').textContent = `${data.slotsUsed || 0} / ${data.slots || 10} 種`;
  if (!rows.length) {
    $('portfolio').innerHTML = '<div class="empty">還沒有任何持股。點選左邊的公司即可買進。</div>';
    return;
  }
  let value = 0, cost = 0;
  const html = rows.map(r => {
    const v = r.shares * r.price, pl = v - r.cost;
    value += v; cost += r.cost;
    const here = r.exchange === SITE.key;
    return `<div class="row" data-id="${r.id}" data-here="${here}">
      <div><span class="tick">${esc(r.ticker)}</span>${here ? '' : `<span class="other">${r.exchange === 'lcn' ? 'LCN' : 'BAWSAQ'}</span>`}<div class="sub">${r.shares} 股 · 成本 ${money(r.cost)}</div></div>
      <div class="r"><b>${money(v)}</b><div class="${cls(pl)}">${pl >= 0 ? '+' : '−'}${money(Math.abs(pl))}</div></div></div>`;
  }).join('');
  const pl = value - cost;
  $('portfolio').innerHTML = `<div class="total"><span>總市值 <b>${money(value)}</b></span><span class="${cls(pl)}">損益 ${pl >= 0 ? '+' : '−'}${money(Math.abs(pl))}</span></div>${html}`;
  $('portfolio').querySelectorAll('.row').forEach(el => el.onclick = () => {
    if (el.dataset.here === 'true') { selected = Number(el.dataset.id); render(); }
  });
}

function renderNews() {
  if (!data.news.length) {
    $('news').innerHTML = '<div class="empty">目前沒有新聞。</div>';
    return;
  }
  $('news').innerHTML = data.news.map(n => `<div class="news">
      <div class="t"><i class="${n.mood > 0 ? 'up' : n.mood < 0 ? 'down' : ''}"></i><span>${esc(n.title)}</span></div>
      ${n.body ? `<p>${esc(n.body)}</p>` : ''}${n.hour != null ? `<div class="when">${when(n.hour)}</div>` : ''}</div>`).join('');
}

// ---- trading ----
function openTrade(buy) {
  const s = data.stocks.find(x => x.id === selected);
  if (!s) return;
  trade = { buy, s };
  $('tTitle').textContent = `${buy ? '買進' : '賣出'} ${s.ticker}`;
  $('tSub').textContent = s.name;
  $('tPrice').textContent = price(s.price);
  $('tTotalLabel').textContent = buy ? '應付總額' : '可得總額';
  $('tGo').className = buy ? 'primary' : 'sell';
  $('tGo').textContent = buy ? '確認買進' : '確認賣出';
  const max = maxShares();
  const quick = buy ? [10, 100, 1000] : [];
  $('tQuick').innerHTML = quick.map(n => `<button data-n="${n}">${n} 股</button>`).join('') + `<button data-n="${max}">${buy ? '全部資金' : '全部持股'}（${max}）</button>`;
  $('tQuick').querySelectorAll('button').forEach(b => b.onclick = () => { $('tShares').value = b.dataset.n; updateTrade(); });
  $('tShares').value = Math.min(1, max) || 1;
  updateTrade();
  $('trade').hidden = false;
}
function maxShares() {
  return trade.buy ? Math.max(0, Math.floor(data.cash / trade.s.price)) : trade.s.held;
}
function updateTrade() {
  const n = Math.max(0, Math.floor(Number($('tShares').value) || 0));
  const total = n * trade.s.price;
  $('tTotal').textContent = price(total);
  const after = trade.buy ? data.cash - Math.ceil(total) : data.cash + Math.floor(total);
  $('tAfter').textContent = money(after);
  $('tGo').disabled = n < 1 || n > maxShares();
}
async function confirmTrade() {
  const n = Math.floor(Number($('tShares').value));
  $('tGo').disabled = true;
  const r = await game.call(trade.buy ? 'stocks.buy' : 'stocks.sell', trade.s.id, n);
  if (r.ok) {
    toast(`已${trade.buy ? '買進' : '賣出'} ${trade.s.ticker} ${n} 股`);
    $('trade').hidden = true;
  } else {
    toast({ money: '資金不足。', slots: '最多只能同時持有 10 種股票。', shares: '持股不足。', closed: '目前沒有報價，無法交易。',
      character: '目前的角色無法交易。', unsupported: '遊戲版本不支援交易。' }[r.error] || '交易失敗。', true);
    $('tGo').disabled = false;
  }
  load();
}

let toastTimer = 0;
function toast(text, error = false) {
  const t = $('toast');
  t.textContent = text;
  t.className = 'toast' + (error ? ' error' : '');
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), 3500);
}

$('search').oninput = render;
document.querySelectorAll('.filters button').forEach(b => b.onclick = () => {
  filter = filter === b.dataset.f ? '' : b.dataset.f;
  document.querySelectorAll('.filters button').forEach(x => x.classList.toggle('on', x.dataset.f === filter));
  render();
});
document.querySelectorAll('th[data-s]').forEach(th => th.onclick = () => {
  sortDir = sortKey === th.dataset.s ? -sortDir : 1;
  sortKey = th.dataset.s;
  render();
});
$('dBuy').onclick = () => openTrade(true);
$('dSell').onclick = () => openTrade(false);
$('tMinus').onclick = () => { $('tShares').value = Math.max(1, Number($('tShares').value) - 1); updateTrade(); };
$('tPlus').onclick = () => { $('tShares').value = Number($('tShares').value) + 1; updateTrade(); };
$('tShares').oninput = updateTrade;
$('tCancel').onclick = () => ($('trade').hidden = true);
$('trade').onclick = e => { if (e.target === $('trade')) $('trade').hidden = true; };
$('tGo').onclick = confirmTrade;
load();
setInterval(() => { if ($('trade').hidden) load(); }, 5000);
