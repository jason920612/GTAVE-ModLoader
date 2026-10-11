// Shared bank site. window.SITE = { key: 'maze' | 'fleeca' | 'liberty', name, tagline, logo (svg), promo: { title, text },
// products: [{ title, text }], notice, footer, accountPrefix }. Data from the bank mod (bank.account).
const characters = ['麥可', '富蘭克林', '崔佛'];
const banks = { maze: ['Maze Bank', 'https://www.maze-bank.com/'], fleeca: ['Fleeca', 'https://www.fleeca.com/'],
  liberty: ['Bank of Liberty', 'https://www.thebankofliberty.com/'] };
const bankOf = ['maze', 'fleeca', 'liberty'];
const categories = { invest: '投資', people: '人物往來', jobs: '工作收入', shopping: '購物', weapons: '武器', vehicles: '載具',
  property: '房地產', fun: '娛樂', bills: '帳單與費用', other: '其他' };
const money = n => (n < 0 ? '-$' : '$') + Math.abs(Number(n)).toLocaleString('en-US', { maximumFractionDigits: 0 });
const signed = n => (n >= 0 ? '+' : '-') + '$' + Math.abs(Number(n)).toLocaleString('en-US', { maximumFractionDigits: 0 });
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const DAY = 1440;

// Game minutes since 2000-01-01 -> date text.
function civil(days) {
  const z = days + 719468 + 10957, era = Math.floor(z / 146097), doe = z - era * 146097;
  const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365);
  const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100)), mp = Math.floor((5 * doy + 2) / 153);
  const d = doy - Math.floor((153 * mp + 2) / 5) + 1, m = mp < 10 ? mp + 3 : mp - 9;
  return [yoe + era * 400 + (m <= 2), m, d];
}
function when(t, withTime = true) {
  if (t < 0) return '較早';
  const [y, m, d] = civil(Math.floor(t / DAY));
  const hm = t % DAY, h = Math.floor(hm / 60), mi = hm % 60;
  return `${y}/${m}/${d}` + (withTime ? ` ${String(h).padStart(2, '0')}:${String(mi).padStart(2, '0')}` : '');
}

document.title = SITE.name;
document.body.insertAdjacentHTML('afterbegin', `
<header class="top"><div class="bar">
  <div class="logo">${SITE.logo}<div>${SITE.name}<small>${SITE.tagline}</small></div></div>
  <div class="end"><div class="who" id="who"></div><button class="theme-toggle" data-theme-toggle></button></div>
</div></header>
<div id="app"></div>
<footer>${SITE.footer}</footer>`);

let data = null, range = 30, shown = 30;
const filter = { type: 'all', category: 'all', text: '' };

async function load() {
  try {
    data = await game.call('bank.account', SITE.key);
  } catch (e) {
    $('app').innerHTML = `<main><div class="card empty">無法連線到網路銀行：${esc(e.message || e)}</div></main>`;
    return;
  }
  data.customer ? renderAccount() : renderPublic();
}

// ---- visitors ----------------------------------------------------------------------------------

function renderPublic() {
  const c = data.character;
  $('who').innerHTML = '';
  const login = c < 0
    ? '網路銀行目前無法使用。'
    : `您好，<b>${characters[c]}</b>。我們查不到您在本行的帳戶——您的往來銀行是 <a href="${banks[bankOf[c]][1]}">${banks[bankOf[c]][0]}</a>。`;
  $('app').innerHTML = `
<section class="promo"><div>
  <h1>${SITE.promo.title}</h1>
  <p>${SITE.promo.text}</p>
  <div class="login">${login}</div>
</div></section>
<main class="grid">
  <div class="products">${SITE.products.map(p => `<div class="card product"><h3>${p.title}</h3><p>${p.text}</p></div>`).join('')}</div>
  <div class="notice">${SITE.notice}</div>
</main>`;
}

// ---- customers ----------------------------------------------------------------------------------

function renderAccount() {
  const c = data.character;
  $('who').innerHTML = `網路銀行<b>${characters[c]}</b>`;
  $('app').innerHTML = `
<main class="grid">
  <div class="hero-acc">
    <div>
      <div class="label">活期存款 · 可用餘額</div>
      <div class="bal">${money(data.balance)}</div>
      <div class="no">${SITE.accountPrefix}-${String(100237 + c * 48611).padStart(6, '0')}-${characters[c]}</div>
    </div>
    <div class="flows"><div><span>近 30 天收入</span><b id="in30"></b></div><div><span>近 30 天支出</span><b id="out30"></b></div></div>
  </div>
  <div class="grid cols">
    <section class="card">
      <h2>餘額走勢 <div class="ranges" id="ranges">
        <button data-r="7">7 天</button><button data-r="30">30 天</button><button data-r="0">全部</button></div></h2>
      <div id="chart"></div>
    </section>
    <section class="card cats">
      <h2>支出分類 <small id="catRange"></small></h2>
      <div id="cats"></div>
    </section>
  </div>
  <section class="card">
    <h2>交易明細 <small id="count"></small></h2>
    <div class="filters">
      <input id="search" type="search" placeholder="搜尋對象">
      <select id="type"><option value="all">全部</option><option value="in">收入</option><option value="out">支出</option></select>
      <select id="category"><option value="all">所有分類</option>${Object.entries(categories).map(([k, v]) => `<option value="${k}">${v}</option>`).join('')}</select>
    </div>
    <table><thead><tr><th>日期</th><th>對象</th><th class="num">金額</th><th class="num">餘額</th></tr></thead><tbody id="rows"></tbody></table>
    <button class="more" id="more">顯示更多</button>
  </section>
  <div class="notice">${SITE.notice}</div>
</main>`;
  $('ranges').onclick = e => { if (e.target.dataset.r !== undefined) { range = Number(e.target.dataset.r); drawRange(); } };
  $('search').oninput = e => { filter.text = e.target.value.trim(); shown = 30; drawRows(); };
  $('type').onchange = e => { filter.type = e.target.value; shown = 30; drawRows(); };
  $('category').onchange = e => { filter.category = e.target.value; shown = 30; drawRows(); };
  $('more').onclick = () => { shown += 30; drawRows(); };
  const recent = data.entries.filter(e => e.t >= 0 && e.t >= data.now - 30 * DAY);
  $('in30').textContent = money(recent.filter(e => e.amount > 0).reduce((s, e) => s + e.amount, 0));
  $('out30').textContent = money(-recent.filter(e => e.amount < 0).reduce((s, e) => s + e.amount, 0));
  drawRange();
  drawRows();
}

function drawRange() {
  document.querySelectorAll('#ranges button').forEach(b => b.classList.toggle('on', Number(b.dataset.r) === range));
  const from = range ? data.now - range * DAY : -Infinity;
  drawChart(from);
  drawCategories(from);
}

function drawChart(from) {
  // The balance before and after each dated entry (oldest first), ending at the balance now.
  const pts = [];
  for (const e of [...data.entries].reverse())
    if (e.t >= 0 && e.balance >= 0) pts.push([e.t, e.balance - e.amount], [e.t, e.balance]);
  pts.push([data.now, data.balance]);
  const before = pts.filter(p => p[0] < from).pop();
  const use = pts.filter(p => p[0] >= from);
  if (before) use.unshift([from, before[1]]);
  if (use.length < 3 && !before) {
    $('chart').innerHTML = '<div class="empty">還沒有足夠的紀錄。之後的每一筆收支都會記在這裡。</div>';
    return;
  }
  const W = 600, H = 220, P = 8, L = 64;
  // The axis spans the chosen range (or all the history) up to now.
  const t1 = data.now, t0 = Math.min(range ? Math.min(from, use[0][0]) : use[0][0], t1 - 60);
  if (use[0][0] > t0) use.unshift([t0, use[0][1]]); // the earliest known balance, back to the start of the axis
  let lo = Math.min(...use.map(p => p[1])), hi = Math.max(...use.map(p => p[1]));
  if (hi === lo) { hi += 1; lo = Math.max(0, lo - 1); }
  const x = t => L + (t - t0) / (t1 - t0) * (W - L - P), y = v => P + (1 - (v - lo) / (hi - lo)) * (H - 2 * P - 18);
  let d = `M${x(use[0][0])},${y(use[0][1])}`;
  for (let i = 1; i < use.length; i++) d += ` H${x(use[i][0])} V${y(use[i][1])}`;
  const ticks = [lo, (lo + hi) / 2, hi].map(v => `<text x="${L - 6}" y="${y(v) + 4}" text-anchor="end" font-size="11" class="axis">${money(Math.round(v))}</text>
    <line x1="${L}" x2="${W - P}" y1="${y(v)}" y2="${y(v)}" class="grid-line"/>`).join('');
  $('chart').innerHTML = `<svg class="chart" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none">${ticks}
    <path d="${d} V${H - 18} H${x(use[0][0])} Z" class="area"/>
    <path d="${d}" class="line"/>
    <text x="${L}" y="${H - 3}" font-size="11" class="axis">${when(t0, false)}</text>
    <text x="${W - P}" y="${H - 3}" font-size="11" class="axis" text-anchor="end">${when(t1, false)}</text></svg>`;
}

function drawCategories(from) {
  $('catRange').textContent = range ? `近 ${range} 天` : '全部紀錄';
  const sums = {};
  for (const e of data.entries)
    if (e.amount < 0 && (range ? e.t >= from : true)) sums[e.category] = (sums[e.category] || 0) - e.amount;
  const list = Object.entries(sums).sort((a, b) => b[1] - a[1]);
  if (!list.length) { $('cats').innerHTML = '<div class="empty">這段期間沒有支出。</div>'; return; }
  const max = list[0][1];
  $('cats').innerHTML = list.map(([k, v]) => `<div class="row"><span>${categories[k] || k}</span>
    <div class="track"><div class="fill" style="width:${Math.max(2, v / max * 100)}%"></div></div><span class="amt">${money(v)}</span></div>`).join('');
}

function drawRows() {
  const text = filter.text.toLowerCase();
  const list = data.entries.filter(e => (filter.type === 'all' || (filter.type === 'in') === (e.amount > 0)) &&
    (filter.category === 'all' || e.category === filter.category) && (!text || e.name.toLowerCase().includes(text)));
  $('count').textContent = `${list.length} 筆`;
  $('rows').innerHTML = list.slice(0, shown).map(e => `<tr>
    <td class="date">${when(e.t)}</td>
    <td>${esc(e.name)}<span class="tag">${categories[e.category] || ''}</span></td>
    <td class="num ${e.amount >= 0 ? 'in' : 'out'}">${signed(e.amount)}</td>
    <td class="num">${e.balance >= 0 ? money(e.balance) : '—'}</td></tr>`).join('') ||
    '<tr><td colspan="4" class="empty">沒有符合的交易。</td></tr>';
  $('more').hidden = list.length <= shown;
}

load();
