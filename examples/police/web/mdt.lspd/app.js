// In-car computer (police mod, mdt.cpp): nearby vehicles with pictures, plate / name search, checked people, cases
// and reports. Everything comes from the game through game.call('mdt.*').
const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const call = (name, ...args) => game.call(name, ...args);

// ---- tabs
document.querySelectorAll('#tabs button').forEach(b => b.addEventListener('click', () => show(b.dataset.tab)));
function show(tab) {
  document.querySelectorAll('#tabs button').forEach(b => b.classList.toggle('on', b.dataset.tab === tab));
  document.querySelectorAll('.tab').forEach(t => t.classList.toggle('on', t.id === tab));
  ({ cars: loadCars, search: loadSearch, people: loadPeople, cases: loadCases })[tab]();
}
$('close').addEventListener('click', () => game.close());

// ---- shared renderers
const picture = (src, cls = 'photo') => src ? `<img class="${cls}" src="${esc(src)}" alt="">` : `<div class="${cls === 'photo' ? 'noPhoto' : 'thumb'}">沒有照片</div>`;
const status = (v, good) => v === good ? `<span class="ok">${esc(v)}</span>` : `<span class="warn">${esc(v)}</span>`;

function personHtml(p, title = '') {
  if (!p || p.error) return `<p class="bad">${esc(p?.error || '查無資料')}</p>`;
  const priors = p.priors?.length ? p.priors.map(esc).join('、') : '<span class="ok">無</span>';
  return `${title ? `<h3>${title}</h3>` : ''}<h2>${esc(p.name)}</h2>
    <dl>
      <dt>出生日期</dt><dd>${esc(p.dob)}</dd>
      <dt>駕照</dt><dd>${status(p.licence, '有效')}</dd>
      <dt>通緝</dt><dd>${p.warrant ? `<span class="bad">有通緝：${esc(p.warrantFor)}</span>` : '<span class="ok">無</span>'}</dd>
      <dt>前科</dt><dd>${priors}</dd>
    </dl>`;
}

function vehicleHtml(v) {
  if (!v || v.error) return `<p class="bad">${esc(v?.error || '查無資料')}</p>`;
  return `${picture(v.photo)}
    <h2 style="margin-top:12px">${esc(v.plate)} ${v.stolen ? '<span class="tag bad">已報失竊</span>' : ''}</h2>
    <dl>
      <dt>車型</dt><dd>${esc([v.make, v.model].filter(Boolean).join(' '))}</dd>
      <dt>行照</dt><dd>${status(v.registration, '有效')}</dd>
      <dt>保險</dt><dd>${status(v.insurance, '有效')}</dd>
      <dt>失竊</dt><dd>${v.stolen ? '<span class="bad">是</span>' : '<span class="ok">否</span>'}</dd>
    </dl>
    ${personHtml(v.owner, '車主')}`;
}

// ---- nearby vehicles
let selectedCar = 0;
async function loadCars() {
  const list = await call('mdt.nearby');
  $('carList').innerHTML = list.length ? list.map(c => `
    <div class="item ${c.id === selectedCar ? 'on' : ''}" data-id="${c.id}">
      ${c.photo ? `<img class="thumb" src="${esc(c.photo)}" alt="">` : '<div class="thumb">沒有照片</div>'}
      <div class="main"><div class="plate">${esc(c.plate)}${c.ahead ? '<span class="tag info">前車</span>' : ''}${c.suspect ? '<span class="tag bad">嫌犯</span>' : ''}</div>
      <div class="sub">${esc(c.model)} · ${c.distance} 公尺${c.occupied ? '' : ' · 無人'}</div></div>
    </div>`).join('') : '<p class="hint" style="padding:12px">附近沒有車輛。</p>';
  $('carList').querySelectorAll('.item').forEach(el => el.addEventListener('click', () => openCar(+el.dataset.id)));
}
async function openCar(id) {
  selectedCar = id;
  $('carList').querySelectorAll('.item').forEach(el => el.classList.toggle('on', +el.dataset.id === id));
  $('carDetail').innerHTML = '<p class="hint">查詢中…</p>';
  $('carDetail').innerHTML = vehicleHtml(await call('mdt.vehicle', id));
}

// ---- search
async function loadSearch() {
  const s = await call('mdt.state');
  if (s.aheadPlate && !$('plate').value) $('plate').value = s.aheadPlate;
  $('readerHint').textContent = s.aheadPlate ? `車牌辨識最後讀到：${s.aheadPlate}` : '';
}
$('plateForm').addEventListener('submit', async e => {
  e.preventDefault();
  $('searchDetail').innerHTML = vehicleHtml(await call('mdt.plate', $('plate').value.trim()));
});
$('nameForm').addEventListener('submit', async e => {
  e.preventDefault();
  $('searchDetail').innerHTML = personHtml(await call('mdt.person', $('name').value.trim()));
});

// ---- checked people
async function loadPeople() {
  const list = await call('mdt.people');
  $('peopleList').innerHTML = list.length ? list.map((p, i) => `
    <div class="item" data-i="${i}"><div class="main"><div><b>${esc(p.name)}</b>${p.warrant ? '<span class="tag bad">通緝</span>' : ''}</div>
    <div class="sub">${esc(p.dob)} · 駕照${esc(p.licence)}</div></div></div>`).join('') : '<p class="hint" style="padding:12px">還沒有盤查過任何人。</p>';
  $('peopleList').querySelectorAll('.item').forEach(el => el.addEventListener('click', () => {
    $('peopleList').querySelectorAll('.item').forEach(x => x.classList.toggle('on', x === el));
    $('personDetail').innerHTML = personHtml(list[+el.dataset.i]);
  }));
}

// ---- cases and reports
let charges = [];
async function loadCases() {
  const [state, reports, all] = await Promise.all([call('mdt.state'), call('mdt.reports'), call('mdt.charges')]);
  charges = all;
  const current = state.case ? `<div class="item" data-cur="1"><div class="main"><div><b>進行中</b><span class="tag info">處理中</span></div>
    <div class="sub">${esc(state.case.title)}</div></div></div>` : '';
  $('caseList').innerHTML = current + (reports.length ? reports.map(r => `
    <div class="item" data-id="${r.id}"><div class="main"><div><b>#${r.id}</b> ${esc(r.title)}</div>
    <div class="sub">${esc(r.date)} ${esc(r.time)} · ${esc(r.place)} ${r.submitted ? `<span class="tag ok">已提交 ${r.score} 分</span>` : '<span class="tag warn">草稿</span>'}</div></div></div>`).join('')
    : '<p class="hint" style="padding:12px">還沒有報告。</p>');
  $('caseList').querySelectorAll('.item').forEach(el => el.addEventListener('click', () => {
    $('caseList').querySelectorAll('.item').forEach(x => x.classList.toggle('on', x === el));
    if (el.dataset.cur) showCurrent(state.case); else showReport(reports.find(r => r.id === +el.dataset.id));
  }));
}

function showCurrent(c) {
  $('caseDetail').innerHTML = `<h2>${esc(c.title)}</h2><p class="hint">${esc(c.place)} · 案件進行中，結束後會產生報告草稿。</p>
    <h3>經過</h3><ul class="log">${c.log.map(l => `<li>${esc(l)}</li>`).join('')}</ul>
    <h3>證物</h3>${c.evidence.length ? `<ul class="log">${c.evidence.map(l => `<li>${esc(l)}</li>`).join('')}</ul>` : '<p class="hint">尚無</p>'}`;
}

function showReport(r) {
  const list = l => l.length ? `<ul class="log">${l.map(x => `<li>${esc(x)}</li>`).join('')}</ul>` : '<p class="hint">無</p>';
  const options = [...new Set([...charges, ...r.suggested, ...r.charges])];
  $('caseDetail').innerHTML = `<h2>報告 #${r.id}：${esc(r.title)}</h2>
    <dl><dt>日期時間</dt><dd>${esc(r.date)} ${esc(r.time)}</dd><dt>地點</dt><dd>${esc(r.place)}</dd><dt>結果</dt><dd>${esc(r.outcome)}</dd>
    <dt>涉案人</dt><dd>${r.people.map(esc).join('、') || '—'}</dd></dl>
    <h3>證物</h3>${list(r.evidence)}
    ${r.conduct?.length ? `<h3>執勤表現 <span class="hint" style="font-weight:400">（計入評分 ${r.conductPoints >= 0 ? '+' : ''}${r.conductPoints}）</span></h3>${list(r.conduct)}` : ''}
    <h3>罪名 <span class="hint" style="font-weight:400">（藍字＝證據支持的建議）</span></h3>
    <div class="charges">${options.map(c => `<label><input type="checkbox" value="${esc(c)}" ${r.charges.includes(c) ? 'checked' : ''} ${r.submitted ? 'disabled' : ''}>
      ${esc(c)}${r.suggested.includes(c) ? ' <span class="sug">建議</span>' : ''}</label>`).join('')}</div>
    <h3>經過</h3><textarea id="narrative" ${r.submitted ? 'readonly' : ''}>${esc(r.narrative)}</textarea>
    ${r.submitted ? `<p>已提交，評分 <span class="score">${r.score}</span></p>` : '<p><button class="primary" id="submit">提交報告</button></p>'}`;
  if (!r.submitted) $('submit').addEventListener('click', async () => {
    const picked = [...$('caseDetail').querySelectorAll('.charges input:checked')].map(i => i.value);
    const res = await call('mdt.submit', r.id, picked, $('narrative').value);
    if (res.ok) { await loadCases(); const again = (await call('mdt.reports')).find(x => x.id === r.id); showReport(again); }
  });
}

// ---- start
call('mdt.state').then(s => { $('who').textContent = s.officer ? `員警 ${s.officer}` : ''; });
loadCars();
