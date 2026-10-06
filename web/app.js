// コンデンサ容量計のブラウザ版 (Web Serial)。Mac アプリ (mac/) と同じ画面と判断ロジック。
// Pico のファームウェアが出す '#' で始まる行 (main.cpp の先頭のコメント) を読んで表示する。
// URL の末尾に #demo を付けると、Pico の出力をまねたデータで画面だけ動かせる。
'use strict';

const PICO_VENDOR_ID = 0x2e8a;  // Raspberry Pi
const MAX_READINGS = 60;
const MAX_LOG = 300;

// ---- 校正 (ファームウェアの 'z' / 'c' / 'x' / 'K') ----

const STEPS = {
  z: {
    number: '①', title: 'ゼロ点校正', activity: 'cal_zero', bit: 1,
    purpose: 'ブレッドボードや配線の浮遊容量を測って、測定値から差し引きます。',
    requirement: '何も挿さない', duration: '1 秒ほど',
    instruction: 'コンデンサを抜いて、GP26 の行と GND の行に何も挿さない状態にしてください。',
  },
  c: {
    number: '②', title: 'しきい値校正', activity: 'cal_threshold', bit: 2,
    purpose: 'GP26 のデジタル入力が HIGH に変わる電圧を測ります。低容量レンジの精度に直結します。',
    requirement: '1nF〜1µF のコンデンサ (103 / 104 など)', duration: '1nF なら一瞬、1µF なら数秒',
    instruction: '1nF〜1µF のコンデンサ (103 や 104 など) を挿してください。',
  },
  x: {
    number: '③', title: 'レンジ間校正', activity: 'cal_cross', bit: 4,
    purpose: '同じコンデンサを両方のレンジで測り、高容量レンジを低容量レンジに合わせます。',
    requirement: '30〜250nF のフィルムコンデンサ (104 など)', duration: '約 8 秒',
    instruction: '30〜250nF のフィルムコンデンサ (104 など) を挿してください。誘電吸収が少ないフィルムが向いています。',
  },
  K: {
    number: '★', title: '基準コンデンサ校正', activity: 'cal_reference', bit: 8,
    purpose: '値の分かっているコンデンサを測り、測定値がその値になるように抵抗値を逆算します。',
    duration: '約 10 秒',
  },
};
const REQUIRED_STEPS = ['z', 'c', 'x'];
const ACTIVITY_TO_STEP = { cal_zero: 'z', cal_threshold: 'c', cal_cross: 'x', cal_reference: 'K' };

const ACTIVITIES = {
  measure_lo: ['低容量レンジで測定中', '1MΩ で充電し、GP26 が HIGH に変わるまでの時間を PIO で数えています (8 回平均)。'],
  measure_hi: ['高容量レンジで測定中', '10kΩ で充電しながら ADC で電圧を読み、充電カーブを当てはめています。'],
  discharge: ['放電中', '次の測定の前にコンデンサを 0V に戻しています。大容量では数十秒かかることがあります。'],
  idle: ['次の測定まで待機中', '0.3 秒ごとに測定を繰り返します。'],
  cal_zero: ['ゼロ点校正中', STEPS.z.purpose],
  cal_threshold: ['しきい値校正中', STEPS.c.purpose],
  cal_cross: ['レンジ間校正中', STEPS.x.purpose],
  cal_reference: ['基準コンデンサ校正中', STEPS.K.purpose],
};

// ファームウェアが受け付ける抵抗値の範囲 [Ω]
const R_LO_RANGE = [500e3, 2e6];
const R_HI_RANGE = [5e3, 20e3];

// ---- 状態 ----

const state = {
  connected: false,
  portLabel: '',
  activity: null,
  activitySince: Date.now(),
  voltage: null,
  voltageAt: 0,
  mode: 'auto',
  cal: null,             // { vth, strayPF, hiGain, done, rLo, rHi } (done / rLo / rHi は古いファームウェアでは null)
  readings: [],          // { at, farads (null: 範囲外), range: 'lo' | 'hi' }
  lastResult: null,      // { title, outcome, message }
  log: [],
  requestedStep: null,   // ボタンを押してから、ファームウェアが校正を始めたと知らせるまで
};

// ---- 表示用の書式 ----

function formatFarads(c) {
  const a = Math.abs(c);
  if (a < 1e-9) return [(c * 1e12).toFixed(2), 'pF'];
  if (a < 1e-6) return [(c * 1e9).toFixed(3), 'nF'];
  if (a < 1e-3) return [(c * 1e6).toFixed(3), 'µF'];
  return [(c * 1e3).toFixed(3), 'mF'];
}
const farads = c => formatFarads(c).join(' ');

function trimNumber(v) {
  return v.toFixed(3).replace(/0+$/, '').replace(/\.$/, '');
}

// ---- 状態の判断 (Mac アプリの MeterModel と同じ) ----

function isDone(step) {
  const cal = state.cal;
  if (!cal) return false;
  if (cal.done !== null) return (cal.done & STEPS[step].bit) !== 0;
  // 印のない古いファームウェア: 値が初期値と違えば校正済みとみなす
  switch (step) {
    case 'z': return cal.strayPF !== 0;
    case 'c': return Math.abs(cal.vth - 0.5) > 1e-6;
    case 'x': return Math.abs(cal.hiGain - 1) > 1e-6;
    default: return false;
  }
}

function calibrating() {
  if (state.requestedStep) return state.requestedStep;
  return ACTIVITY_TO_STEP[state.activity] || null;
}

function latest() { return state.readings[state.readings.length - 1] || null; }

// ゼロ点校正前は浮遊容量 (約 27pF) がそのまま見えるので、「何も挿さっていない」の上限を変える
function emptyThreshold() { return isDone('z') ? 3e-12 : 100e-12; }

// 'unknown' | 'empty' | 'outOfRange' | { capacitor: F }
function presence() {
  const r = latest();
  if (!r) return 'unknown';
  if (r.farads === null) return 'outOfRange';
  return r.farads < emptyThreshold() ? 'empty' : { capacitor: r.farads };
}

function isStable() {
  const recent = state.readings.slice(-3);
  if (recent.length < 3) return false;
  const range = recent[2].range;
  if (!recent.every(r => r.range === range && r.farads !== null)) return false;
  const values = recent.map(r => r.farads);
  const lo = Math.min(...values), hi = Math.max(...values);
  return hi - lo <= Math.max(Math.abs(hi) * 0.003, 0.5e-12);
}

function liveVoltage() {
  return state.voltage !== null && Date.now() - state.voltageAt < 1500 ? state.voltage : null;
}

// 校正を今実行できるか: [ok, 理由]
function readiness(step) {
  if (!state.connected) return [false, 'Pico がつながっていません'];
  if (calibrating()) return [false, 'ほかの校正を実行中です'];
  const p = presence();
  if (p === 'unknown') return [false, '最初の測定を待っています'];
  if (step === 'z') return p === 'empty' ? [true, '何も挿さっていません'] : [false, 'コンデンサを抜いてください'];
  if (p === 'empty') return [false, 'コンデンサが挿さっていません'];
  if (p === 'outOfRange') return [false, '測定範囲外です'];
  const c = p.capacitor;
  const ok = step === 'c' ? c >= 1e-9 && c <= 1e-6 : c >= 30e-9 && c <= 250e-9;
  return ok ? [true, `${farads(c)} が挿さっています`] : [false, `今の ${farads(c)} では範囲外です`];
}

function referenceReadiness(value) {
  if (!state.connected) return [false, 'Pico がつながっていません'];
  if (calibrating()) return [false, 'ほかの校正を実行中です'];
  if (!state.cal) return [false, '校正値を読み込んでいます'];
  if (state.cal.rLo === null) return [false, 'ファームウェアを新しくすると使えます'];
  if (!isDone('z') || !isDone('c')) return [false, '先にゼロ点校正としきい値校正をしてください'];
  if (value === null) return [false, '基準コンデンサの値を入れてください'];
  if (value < 1e-9 || value > 250e-9) return [false, '基準コンデンサは 1〜250 nF にしてください'];
  const p = presence();
  if (typeof p !== 'object') return [false, '基準コンデンサを挿してください'];
  if (Math.abs(p.capacitor / value - 1) > 0.2) return [false, `挿してあるもの (${farads(p.capacitor)}) と値が合いません`];
  const note = value >= 30e-9 ? '10kΩ側とレンジ間校正もまとめて済みます' : '30nF 以上なら 10kΩ側も決まります';
  return [true, `${farads(p.capacitor)} が挿さっています。${note}`];
}

function nextCalibration() {
  if (!state.cal) return null;
  return REQUIRED_STEPS.find(s => !isDone(s)) || null;
}

// ---- 受信した行の処理 ----

function appendLog(at, text) {
  const time = new Date(at).toLocaleTimeString('ja-JP', { hour12: false });
  state.log.push(`${time}  ${text}`);
  if (state.log.length > MAX_LOG) state.log.splice(0, state.log.length - MAX_LOG);
}

function handleLine(text, at) {
  appendLog(at, text);
  if (!text.startsWith('#')) return;
  const [kind, ...rest] = text.slice(1).split(',');
  // メッセージ (#done の 4 つ目) にはカンマが入りうるので、残りをつなぎ直す
  switch (kind) {
    case 'state': {
      const a = rest[0];
      if (!ACTIVITIES[a]) return;
      if (a !== state.activity) state.activitySince = at;
      state.activity = a;
      if (ACTIVITY_TO_STEP[a]) state.requestedStep = null;
      if (a === 'idle' || a === 'measure_lo') state.voltage = null;
      break;
    }
    case 'v': {
      const v = parseFloat(rest[0]);
      if (!Number.isNaN(v)) { state.voltage = v; state.voltageAt = at; }
      break;
    }
    case 'meas': {
      if (rest.length < 2) return;
      const v = parseFloat(rest[1]);
      state.readings.push({ at, farads: Number.isFinite(v) ? v : null, range: rest[0] });
      if (state.readings.length > MAX_READINGS) state.readings.splice(0, state.readings.length - MAX_READINGS);
      break;
    }
    case 'mode':
      if (['auto', 'lo', 'hi'].includes(rest[0])) state.mode = rest[0];
      break;
    case 'cal': {
      // 古いファームウェアは後ろの項目がない (済んだ校正の印、抵抗値)
      const n = rest.map(parseFloat);
      if (rest.length < 3 || n.slice(0, 3).some(Number.isNaN)) return;
      state.cal = {
        vth: n[0], strayPF: n[1], hiGain: n[2],
        done: rest.length >= 4 && !Number.isNaN(n[3]) ? n[3] : null,
        rLo: rest.length >= 6 && !Number.isNaN(n[4]) ? n[4] : null,
        rHi: rest.length >= 6 && !Number.isNaN(n[5]) ? n[5] : null,
      };
      fillResistorsFromDevice();
      break;
    }
    case 'done': {
      if (rest.length < 3) return;
      const title = rest[0] === 'R' ? '抵抗値の設定' : STEPS[rest[0]]?.title;
      if (!title) return;
      state.lastResult = { title, outcome: rest[1], message: rest.slice(2).join(',') };
      state.requestedStep = null;
      break;
    }
  }
}

// ---- シリアル (Web Serial) ----

let port = null;
let reader = null;

async function send(text) {
  if (!port || !port.writable) return;
  const writer = port.writable.getWriter();
  try {
    await writer.write(new TextEncoder().encode(text + '\r'));
  } finally {
    writer.releaseLock();
  }
}

async function open(p) {
  if (state.connected) return;
  try {
    await p.open({ baudRate: 115200 });
    // Pico の USB シリアルは、ホストが DTR を立てるまで出力しない
    await p.setSignals({ dataTerminalReady: true });
  } catch (e) {
    appendLog(Date.now(), `● 接続できません: ${e.message}`);
    render();
    return;
  }
  port = p;
  state.connected = true;
  state.portLabel = 'Pico — USB シリアル';
  appendLog(Date.now(), '● 接続しました');
  render();
  send('i');  // 校正値とモードを聞く
  readLoop();
}

async function readLoop() {
  const decoder = new TextDecoder();
  let buffer = '';
  try {
    while (port && port.readable) {
      reader = port.readable.getReader();
      try {
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          const at = Date.now();
          buffer += decoder.decode(value, { stream: true });
          const lines = buffer.split(/\r?\n|\r/);
          buffer = lines.pop();
          for (const line of lines) if (line) handleLine(line, at);
          scheduleRender();
        }
      } finally {
        reader.releaseLock();
        reader = null;
      }
    }
  } catch (e) {
    appendLog(Date.now(), `● 受信エラー: ${e.message}`);
  }
  disconnected();
}

async function disconnected() {
  if (!state.connected) return;
  state.connected = false;
  state.activity = null;
  state.voltage = null;
  state.requestedStep = null;
  appendLog(Date.now(), '● 切断されました');
  try { await port?.close(); } catch { /* すでに閉じている */ }
  port = null;
  render();
}

async function connectByUser() {
  try {
    const p = await navigator.serial.requestPort({ filters: [{ usbVendorId: PICO_VENDOR_ID }] });
    await open(p);
  } catch (e) {
    if (e.name !== 'NotFoundError') appendLog(Date.now(), `● ${e.message}`);  // NotFoundError: 選ばずに閉じた
    render();
  }
}

// 一度許可したポートは、ページを開いたときと USB を挿したときに自動でつなぐ
async function connectKnownPort() {
  if (state.connected) return;
  const ports = await navigator.serial.getPorts();
  const pico = ports.find(p => p.getInfo().usbVendorId === PICO_VENDOR_ID);
  if (pico) await open(pico);
}

// ---- 操作 ----

function setMode(mode) { send({ auto: 'a', lo: 'l', hi: 'h' }[mode]); }

function run(step) {
  state.requestedStep = step;
  state.lastResult = null;
  send(step);
  render();
}

function parseNumber(text) {
  const v = parseFloat(text.trim().replace(/,/g, ''));
  return Number.isFinite(v) ? v : null;
}

// ---- 描画 ----

const $ = id => document.getElementById(id);
let renderQueued = false;
function scheduleRender() {
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(() => { renderQueued = false; render(); });
}

function guidance() {
  if (!state.connected) {
    return { icon: '🔌', title: 'Pico を USB でつないでください',
      detail: 'つないだら上の「Pico に接続」を押して、Pico を選んでください。一度選べば、次からは自動でつながります。' };
  }
  const step = calibrating();
  if (step) return { icon: '⚙️', title: `${STEPS[step].title}中です`, detail: `終わるまでコンデンサに触らないでください (${STEPS[step].duration})。` };
  if (!state.cal) return { icon: '⏳', title: '校正値を読み込んでいます', detail: '' };
  const next = nextCalibration();
  if (next) {
    const [ok, reason] = readiness(next);
    return {
      icon: ok ? '👉' : '⬇️',
      title: `${STEPS[next].number} ${STEPS[next].title}をしてください`,
      detail: ok ? `準備ができています (${reason})。ボタンを押してください。` : `${STEPS[next].instruction}\n今の状態: ${reason}`,
      action: { label: `${STEPS[next].title}を実行`, enabled: ok, perform: () => run(next) },
    };
  }
  const p = presence();
  if (p === 'unknown') return { icon: '⏳', title: '最初の測定を待っています', detail: '' };
  if (p === 'empty') {
    return { icon: '⬇️', title: '測りたいコンデンサを挿してください',
      detail: 'GP26 の行と GND の行のあいだに挿します。電解コンデンサは + を GP26 側にし、挿す前に足をショートして放電してください。' };
  }
  if (p === 'outOfRange') {
    if (state.mode !== 'auto') {
      return { icon: '⚠️', title: 'このレンジでは測れません', detail: '低容量 / 高容量に固定されています。自動に切り替えると測れる可能性があります。',
        action: { label: '自動に切り替える', enabled: true, perform: () => setMode('auto') } };
    }
    return { icon: '⚠️', title: '測定範囲外です', detail: '約 1000µF を超えているか、足の接触が悪い可能性があります。挿し直してみてください。' };
  }
  const slow = latest()?.range === 'hi' ? '大容量は 1 回の測定 (主に放電) に時間がかかります。' : '';
  return isStable()
    ? { icon: '✅', title: '測定できています', detail: `値が安定しています。別のコンデンサを測るときは、抜いて挿し替えてください。${slow}` }
    : { icon: '〰️', title: '測定値が落ち着くのを待っています', detail: `3 回続けて値がそろうと「安定」になります。${slow}` };
}

function renderGuidance() {
  const g = guidance();
  $('guide-icon').textContent = g.icon;
  $('guide-title').textContent = g.title;
  $('guide-detail').textContent = g.detail;
  const button = $('guide-action');
  button.hidden = !g.action;
  if (g.action) {
    button.textContent = g.action.label;
    button.disabled = !g.action.enabled;
    button.onclick = g.action.perform;
  }
  const res = state.lastResult, box = $('result');
  box.hidden = !res;
  if (res) {
    const verb = { ok: 'が完了しました', abort: 'を中断しました' }[res.outcome] || 'に失敗しました';
    box.className = `result ${['ok', 'abort'].includes(res.outcome) ? res.outcome : 'fail'}`;
    box.textContent = '';
    const strong = document.createElement('strong');
    strong.textContent = `${res.title}${verb}　`;
    box.append(strong, res.message);
  }
}

function renderReading() {
  const p = presence(), r = latest();
  let number = '—', unit = '', range = '', stable = '';
  if (p === 'unknown') range = state.connected ? '最初の測定を待っています' : '未接続';
  else if (p === 'empty') range = `何も挿さっていません (${farads(r.farads)})`;
  else if (p === 'outOfRange') { number = '範囲外'; range = rangeLabel(r.range); }
  else {
    [number, unit] = formatFarads(p.capacitor);
    range = rangeLabel(r.range);
    stable = isStable() ? '<span class="stable">● 安定</span>' : '<span class="muted">… 読み取り中</span>';
  }
  $('reading-number').textContent = number;
  $('reading-unit').textContent = unit;
  $('reading-range').textContent = range;
  $('reading-stable').innerHTML = stable;
}
const rangeLabel = r => r === 'hi' ? '高容量レンジ (10kΩ / ADC)' : '低容量レンジ (1MΩ / PIO)';

function renderActivity() {
  let title, detail;
  if (!state.connected) [title, detail] = ['未接続', 'Pico をつなぐと、ここに動作の状況が出ます。'];
  else if (!state.activity) [title, detail] = ['状態を確認中', '次の動作が始まると表示されます。'];
  else [title, detail] = ACTIVITIES[state.activity];
  $('activity-title').textContent = title;
  $('activity-detail').textContent = detail;
  const elapsed = (Date.now() - state.activitySince) / 1000;
  $('activity-elapsed').textContent =
    state.connected && state.activity && state.activity !== 'idle' && elapsed >= 2 ? `${Math.floor(elapsed)} 秒` : '';
  const v = liveVoltage();
  $('voltage').hidden = v === null;
  if (v !== null) {
    $('voltage-bar').style.width = `${Math.min(Math.max(v / 3.3, 0), 1) * 100}%`;
    $('voltage-text').textContent = `${v.toFixed(2)} V`;
  }
}

function renderChart() {
  const svg = $('chart');
  const points = state.readings.filter(r => r.farads !== null);
  if (points.length === 0) {
    svg.innerHTML = '<text x="200" y="85" text-anchor="middle" fill="currentColor" opacity="0.5" font-size="12">まだ測定値がありません</text>';
    $('chart-axis').textContent = '';
    return;
  }
  const unit = formatFarads(points[points.length - 1].farads)[1];
  const scale = { pF: 1e-12, nF: 1e-9, µF: 1e-6, mF: 1e-3 }[unit];
  const ys = points.map(p => p.farads / scale);
  let lo = Math.min(...ys), hi = Math.max(...ys);
  if (hi - lo < 1e-9) { lo -= 0.5; hi += 0.5; }
  const pad = (hi - lo) * 0.1;
  lo -= pad; hi += pad;
  const t0 = points[0].at, t1 = Math.max(points[points.length - 1].at, t0 + 1);
  const X = t => ((t - t0) / (t1 - t0)) * 392 + 4;
  const Y = v => 156 - ((v - lo) / (hi - lo)) * 152;
  const path = points.map((p, i) => `${i ? 'L' : 'M'}${X(p.at).toFixed(1)} ${Y(ys[i]).toFixed(1)}`).join(' ');
  svg.innerHTML = `<path d="${path}" fill="none" stroke="var(--accent)" stroke-width="2" vector-effect="non-scaling-stroke"/>`;
  const fmt = v => v.toFixed(Math.max(0, 3 - Math.floor(Math.log10(Math.max(Math.abs(hi), 1e-9)))));
  $('chart-axis').textContent = '';
  const left = document.createElement('span');
  left.textContent = `${fmt(lo + pad)} 〜 ${fmt(hi - pad)} ${unit}`;
  const right = document.createElement('span');
  right.textContent = `${points.length} 点 / ${Math.round((t1 - t0) / 1000)} 秒`;
  $('chart-axis').append(left, right);
}

function stepValue(step) {
  const c = state.cal;
  if (!c) return '';
  if (step === 'z') return `${c.strayPF.toFixed(2)} pF`;
  if (step === 'c') return `${c.vth.toFixed(4)} × VDD (${(c.vth * 3.3).toFixed(3)} V)`;
  if (step === 'x') return `× ${c.hiGain.toFixed(4)}`;
  return '';
}

// 校正の一覧は最初に 1 回だけ作り、あとは中身だけを書き換える
// (作り直すと、押している途中のボタンが入れ替わってクリックが空振りする)
const stepRows = {};

function buildSteps() {
  const box = $('steps');
  for (const step of REQUIRED_STEPS) {
    const row = document.createElement('div');
    row.className = 'step';
    row.title = STEPS[step].purpose;
    const mark = document.createElement('span');
    const body = document.createElement('div');
    body.className = 'body';
    const head = document.createElement('div');
    const name = document.createElement('span');
    name.className = 'bold';
    name.textContent = `${STEPS[step].number} ${STEPS[step].title}`;
    const value = document.createElement('span');
    value.className = 'muted mono small';
    head.append(name, ' ', value);
    const need = document.createElement('div');
    need.className = 'small';
    need.textContent = `必要なもの: ${STEPS[step].requirement}`;
    const status = document.createElement('div');
    body.append(head, need, status);
    const button = document.createElement('button');
    button.onclick = () => run(step);
    row.append(mark, body, button);
    box.append(row);
    stepRows[step] = { mark, value, status, button };
  }
}

function renderSteps() {
  for (const step of REQUIRED_STEPS) {
    const { mark, value, status, button } = stepRows[step];
    const done = isDone(step), [ok, reason] = readiness(step), running = calibrating() === step;
    mark.className = `mark ${done ? 'done' : 'muted'}`;
    mark.textContent = done ? '✔︎' : '○';
    value.textContent = done ? stepValue(step) : '';
    status.className = `small ${ok || running ? 'ok-text' : 'muted'}`;
    status.textContent = running ? '実行中…' : reason;
    button.textContent = done ? 'やり直す' : '実行';
    button.disabled = !ok || running;
  }
}

function fillResistorsFromDevice() {
  const c = state.cal;
  if (!c || c.rLo === null) return;
  // 入力中の欄は上書きしない
  if (document.activeElement !== $('r-lo')) $('r-lo').value = trimNumber(c.rLo / 1e3);
  if (document.activeElement !== $('r-hi')) $('r-hi').value = trimNumber(c.rHi / 1e3);
}

function renderResistors() {
  const c = state.cal;
  let source = '読み込み中';
  if (c && c.rLo === null) source = 'このファームウェアは抵抗値の設定に対応していません';
  else if (c && isDone('K')) source = '★ 基準コンデンサで決めた値を使っています';
  else if (c && c.rLo === 1e6 && c.rHi === 10e3) source = '公称値 (1MΩ / 10kΩ) のままです';
  else if (c) source = '入力した値を使っています';
  $('resistor-source').textContent = source;

  const lo = parseNumber($('r-lo').value), hi = parseNumber($('r-hi').value);
  let problem = null;
  if (lo === null || hi === null) problem = '数字を入れてください';
  else if (lo * 1e3 < R_LO_RANGE[0] || lo * 1e3 > R_LO_RANGE[1]) problem = '1MΩ 側は 500〜2000 kΩ で入れてください';
  else if (hi * 1e3 < R_HI_RANGE[0] || hi * 1e3 > R_HI_RANGE[1]) problem = '10kΩ 側は 5〜20 kΩ で入れてください';
  $('r-problem').textContent = problem || '';
  const changed = c && c.rLo !== null && lo !== null && hi !== null &&
    (Math.abs(lo * 1e3 - c.rLo) >= 0.5 || Math.abs(hi * 1e3 - c.rHi) >= 0.5);
  $('r-save').disabled = !state.connected || !!calibrating() || !!problem || !changed;

  const ref = parseNumber($('ref-value').value);
  const [ok, reason] = referenceReadiness(ref === null ? null : ref * 1e-9);
  const running = calibrating() === 'K';
  $('ref-reason').textContent = running ? '実行中…' : reason;
  $('ref-reason').className = `small ${ok || running ? 'ok-text' : 'muted'}`;
  $('ref-run').disabled = !ok || running;
}

function renderLog() {
  const log = $('log');
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
  log.textContent = state.log.join('\n');
  if (atBottom) log.scrollTop = log.scrollHeight;
}

function render() {
  $('dot').classList.toggle('on', state.connected);
  $('port-label').textContent = state.connected ? state.portLabel : '未接続';
  $('connect').hidden = state.connected;
  for (const b of $('mode').children) {
    b.setAttribute('aria-checked', String(b.dataset.mode === state.mode));
    b.disabled = !state.connected || !!calibrating();
  }
  renderGuidance();
  renderReading();
  renderActivity();
  renderChart();
  renderSteps();
  renderResistors();
  renderLog();
}

// ---- デモ (Pico の出力をまねる。#demo のとき) ----

function startDemo() {
  state.connected = true;
  state.portLabel = 'デモ (Pico の出力をまねたデータ)';
  const emit = (text) => handleLine(text, Date.now());
  let cap = 0;
  const cal = { vth: 0.5, stray: 0, gain: 1, done: 0, rLo: 1e6, rHi: 10e3 };
  const sendCal = () => emit(`#cal,${cal.vth.toFixed(4)},${cal.stray.toFixed(2)},${cal.gain.toFixed(4)},${cal.done},${cal.rLo},${cal.rHi}`);
  emit('#mode,auto');
  sendCal();
  // コンソールから demo.insert(105e-9) / demo.remove() でコンデンサの抜き差しをまねる
  window.demo = { insert: f => { cap = f; }, remove: () => { cap = 0; } };
  // デモ中の送信: 校正コマンドに応える
  send = async (text) => {
    appendLog(Date.now(), `→ ${text}`);
    const cmd = text[0];
    const finish = (bit, msg, apply) => setTimeout(() => {
      apply(); cal.done |= bit; emit(`#done,${cmd},ok,${msg}`); sendCal();
      emit('#state,idle');  // 本物のファームウェアも、校正のあとは次の測定に移る
      scheduleRender();
    }, 1500);
    if (cmd === 'z') { emit('#state,cal_zero'); finish(1, '浮遊容量: 27.10 pF', () => { cal.stray = 27.1; }); }
    if (cmd === 'c') { emit('#state,cal_threshold'); finish(2, 'しきい値: 0.4931 × VDD (1.627 V @3.3V)', () => { cal.vth = 0.4931; }); }
    if (cmd === 'x') { emit('#state,cal_cross'); finish(4, '低容量 105.129 nF / 高容量 105.133 nF -> 補正係数 1.0000', () => { cal.gain = 0.99996; }); }
    if ('alh'.includes(cmd)) emit(`#mode,${{ a: 'auto', l: 'lo', h: 'hi' }[cmd]}`);
    scheduleRender();
  };
  setInterval(() => {
    if (ACTIVITY_TO_STEP[state.activity]) return;
    emit('#state,measure_lo');
    const noise = (Math.random() - 0.5) * 2e-4;
    const value = cap === 0 ? (cal.stray ? 0.1e-12 : 27e-12) : cap * (1 + noise);
    emit(`#meas,lo,${value.toExponential(6)}`);
    emit('#state,idle');
    scheduleRender();
  }, 700);
}

// ---- 初期化 ----

function init() {
  buildSteps();
  for (const b of $('mode').children) b.onclick = () => setMode(b.dataset.mode);
  $('connect').onclick = connectByUser;
  for (const id of ['r-lo', 'r-hi', 'ref-value']) $(id).addEventListener('input', render);
  $('r-save').onclick = () => {
    const lo = parseNumber($('r-lo').value), hi = parseNumber($('r-hi').value);
    if (lo === null || hi === null) return;
    state.lastResult = null;
    send(`R ${Math.round(lo * 1e3)} ${Math.round(hi * 1e3)}`);
  };
  $('ref-run').onclick = () => {
    const nf = parseNumber($('ref-value').value);
    if (nf === null) return;
    state.requestedStep = 'K';
    state.lastResult = null;
    send(`K ${nf.toFixed(4)}`);
    render();
  };
  setInterval(render, 500);  // 経過秒数と、途中経過の電圧の表示を更新する

  if (location.hash === '#demo') {
    startDemo();
  } else if (!('serial' in navigator)) {
    $('unsupported').hidden = false;
    $('connect').disabled = true;
  } else {
    navigator.serial.addEventListener('connect', connectKnownPort);
    navigator.serial.addEventListener('disconnect', e => { if (e.target === port) disconnected(); });
    connectKnownPort();
  }
  render();
}

init();
