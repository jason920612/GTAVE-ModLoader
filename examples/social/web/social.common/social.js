// Shared timeline engine of Bleeter and Lifeinvader: turns social.feed() (story missions passed, events) and the content
// in data.js into dated posts. Everything is deterministic: the same game state gives the same timeline.
window.Social = (() => {
  const DAY = 1440;

  function hash(s) {
    let h = 2166136261;
    for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
    return h >>> 0;
  }
  function rng(seed) {
    let a = typeof seed === 'number' ? seed : hash(String(seed));
    return () => {
      a = (a + 0x6D2B79F5) | 0;
      let t = Math.imul(a ^ (a >>> 15), 1 | a);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  // Game minutes since 2000-01-01 -> [y, m, d].
  function civil(days) {
    const z = days + 719468 + 10957, era = Math.floor(z / 146097), doe = z - era * 146097;
    const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365);
    const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100)), mp = Math.floor((5 * doy + 2) / 153);
    const d = doy - Math.floor((153 * mp + 2) / 5) + 1, m = mp < 10 ? mp + 3 : mp - 9;
    return [yoe + era * 400 + (m <= 2), m, d];
  }
  function ago(t, now) {
    const d = now - t;
    if (d < 1) return '剛剛';
    if (d < 60) return `${d} 分鐘前`;
    if (d < DAY) return `${Math.floor(d / 60)} 小時前`;
    const [y, m, dd] = civil(Math.floor(t / DAY)), [ny] = civil(Math.floor(now / DAY));
    const hm = `${String(Math.floor(t % DAY / 60)).padStart(2, '0')}:${String(t % 60).padStart(2, '0')}`;
    return (y === ny ? `${m}月${dd}日` : `${y}年${m}月${dd}日`) + ' ' + hm;
  }
  const money = n => '$' + Math.abs(Number(n)).toLocaleString('en-US');

  // When each passed mission happened: recorded times, and for missions passed before the mod watched, a day apart in
  // the game's own order, ending a day before the first recorded one (or today).
  function missionTimes(feed) {
    const out = {};
    const known = feed.missions.filter(m => m.passed && m.t >= 0);
    const unknown = feed.missions.filter(m => m.passed && m.t < 0).sort((a, b) => a.order - b.order);
    const first = known.length ? Math.min(...known.map(m => m.t)) : feed.now;
    unknown.forEach((m, i) => { out[m.script.toLowerCase()] = first - (unknown.length - i) * DAY + 9 * 60 + (hash(m.script) % 600); });
    for (const m of known) out[m.script.toLowerCase()] = m.t;
    return out;
  }

  function fill(text, vars) {
    return text.replace(/\{(\w+)\}/g, (_, k) => vars[k] ?? '');
  }

  // All posts up to now: { id, t, who, text, likes, site: 'b' | 'l', on, self }
  function posts(feed) {
    const now = feed.now, out = [];
    const passed = missionTimes(feed);
    // Story
    STORY.forEach((p, i) => {
      const t0 = passed[p.after.toLowerCase()];
      if (t0 === undefined) return;
      const post = p.b || p.l, t = t0 + p.in;
      if (t > now) return;
      out.push({ id: 's' + i, t, who: post.who, text: post.text, likes: post.likes, site: p.b ? 'b' : 'l', on: p.l ? (post.on || post.who) : null });
    });
    // Events
    const me = CHARACTERS;
    feed.events.forEach((e, i) => {
      const r = rng(`${e.t}:${e.type}:${i}`);
      const vars = { zone: e.a || '市區', vehicle: e.b || '車', stars: e.n, shop: e.a, amount: money(e.n), name: e.a,
        ticker: e.a, company: e.b, pct: (Math.abs(e.n) / 100).toFixed(1) };
      let list = [];
      if (e.type === 'chase') list = EVENTS.chase.filter(x => e.b || !x.text.includes('{vehicle}')).sort(() => r() - 0.5).slice(0, 3);
      else if (e.type === 'buy') list = e.b === 'property' ? EVENTS.buyProperty : EVENTS.buyVehicle;
      else if (e.type === 'hospital') list = EVENTS.hospital;
      else if (e.type === 'arrest') list = EVENTS.arrest;
      else if (e.type === 'stock') list = e.n >= 0 ? EVENTS.stockUp : EVENTS.stockDown;
      // One of the "self" templates (a status of the character), plus the public ones.
      const selfs = list.filter(x => x.self), others = list.filter(x => !x.self);
      const picked = [...(selfs.length ? [selfs[Math.floor(r() * selfs.length)]] : []), ...others];
      picked.forEach((x, k) => {
        const t = e.t + (x.at || 0);
        if (t > now) return;
        const self = !!x.self;
        if (self && e.c < 0) return;
        out.push({ id: `e${i}.${k}`, t, who: self ? me[e.c] : x.who, text: fill(x.text, vars), likes: Math.floor(r() * 300),
          site: self ? 'l' : 'b', on: self ? me[e.c] : null, self });
      });
    });
    // Everyday Bleeter posts: five a game day for the last three days, picked by the day.
    const today = Math.floor(now / DAY);
    for (let day = today - 2; day <= today; day++) {
      const r = rng('day' + day);
      const pool = AMBIENT.filter(a => !a.w || (day === today && a.w.includes(feed.weather)));
      const picks = new Set();
      while (picks.size < Math.min(5, pool.length)) picks.add(Math.floor(r() * pool.length));
      [...picks].forEach(k => {
        const a = pool[k], [h0, h1] = a.h || [7, 23];
        const t = day * DAY + h0 * 60 + Math.floor(r() * (h1 - h0) * 60);
        if (t <= now) out.push({ id: `a${day}.${k}`, t, who: a.who, text: a.text, likes: Math.floor(r() * 120), site: 'b' });
      });
    }
    return out.sort((a, b) => b.t - a.t);
  }

  function person(id) { return PEOPLE[id] || { name: id, handle: id, color: '#888', mark: '?' }; }
  function avatar(id, size = 44) {
    const p = person(id);
    return `<span class="avatar" style="background:${p.color};width:${size}px;height:${size}px;font-size:${Math.round(size * 0.42)}px">${p.mark}</span>`;
  }
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  // Hashtags and @handles as links.
  function rich(text) {
    return esc(text).replace(/#([^\s#，。！？、]+)/g, '<a class="tag" data-tag="$1">#$1</a>').replace(/@(\w+)/g, '<a class="mention">@$1</a>');
  }

  return { DAY, hash, rng, ago, money, posts, person, avatar, esc, rich, missionTimes };
})();
