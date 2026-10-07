// Shared vehicle store page: window.SITE = { key, title, tagline }. Data from the property mod (shop.list / shop.buy).
const characters = ['麥可', '富蘭克林', '崔佛'];
const kinds = ['', '飛機', '船', '直升機'];
const storageName = { 1: '機庫', 2: '碼頭停泊位', 3: '停機坪' };
const storageKey = { 1: 'hangar', 2: 'marina', 3: 'helipad' };
const money = n => '$' + Number(n).toLocaleString('en-US');
const $ = id => document.getElementById(id);

document.title = SITE.title;
document.body.insertAdjacentHTML('afterbegin', `
<header class="top">
  <div class="logo">${SITE.title}<small>${SITE.tagline}</small></div>
  <div class="account"><span id="who"></span> <b id="cash"></b></div>
</header>
<section class="hero">
  <h1>${SITE.headline}</h1>
  <p>${SITE.intro}</p>
  <div class="filters">
    <select id="sort">
      <option value="price-asc">價格：低到高</option>
      <option value="price-desc">價格：高到低</option>
      <option value="name">名稱</option>
    </select>
    <input id="search" type="search" placeholder="搜尋車款或廠牌">
  </div>
</section>
<main>
  <div class="summary" id="summary"></div>
  <div class="grid" id="grid"></div>
  <p class="loading" id="loading" hidden>正在整理型錄，第一次開啟需要一點時間…</p>
  <p class="empty" id="empty" hidden>沒有符合條件的商品。</p>
</main>
<div class="overlay" id="detail" hidden>
  <div class="panel">
    <button class="close" id="detailClose">×</button>
    <div id="dPhoto"></div>
    <div class="body">
      <div class="maker" id="dMaker"></div>
      <h2 id="dName"></h2>
      <div class="deliver" id="dDeliver"></div>
      <div class="buy"><div class="price" id="dPrice"></div><button class="primary" id="dBuy">購買</button></div>
    </div>
  </div>
</div>
<div class="toast" id="toast" hidden></div>`);

let data = { items: [], garages: [], storage: {} };
let current = null;

function picture(p, cls) {
  return p ? `<img class="${cls}" loading="lazy" src="https://gametextures/${p}.png" alt="">` : `<div class="noimg">${SITE.title}</div>`;
}

async function load() {
  data = await game.call('shop.list', SITE.key);
  $('who').textContent = characters[data.character] || '';
  $('cash').textContent = money(data.cash);
  $('loading').hidden = data.ready;
  if (!data.ready) { setTimeout(load, 2000); }
  render();
}

function render() {
  const q = $('search').value.trim().toLowerCase();
  const by = { 'price-asc': (a, b) => a.price - b.price, 'price-desc': (a, b) => b.price - a.price,
    'name': (a, b) => a.name.localeCompare(b.name, 'zh-Hant') }[$('sort').value];
  const list = data.items.filter(i => !q || i.name.toLowerCase().includes(q) || i.maker.toLowerCase().includes(q)).sort(by);
  $('summary').textContent = data.ready ? `共 ${list.length} 款` : '';
  $('empty').hidden = !data.ready || list.length > 0;
  $('grid').replaceChildren(...list.map(i => {
    const el = document.createElement('div');
    el.className = 'card';
    el.innerHTML = picture(i.photo, 'photo') + '<div class="info"><div class="maker"></div><div class="name"></div><div class="price"></div></div>';
    el.querySelector('.maker').textContent = i.maker;
    el.querySelector('.name').textContent = i.name;
    el.querySelector('.price').textContent = money(i.price);
    el.onclick = () => open(i);
    return el;
  }));
}

function open(i) {
  current = i;
  $('dPhoto').innerHTML = picture(i.photo, 'photo');
  $('dMaker').textContent = [i.maker, kinds[i.kind]].filter(Boolean).join(' · ');
  $('dName').textContent = i.name;
  $('dPrice').textContent = money(i.price);
  const d = $('dDeliver');
  let can = data.cash >= i.price;
  if (i.kind === 0) {
    const free = data.garages.filter(g => g.free > 0);
    if (free.length) {
      d.innerHTML = '<label>交車到</label><select id="dGarage"></select><p class="note">新車會停在所選物業車庫的空車位，原廠出廠狀態。</p>';
      $('dGarage').replaceChildren(...free.map(g => new Option(`${g.name}（剩 ${g.free} 個車位）`, g.id)));
    } else {
      can = false;
      d.innerHTML = `<p class="note warn">${data.garages.length ? '你的車庫都停滿了。' : '你沒有可以停車的車庫。'}請先到朝代 8 房地產（www.dynasty8realestate.com）購買公寓或車庫。</p>`;
    }
  } else {
    const has = data.storage[storageKey[i.kind]];
    if (!has) can = false;
    d.innerHTML = has
      ? `<p class="note">會送到${characters[data.character] || '角色'}的${storageName[i.kind]}，原有的同類載具會被取代（與原版網站相同）。</p>`
      : `<p class="note warn">${characters[data.character] || '角色'}沒有${storageName[i.kind]}，無法購買。</p>`;
  }
  $('dBuy').disabled = !can;
  $('dBuy').textContent = data.cash < i.price ? '資金不足' : '購買';
  $('detail').hidden = false;
}

async function buy() {
  const i = current;
  const garage = i.kind === 0 ? Number($('dGarage').value) : 0;
  $('dBuy').disabled = true;
  const r = await game.call('shop.buy', i.item, garage);
  if (r.ok) {
    toast(`已購買「${i.name}」！`);
    $('detail').hidden = true;
  } else {
    toast({ money: '資金不足。', garage: '這個車庫沒有空位了。', storage: '沒有可以存放的地方。', unsupported: '遊戲版本不支援這項購買。' }[r.error] || '交易失敗。', true);
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

$('sort').onchange = render;
$('search').oninput = render;
$('detailClose').onclick = () => ($('detail').hidden = true);
$('detail').onclick = e => { if (e.target === $('detail')) $('detail').hidden = true; };
$('dBuy').onclick = buy;
load();
