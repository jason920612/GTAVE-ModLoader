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
let view = 'list';

// World position -> pixel on the game's map (1024 x 1536), fitted against the game's water (research/phase0.md §29).
const mapPoint = p => ({ x: 460.5 + 0.1355 * p.x, y: 1037 - 0.1355 * p.y });
const mapState = { scale: 0.6, x: 0, y: 0 };

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
  $('grid').hidden = view !== 'list';
  $('map').hidden = view !== 'map';
  if (view === 'list')
    $('grid').replaceChildren(...list.map(card));
  else
    renderPins(list);
}

function renderPins(list) {
  $('pins').replaceChildren(...list.map(p => {
    const pin = document.createElement('div');
    const at = mapPoint(p);
    pin.className = 'pin' + (p.owned ? ' owned' : '');
    pin.style.left = at.x + 'px';
    pin.style.top = at.y + 'px';
    pin.addEventListener('click', e => { e.stopPropagation(); openDetail(p); });
    pin.addEventListener('mouseenter', () => showTip(p, at));
    pin.addEventListener('mouseleave', () => ($('mapTip').hidden = true));
    return pin;
  }));
  applyMap();
}

function showTip(p, at) {
  const tip = $('mapTip');
  tip.innerHTML = '<span></span> · <b></b>';
  tip.children[0].textContent = p.name;
  tip.children[1].textContent = money(p.price);
  tip.style.left = (mapState.x + at.x * mapState.scale) + 'px';
  tip.style.top = (mapState.y + at.y * mapState.scale - 22) + 'px';
  tip.hidden = false;
}

function applyMap() {
  const box = $('map').getBoundingClientRect();
  const w = 1024 * mapState.scale, h = 1536 * mapState.scale;
  // Keep the map covering the view (or centred when smaller).
  mapState.x = w <= box.width ? (box.width - w) / 2 : Math.min(0, Math.max(box.width - w, mapState.x));
  mapState.y = h <= box.height ? (box.height - h) / 2 : Math.min(0, Math.max(box.height - h, mapState.y));
  $('mapInner').style.transform = `translate(${mapState.x}px, ${mapState.y}px) scale(${mapState.scale})`;
}

function zoomMap(factor, cx, cy) {
  const box = $('map').getBoundingClientRect();
  cx ??= box.width / 2;
  cy ??= box.height / 2;
  const s = Math.min(3, Math.max(0.3, mapState.scale * factor));
  mapState.x = cx - (cx - mapState.x) * s / mapState.scale;
  mapState.y = cy - (cy - mapState.y) * s / mapState.scale;
  mapState.scale = s;
  applyMap();
}

function centreMapOn(p, scale) {
  const box = $('map').getBoundingClientRect();
  const at = mapPoint(p);
  mapState.scale = scale;
  mapState.x = box.width / 2 - at.x * scale;
  mapState.y = box.height / 2 - at.y * scale;
  applyMap();
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
  const at = mapPoint(p);
  $('dPin').style.left = at.x + 'px';
  $('dPin').style.top = at.y + 'px';
  $('detail').hidden = false;
  const loc = $('dMap').parentElement.getBoundingClientRect();
  const s = 0.8;
  $('dMap').style.transform = `translate(${loc.width / 2 - at.x * s}px, ${loc.height / 2 - at.y * s + 10}px) scale(${s})`;
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
$('view').addEventListener('click', e => {
  const b = e.target.closest('button');
  if (!b) return;
  view = b.dataset.value;
  for (const x of $('view').children) x.classList.toggle('on', x === b);
  render();
  if (view === 'map' && !mapState.placed) {
    mapState.placed = true;
    centreMapOn({ x: -300, y: -400 }, 0.9); // Los Santos
  }
});
{
  const map = $('map');
  let drag = null;
  map.addEventListener('mousedown', e => { drag = { x: e.clientX, y: e.clientY, mx: mapState.x, my: mapState.y }; map.classList.add('dragging'); });
  window.addEventListener('mousemove', e => {
    if (!drag) return;
    mapState.x = drag.mx + e.clientX - drag.x;
    mapState.y = drag.my + e.clientY - drag.y;
    applyMap();
  });
  window.addEventListener('mouseup', () => { drag = null; map.classList.remove('dragging'); });
  map.addEventListener('wheel', e => {
    e.preventDefault();
    const box = map.getBoundingClientRect();
    zoomMap(e.deltaY < 0 ? 1.2 : 1 / 1.2, e.clientX - box.left, e.clientY - box.top);
  }, { passive: false });
  $('zoomIn').addEventListener('click', e => { e.stopPropagation(); zoomMap(1.3); });
  $('zoomOut').addEventListener('click', e => { e.stopPropagation(); zoomMap(1 / 1.3); });
  $('zoomIn').addEventListener('mousedown', e => e.stopPropagation());
  $('zoomOut').addEventListener('mousedown', e => e.stopPropagation());
  window.addEventListener('resize', applyMap);
}
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
