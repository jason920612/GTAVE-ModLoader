// Dynasty 8: listings from the property mod (property.list), purchases through property.buy.
const characters = ['麥可', '富蘭克林', '崔佛'];
const tierNames = { 6: '高級公寓', 5: '中級公寓', 4: '平價公寓', 3: '10 車位車庫', 2: '6 車位車庫', 1: '2 車位車庫' };
const tierLevel = t => (t === 6 || t === 3) ? 'high' : (t === 5 || t === 2) ? 'mid' : 'low';
const money = n => '$' + Number(n).toLocaleString('en-US');
const photo = p => p ? `https://gametextures/${p}/${p}.png` : '';
const $ = id => document.getElementById(id);

let data = { character: -1, cash: 0, properties: [] };
const filter = { kind: 'all', tier: 'all', sort: 'price-asc', search: '', ownedOnly: false };
let current = null;

async function load() {
  try {
    data = await game.call('property.list');
  } catch (e) {
    toast('無法讀取物業資料：' + e.message, true);
  }
  renderAccount();
  render();
}

function renderAccount() {
  $('who').textContent = characters[data.character] || '';
  $('cash').textContent = money(data.cash);
}

function visible() {
  const q = filter.search.trim().toLowerCase();
  const list = data.properties.filter(p =>
    (filter.kind === 'all' || p.kind === filter.kind) &&
    (filter.tier === 'all' || tierLevel(p.tier) === filter.tier) &&
    (!filter.ownedOnly || p.owned) &&
    (!q || p.name.toLowerCase().includes(q) || p.area.toLowerCase().includes(q)));
  const by = {
    'price-asc': (a, b) => a.price - b.price,
    'price-desc': (a, b) => b.price - a.price,
    'name': (a, b) => a.name.localeCompare(b.name, 'zh-Hant'),
  }[filter.sort];
  return list.sort(by);
}

function render() {
  const list = visible();
  const owned = data.properties.filter(p => p.owned).length;
  $('summary').textContent = `共 ${list.length} 筆物業` + (owned ? `，你擁有 ${owned} 筆` : '');
  $('empty').hidden = list.length > 0;
  const grid = $('grid');
  grid.replaceChildren(...list.map(card));
}

function card(p) {
  const el = document.createElement('div');
  el.className = 'card';
  el.innerHTML = `
    <img loading="lazy" src="${photo(p.photo)}" alt="">
    ${p.owned ? '<div class="ribbon">已擁有</div>' : ''}
    <div class="badge">${tierNames[p.tier] || ''}</div>
    <div class="info">
      <div class="title"></div>
      <div class="meta"><span class="area"></span><span>🚗 ${p.cars}</span></div>
      <div class="price">${money(p.price)}</div>
    </div>`;
  el.querySelector('.title').textContent = p.name;
  el.querySelector('.area').textContent = p.area;
  el.addEventListener('click', () => openDetail(p));
  return el;
}

function openDetail(p) {
  current = p;
  $('dPhoto').src = photo(p.photo);
  $('dName').textContent = p.name;
  $('dArea').textContent = p.area;
  $('dDesc').textContent = p.description;
  $('dPrice').textContent = money(p.price);
  $('dTags').replaceChildren(...[tierNames[p.tier], `${p.cars} 個車位`].map(t => {
    const s = document.createElement('span');
    s.textContent = t;
    return s;
  }));
  const buy = $('dBuy');
  buy.disabled = p.owned || data.cash < p.price;
  buy.textContent = p.owned ? '已擁有' : data.cash < p.price ? '資金不足' : '購買';
  $('detail').hidden = false;
}

function askBuy() {
  if (!current) return;
  $('cText').textContent = `確定以 ${money(current.price)} 購買「${current.name}」嗎？款項會從${characters[data.character] || '角色'}的現金中扣除。`;
  $('confirm').hidden = false;
}

async function buy() {
  $('confirm').hidden = true;
  const p = current;
  const r = await game.call('property.buy', p.id);
  if (r.ok) {
    toast(`恭喜！你已購買「${p.name}」。`);
    $('detail').hidden = true;
  } else {
    const why = { money: '資金不足。', owned: '你已經擁有這筆物業。', character: '目前的角色不能購買物業。' }[r.error] || '交易失敗。';
    toast(why, true);
  }
  await load();
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

$('kind').addEventListener('click', e => {
  const b = e.target.closest('button');
  if (!b) return;
  filter.kind = b.dataset.value;
  for (const x of $('kind').children) x.classList.toggle('on', x === b);
  render();
});
$('tier').addEventListener('change', e => { filter.tier = e.target.value; render(); });
$('sort').addEventListener('change', e => { filter.sort = e.target.value; render(); });
$('search').addEventListener('input', e => { filter.search = e.target.value; render(); });
$('ownedOnly').addEventListener('change', e => { filter.ownedOnly = e.target.checked; render(); });
$('detailClose').addEventListener('click', () => ($('detail').hidden = true));
$('detail').addEventListener('click', e => { if (e.target === $('detail')) $('detail').hidden = true; });
$('dBuy').addEventListener('click', askBuy);
$('cCancel').addEventListener('click', () => ($('confirm').hidden = true));
$('cOk').addEventListener('click', buy);

load();
