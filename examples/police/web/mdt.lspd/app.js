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
  ({ cars: loadCars, search: loadSearch, people: loadPeople, cases: loadCases, duty: loadDuty, career: loadCareer, colleagues: loadMates })[tab]();
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

// ---- duty: the shift, the roster, payslips and attendance (shift.cpp)
const hours = h => h >= 1 ? `${h.toFixed(1)} 小時` : h > 0 ? `${Math.round(h * 60)} 分鐘` : '—';
async function loadDuty() {
  const d = await call('mdt.shift');
  if (!d.ready) { $('dutyNow').innerHTML = '<p class="hint">還沒有資料。</p>'; return; }
  const n = d.next;
  $('dutyNow').innerHTML = `<h2>${n.underway ? '本班勤務' : '下一班'}</h2>
    <dl><dt>日期</dt><dd>${esc(n.date)} ${esc(n.kind)} ${esc(n.hours)}</dd>
    <dt>勤務區</dt><dd>${esc(n.beat)}</dd><dt>呼號</dt><dd><b>${esc(n.callsign)}</b></dd>
    <dt>協尋車輛</dt><dd>${esc(n.boloCar)}</dd><dt>通緝</dt><dd>${esc(n.boloName)}（${esc(n.boloCrime)}）</dd>
    <dt>今日重點</dt><dd>${esc(n.focus)}</dd></dl>
    ${d.session ? `<p>已上班：${esc(d.session.since)} 起，${d.session.hours} 小時${d.session.overtime ? `（加班 ${d.session.overtime} 小時）` : ''}</p>` : '<p class="hint">目前沒有上班。</p>'}
    <p class="hint">遊戲時間 ${esc(d.now)}</p>`;
  $('dutyMe').innerHTML = `<h2>${esc(d.name || '員警資料')}</h2>
    <dl><dt>職級</dt><dd>${esc(d.rank)}（時薪 $${d.rate}）</dd><dt>到職</dt><dd>${esc(d.hired)}</dd>
    <dt>年資</dt><dd>${d.shifts} 班${d.probation ? `（試用期還有 ${d.probation} 班）` : ''}</dd>
    <dt>班別</dt><dd>${esc(d.kind)}${d.request ? `，已申請改 ${esc(d.request)}` : ''}</dd>
    <dt>出勤紀錄</dt><dd>遲到 ${d.late}、早退 ${d.early}、缺勤 ${d.absent}、報告逾期 ${d.overdue}</dd>
    ${d.fired ? '<dt>狀態</dt><dd class="bad">已免職</dd>' : d.suspended ? `<dt>狀態</dt><dd class="bad">停職中（還有 ${d.suspended} 班）</dd>` : ''}</dl>`;
  $('dutyWeek').innerHTML = `<h2>本週班表</h2><table class="table"><tbody>${d.week.map(w =>
    `<tr class="${w.work ? '' : 'off'}"><td>${esc(w.date)}</td><td>${esc(w.kind)}</td><td>${esc(w.hours)}</td></tr>`).join('')}</tbody></table>`;
  const pays = [...d.payslips].reverse();
  $('dutyPay').innerHTML = `<h2>薪資單</h2>${pays.length ? `<table class="table"><thead><tr><th>日期</th><th>時間</th><th>職級</th><th>正常</th><th>加班</th><th>夜間</th><th>實領</th></tr></thead><tbody>${pays.map(p =>
    `<tr><td>${esc(p.date)}</td><td>${esc(p.from)}–${esc(p.to)}</td><td>${esc(p.rank)}</td><td>${hours(p.regular)}${p.alone ? `<span class="hint">（自己上班 ${p.alone}）</span>` : ''}</td>
     <td>${hours(p.overtime)}</td><td>${hours(p.night)}</td><td><b>$${p.pay.toLocaleString()}</b></td></tr>`).join('')}</tbody></table>` : '<p class="hint">還沒有薪資單。</p>'}`;
  const days = [...d.attendance].reverse();
  $('dutyAttend').innerHTML = `<h2>出勤</h2>${days.length ? `<table class="table"><tbody>${days.map(a =>
    `<tr><td>${esc(a.date)}</td><td>${esc(a.kind)}</td><td>${/遲到|缺勤/.test(a.status) ? `<span class="warn">${esc(a.status)}</span>` : esc(a.status)}</td>
     <td>${a.notes.map(x => `<span class="tag warn">${esc(x)}</span>`).join(' ')}</td></tr>`).join('')}</tbody></table>` : '<p class="hint">還沒有紀錄。</p>'}`;
}

// ---- career: the promotion's requirements and the exam (career.cpp)
const bar = ([have, need]) => `<div class="bar"><span style="width:${Math.min(100, need ? have / need * 100 : 100)}%"></span></div><small>${have} / ${need}</small>`;
async function loadCareer() {
  $('examView').hidden = true;
  $('careerView').hidden = false;
  const c = await call('mdt.career');
  const n = c.next;
  $('careerNow').innerHTML = `<h2>${esc(c.title)}</h2><p>時薪 $${c.rate}</p>
    ${n ? `<h3>升任 ${esc(n.title)}（時薪 $${n.rate}）</h3>
      <dl><dt>年資</dt><dd>${bar(n.shifts)}</dd><dt>考評</dt><dd>${bar(n.eval)}</dd><dt>報告</dt><dd>${bar(n.reports)}</dd></dl>
      ${n.why ? `<p class="hint">${esc(n.why)}</p>` : n.atStation ? `<p class="ok">可以參加升等考試：${c.questions} 題，答對 ${c.pass} 題及格。</p>
        <p><button class="primary" id="examStart">開始考試</button></p>` : '<p class="warn">條件都到了：回到自己的警局，用警局電腦考試（置物櫃按 G）。</p>'}`
    : '<p class="ok">已經是最高職級。</p>'}`;
  if ($('examStart')) $('examStart').addEventListener('click', startExam);
  const e = c.eval;
  $('careerEval').innerHTML = `<h2>考評 <span class="score">${e.score}</span></h2>
    <dl><dt>報告平均</dt><dd>${e.reportsAvg} 分（${e.reports} 份）</dd>
    ${e.trainer >= 0 ? `<dt>訓練官評分</dt><dd>${e.trainer} 分（試用期，與報告各佔一半）</dd>` : ''}
    <dt>扣分</dt><dd>${e.penalty ? `−${e.penalty}` : '0'}<span class="hint">（遲到 ${e.late}、早退 ${e.early}、缺勤 ${e.absent}、報告逾期 ${e.overdue}）</span></dd>
    ${e.bonus ? `<dt>加分</dt><dd>+${e.bonus}<span class="hint">（帶新人、督導，每個職級最多 +15）</span></dd>` : ''}
    ${e.discipline ? `<dt>懲處</dt><dd class="bad">−${e.discipline}<span class="hint">（警告 −5、停職 −10）</span></dd>` : ''}</dl>
    <p class="hint">升遷後重新計算。遲到 −3、早退 −4、缺勤 −10、報告逾期 −5；沒有報告時以 60 分計。</p>`;
  $('careerUnlocks').innerHTML = `<h2>職級與解鎖</h2><table class="table"><tbody>${c.unlocks.map(u =>
    `<tr class="${u.have ? '' : 'off'}"><td>${u.have ? '✔' : ''}</td><td><b>${esc(u.title)}</b><br><small>$${u.rate}/小時</small></td><td>${esc(u.items)}</td></tr>`).join('')}</tbody></table>`;
  const cases = c.discipline || [];
  $('careerDiscipline').innerHTML = `<h2>內部調查</h2>${cases.length ? `<table class="table"><thead><tr><th>案號</th><th>案由</th><th>說明</th><th>結果</th></tr></thead><tbody>${cases.map(k =>
    `<tr><td>IA-${k.id}</td><td>${esc(k.title)}</td><td class="hint">${esc(k.detail)}</td><td>${k.closed ? `<span class="${k.sustained ? 'bad' : 'ok'}">${esc(k.result)}</span>` : '<span class="tag warn">調查中</span>'}</td></tr>`).join('')}</tbody></table>`
    : '<p class="hint">沒有調查案件。開槍會自動列入使用武力審查；傷及民眾、對已上銬者施暴、棄置嫌犯、遭通緝、缺勤與民眾投訴都會立案。</p>'}`;
  const rows = [...c.history.map(h => ({ date: h.date, text: /降為|免職|重新/.test(h.to) ? h.to : `升任 ${h.to}`, ok: !/降為|免職/.test(h.to) })),
    ...c.exams.map(x => ({ date: x.date, text: `${x.title}考試：${x.correct}/${x.total} ${x.pass ? '及格' : '不及格'}`, ok: x.pass }))];
  $('careerHistory').innerHTML = `<h2>紀錄</h2>${rows.length ? `<table class="table"><tbody>${rows.map(r =>
    `<tr><td>${esc(r.date)}</td><td class="${r.ok ? 'ok' : 'warn'}">${esc(r.text)}</td></tr>`).join('')}</tbody></table>` : '<p class="hint">還沒有紀錄。</p>'}`;
}

async function startExam() {
  const x = await call('mdt.examStart');
  if (!x.ok) { $('careerNow').insertAdjacentHTML('beforeend', `<p class="bad">${esc(x.error)}</p>`); return; }
  $('careerView').hidden = true;
  const v = $('examView');
  v.hidden = false;
  v.innerHTML = `<h2>${esc(x.title)}升等考試</h2><p class="hint">共 ${x.questions.length} 題，答對 ${x.pass} 題及格。</p>
    <ol class="questions">${x.questions.map((q, i) => `<li><p>${esc(q.text)}</p>${q.options.map((o, k) =>
      `<label><input type="radio" name="q${i}" value="${k}"> ${esc(o)}</label>`).join('')}</li>`).join('')}</ol>
    <p><button class="primary" id="examSubmit">交卷</button> <span class="hint" id="examLeft"></span></p>`;
  const left = () => x.questions.filter((_, i) => !v.querySelector(`input[name=q${i}]:checked`)).length;
  v.addEventListener('change', () => { const n = left(); $('examLeft').textContent = n ? `還有 ${n} 題沒答` : ''; });
  let warned = false;
  $('examSubmit').addEventListener('click', async () => {
    if (left() && !warned) { warned = true; $('examLeft').textContent = `還有 ${left()} 題沒答：再按一次交卷`; return; }
    const answers = x.questions.map((_, i) => { const c = v.querySelector(`input[name=q${i}]:checked`); return c ? +c.value : -1; });
    const r = await call('mdt.examSubmit', answers);
    if (!r.ok) { $('examLeft').textContent = r.error; return; }
    v.querySelectorAll('.questions > li').forEach((li, i) => {
      li.classList.add(r.results[i].right ? 'right' : 'wrong');
      if (!r.results[i].right) li.insertAdjacentHTML('beforeend', `<p class="ok">正確答案：${esc(r.results[i].answer)}</p>`);
    });
    v.querySelectorAll('input').forEach(i => i.disabled = true);
    $('examSubmit').remove();
    v.insertAdjacentHTML('afterbegin', `<div class="result ${r.pass ? 'pass' : 'fail'}"><b>${r.correct} / ${r.total}</b> ${r.pass ? `及格！升任 ${esc(r.title)}` : '不及格，再上 3 班可以重考'}
      <button id="examBack">回升遷頁</button></div>`);
    $('examBack').addEventListener('click', loadCareer);
    v.scrollIntoView();
  });
}

// ---- colleagues: who they are, rapport, typing with them (colleagues.cpp)
let mate = 0, mateTimer = 0;
async function loadMates(open) {
  const list = await call('mdt.colleagues');
  if (open) mate = open;
  $('mateList').innerHTML = list.map(c => `
    <div class="item ${c.id === mate ? 'on' : ''}" data-id="${c.id}"><div class="main">
      <div><b>${esc(c.name)}</b> <span class="hint">${esc(c.title)}</span>${c.onDuty ? `<span class="tag ${c.busy ? 'warn' : 'ok'}">${c.busy ? '出勤中' : '值勤'}</span>` : ''}</div>
      <div class="sub">${esc(c.callsign)} · ${esc(c.shift)} · ${esc(c.trait)} · 年資 ${c.years} 年</div>
      <div class="rapport"><span style="width:${(c.rapport + 100) / 2}%"></span></div><div class="sub">關係：${esc(c.rapportName)}（${c.rapport > 0 ? '+' : ''}${c.rapport}）</div>
    </div></div>`).join('');
  $('mateList').querySelectorAll('.item').forEach(el => el.addEventListener('click', () => openMate(+el.dataset.id, list)));
  if (mate) openMate(mate, list);
}
async function openMate(id, list) {
  mate = id;
  $('mateList').querySelectorAll('.item').forEach(el => el.classList.toggle('on', +el.dataset.id === id));
  const c = list.find(x => x.id === id);
  if (!c) return;
  $('mateChat').innerHTML = `<h2>${esc(c.title)} ${esc(c.name)}</h2><div class="bubbles" id="bubbles"></div>
    <form id="sayForm" class="row"><input id="sayText" autocomplete="off" maxlength="200" placeholder="想跟${esc(c.name)}說什麼？"><button>送出</button></form>
    <p class="hint" id="aiHint"></p>`;
  $('sayForm').addEventListener('submit', async e => {
    e.preventDefault();
    const text = $('sayText').value.trim();
    if (!text) return;
    const r = await call('mdt.say', id, text);
    if (r.ok) $('sayText').value = '';
    refreshChat();
  });
  refreshChat();
}
async function refreshChat() {
  clearTimeout(mateTimer);
  if (!mate || !$('bubbles')) return;
  const c = await call('mdt.chat', mate);
  $('bubbles').innerHTML = c.lines.map(l => `<div class="bubble ${l.mine ? 'mine' : ''}">${esc(l.text)}</div>`).join('') +
    (c.pending ? '<div class="bubble typing">⋯</div>' : '');
  $('bubbles').scrollTop = $('bubbles').scrollHeight;
  $('aiHint').textContent = c.ai ? '' : '本機文字模型沒有開（載入器設定）：同事會用預設的話回答。';
  if (c.pending) mateTimer = setTimeout(refreshChat, 1500);
}

// ---- start
call('mdt.state').then(s => { $('who').textContent = s.officer ? `員警 ${s.officer}` : ''; });
{
  const [tab, arg] = location.hash.slice(1).split('/');
  if (tab === 'colleagues' && arg) { document.querySelectorAll('#tabs button').forEach(b => b.classList.toggle('on', b.dataset.tab === tab));
    document.querySelectorAll('.tab').forEach(t => t.classList.toggle('on', t.id === tab)); loadMates(+arg); }
  else show(['cars', 'search', 'people', 'cases', 'duty', 'career', 'colleagues'].includes(tab) ? tab : 'cars');
}
