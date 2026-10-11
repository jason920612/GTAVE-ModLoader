// Bleeter: the public posts of the shared timeline (social.common), plus what the visitor posts (kept in this browser).
const $ = id => document.getElementById(id);
const { DAY, rng, ago, person, avatar, esc, rich } = Social;
const BIOS = {
  weazel: '洛聖都最快、最吵、偶爾正確的新聞。', lspd: '服務與保護。緊急狀況請撥 911，不要在這裡留言。',
  globe: '聖安地列斯最老牌的報紙，還在努力活下去。', bawsaq: 'BAWSAQ 即時行情。投資一定有風險，我們只負責轉播。',
  lifeinvader: '讓你和你在乎的人，和你不在乎的廣告商，保持聯繫。', jaynorris: 'Lifeinvader 創辦人兼執行長。隱私是給有東西要藏的人。',
  lazlow: '電視主持人、廣播傳奇、褲子目前完好。', merryweather: '私人安全顧問。全球服務，不問問題。',
  epsilon: 'Kifflom！Kraft 在等你。', devin: '投資人、收藏家、成功人士。你可能買不起我的任何東西。',
  solomon: '傳奇製片。好麥塢就是我寫的。', tracey: '未來的巨星 ⭐ 歌手/舞者/演員/網紅，經紀約洽私訊',
  jimmy: '饒舌歌手 / 遊戲實況主 / 我爸不懂我', ron: '真相就在那裡。政府不想讓你看這個帳號。',
  c_truth: '我只是在問問題。', c_gamer: '排位比人生重要。', c_foodie: '吃遍洛聖都，胃已經不是我的了。',
};
let feed = null, all = [], view = 'home', arg = null;

const store = {
  get(k, d) { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* storage off */ } },
};
const liked = new Set(store.get('liked', []));

async function load() {
  try {
    feed = await game.call('social.feed');
  } catch (e) {
    $('feed').innerHTML = `<div class="empty">Bleeter 目前無法連線：${esc(e.message || e)}</div>`;
    return;
  }
  const me = CHARACTERS[feed.character];
  const own = store.get('bleets', []).filter(b => b.t <= feed.now);
  all = [...Social.posts(feed).filter(p => p.site === 'b'), ...own].sort((a, b) => b.t - a.t);
  $('me').innerHTML = me ? `${avatar(me, 40)}<div><b>${esc(person(me).name)}</b><small>@${person(me).handle}</small></div>` : '';
  $('composeAvatar').innerHTML = me ? avatar(me, 44) : '';
  $('compose').hidden = !me;
  side();
  render();
}

function postHtml(p) {
  const who = person(p.who), r = rng(p.id);
  const likes = (p.likes || 0) + (liked.has(p.id) ? 1 : 0);
  const replies = Math.floor((p.likes || 0) * (0.05 + r() * 0.15)), rebleets = Math.floor((p.likes || 0) * (0.1 + r() * 0.4));
  return `<article class="post">
    <a data-user="${p.who}">${avatar(p.who)}</a>
    <div class="body">
      <div class="head"><span class="name" data-user="${p.who}">${esc(who.name)}</span>${who.verified ? '<span class="verified">✓</span>' : ''}
        <span class="handle">@${who.handle}</span><span class="time">· ${ago(p.t, feed.now)}</span></div>
      <div class="text">${rich(p.text)}</div>
      <div class="actions"><button>💬 ${replies}</button><button>🔁 ${rebleets}</button>
        <button data-like="${p.id}" class="${liked.has(p.id) ? 'liked' : ''}">${liked.has(p.id) ? '♥' : '♡'} ${likes}</button><button>↗</button></div>
    </div></article>`;
}

function render() {
  let list = all, title = '首頁';
  $('profile').innerHTML = '';
  $('back').hidden = view === 'home';
  $('compose').hidden = view !== 'home' || !CHARACTERS[feed.character];
  if (view === 'tag') { list = all.filter(p => p.text.includes('#' + arg)); title = '#' + arg; }
  if (view === 'search') { const q = arg.toLowerCase(); list = all.filter(p => p.text.toLowerCase().includes(q) || person(p.who).name.toLowerCase().includes(q)); title = `搜尋「${arg}」`; }
  if (view === 'trends') { title = '熱門話題'; list = []; $('profile').innerHTML = `<div class="box" style="margin:12px 16px">${trendsHtml(12)}</div>`; }
  if (view === 'user' || view === 'me') {
    const id = view === 'me' ? CHARACTERS[feed.character] : arg;
    if (!id) { $('feed').innerHTML = '<div class="empty">目前沒有登入的帳號。</div>'; return; }
    const p = person(id), r = rng('u' + id);
    list = all.filter(x => x.who === id);
    title = p.name;
    $('profile').innerHTML = `<div class="profile"><div class="cover" style="background:linear-gradient(135deg, ${p.color}, #222)"></div>
      <div class="info">${avatar(id, 92)}<h2>${esc(p.name)} ${p.verified ? '<span class="verified">✓</span>' : ''}</h2>
      <div class="handle">@${p.handle}</div><p>${esc(BIOS[id] || '')}</p>
      <div class="stats"><span><b>${Math.floor(r() * 900 + 20)}</b> 追蹤中</span><span><b>${(p.verified ? Math.floor(r() * 900000 + 50000) : Math.floor(r() * 3000 + 40)).toLocaleString()}</b> 追蹤者</span></div></div></div>`;
  }
  $('title').textContent = title;
  document.querySelectorAll('.side a[data-view]').forEach(a => a.classList.toggle('on', a.dataset.view === view));
  if (view === 'trends') { $('feed').innerHTML = ''; return; }
  $('feed').innerHTML = list.map(postHtml).join('') || '<div class="empty">這裡還沒有任何貼文。<br>推進劇情、或在城裡搞點事情，大家就會開始討論。</div>';
}

function trends() {
  const counts = {};
  for (const p of all.filter(p => p.t >= feed.now - 3 * DAY))
    for (const m of p.text.matchAll(/#([^\s#，。！？、]+)/g)) counts[m[1]] = (counts[m[1]] || 0) + (p.likes || 1);
  for (const t of ['洛聖都', '好麥塢', 'CluckinBell', 'BAWSAQ']) counts[t] = counts[t] || 1;
  return Object.entries(counts).sort((a, b) => b[1] - a[1]);
}
function trendsHtml(n) {
  return trends().slice(0, n).map(([t, c], i) => `<a class="trend" data-tag="${esc(t)}"><small>${i + 1} · 聖安地列斯的熱門話題</small><b>#${esc(t)}</b>
    <small>${Math.max(3, Math.floor(c * 3.7)).toLocaleString()} 則 Bleet</small></a>`).join('');
}
function side() {
  $('trends').innerHTML = trendsHtml(5);
  const r = rng('follow' + Math.floor(feed.now / DAY));
  const ids = Object.keys(PEOPLE).filter(id => PEOPLE[id].verified).sort(() => r() - 0.5).slice(0, 4);
  $('follow').innerHTML = ids.map(id => `<a class="who" data-user="${id}">${avatar(id, 40)}<div><b>${esc(person(id).name)}</b><small>@${person(id).handle}</small></div></a>`).join('');
}

function go(v, a = null) { view = v; arg = a; render(); window.scrollTo(0, 0); }

document.addEventListener('click', e => {
  const el = e.target.closest('[data-view],[data-user],[data-tag],[data-like]');
  if (!el) return;
  if (el.dataset.view) go(el.dataset.view);
  else if (el.dataset.user) go('user', el.dataset.user);
  else if (el.dataset.tag) go('tag', el.dataset.tag);
  else if (el.dataset.like) {
    const id = el.dataset.like;
    liked.has(id) ? liked.delete(id) : liked.add(id);
    store.set('liked', [...liked]);
    render();
  }
});
$('back').onclick = () => go('home');
$('search').onkeydown = e => { if (e.key === 'Enter' && e.target.value.trim()) go('search', e.target.value.trim()); };
$('text').oninput = e => { $('count').textContent = 160 - e.target.value.length; $('send').disabled = !e.target.value.trim(); };
$('send').onclick = () => {
  const text = $('text').value.trim(), me = CHARACTERS[feed.character];
  if (!text || !me) return;
  const own = store.get('bleets', []);
  own.push({ id: 'u' + Date.now(), t: feed.now, who: me, text, likes: 0, site: 'b' });
  store.set('bleets', own.slice(-200));
  $('text').value = '';
  $('send').disabled = true;
  $('count').textContent = 160;
  load();
};

load();
