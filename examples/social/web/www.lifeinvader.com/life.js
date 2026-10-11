// Lifeinvader: the logged-in story character's profile and their friends' news feed.
// Lifeinvader-only content: profiles, friends (some arrive with the story), friends' everyday statuses, and ads.
const PROFILES = {
  michael: { intro: '退休人士。熱愛老電影、高爾夫，以及在泳池邊思考人生。', work: '自營（退休中）', home: '羅克福德山', from: '北揚克頓（大概）', rel: '已婚（很複雜）' },
  franklin: { intro: '想要的不只是街頭。正在往上爬。', work: '汽車業務 → 自由業', home: '史特勞貝利 → 好麥塢山莊', from: '洛聖都南區', rel: '單身（別問塔妮莎）' },
  trevor: { intro: 'Trevor Philips Industries 創辦人兼執行長。我們什麼都做，不要問。', work: 'Trevor Philips Industries', home: '沙岸地區', from: '加拿大（不准笑）', rel: '這不關你的事' },
};
// Friends of each character; `after` = the story mission that brings them in.
const FRIENDS = {
  michael: [{ id: 'amanda' }, { id: 'jimmy' }, { id: 'tracey' }, { id: 'friedlander', after: 'drf1' }, { id: 'franklin', after: 'armenian3' },
    { id: 'trevor', after: 'trevor3' }, { id: 'solomon', after: 'solomon1' }, { id: 'lazlow', after: 'family4' }],
  franklin: [{ id: 'lamar' }, { id: 'tanisha' }, { id: 'denise' }, { id: 'simeon' }, { id: 'michael', after: 'armenian3' },
    { id: 'trevor', after: 'trevor3' }],
  trevor: [{ id: 'ron' }, { id: 'wade' }, { id: 'michael', after: 'trevor3' }, { id: 'franklin', after: 'trevor3' }],
};
// Friends' everyday statuses, a few a day.
const DAILY = {
  amanda: ['在羅克福德山的新瑜珈教室，老師說我的氣場很「混亂」。他說對了。', '今天的購物清單：寧靜、自我、一雙新的鞋。只買到鞋。', '有人知道好的婚姻諮商師嗎？要收費便宜、不會八卦的。'],
  jimmy: ['今晚直播打 Righteous Slaughter，誰來刷禮物我就叫他爸爸', '我的混音帶就快完成了，只差歌詞、旋律、和錄音。', '我爸又把我的網路線拔了。這是虐待。'],
  tracey: ['新自拍 💋 濾鏡用了七層，這才是真實的我', '試鏡結果出來了：他們說我「太有個性」。他們不懂。', '有人想一起去 Vinewood 夜店嗎？我爸說不行所以我一定要去'],
  friedlander: ['今日箴言：你無法改變過去，但可以支付我改變它的感覺。', '我的新書《打開你的心門（以及錢包）》即將上市。'],
  lamar: ['今天的穿搭：100% 正宗街頭貴氣，不接受批評', '我在寫一本書，叫做《拉瑪的成功之道》。第一章：認識對的人。第二章：還沒想到。', '狗是人類最好的朋友。尤其是 Chop，除了他吃我鞋子的時候。'],
  tanisha: ['有些人永遠不會長大，你只能祝他們好運，然後封鎖他們。', '新工作很好，同事很好，前男友不好。'],
  denise: ['今天的自我成長小組教我們：愛自己，不愛那個不付房租的姪子。', '練了一個小時的有氧，現在要吃一整盒炸雞獎勵自己。', '姪子終於搬出去了，我要開派對！'],
  simeon: ['Premium Deluxe Motorsport 本月促銷！零利率！信用不良也 OK！', '我們的展示窗已經修好了。這次是防彈的。'],
  ron: ['今晚的廣播節目主題：路燈在監視你嗎？答案：是的。', '把手機放進錫箔袋裡，他們就聽不到了。他們是誰？你懂的。', '崔佛今天心情很好，所以沒有人受傷。這是好日子。'],
  wade: ['今天學會用微波爐了！雖然把湯匙放進去好像不行', '表哥說我要找份工作。我找到一隻貓。', '洛聖都的海好大，比沙岸的亞拉摩海還大'],
  solomon: ['在片場，每個人都是演員。尤其是會計。', '我拍過的電影比你吃過的晚餐還多。而且我的電影都比較好吃。'],
  lazlow: ['新節目企劃：讓兩個政治人物互相打一架。電視台說太有教育意義了。', '我的褲子事件已經過去了。請不要再私訊我那段影片。'],
  michael: ['坐在泳池邊看日落。有些東西買不到，例如安靜的家庭生活。', '又看了一次經典老片。現在的電影都在爆炸，以前的電影會爆炸還有劇情。'],
  franklin: ['往上爬的路很陡，但我不打算回頭。', '新地方、新車、新的麻煩。至少風景變好了。'],
  trevor: ['TPI 業績蒸蒸日上。有意見的人可以來沙岸找我當面談。', '今天心情很好。這通常代表某人心情會很差。'],
};
const ADS = [
  { t: 'Pißwasser', d: '你一定喝得下去。德國啤酒，美國人的膽量。' },
  { t: 'Cluckin\' Bell', d: '我們的雞從不撒謊。牠們只是被炸得很好吃。' },
  { t: 'Righteous Slaughter 7', d: '這次你可以殺得更正義。全年齡分級（我們沒問）。' },
  { t: 'Epsilon 計畫', d: '懷疑是一種病。點這裡接受治療。Kifflom！' },
  { t: 'Bean Machine', d: '咖啡因是新的氧氣。第二杯半價，心悸免費。' },
  { t: 'Sprunk', d: '口渴了？Sprunk。不渴？還是 Sprunk。' },
  { t: '費藍德醫師', d: '你的問題不是你的錯。但治療它是你的錢。' },
  { t: 'Maze Bank', d: '您的財富，值得一座迷宮。' },
];

const $ = id => document.getElementById(id);
const { DAY, rng, ago, person, avatar, esc, rich } = Social;
let feed = null, me = null, posts = [], friends = [], tab = 'feed', passed = {};
const store = {
  get(k, d) { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* storage off */ } },
};
const liked = new Set(store.get('liked', []));

async function load() {
  try {
    feed = await game.call('social.feed');
  } catch (e) {
    $('main').innerHTML = `<div class="card empty">Lifeinvader 目前無法連線：${esc(e.message || e)}</div>`;
    return;
  }
  me = CHARACTERS[feed.character];
  if (!me) {
    $('main').innerHTML = '<div class="card empty">請先登入。（目前沒有可用的帳號）</div>';
    return;
  }
  passed = Social.missionTimes(feed);
  friends = FRIENDS[me].filter(f => !f.after || passed[f.after] !== undefined).map(f => f.id);
  const story = Social.posts(feed).filter(p => p.site === 'l');
  // Friends' everyday statuses: one or two a day for the last three days.
  // (Each status at most once in those days.)
  const daily = [], today = Math.floor(feed.now / DAY), used = new Set();
  for (let day = today - 2; day <= today; day++) {
    const r = rng(`life${me}${day}`);
    for (const id of friends.concat(me)) {
      if (!DAILY[id] || r() < 0.55) continue;
      const t = day * DAY + 8 * 60 + Math.floor(r() * 14 * 60), text = DAILY[id][Math.floor(r() * DAILY[id].length)];
      if (t > feed.now || used.has(text)) continue;
      used.add(text);
      daily.push({ id: `d${id}${day}`, t, who: id, text, likes: Math.floor(r() * 40), on: id });
    }
  }
  const own = store.get(`status.${me}`, []).filter(p => p.t <= feed.now);
  posts = [...story, ...daily, ...own].sort((a, b) => b.t - a.t);
  render();
}

function comments(p) {
  // A couple of friends react, picked by the post.
  const r = rng('c' + p.id), pool = friends.filter(f => f !== p.who);
  if (!pool.length || r() < 0.4) return '';
  const lines = ['哈哈哈哈', '你還好嗎？', '讚！', '這是真的嗎', '打給我', '笑死', '我就知道', '你又來了', '下次找我', '❤️'];
  const n = 1 + Math.floor(r() * 2);
  let html = '';
  for (let i = 0; i < n && pool.length; i++) {
    const who = pool[Math.floor(r() * pool.length)];
    html += `<div class="comment">${avatar(who, 30)}<div><b>${esc(person(who).name)}</b> ${lines[Math.floor(r() * lines.length)]}</div></div>`;
  }
  return `<div class="comments">${html}</div>`;
}

function postHtml(p) {
  const who = person(p.who), likes = (p.likes || 0) + (liked.has(p.id) ? 1 : 0);
  const onOther = p.on && p.on !== p.who ? ` ▸ <b>${esc(person(p.on).name)}</b>` : '';
  return `<article class="card post">
    <div class="head">${avatar(p.who)}<div><b>${esc(who.name)}</b>${onOther}<small>${ago(p.t, feed.now)} · 🌐</small></div></div>
    <div class="text">${rich(p.text)}</div>
    <div class="meta">👍 ${likes} 個人說讚</div>
    <div class="buttons"><button data-like="${p.id}" class="${liked.has(p.id) ? 'on' : ''}">👍 讚</button><button>💬 留言</button><button>↗ 分享</button></div>
    ${comments(p)}
  </article>`;
}

function render() {
  const p = person(me), prof = PROFILES[me];
  const mourning = passed.lester1 !== undefined;
  $('who').innerHTML = `${avatar(me, 30)} ${esc(p.name)}`;
  $('memorial').hidden = !mourning;
  $('left').innerHTML = `<div class="card mini">${avatar(me, 64)}<b>${esc(p.name)}</b><small>${esc(prof.intro)}</small></div>
    <div class="card menu"><a data-tab="feed" class="${tab === 'feed' ? 'on' : ''}">📰 動態消息</a><a data-tab="profile" class="${tab === 'profile' ? 'on' : ''}">👤 個人檔案</a>
    <a data-tab="friends" class="${tab === 'friends' ? 'on' : ''}">👥 朋友（${friends.length}）</a></div>`;
  const r = rng('ads' + Math.floor(feed.now / DAY));
  $('right').innerHTML = `<div class="card"><h3>贊助</h3>${ADS.slice().sort(() => r() - 0.5).slice(0, 3).map(a => `<div class="ad"><b>${esc(a.t)}</b><small>${esc(a.d)}</small></div>`).join('')}</div>
    <div class="card"><h3>聯絡人</h3>${friends.map(f => `<div class="contact">${avatar(f, 28)} ${esc(person(f).name)}<i></i></div>`).join('')}</div>`;
  let body = '';
  if (tab === 'feed') {
    const list = posts.filter(x => friends.includes(x.who) || x.who === me || x.on === me);
    body = composer() + (list.map(postHtml).join('') || '<div class="card empty">你的朋友最近都很安靜。</div>');
  } else if (tab === 'profile') {
    const list = posts.filter(x => x.on === me || (x.who === me && !x.on));
    body = `<div class="card profile"><div class="cover" style="background:linear-gradient(120deg, ${p.color}, #1c1c1c)"></div>
      <div class="id">${avatar(me, 110)}<div><h1>${esc(p.name)}</h1><small>${friends.length} 位朋友</small></div></div>
      <ul class="about"><li>💼 ${esc(prof.work)}</li><li>🏠 住在 ${esc(prof.home)}</li><li>📍 來自 ${esc(prof.from)}</li><li>❤️ ${esc(prof.rel)}</li></ul></div>`
      + composer() + (list.map(postHtml).join('') || '<div class="card empty">還沒有任何貼文。</div>');
  } else {
    body = `<div class="card"><h3>朋友</h3><div class="friends">${friends.map(f => `<div class="friend">${avatar(f, 72)}<b>${esc(person(f).name)}</b></div>`).join('')}</div></div>`;
  }
  $('main').innerHTML = body;
}

function composer() {
  return `<div class="card compose">${avatar(me, 40)}<input id="status" maxlength="200" placeholder="${esc(person(me).name)}，你在想什麼？"><button id="share">分享</button></div>`;
}

document.addEventListener('click', e => {
  const t = e.target.closest('[data-tab],[data-like],#share');
  if (!t) return;
  if (t.dataset.tab) { tab = t.dataset.tab; render(); window.scrollTo(0, 0); }
  else if (t.dataset.like) {
    const id = t.dataset.like;
    liked.has(id) ? liked.delete(id) : liked.add(id);
    store.set('liked', [...liked]);
    render();
  } else if (t.id === 'share') {
    const text = $('status').value.trim();
    if (!text) return;
    const own = store.get(`status.${me}`, []);
    own.push({ id: 'u' + Date.now(), t: feed.now, who: me, text, likes: 0, on: me });
    store.set(`status.${me}`, own.slice(-200));
    load();
  }
});
document.addEventListener('keydown', e => { if (e.key === 'Enter' && e.target.id === 'status') $('share').click(); });

load();
