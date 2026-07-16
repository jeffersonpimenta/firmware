// Irrigação — Gateway painel. Vanilla JS, sem dependências (dispositivo offline).
// Roteador de abas + fetch/render de Visão Geral e Estações (leitura).
// Zonas/Programas adicionados nas Tasks 12/13 estendendo o mapa RENDER.
'use strict';

const API = '/api/irrigation';
const view = document.getElementById('view');
const syncChip = document.getElementById('syncChip');
let current = 'overview';
let timer = null;

async function getJson(path) {
  const r = await fetch(API + path);
  if (!r.ok) throw new Error(r.status);
  return r.json();
}

async function postJson(path, body) {
  const r = await fetch(API + path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  const j = await r.json().catch(() => ({}));
  return { ok: r.ok, body: j };
}

// Escapa texto para inserção segura em HTML (nomes de estação vêm do usuário).
function esc(v) {
  return String(v == null ? '' : v)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

// Número seguro (fallback 0 se ausente/NaN).
function num(v) {
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
}

function fmtSince(s) {
  s = num(s);
  if (s < 60) return 'há ' + s + ' s';
  if (s < 3600) return 'há ' + Math.floor(s / 60) + ' min';
  return 'há ' + Math.floor(s / 3600) + ' h';
}

function fmtVolts(centiV) {
  return (num(centiV) / 100).toFixed(1) + ' V';
}

function nodeHex(node) {
  return '0x' + (num(node) >>> 0).toString(16);
}

async function renderOverview() {
  const o = (await getJson('/overview')) || {};
  syncChip.textContent = o.hasRtc ? 'com relógio' : 'sem relógio';
  syncChip.className = 'chip ' + (o.hasRtc ? 'green' : 'amber');

  const running = o.running
    ? 'zona ' + num(o.runningZoneId) + ' · ' + num(o.runningRemainMin) + ' min'
    : '—';

  view.innerHTML =
    `<div class="row3">
      <div class="card stat"><div class="lbl">Estações</div><div class="val">${num(o.stationCount)}</div></div>
      <div class="card stat"><div class="lbl">Em execução</div><div class="val">${running}</div></div>
      <div class="card stat"><div class="lbl">Alertas</div><div class="val warn">${num(o.alertCount)}</div></div>
    </div>` +
    (o.pairingPending
      ? `<div class="card amberbox">Pareamento pendente — nó ${nodeHex(o.pairingNodeId)} · expira em ${num(o.pairingSecondsLeft)}s</div>`
      : '') +
    (!o.hasRtc
      ? `<div class="card amberbox">Sem relógio — cronograma inativo. Modo espelho segue operando.</div>`
      : '');
}

const SYNC_CLASS = { sincronizada: 'green', pendente: 'amber', inalcancavel: 'red' };

async function renderStations() {
  const list = (await getJson('/stations')) || [];
  const rows = Array.isArray(list) ? list : [];
  view.innerHTML =
    rows
      .map((s) => {
        s = s || {};
        const name = s.name ? esc(s.name) : nodeHex(s.node);
        const cls = SYNC_CLASS[s.sync] || 'red';
        const sync = esc(s.sync || 'inalcancavel');
        return `<div class="card station">
      <div class="name">${name}</div>
      <div class="sub">${fmtSince(s.secsSinceHeard)} · ${fmtVolts(s.vbatCentiV)}</div>
      <div class="chips"><span class="chip ${cls}">${sync}</span></div>
    </div>`;
      })
      .join('') || '<div class="empty">Nenhuma estação registrada.</div>';
}

// Mapa extensível: zones/programs adicionados nas Tasks 12/13.
const RENDER = { overview: renderOverview, stations: renderStations };

async function show(tab) {
  current = tab;
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.dataset.tab === tab));
  if (timer) {
    clearInterval(timer);
    timer = null;
  }
  const fn = RENDER[tab];
  if (!fn) {
    view.innerHTML = '<div class="empty">Em breve.</div>';
    return;
  }
  try {
    await fn();
  } catch (e) {
    view.innerHTML = '<div class="empty">Erro ao carregar (' + esc(e.message) + ').</div>';
  }
  if (tab === 'overview' || tab === 'stations') {
    timer = setInterval(() => fn().catch(() => {}), 3000);
  }
}

document.querySelectorAll('.tab').forEach((t) => {
  if (t.disabled) return;
  t.addEventListener('click', () => show(t.dataset.tab));
});

show('overview');
