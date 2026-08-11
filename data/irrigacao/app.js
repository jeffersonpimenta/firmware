// Irrigação — Gateway painel. Vanilla JS, sem dependências (dispositivo offline).
// Roteador de abas + fetch/render de Visão Geral e Estações (leitura).
// Zonas/Programas adicionados nas Tasks 12/13 estendendo o mapa RENDER.
'use strict';

const API = '/api/irrigation';
const view = document.getElementById('view');
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

// AlertType (StationMonitor.h) → [mensagem, classe de cor do ponto].
const ALERT_INFO = {
  1: ['Bateria em aviso', 'amber'],
  2: ['Bateria crítica', 'red'],
  3: ['Hibernação por bateria', 'red'],
  4: ['Bateria recuperada', 'green'],
  5: ['Estação silenciosa', 'red'],
  6: ['Estação voltou ao ar', 'green'],
  7: ['Reboots anômalos', 'amber'],
  8: ['Falha de comando (sem ACK)', 'red'],
  9: ['Config adotada do serviço', 'green'],
  10: ['Boia sem sinal — bomba desligada', 'red'],
};

// Minutos até o próximo disparo de um programa, dado o relógio do CLIENTE (o celular tem
// hora real mesmo se o gateway não tem RTC). daysMask bit0=dom..bit6=sáb. Infinity se nunca.
function nextFireMinutes(mask, startMin, now) {
  mask = num(mask) & 127;
  if (!mask) return Infinity;
  const dow = now.getDay(); // 0=domingo
  const nowMin = now.getHours() * 60 + now.getMinutes();
  for (let d = 0; d < 8; d++) {
    const day = (dow + d) % 7;
    if (!(mask & (1 << day))) continue;
    if (d === 0 && startMin <= nowMin) continue; // já passou hoje
    return d * 1440 + startMin - nowMin;
  }
  return Infinity;
}

async function renderOverview() {
  const [o, alerts, stations, programs, zones] = await Promise.all([
    getJson('/overview'),
    getJson('/alerts').catch(() => []),
    getJson('/stations').catch(() => []),
    getJson('/programs').catch(() => []),
    getJson('/zones').catch(() => []),
  ]);
  const ov = o || {};

  const tb = document.getElementById('timeBadge');
  if (tb) {
    if (ov.hasRtc) {
      const now = new Date();
      const p = (n) => String(n).padStart(2, '0');
      tb.textContent = `${p(now.getHours())}:${p(now.getMinutes())}`;
      tb.classList.remove('hidden');
    } else {
      tb.classList.add('hidden');
    }
  }

  const alertN = num(ov.alertCount);
  let html =
    `<div class="row3">
      <div class="card stat"><div class="lbl">Estações</div><div class="val">${num(ov.stationCount)}</div></div>
      <div class="card stat"><div class="lbl">Em execução</div><div class="val">${ov.running ? 'zona ' + num(ov.runningZoneId) : '—'}</div></div>
      <div class="card stat"><div class="lbl">Alertas</div><div class="val${alertN ? ' warn' : ''}">${alertN}</div></div>
    </div>`;

  if (ov.running) {
    html += `<div class="card greenbox exec">
      <div class="lbl green">Em execução agora</div>
      <div class="big">Zona ${num(ov.runningZoneId)} · restam ${num(ov.runningRemainMin)} min</div>
      <div class="asub">Fecha sozinha pelo fail-safe local ao expirar.</div>
    </div>`;
  }

  if (ov.pairingPending) {
    html += `<div class="card amberbox pair-goto" data-goto-stations role="button" tabindex="0">Pareamento pendente — nó ${nodeHex(ov.pairingNodeId)} · expira em ${num(ov.pairingSecondsLeft)}s <span class="chev">›</span></div>`;
  }
  if (!ov.hasRtc) {
    html += `<div class="card amberbox">Sem relógio — cronograma inativo. Modo espelho segue operando.</div>`;
  }

  const al = Array.isArray(alerts) ? alerts : [];
  html += `<div class="sec-title">Alertas não reconhecidos</div>`;
  html += al.length
    ? '<div class="alerts">' +
      al
        .map((a) => {
          const info = ALERT_INFO[num(a.type)] || ['Alerta', 'amber'];
          return `<div class="card alert">
          <span class="dot ${info[1]}"></span>
          <div class="ainfo"><div class="amsg">${esc(info[0])}</div><div class="asub">${esc(stationName(stations, a.node))} · ${fmtSince(a.ageS)}</div></div>
          <button class="btn ghost sm" data-ack data-node="${num(a.node)}" data-type="${num(a.type)}" data-arg="${num(a.arg)}" data-at="${num(a.atMs)}">Reconhecer</button>
        </div>`;
        })
        .join('') +
      '</div>'
    : '<div class="empty">Nenhum alerta pendente.</div>';

  if (ov.hasRtc) {
    const now = new Date();
    const ups = (Array.isArray(programs) ? programs : [])
      .filter((p) => p && p.enabled)
      .map((p) => ({ p, inMin: nextFireMinutes(p.daysMask, num(p.startMinute), now) }))
      .filter((x) => Number.isFinite(x.inMin))
      .sort((a, b) => a.inMin - b.inMin)
      .slice(0, 3);
    if (ups.length) {
      html +=
        `<div class="sec-title">Próximas execuções</div>` +
        ups
          .map(
            ({ p }) => `<div class="card">
             <div class="amsg">${esc(diasLabel(p.daysMask))} · ${esc(minToTime(p.startMinute))}</div>
             <div class="asub">${esc(seqText(p.steps, zones))}</div>
           </div>`
          )
          .join('');
    }
  }

  view.innerHTML = html;
  // Card de pareamento pendente → leva à aba Estações (onde fica o botão Aprovar).
  const pgoto = view.querySelector('[data-goto-stations]');
  if (pgoto) pgoto.addEventListener('click', () => show('stations'));
  // Reconhece só o alerta clicado: ecoa sua identidade (node+type+arg+atMs) ao backend (ackMatch).
  view.querySelectorAll('[data-ack]').forEach((b) =>
    b.addEventListener('click', async () => {
      await postJson('/command', {
        kind: 'ack',
        node: num(b.dataset.node),
        type: num(b.dataset.type),
        arg: num(b.dataset.arg),
        atMs: num(b.dataset.at),
      });
      renderOverview().catch(() => {});
    })
  );
}

// Estado do sync → [classe de cor, rótulo]. Fonte única p/ pills e sheet.
function stationSyncLabel(sync) {
  return (
    {
      sincronizada: ['green', 'Sincronizada'],
      pendente: ['amber', 'Pendente'],
      inalcancavel: ['red', 'Inalcançável'],
    }[sync || 'inalcancavel'] || ['red', 'Inalcançável']
  );
}

// Pills de estado derivadas do StationView (sem backend extra).
function stationPills(s) {
  const p = [stationSyncLabel(s.sync)];
  const f = num(s.flags);
  if (f & 1) p.push(['red', 'Tamper']);
  if (f & 2) p.push(['red', 'Modo seguro']);
  if (f & 4) p.push(['amber', 'Hibernando']);
  const vb = num(s.vbatCentiV);
  if (vb && vb < 1180) p.push(['red', 'Bateria crítica']);
  else if (vb && vb < 1220) p.push(['amber', 'Bateria em aviso']);
  if (s.snrQuarterDb != null && num(s.snrQuarterDb) / 4 < 3) p.push(['amber', 'Enlace degradando']);
  return p.map(([c, t]) => `<span class="chip ${c}">${esc(t)}</span>`).join('');
}

function stationByNode(list, node) {
  return (Array.isArray(list) ? list : []).find((x) => x && num(x.node) === num(node));
}

async function renderStations() {
  const [list, ov] = await Promise.all([getJson('/stations'), getJson('/overview').catch(() => ({}))]);
  const rows = Array.isArray(list) ? list : [];

  // §6: card de pareamento pendente (nó anunciou com a janela fechada) + botão Aprovar.
  let pairHtml = '';
  if (ov && ov.pairingPending) {
    pairHtml = `<div class="card pairbox">
      <div class="pairhdr">
        <div class="pairtitle">Nó novo detectado</div>
        <div class="pairsec">${num(ov.pairingSecondsLeft)}s</div>
      </div>
      <div class="pairsub">${esc(nodeHex(ov.pairingNodeId))} · janela de pareamento</div>
      <button class="btn solid big" id="pairApprove" data-node="${num(ov.pairingNodeId)}">Aprovar pareamento</button>
    </div>`;
  }

  const cards =
    rows
      .map((s) => {
        s = s || {};
        const name = s.name ? esc(s.name) : nodeHex(s.node);
        return `<div class="card station" data-node="${nodeHex(s.node)}" role="button" tabindex="0">
      <div class="st-main">
        <div class="name">${name}</div>
        <div class="sub">${fmtSince(s.secsSinceHeard)} · ${fmtVolts(s.vbatCentiV)}</div>
        <div class="chips">${stationPills(s)}</div>
      </div>
      <span class="chev">›</span>
    </div>`;
      })
      .join('') || '<div class="empty">Nenhuma estação registrada.</div>';
  view.innerHTML = pairHtml + cards;

  const approve = view.querySelector('#pairApprove');
  if (approve) {
    approve.addEventListener('click', async () => {
      approve.disabled = true;
      await postJson('/command', { kind: 'approve_pairing', node: num(approve.dataset.node) });
      renderStations().catch(() => {});
    });
  }

  view.querySelectorAll('.card.station').forEach((el) => {
    const node = parseInt(el.dataset.node, 16);
    const open = () => openStationSheet(node);
    el.addEventListener('click', open);
    el.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        open();
      }
    });
  });

  // Poll re-render: se um sheet está aberto em modo leitura, atualiza com o dado fresco.
  // Em edição, não re-renderiza (preservaria o form/inputs do usuário).
  if (openStationNode != null && stSheetMode === 'view') {
    const s = stationByNode(rows, openStationNode);
    if (s) renderStationSheet(s);
    else closeStationSheet();
  }
}

// ===== Sheet de detalhe da estação (overlay no body, sobrevive ao poll do #view) =====
let openStationNode = null;
let stSheetAudit = [];
let stSheetStation = null; // última estação renderizada (p/ transições sem refetch)
let stSheetMode = 'view'; // 'view' | 'edit'
let stSheetDraft = null; // rascunho do form em edição
let stSheetErrors = []; // erros do último salvar
let stSheetDeleteConfirm = false;
let stSheetPulse = 'idle'; // 'idle' | 'enviando' | 'aberto' | 'fechado'
let stSheetPulseSel = 0; // índice da saída escolhida em s.outputs

async function openStationSheet(node) {
  openStationNode = node;
  stSheetMode = 'view';
  stSheetDraft = null;
  stSheetErrors = [];
  stSheetDeleteConfirm = false;
  stSheetPulse = 'idle';
  stSheetPulseSel = 0;
  const [list, audit] = await Promise.all([
    getJson('/stations').catch(() => []),
    getJson('/audit?fmt=json&n=500').catch(() => []),
  ]);
  stSheetAudit = Array.isArray(audit) ? audit : [];
  const s = stationByNode(list, node);
  if (!s) {
    closeStationSheet();
    return;
  }
  renderStationSheet(s);
}

function closeStationSheet() {
  openStationNode = null;
  stSheetStation = null;
  const el = document.getElementById('stSheet');
  if (el) el.remove();
}

function stationMiniLog(node) {
  const rows = stSheetAudit.filter((a) => a && num(a.node) === num(node)).slice(0, 8);
  if (!rows.length) return '<div class="ml-empty">Sem eventos.</div>';
  return rows
    .map(
      (a) =>
        `<div class="ml-row"><span class="ml-ts">${esc(fmtEpoch(a.ts))}</span>` +
        `<span class="ml-ac">${esc(ACOES_LABEL[num(a.acao)] || 'ação ' + num(a.acao))}</span></div>`
    )
    .join('');
}

// Rótulo de uma saída física da estação (outputs[]).
function outputLabel(o) {
  if (o && o.label) return esc(o.label);
  const t = num(o && o.tipo) === 1 ? 'GPO' : 'Válvula';
  return esc(t + ' ' + (num(o && o.index) + 1));
}

// Bloco do teste de pulso (view): seletor de saída + botão com estados.
function pulseBlock(s) {
  const outs = Array.isArray(s.outputs) ? s.outputs : [];
  if (!outs.length) return '';
  const chips = outs
    .map(
      (o, i) =>
        `<button class="oput ${i === stSheetPulseSel ? 'sel' : ''}" data-out="${i}">${outputLabel(o)}</button>`
    )
    .join('');
  let action;
  if (stSheetPulse === 'enviando') action = '<div class="pulse-state">Enviando comando…</div>';
  else if (stSheetPulse === 'aberto') action = '<div class="pulse-state open">Aberto — fechará automaticamente</div>';
  else if (stSheetPulse === 'fechado') action = '<div class="pulse-state">Fechado — teste concluído</div>';
  else action = '<button class="btn outline big" id="stPulse">Teste de pulso (10 s)</button>';
  return `<div class="ml-wrap">
      <div class="sg-lbl">Teste de pulso</div>
      <div class="oput-row">${chips}</div>
      ${action}
    </div>`;
}

function renderStationSheet(s) {
  stSheetStation = s;
  const name = s.name ? esc(s.name) : nodeHex(s.node);
  const [syncCls, syncTxt] = stationSyncLabel(s.sync);
  const inner = stSheetMode === 'edit' ? stationSheetEdit(s) : stationSheetView(s, name, syncCls, syncTxt);

  const html = `
    <div class="sheet-backdrop" id="stSheet">
      <div class="sheet" role="dialog" aria-label="Detalhe da estação">
        <div class="sheet-grip"></div>
        ${inner}
      </div>
    </div>`;

  const existing = document.getElementById('stSheet');
  if (existing) existing.outerHTML = html;
  else document.body.insertAdjacentHTML('beforeend', html);
  wireStationSheet(s);
}

function stationSheetView(s, name, syncCls, syncTxt) {
  const lat = num(s.lat);
  const lon = num(s.lon);
  const coords = lat || lon ? `${(lat / 1e5).toFixed(5)}, ${(lon / 1e5).toFixed(5)}` : '—';
  const snr = s.snrQuarterDb != null ? (num(s.snrQuarterDb) / 4).toFixed(1) + ' dB' : '—';
  const rssi = s.rssiDbm != null ? num(s.rssiDbm) + ' dBm' : '—';
  const hb = s.hbMinutes != null ? 'A cada ' + num(s.hbMinutes) + ' min' : '—';
  const limiares =
    s.vbatAvisoCentiV != null
      ? `Aviso ${fmtVolts(s.vbatAvisoCentiV)} · crítica ${fmtVolts(s.vbatCriticaCentiV)}`
      : '';
  return `
    <div class="sheet-hdr">
      <div class="sheet-hmain">
        <div class="sheet-title">${name}</div>
        <div class="sheet-sub">${esc(nodeHex(s.node))} · último contato ${fmtSince(s.secsSinceHeard)}</div>
      </div>
      <button class="btn ghost sm" id="stEdit">Editar</button>
    </div>
    <div class="sheet-sync ${syncCls}">${syncTxt}</div>
    <div class="sheet-grid">
      <div class="sg-box"><div class="sg-lbl">Bateria</div><div class="sg-val">${fmtVolts(s.vbatCentiV)}</div></div>
      <div class="sg-box"><div class="sg-lbl">Painel solar</div><div class="sg-val">${fmtVolts(s.vpanelCentiV)}</div></div>
      <div class="sg-box"><div class="sg-lbl">SNR / RSSI</div><div class="sg-val sm">${snr} / ${rssi}</div></div>
      <div class="sg-box"><div class="sg-lbl">Reboots</div><div class="sg-val">${num(s.rebootCount)}</div></div>
    </div>
    <div class="sg-box wide"><div class="sg-lbl">Coordenadas</div><div class="sg-val sm">${esc(coords)}</div></div>
    <div class="sg-box wide">
      <div class="sg-lbl">Heartbeat &amp; limiares de bateria</div>
      <div class="sg-val sm">${hb}</div>
      ${limiares ? `<div class="sg-note">${limiares}</div>` : ''}
    </div>
    ${pulseBlock(s)}
    <div class="ml-wrap">
      <div class="sg-lbl">Log remoto — esta estação</div>
      <div class="ml-list">${stationMiniLog(s.node)}</div>
    </div>
    <button class="btn solid big" id="stClose">Fechar</button>`;
}

function stationSheetEdit(s) {
  const d = stSheetDraft;
  const errs = stSheetErrors.length
    ? `<div class="card redbox sheet-errs">${stSheetErrors.map((e) => `<div>${esc(e)}</div>`).join('')}</div>`
    : '';
  let del = '';
  if (stSheetDeleteConfirm) {
    del = `<div class="card redbox">
        <div class="qtext">Remover esta estação? Esta ação não pode ser desfeita.</div>
        <div class="btnrow">
          <button class="btn ghost sm" id="stDelCancel">Cancelar</button>
          <button class="btn danger sm" id="stDelConfirm">Remover</button>
        </div>
      </div>`;
  } else {
    del = '<button class="btn dangerline" id="stDelReq">Remover estação</button>';
  }
  return `
    <div class="sheet-hdr">
      <div class="sheet-hmain">
        <div class="sheet-title">${s.name ? esc(s.name) : nodeHex(s.node)}</div>
        <div class="sheet-sub">${esc(nodeHex(s.node))}</div>
      </div>
    </div>
    ${errs}
    <div class="card form sheet-form">
      <label class="fld"><span class="flbl">Heartbeat (min)</span>
        <input class="finput" id="edHb" type="number" min="1" max="1440" value="${esc(d.hbMinutes)}"></label>
      <div class="frow">
        <label class="fld"><span class="flbl">Aviso (V)</span>
          <input class="finput" id="edAviso" type="number" step="0.1" value="${esc(d.vbatAvisoV)}"></label>
        <label class="fld"><span class="flbl">Crítica (V)</span>
          <input class="finput" id="edCritica" type="number" step="0.1" value="${esc(d.vbatCriticaV)}"></label>
      </div>
      <div class="frow">
        <label class="fld"><span class="flbl">Latitude</span>
          <input class="finput" id="edLat" type="number" step="0.00001" value="${esc(d.lat)}"></label>
        <label class="fld"><span class="flbl">Longitude</span>
          <input class="finput" id="edLon" type="number" step="0.00001" value="${esc(d.lon)}"></label>
      </div>
    </div>
    <div class="frow sheet-actions">
      <button class="btn ghost sm" id="stEditCancel">Cancelar</button>
      <button class="btn solid sm grow" id="stEditSave">Salvar</button>
    </div>
    ${del}`;
}

function enterStationEdit(s) {
  stSheetMode = 'edit';
  stSheetErrors = [];
  stSheetDeleteConfirm = false;
  stSheetDraft = {
    hbMinutes: s.hbMinutes != null ? num(s.hbMinutes) : 10,
    vbatAvisoV: ((s.vbatAvisoCentiV != null ? num(s.vbatAvisoCentiV) : 1220) / 100).toFixed(1),
    vbatCriticaV: ((s.vbatCriticaCentiV != null ? num(s.vbatCriticaCentiV) : 1180) / 100).toFixed(1),
    lat: (num(s.lat) / 1e5).toFixed(5),
    lon: (num(s.lon) / 1e5).toFixed(5),
  };
  renderStationSheet(s);
}

async function saveStationEdit() {
  const g = (id) => document.getElementById(id);
  // Contrato inteiro (o parser do firmware é int-only): volts→centiV, graus→×1e7.
  const payload = {
    node: openStationNode,
    hbMinutes: Number(g('edHb').value),
    vbatAvisoCentiV: Math.round(Number(g('edAviso').value) * 100),
    vbatCriticaCentiV: Math.round(Number(g('edCritica').value) * 100),
    latE7: Math.round(Number(g('edLat').value) * 1e7),
    lonE7: Math.round(Number(g('edLon').value) * 1e7),
  };
  const r = await postJson('/stations/config', payload);
  if (!r.ok) {
    stSheetErrors = (r.body && r.body.errors) || ['Falha ao salvar.'];
    renderStationSheet(stSheetStation);
    return;
  }
  stSheetMode = 'view';
  stSheetErrors = [];
  await openStationSheet(openStationNode); // recarrega com dado fresco (sync → pendente)
}

async function requestStationDelete() {
  const r = await postJson('/stations/delete', { node: openStationNode });
  if (!r.ok) {
    stSheetErrors = (r.body && r.body.errors) || ['Não foi possível remover.'];
    stSheetDeleteConfirm = false;
    renderStationSheet(stSheetStation);
    return;
  }
  closeStationSheet();
  if (current === 'stations') renderStations();
}

async function runStationPulse() {
  const s = stSheetStation;
  const outs = Array.isArray(s.outputs) ? s.outputs : [];
  const o = outs[stSheetPulseSel];
  if (!o) return;
  stSheetPulse = 'enviando';
  renderStationSheet(s);
  const r = await postJson('/stations/pulse', {
    node: openStationNode,
    tipo: num(o.tipo),
    index: num(o.index),
    durationS: 10,
  });
  if (!r.ok) {
    stSheetPulse = 'idle';
    renderStationSheet(s);
    return;
  }
  stSheetPulse = 'aberto';
  renderStationSheet(s);
  setTimeout(() => {
    if (openStationNode == null) return;
    stSheetPulse = 'fechado';
    renderStationSheet(stSheetStation);
    setTimeout(() => {
      if (openStationNode == null) return;
      stSheetPulse = 'idle';
      renderStationSheet(stSheetStation);
    }, 1600);
  }, 1600);
}

function wireStationSheet(s) {
  const root = document.getElementById('stSheet');
  if (!root) return;
  root.addEventListener('click', (e) => {
    if (e.target === root) closeStationSheet();
  });
  const on = (id, ev, fn) => {
    const el = document.getElementById(id);
    if (el) el.addEventListener(ev, fn);
  };
  on('stClose', 'click', closeStationSheet);
  on('stEdit', 'click', () => enterStationEdit(s));
  on('stEditCancel', 'click', () => {
    stSheetMode = 'view';
    stSheetErrors = [];
    renderStationSheet(s);
  });
  on('stEditSave', 'click', saveStationEdit);
  on('stDelReq', 'click', () => {
    stSheetDeleteConfirm = true;
    renderStationSheet(s);
  });
  on('stDelCancel', 'click', () => {
    stSheetDeleteConfirm = false;
    renderStationSheet(s);
  });
  on('stDelConfirm', 'click', requestStationDelete);
  on('stPulse', 'click', runStationPulse);
  root.querySelectorAll('.oput').forEach((el) => {
    el.addEventListener('click', () => {
      stSheetPulseSel = num(el.dataset.out);
      renderStationSheet(stSheetStation);
    });
  });
}

// ===== Zonas =====
const TIPO_LABEL = { 0: 'Válvula', 1: 'Motor/GPO' };

// Nome amigável de uma estação a partir do nó (usa /stations; fallback hex).
function stationName(stations, node) {
  const s = (Array.isArray(stations) ? stations : []).find((x) => x && num(x.node) === num(node));
  return s && s.name ? esc(s.name) : nodeHex(node);
}

async function renderZones() {
  const [zones, stations, weather, ov] = await Promise.all([
    getJson('/zones'),
    getJson('/stations').catch(() => []),
    getJson('/weather').catch(() => null),
    getJson('/overview').catch(() => ({})),
  ]);
  const rows = Array.isArray(zones) ? zones : [];
  const selfNode = num(ov && ov.selfNode) || 0;
  // Build zoneId → mensagem map from currently-triggered weather rules.
  const zoneSupMap = {};
  if (weather && Array.isArray(weather.rules)) {
    weather.rules.filter((r) => r && r.triggered).forEach((r) => {
      if (Array.isArray(r.zonaIds)) {
        r.zonaIds.forEach((zid) => {
          if (!(zid in zoneSupMap)) zoneSupMap[zid] = r.mensagem || r.nome || '';
        });
      }
    });
  }

  const cards = rows
    .map((z) => {
      z = z || {};
      const meta =
        stationName(stations, z.node) + ' · saída ' + num(z.index) + ' · ' + (TIPO_LABEL[num(z.tipo)] || '—');
      const supMsg = zoneSupMap[num(z.id)];
      const supLine = supMsg != null
        ? `<div style="font-size:12px;color:oklch(0.55 0.14 230);margin-top:8px;">Suprimida por meteorologia — ${esc(supMsg)}</div>`
        : '';
      return `<div class="card zone">
      <div class="zrow">
        <div class="zinfo">
          <div class="name">${esc(z.name)}</div>
          <div class="sub">${meta}</div>
        </div>
        <div class="zbtns">
          <button class="btn ghost sm" data-zedit="${num(z.id)}">Editar</button>
          <button class="btn outline sm" data-zopen="${num(z.id)}">Abrir</button>
          <button class="btn solid sm" data-zclose="${num(z.id)}">Fechar</button>
        </div>
      </div>
      ${supLine}</div>`;
    })
    .join('');

  view.innerHTML =
    `<button class="btn dashed" data-znew>+ Nova zona</button>` +
    (cards || '<div class="empty">Nenhuma zona configurada.</div>');

  view.querySelector('[data-znew]').addEventListener('click', () => zoneEditForm(null, rows, stations, selfNode));
  view.querySelectorAll('[data-zedit]').forEach((b) => {
    b.addEventListener('click', () => {
      const z = rows.find((x) => x && num(x.id) === num(b.dataset.zedit));
      zoneEditForm(z || null, rows, stations, selfNode);
    });
  });
  view.querySelectorAll('[data-zopen]').forEach((b) => {
    b.addEventListener('click', async () => {
      const z = rows.find((x) => x && num(x.id) === num(b.dataset.zopen));
      const durationS = Math.min(7200, Math.max(1, num(z && z.padraoMin) * 60 || 60));
      await postJson('/command', { kind: 'open', zoneId: num(b.dataset.zopen), durationS });
      renderZones().catch(() => {});
    });
  });
  view.querySelectorAll('[data-zclose]').forEach((b) => {
    b.addEventListener('click', async () => {
      await postJson('/command', { kind: 'close', zoneId: num(b.dataset.zclose) });
      renderZones().catch(() => {});
    });
  });
}

// Formulário nova/editar zona. z=null → nova. rows/stations reaproveitados da lista.
function zoneEditForm(z, rows, stations, selfNode) {
  const editing = !!z;
  // Estado local do formulário (defaults para nova zona).
  const st = {
    id: editing ? num(z.id) : 0,
    name: editing ? String(z.name || '') : '',
    node: editing ? num(z.node) : num((Array.isArray(stations) && stations[0] && stations[0].node) || 0),
    tipo: editing ? num(z.tipo) : 0,
    index: editing ? num(z.index) : 0,
    maxMin: editing ? num(z.maxMin) || 30 : 30,
    padraoMin: editing ? num(z.padraoMin) || 15 : 15,
    fonteInput: editing ? (z.fonteInput == null ? -1 : num(z.fonteInput)) : -1,
  };
  const sts = Array.isArray(stations) ? stations : [];
  // Alvo local do gateway no topo: irriga sem depender de estação (motor na placa do gateway).
  const targets = selfNode
    ? [{ node: selfNode, name: 'Gateway (local)' }, ...sts]
    : sts.slice();
  let errors = [];
  let confirmDel = false;

  function render() {
    const errBox = errors.length
      ? `<div class="card redbox">${errors.map((e) => `<div>${esc(e)}</div>`).join('')}</div>`
      : '';

    const stationChips = targets.length
      ? targets
          .map((s) => {
            s = s || {};
            const active = num(s.node) === st.node ? ' active' : '';
            const label = s.name ? esc(s.name) : nodeHex(s.node);
            return `<button class="pill${active}" data-node="${num(s.node)}">${label}</button>`;
          })
          .join('')
      : '<div class="empty">Nenhuma estação — pareie uma antes.</div>';

    let saidas = '';
    for (let i = 0; i < 8; i++) {
      saidas += `<button class="seg${i === st.index ? ' active' : ''}" data-idx="${i}">${i}</button>`;
    }

    const delBlock = editing
      ? (confirmDel
          ? `<div class="card redbox">
              <div class="qtext">Excluir esta zona?</div>
              <div class="btnrow">
                <button class="btn ghost" data-delcancel>Cancelar</button>
                <button class="btn danger" data-delconfirm>Excluir</button>
              </div>
            </div>`
          : '') + `<button class="btn dangerline" data-delreq>Excluir zona</button>`
      : '';

    view.innerHTML =
      `<div class="backlink" data-zback>‹ Zonas</div>
       <div class="ztitle">${editing ? 'Editar zona' : 'Nova zona'}</div>
       ${errBox}
       <div class="card form">
         <label class="fld">
           <span class="flbl">Nome</span>
           <input class="finput" id="z-name" maxlength="15" placeholder="Ex.: Horta" value="${esc(st.name)}">
         </label>
         <div class="frow">
           <label class="fld">
             <span class="flbl">Tempo (min)</span>
             <input class="finput" id="z-padrao" type="number" min="1" max="120" value="${st.padraoMin}">
           </label>
           <label class="fld">
             <span class="flbl">Failsafe (min)</span>
             <input class="finput" id="z-max" type="number" min="1" max="120" value="${st.maxMin}">
           </label>
         </div>
       </div>
       <div class="card form">
         <div class="flbl">Estação</div>
         <div class="pills">${stationChips}</div>
         <div class="fld">
           <span class="flbl">Tipo de saída</span>
           <div class="segrow">
             <button class="seg wide${st.tipo === 0 ? ' active' : ''}" data-tipo="0">Válvula</button>
             <button class="seg wide${st.tipo === 1 ? ' active' : ''}" data-tipo="1">Motor / GPO</button>
           </div>
         </div>
         <div class="fld">
           <span class="flbl">Saída física</span>
           <div class="segrow">${saidas}</div>
         </div>
       </div>
       <button class="btn solid big" data-zsave>Salvar zona</button>
       ${delBlock}`;

    view.querySelector('[data-zback]').addEventListener('click', () => renderZones().catch(() => {}));
    view.querySelector('#z-name').addEventListener('input', (e) => {
      st.name = e.target.value;
    });
    view.querySelector('#z-padrao').addEventListener('input', (e) => {
      st.padraoMin = num(e.target.value);
    });
    view.querySelector('#z-max').addEventListener('input', (e) => {
      st.maxMin = num(e.target.value);
    });
    view.querySelectorAll('[data-node]').forEach((b) => {
      b.addEventListener('click', () => {
        st.node = num(b.dataset.node);
        render();
      });
    });
    view.querySelectorAll('[data-tipo]').forEach((b) => {
      b.addEventListener('click', () => {
        st.tipo = num(b.dataset.tipo);
        render();
      });
    });
    view.querySelectorAll('[data-idx]').forEach((b) => {
      b.addEventListener('click', () => {
        st.index = num(b.dataset.idx);
        render();
      });
    });
    view.querySelector('[data-zsave]').addEventListener('click', save);
    const dr = view.querySelector('[data-delreq]');
    if (dr)
      dr.addEventListener('click', () => {
        confirmDel = true;
        render();
      });
    const dc = view.querySelector('[data-delcancel]');
    if (dc)
      dc.addEventListener('click', () => {
        confirmDel = false;
        render();
      });
    const dok = view.querySelector('[data-delconfirm]');
    if (dok) dok.addEventListener('click', del);
  }

  async function save() {
    const body = {
      id: st.id,
      name: st.name.trim(),
      node: st.node,
      tipo: st.tipo,
      index: st.index,
      maxMin: st.maxMin,
      padraoMin: st.padraoMin,
      fonteInput: st.fonteInput,
    };
    const r = await postJson('/zones', body);
    if (r.ok) {
      renderZones().catch(() => {});
    } else {
      errors = Array.isArray(r.body && r.body.errors) ? r.body.errors : ['Falha ao salvar.'];
      render();
    }
  }

  async function del() {
    const r = await postJson('/zones/delete', { id: st.id });
    if (r.ok) {
      renderZones().catch(() => {});
    } else {
      errors = Array.isArray(r.body && r.body.errors) ? r.body.errors : ['Falha ao excluir.'];
      confirmDel = false;
      render();
    }
  }

  render();
}

// ===== Programas =====
// Ordem dos bits em daysMask: bit0=domingo .. bit6=sábado.
const DIAS = ['dom', 'seg', 'ter', 'qua', 'qui', 'sex', 'sáb'];

// startMinute (0..1439) → "HH:MM" (ex.: 360 → "06:00").
function minToTime(m) {
  m = Math.max(0, Math.min(1439, num(m)));
  const h = Math.floor(m / 60);
  const mm = m % 60;
  return (h < 10 ? '0' + h : h) + ':' + (mm < 10 ? '0' + mm : mm);
}

// "HH:MM" → startMinute (horas*60+min); fallback 0 se inválido.
function timeToMin(v) {
  const parts = String(v || '').split(':');
  const h = num(parts[0]);
  const mm = num(parts[1]);
  return Math.max(0, Math.min(1439, h * 60 + mm));
}

// Resumo dos dias a partir do daysMask (bit0=dom..bit6=sáb).
function diasLabel(mask) {
  mask = num(mask) & 127;
  if (mask === 127) return 'Todos os dias';
  if (mask === 0) return 'Nenhum dia';
  const on = [];
  for (let b = 0; b < 7; b++) if (mask & (1 << b)) on.push(DIAS[b]);
  return on.join(', ');
}

// Resumo da sequência de etapas (usa nomes de zonas de /zones; fallback "Zona N").
function seqText(steps, zones) {
  const list = Array.isArray(steps) ? steps : [];
  if (!list.length) return 'Sem etapas';
  const zs = Array.isArray(zones) ? zones : [];
  return list
    .map((s) => {
      s = s || {};
      const z = zs.find((x) => x && num(x.id) === num(s.zoneId));
      const nome = z && z.name ? z.name : 'Zona ' + num(s.zoneId);
      return nome + ' ' + num(s.durationMin) + ' min';
    })
    .join(' → ');
}

async function renderPrograms() {
  const [programs, zones] = await Promise.all([getJson('/programs'), getJson('/zones').catch(() => [])]);
  const rows = Array.isArray(programs) ? programs : [];
  const zl = Array.isArray(zones) ? zones : [];

  const cards = rows
    .map((p) => {
      p = p || {};
      const ativo = !!p.enabled;
      const tglCls = ativo ? 'chip green' : 'chip';
      const tglTxt = ativo ? 'Ativo' : 'Pausado';
      return `<div class="card program">
      <div class="pinfo" data-pedit="${num(p.id)}">
        <div class="name">${esc(diasLabel(p.daysMask))} · ${esc(minToTime(p.startMinute))}</div>
        <div class="sub">${esc(seqText(p.steps, zl))}</div>
      </div>
      <button class="ptoggle ${tglCls}" data-ptoggle="${num(p.id)}" data-enabled="${ativo ? 1 : 0}">${tglTxt}</button>
    </div>`;
    })
    .join('');

  view.innerHTML =
    `<button class="btn dashed" data-pnew>+ Novo programa</button>` +
    (cards || '<div class="empty">Nenhum programa configurado.</div>');

  view.querySelector('[data-pnew]').addEventListener('click', () => programEditForm(null, zl));
  view.querySelectorAll('[data-pedit]').forEach((b) => {
    b.addEventListener('click', () => {
      const p = rows.find((x) => x && num(x.id) === num(b.dataset.pedit));
      programEditForm(p || null, zl);
    });
  });
  view.querySelectorAll('[data-ptoggle]').forEach((b) => {
    b.addEventListener('click', async () => {
      await postJson('/programs/toggle', { id: num(b.dataset.ptoggle), enabled: b.dataset.enabled !== '1' });
      renderPrograms().catch(() => {});
    });
  });
}

// Formulário novo/editar programa. p=null → novo. zones reaproveitadas da lista.
function programEditForm(p, zones) {
  const editing = !!p;
  const zs = Array.isArray(zones) ? zones : [];
  const firstZone = num((zs[0] && zs[0].id) || 1) || 1;
  const st = {
    id: editing ? num(p.id) : 0,
    enabled: editing ? !!p.enabled : true,
    daysMask: editing ? num(p.daysMask) & 127 : 127,
    startMinute: editing ? num(p.startMinute) : 360,
    steps:
      editing && Array.isArray(p.steps) && p.steps.length
        ? p.steps.map((s) => ({ zoneId: num((s || {}).zoneId) || firstZone, durationMin: num((s || {}).durationMin) || 15 }))
        : [{ zoneId: firstZone, durationMin: 15 }],
  };
  let errors = [];
  let confirmDel = false;

  function render() {
    const errBox = errors.length
      ? `<div class="card redbox">${errors.map((e) => `<div>${esc(e)}</div>`).join('')}</div>`
      : '';

    const diaChips = DIAS.map((d, b) => {
      const active = st.daysMask & (1 << b) ? ' active' : '';
      return `<button class="pill${active}" data-dia="${b}">${d}</button>`;
    }).join('');

    const totalMin = st.steps.reduce((a, s) => a + num(s.durationMin), 0);

    const passos = st.steps
      .map((s, i) => {
        const zoneChips = zs.length
          ? zs
              .map((z) => {
                z = z || {};
                const active = num(z.id) === num(s.zoneId) ? ' active' : '';
                return `<button class="pill${active}" data-step="${i}" data-zone="${num(z.id)}">${esc(z.name)}</button>`;
              })
              .join('')
          : '<div class="empty">Nenhuma zona — configure uma antes.</div>';
        const rem =
          st.steps.length > 1
            ? `<span class="steprm" data-steprm="${i}">Remover</span>`
            : '';
        return `<div class="card step">
          <div class="stephdr"><span class="flbl">Etapa ${i + 1}</span>${rem}</div>
          <div class="pills">${zoneChips}</div>
          <div class="stepdur">
            <span class="durlbl">Duração (min)</span>
            <input class="finput dur" type="number" min="1" max="120" data-dur="${i}" value="${num(s.durationMin)}">
          </div>
        </div>`;
      })
      .join('');

    const delBlock = editing
      ? (confirmDel
          ? `<div class="card redbox">
              <div class="qtext">Excluir este programa?</div>
              <div class="btnrow">
                <button class="btn ghost" data-delcancel>Cancelar</button>
                <button class="btn danger" data-delconfirm>Excluir</button>
              </div>
            </div>`
          : '') + `<button class="btn dangerline" data-delreq>Excluir programa</button>`
      : '';

    view.innerHTML =
      `<div class="backlink" data-pback>‹ Programas</div>
       <div class="ztitle">${editing ? 'Editar programa' : 'Novo programa'}</div>
       ${errBox}
       <div class="card form">
         <div class="fld">
           <span class="flbl">Dias</span>
           <div class="pills">${diaChips}</div>
         </div>
         <div class="fld">
           <span class="flbl">Horário de início</span>
           <input class="finput time" id="p-time" type="time" value="${esc(minToTime(st.startMinute))}">
         </div>
         <div class="toggle-row" data-pactive>
           <span class="tlbl">Programa ativo</span>
           <span class="switch${st.enabled ? ' on' : ''}"><span class="knob"></span></span>
         </div>
       </div>
       <div class="fld">
         <span class="flbl">Sequência · ${totalMin} min total</span>
         <div class="steps">${passos}</div>
         <button class="btn stepadd" data-stepadd ${st.steps.length >= 8 ? 'disabled' : ''}>+ Adicionar etapa</button>
       </div>
       <button class="btn solid big" data-psave>Salvar programa</button>
       ${delBlock}`;

    view.querySelector('[data-pback]').addEventListener('click', () => renderPrograms().catch(() => {}));
    view.querySelector('#p-time').addEventListener('input', (e) => {
      st.startMinute = timeToMin(e.target.value);
    });
    view.querySelector('[data-pactive]').addEventListener('click', () => {
      st.enabled = !st.enabled;
      render();
    });
    view.querySelectorAll('[data-dia]').forEach((b) => {
      b.addEventListener('click', () => {
        st.daysMask ^= 1 << num(b.dataset.dia);
        render();
      });
    });
    view.querySelectorAll('[data-zone]').forEach((b) => {
      b.addEventListener('click', () => {
        st.steps[num(b.dataset.step)].zoneId = num(b.dataset.zone);
        render();
      });
    });
    view.querySelectorAll('[data-dur]').forEach((inp) => {
      inp.addEventListener('input', (e) => {
        st.steps[num(inp.dataset.dur)].durationMin = num(e.target.value);
      });
    });
    view.querySelectorAll('[data-steprm]').forEach((b) => {
      b.addEventListener('click', () => {
        if (st.steps.length > 1) {
          st.steps.splice(num(b.dataset.steprm), 1);
          render();
        }
      });
    });
    const add = view.querySelector('[data-stepadd]');
    add.addEventListener('click', () => {
      if (st.steps.length < 8) {
        st.steps.push({ zoneId: firstZone, durationMin: 15 });
        render();
      }
    });
    view.querySelector('[data-psave]').addEventListener('click', save);
    const dr = view.querySelector('[data-delreq]');
    if (dr)
      dr.addEventListener('click', () => {
        confirmDel = true;
        render();
      });
    const dc = view.querySelector('[data-delcancel]');
    if (dc)
      dc.addEventListener('click', () => {
        confirmDel = false;
        render();
      });
    const dok = view.querySelector('[data-delconfirm]');
    if (dok) dok.addEventListener('click', del);
  }

  // Validação espelhada do firmware (Task 7): etapas 1..8, zoneId 1..255,
  // durationMin 1..120, daysMask 0..127, startMinute 0..1439.
  function validate() {
    const errs = [];
    const mask = st.daysMask & 127;
    if (mask < 0 || mask > 127) errs.push('Dias inválidos.');
    if (st.startMinute < 0 || st.startMinute > 1439) errs.push('Horário inválido.');
    if (st.steps.length < 1 || st.steps.length > 8) errs.push('Use de 1 a 8 etapas.');
    st.steps.forEach((s, i) => {
      const zid = num(s.zoneId);
      if (zid < 1 || zid > 255) errs.push('Etapa ' + (i + 1) + ': selecione uma zona.');
      const d = num(s.durationMin);
      if (d < 1 || d > 120) errs.push('Etapa ' + (i + 1) + ': duração deve ser 1..120 min.');
    });
    return errs;
  }

  async function save() {
    errors = validate();
    if (errors.length) {
      render();
      return;
    }
    const body = {
      id: st.id,
      enabled: st.enabled,
      daysMask: st.daysMask & 127,
      startMinute: st.startMinute,
      steps: st.steps.map((s) => ({ zoneId: num(s.zoneId), durationMin: num(s.durationMin) })),
    };
    const r = await postJson('/programs', body);
    if (r.ok) {
      renderPrograms().catch(() => {});
    } else {
      errors = Array.isArray(r.body && r.body.errors) ? r.body.errors : ['Falha ao salvar.'];
      render();
    }
  }

  async function del() {
    const r = await postJson('/programs/delete', { id: st.id });
    if (r.ok) {
      renderPrograms().catch(() => {});
    } else {
      errors = Array.isArray(r.body && r.body.errors) ? r.body.errors : ['Falha ao excluir.'];
      confirmDel = false;
      render();
    }
  }

  render();
}

// ===== Labels de auditoria — contrato 3-vias com AuditLog.h (append-only; não reordenar) =====
// AuditOrigin: SISTEMA=0,CRONOGRAMA=1,PAINEL=2,PORTAL_CAMPO=3,BOTAO_FISICO=4,ENTRADA_FISICA=5,INTERTRAVAMENTO=6,FAILSAFE_TIMER=7,SERVICO=8,GRUPO_HIDRAULICO=9,NIVEL=10
const ORIGENS_LABEL = ['Sistema','Cronograma','Painel','Portal campo','Botão físico','Entrada física','Intertravamento','Failsafe timer','Serviço','Grupo hidráulico','Nível'];
// AuditAction: ABRIR=0,FECHAR=1,PULSO=2,GPO_ON=3,GPO_OFF=4,PAREAR=5,FACTORY_RESET=6,CONFIG_EPOCH=7,SAFE_MODE_IN=8,SAFE_MODE_OUT=9,TAMPER=10,REBOOT=11,HIBERNA_IN=12,HIBERNA_OUT=13,CMD_REJEITADO=14,ESPELHO=15
const ACOES_LABEL = ['Abrir','Fechar','Pulso','GPO ligar','GPO desligar','Parear','Factory reset','Config epoch','Safe mode in','Safe mode out','Tamper','Reboot','Hibernar in','Hibernar out','Cmd rejeitado','Espelho'];
// AuditResult: OK=0,NACK=1,TIMEOUT=2
const RESULTADOS_LABEL = ['OK','NACK','Timeout'];

// Labels para InterlockRule
const INTERLOCK_TIPO_LABEL = ['Sensor','Simultaneidade'];
const INTERLOCK_COND_LABEL = ['Ativo','Inativo','Menor que','Maior que'];
const INTERLOCK_ACAO_LABEL = ['Bloquear abertura','Fechar e bloquear'];

// Formata época Unix (segundos) como data/hora local.
function fmtEpoch(ts) {
  ts = num(ts);
  if (!ts) return '—';
  try {
    return new Date(ts * 1000).toLocaleString('pt-BR');
  } catch (_) {
    return String(ts);
  }
}

// Formata centi-unidades: 1234 → "12.34"; digital (tipo 0): 0/1 → "inativo"/"ativo".
function fmtSensorVal(tipo, valor) {
  if (num(tipo) === 0) return num(valor) ? 'ativo' : 'inativo';
  return (num(valor) / 100).toFixed(2);
}

// ===== Sensores =====
async function renderSensores() {
  // Pula o refresh automático (3s) enquanto o usuário edita um nome — re-renderizar
  // apagaria o texto ainda não salvo. O initial render nunca tem input focado.
  const ae = document.activeElement;
  if (ae && ae.classList && ae.classList.contains('sn-input')) return;

  const list = (await getJson('/sensors')) || [];
  const rows = Array.isArray(list) ? list : [];

  if (!rows.length) {
    view.innerHTML = '<div class="empty">Nenhuma estação com sensores configurados.</div>';
    return;
  }

  const cards = rows.map((st) => {
    st = st || {};
    const nome = st.nome ? esc(st.nome) : nodeHex(st.node);
    const tamper = st.tamper ? `<span class="chip red">tamper</span>` : '';
    const sensores = Array.isArray(st.sensores) ? st.sensores : [];
    const sRows = sensores.map((s) => {
      s = s || {};
      const tipoStr = num(s.tipo) === 0 ? 'Digital' : 'Analógico';
      const valStr = esc(fmtSensorVal(s.tipo, s.valor));
      const nomeAtual = esc(s.nome || '');
      return `<tr>
        <td>${num(s.idx)}</td>
        <td><input class="finput sn-input" data-node="${num(st.node)}" data-sensor="${num(s.idx)}" maxlength="15" placeholder="(sem nome)" value="${nomeAtual}"></td>
        <td>${tipoStr}</td>
        <td class="mono">${valStr}</td>
      </tr>`;
    }).join('');

    return `<div class="card">
      <div class="sens-hdr">
        <span class="name">${nome}</span>
        <span class="sub">${nodeHex(st.node)}</span>
        ${tamper}
      </div>
      <table class="sens-table">
        <thead><tr><th>#</th><th>Nome</th><th>Tipo</th><th>Valor</th></tr></thead>
        <tbody>${sRows || '<tr><td colspan="4" class="empty">Sem sensores</td></tr>'}</tbody>
      </table>
    </div>`;
  }).join('');

  view.innerHTML = cards;

  // Salva nome ao desfocar o input.
  view.querySelectorAll('.sn-input').forEach((inp) => {
    inp.addEventListener('change', async () => {
      const node = num(inp.dataset.node);
      const sensor = num(inp.dataset.sensor);
      const nome = inp.value.trim();
      const r = await postJson('/sensors/name', { node, sensor, nome });
      if (!r.ok) {
        alert('Falha ao salvar nome: ' + (Array.isArray(r.body && r.body.errors) ? r.body.errors.join(', ') : 'erro'));
      }
    });
  });
}

// ===== GPO =====
async function renderGpo() {
  const [zones, stations] = await Promise.all([getJson('/zones'), getJson('/stations').catch(() => [])]);
  const rows = Array.isArray(zones) ? zones : [];
  const gpos = rows.filter((z) => z && num(z.tipo) === 1);

  if (!gpos.length) {
    view.innerHTML = '<div class="empty">Nenhuma zona do tipo GPO configurada.</div>';
    return;
  }

  const cards = gpos.map((z) => {
    z = z || {};
    const meta = stationName(stations, z.node) + ' · saída ' + num(z.index);
    const bistable = !num(z.padraoMin); // durationMin 0 = biestável
    const tag = bistable ? '<span class="chip amber">biestável</span>' : '<span class="chip green">temporizado</span>';
    return `<div class="card zone">
      <div class="zrow">
        <div class="zinfo">
          <div class="name">${esc(z.name)}</div>
          <div class="sub">${meta}</div>
          <div style="margin-top:6px">${tag}</div>
        </div>
        <div class="zbtns">
          <button class="btn outline sm" data-gpoopen="${num(z.id)}" data-bistable="${bistable ? 1 : 0}">Ligar</button>
          <button class="btn solid sm" data-gpoclose="${num(z.id)}">Desligar</button>
        </div>
      </div>
    </div>`;
  }).join('');

  view.innerHTML = cards;

  view.querySelectorAll('[data-gpoopen]').forEach((b) => {
    b.addEventListener('click', async () => {
      const bistable = b.dataset.bistable === '1';
      if (bistable && !confirm('Esta GPO é biestável e permanecerá ligada até ser desligada manualmente. Confirma?')) return;
      const z = gpos.find((x) => x && num(x.id) === num(b.dataset.gpoopen));
      const durationS = bistable ? 0 : Math.min(7200, Math.max(1, num(z && z.padraoMin) * 60 || 60));
      const r = await postJson('/command', { kind: 'open', zoneId: num(b.dataset.gpoopen), durationS });
      if (!r.ok) alert('Falha: ' + (Array.isArray(r.body && r.body.errors) ? r.body.errors.join(', ') : 'erro'));
      renderGpo().catch(() => {});
    });
  });

  view.querySelectorAll('[data-gpoclose]').forEach((b) => {
    b.addEventListener('click', async () => {
      const r = await postJson('/command', { kind: 'close', zoneId: num(b.dataset.gpoclose) });
      if (!r.ok) alert('Falha: ' + (Array.isArray(r.body && r.body.errors) ? r.body.errors.join(', ') : 'erro'));
      renderGpo().catch(() => {});
    });
  });
}

// ===== Intertravamentos =====
// A lista de intertravamentos é renderizada em conjunto com os grupos hidráulicos
// na tela combinada `renderGruposInterlocks` (modelo do mockup "Grupos & Intertravamentos").
// Aqui ficam só o formulário de edição e o helper de id.

// Menor id livre em 1..255 (backend exige id != 0; regras novas não têm id).
function nextInterlockId(allRules) {
  const used = new Set((Array.isArray(allRules) ? allRules : []).map((r) => num(r && r.id)));
  for (let i = 1; i <= 255; i++) if (!used.has(i)) return i;
  return 0; // tabela cheia
}

// Sensores (com nome) de uma estação a partir do endpoint /sensors.
function stationSensors(sensors, node) {
  const s = (Array.isArray(sensors) ? sensors : []).find((x) => x && num(x.node) === num(node));
  return s && Array.isArray(s.sensores) ? s.sensores : [];
}

// Formulário de intertravamento — layout do mockup "Grupos & Intertravamentos"
// (classes lvl-*). Seleção por pills/botões (estação, sensor, condição, ação,
// zonas) em vez de campos numéricos; nomes em vez de índices onde possível.
function interlockForm(rule, stations, zones, sensors, allRules) {
  const editing = !!rule;
  const sts = Array.isArray(stations) ? stations : [];
  const zs = Array.isArray(zones) ? zones : [];
  const sens = Array.isArray(sensors) ? sensors : [];
  const st = {
    id: editing ? num(rule.id) : nextInterlockId(allRules),
    tipo: editing ? num(rule.tipo) : 0,
    node: editing ? num(rule.node) : num((sts[0] && sts[0].node) || 0),
    sensor: editing ? num(rule.sensor) : 0,
    condicao: editing ? num(rule.condicao) : 0,
    valor: editing ? num(rule.valor) : 0,
    histerese: editing ? num(rule.histerese) : 0,
    acao: editing ? num(rule.acao) : 0,
    zonas: editing && Array.isArray(rule.zonas) ? rule.zonas.slice() : [],
    todas: editing ? !!rule.todas : false,
    mensagem: editing ? String(rule.mensagem || '') : '',
    maxAbertas: editing ? num(rule.maxAbertas) : 1,
  };
  let errors = [];
  let deleteConfirm = false;

  function render() {
    const errBox = errors.length
      ? `<div class="lvl-errbox">${errors.map((e) => `<div>${esc(e)}</div>`).join('')}</div>`
      : '';

    const tipoBtns = [0, 1].map((t) =>
      `<button class="lvl-polbtn${st.tipo === t ? ' sel' : ''}" data-itipo="${t}">${INTERLOCK_TIPO_LABEL[t]}</button>`
    ).join('');

    let body = '';
    if (st.tipo === 0) {
      const stationPills = sts.length
        ? sts.map((s) => `<button class="lvl-pill${num(s.node) === num(st.node) ? ' sel' : ''}" data-inode="${num(s.node)}">${s.name ? esc(s.name) : nodeHex(s.node)}</button>`).join('')
        : '<div class="lvl-muted">Nenhuma estação.</div>';
      const list = stationSensors(sens, st.node);
      const sensorPills = list.length
        ? list.map((se) => `<button class="lvl-pill${num(se.idx) === num(st.sensor) ? ' sel' : ''}" data-isensor="${num(se.idx)}">${se.nome ? esc(se.nome) : ('sensor ' + num(se.idx))}</button>`).join('')
        : '<div class="lvl-muted">Estação sem sensores nomeados — usando índice ' + num(st.sensor) + '.</div>';
      const condBtns = [0, 1, 2, 3].map((c) =>
        `<button class="lvl-polbtn${st.condicao === c ? ' sel' : ''}" data-icond="${c}">${INTERLOCK_COND_LABEL[c]}</button>`
      ).join('');
      const valBlock = st.condicao >= 2 ? `
        <div class="lvl-advrow">
          <div class="lvl-advfld"><div class="lvl-advlbl">Valor (centi-unid.)</div><input class="lvl-input" id="itl-valor" type="number" value="${st.valor}"></div>
          <div class="lvl-advfld"><div class="lvl-advlbl">Histerese (centi-unid.)</div><input class="lvl-input" id="itl-histerese" type="number" value="${st.histerese}"></div>
        </div>` : '';
      const acaoBtns = [0, 1].map((a) =>
        `<button class="lvl-polbtn${st.acao === a ? ' sel' : ''}" data-iacao="${a}">${INTERLOCK_ACAO_LABEL[a]}</button>`
      ).join('');
      body = `
        <div><div class="lvl-seclbl">Estação</div><div class="lvl-pills">${stationPills}</div></div>
        <div><div class="lvl-seclbl">Sensor</div><div class="lvl-pills">${sensorPills}</div></div>
        <div><div class="lvl-seclbl">Condição</div><div class="lvl-pol2" style="flex-wrap:wrap;">${condBtns}</div></div>
        ${valBlock}
        <div><div class="lvl-seclbl">Ação</div><div class="lvl-pol2">${acaoBtns}</div></div>
        <div><div class="lvl-seclbl">Mensagem de alerta</div><input class="lvl-input" id="itl-mensagem" maxlength="40" placeholder="Ex.: Reservatório em nível baixo" value="${esc(st.mensagem)}"></div>`;
    } else {
      body = `<div><div class="lvl-seclbl">Máx. saídas abertas simultaneamente</div><input class="lvl-input" id="itl-maxAbertas" type="number" min="1" max="255" value="${st.maxAbertas}"></div>`;
    }

    const zonePills = zs.length
      ? zs.map((z) => `<button class="lvl-pill${st.zonas.includes(num(z.id)) ? ' sel' : ''}" data-izone="${num(z.id)}">${z.name ? esc(z.name) : ('zona ' + num(z.id))}</button>`).join('')
      : '<div class="lvl-muted">Nenhuma zona.</div>';

    const delBlock = editing
      ? (deleteConfirm
          ? `<div class="lvl-delbox">Excluir este intertravamento?<div class="lvl-delbtns"><button class="lvl-delcancel" data-itldelcancel>Cancelar</button><button class="lvl-delconfirm" data-itldelconfirm>Excluir</button></div></div>`
          : `<button class="lvl-delbtn" data-itldel>Excluir intertravamento</button>`)
      : '';

    view.innerHTML = `
      <div class="lvl-wrap">
        <div class="lvl-back" data-itlback>‹ Grupos &amp; Intertravamentos</div>
        <div class="lvl-title">${editing ? 'Editar intertravamento' : 'Novo intertravamento'}</div>
        ${errBox}
        <div class="lvl-panel">
          <div><div class="lvl-seclbl">Tipo de regra</div><div class="lvl-pol2">${tipoBtns}</div></div>
          ${body}
          <div><div class="lvl-seclbl">Zonas afetadas ${st.tipo === 1 ? '(vazio = todas)' : ''}</div><div class="lvl-pills">${zonePills}</div></div>
          <div class="toggle-row" data-itltodas>
            <span class="tlbl">Todas as zonas</span>
            <span class="switch${st.todas ? ' on' : ''}"><span class="knob"></span></span>
          </div>
        </div>
        <button class="lvl-save" data-itlsave>Salvar intertravamento</button>
        ${delBlock}
      </div>`;

    view.querySelector('[data-itlback]').addEventListener('click', () => renderGruposInterlocks().catch(() => {}));
    view.querySelectorAll('[data-itipo]').forEach((b) => b.addEventListener('click', () => { st.tipo = num(b.dataset.itipo); render(); }));
    view.querySelectorAll('[data-inode]').forEach((b) => b.addEventListener('click', () => { st.node = num(b.dataset.inode); st.sensor = 0; render(); }));
    view.querySelectorAll('[data-isensor]').forEach((b) => b.addEventListener('click', () => { st.sensor = num(b.dataset.isensor); render(); }));
    view.querySelectorAll('[data-icond]').forEach((b) => b.addEventListener('click', () => { st.condicao = num(b.dataset.icond); render(); }));
    view.querySelectorAll('[data-iacao]').forEach((b) => b.addEventListener('click', () => { st.acao = num(b.dataset.iacao); render(); }));
    view.querySelectorAll('[data-izone]').forEach((b) => b.addEventListener('click', () => {
      const zid = num(b.dataset.izone);
      const i = st.zonas.indexOf(zid);
      if (i >= 0) st.zonas.splice(i, 1); else st.zonas.push(zid);
      render();
    }));
    const todaBtn = view.querySelector('[data-itltodas]');
    if (todaBtn) todaBtn.addEventListener('click', () => { st.todas = !st.todas; render(); });

    const valInp = view.querySelector('#itl-valor');
    if (valInp) valInp.addEventListener('input', (e) => { st.valor = num(e.target.value); });
    const histInp = view.querySelector('#itl-histerese');
    if (histInp) histInp.addEventListener('input', (e) => { st.histerese = num(e.target.value); });
    const maxInp = view.querySelector('#itl-maxAbertas');
    if (maxInp) maxInp.addEventListener('input', (e) => { st.maxAbertas = num(e.target.value); });
    const msgInp = view.querySelector('#itl-mensagem');
    if (msgInp) msgInp.addEventListener('input', (e) => { st.mensagem = e.target.value; });

    view.querySelector('[data-itlsave]').addEventListener('click', save);
    const delBtn = view.querySelector('[data-itldel]');
    if (delBtn) delBtn.addEventListener('click', () => { deleteConfirm = true; render(); });
    const delCancel = view.querySelector('[data-itldelcancel]');
    if (delCancel) delCancel.addEventListener('click', () => { deleteConfirm = false; render(); });
    const delConfirm = view.querySelector('[data-itldelconfirm]');
    if (delConfirm) delConfirm.addEventListener('click', async () => {
      const r = await postJson('/interlocks/delete', { id: st.id });
      if (r.ok) renderGruposInterlocks().catch(() => {});
      else alert('Falha ao excluir: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
    });
  }

  async function save() {
    if (!st.id) {
      errors = ['Tabela de intertravamentos cheia (máx. 16 regras).'];
      render();
      return;
    }
    const body = {
      id: st.id,
      tipo: st.tipo,
      node: st.node,
      sensor: st.sensor,
      condicao: st.condicao,
      valor: st.valor,
      histerese: st.histerese,
      acao: st.acao,
      zonas: st.zonas,
      todas: st.todas,
      mensagem: st.mensagem.trim(),
      maxAbertas: st.maxAbertas,
    };
    const r = await postJson('/interlocks', body);
    if (r.ok) {
      renderGruposInterlocks().catch(() => {});
    } else {
      errors = Array.isArray(r.body && r.body.errors) ? r.body.errors : ['Falha ao salvar.'];
      render();
    }
  }

  render();
}

// ===== Log de auditoria =====
async function renderAuditLog() {
  const records = (await getJson('/audit?fmt=json&n=500')) || [];
  const rows = Array.isArray(records) ? records : [];

  const filterRow = `
    <div class="card form log-filters">
      <div class="frow">
        <label class="fld">
          <span class="flbl">Filtrar origem</span>
          <select class="finput" id="log-orig">
            <option value="">Todas</option>
            ${ORIGENS_LABEL.map((l, i) => `<option value="${i}">${esc(l)}</option>`).join('')}
          </select>
        </label>
        <label class="fld">
          <span class="flbl">Filtrar ação</span>
          <select class="finput" id="log-acao">
            <option value="">Todas</option>
            ${ACOES_LABEL.map((l, i) => `<option value="${i}">${esc(l)}</option>`).join('')}
          </select>
        </label>
      </div>
      <label class="fld">
        <span class="flbl">Filtrar estação (node hex, ex.: 0x1a2b3c4d)</span>
        <input class="finput" id="log-node" placeholder="0x...">
      </label>
    </div>
    <div class="log-export-row">
      <a class="btn ghost sm" href="${API}/audit?fmt=csv&n=500" download="audit.csv">Exportar CSV</a>
      <a class="btn ghost sm" href="${API}/audit?fmt=json&n=500" download="audit.json">Exportar JSON</a>
    </div>`;

  const tableWrap = `<div class="log-table-wrap"><table class="log-table">
    <thead><tr>
      <th>Data/hora</th><th>Origem</th><th>Ação</th><th>Alvo</th><th>Res.</th><th>Estação</th><th>Seq</th>
    </tr></thead>
    <tbody id="log-tbody">
      ${rows.length ? renderLogRows(rows) : '<tr><td colspan="7" class="empty">Log vazio.</td></tr>'}
    </tbody>
  </table></div>`;

  view.innerHTML = filterRow + tableWrap;

  function applyFilter() {
    const origVal = view.querySelector('#log-orig').value;
    const acaoVal = view.querySelector('#log-acao').value;
    const nodeVal = (view.querySelector('#log-node').value || '').trim().toLowerCase();
    const filtered = rows.filter((r) => {
      if (origVal !== '' && num(r.origem) !== num(origVal)) return false;
      if (acaoVal !== '' && num(r.acao) !== num(acaoVal)) return false;
      if (nodeVal) {
        const rHex = nodeHex(r.node).toLowerCase();
        const rDec = String(num(r.node));
        if (!rHex.includes(nodeVal) && !rDec.includes(nodeVal)) return false;
      }
      return true;
    });
    const tbody = view.querySelector('#log-tbody');
    if (tbody) tbody.innerHTML = filtered.length ? renderLogRows(filtered) : '<tr><td colspan="7" class="empty">Sem resultados.</td></tr>';
  }

  view.querySelector('#log-orig').addEventListener('change', applyFilter);
  view.querySelector('#log-acao').addEventListener('change', applyFilter);
  view.querySelector('#log-node').addEventListener('input', applyFilter);
}

function renderLogRows(rows) {
  return rows.map((r) => {
    r = r || {};
    const origem = ORIGENS_LABEL[num(r.origem)] || ('orig ' + num(r.origem));
    const acao = ACOES_LABEL[num(r.acao)] || ('acao ' + num(r.acao));
    const resultado = RESULTADOS_LABEL[num(r.resultado)] || ('res ' + num(r.resultado));
    return `<tr>
      <td class="log-ts">${esc(fmtEpoch(r.ts))}</td>
      <td>${esc(origem)}</td>
      <td>${esc(acao)}</td>
      <td class="mono">${esc(String(r.alvo == null ? '—' : r.alvo))}</td>
      <td>${esc(resultado)}</td>
      <td class="mono">${esc(nodeHex(r.node))}</td>
      <td class="mono">${esc(String(r.seq == null ? '—' : r.seq))}</td>
    </tr>`;
  }).join('');
}

// ===== Tamper / Manutenção =====
async function renderTamper() {
  const list = (await getJson('/sensors')) || [];
  const rows = Array.isArray(list) ? list : [];

  if (!rows.length) {
    view.innerHTML = '<div class="empty">Nenhuma estação registrada.</div>';
    return;
  }

  const cards = rows.map((st) => {
    st = st || {};
    const nome = st.nome ? esc(st.nome) : nodeHex(st.node);
    const tamperBadge = st.tamper
      ? `<span class="chip red">tamper ativo</span>`
      : `<span class="chip green">normal</span>`;
    return `<div class="card">
      <div class="sens-hdr">
        <span class="name">${nome}</span>
        ${tamperBadge}
      </div>
      <div class="sub maint-sub">${nodeHex(st.node)}</div>
      <div class="maint-row">
        <label class="fld maint-fld">
          <span class="flbl">Janela (min)</span>
          <input class="finput maint-min" type="number" min="1" max="60" value="10" data-node="${num(st.node)}">
        </label>
        <button class="btn outline sm maint-open" data-node="${num(st.node)}">Abrir janela</button>
        <button class="btn ghost sm maint-close" data-node="${num(st.node)}">Fechar agora</button>
      </div>
    </div>`;
  }).join('');

  view.innerHTML = cards;

  view.querySelectorAll('.maint-open').forEach((b) => {
    b.addEventListener('click', async () => {
      const node = num(b.dataset.node);
      const minInp = view.querySelector(`.maint-min[data-node="${node}"]`);
      const minutes = Math.max(1, Math.min(60, num(minInp && minInp.value)));
      const r = await postJson('/maint', { node, minutes });
      if (r.ok) {
        alert('Janela de manutenção aberta por ' + minutes + ' min.');
      } else {
        alert('Falha: ' + (Array.isArray(r.body && r.body.errors) ? r.body.errors.join(', ') : 'erro'));
      }
    });
  });

  view.querySelectorAll('.maint-close').forEach((b) => {
    b.addEventListener('click', async () => {
      const node = num(b.dataset.node);
      const r = await postJson('/maint', { node, minutes: 0 });
      if (r.ok) {
        alert('Janela de manutenção fechada.');
      } else {
        alert('Falha: ' + (Array.isArray(r.body && r.body.errors) ? r.body.errors.join(', ') : 'erro'));
      }
    });
  });
}

// ===== Grupos hidráulicos & Intertravamentos (tela combinada — modelo do mockup) =====
const GROUP_STATE_CLASS = {
  ocioso: 'gray', abrindo: 'amber', aguardando_partida: 'amber', partindo_bomba: 'amber',
  rodando: 'green', transicao: 'amber', parando_bomba: 'amber', drenando: 'amber',
  fechando: 'amber', adiado: 'red', desconhecido: 'gray',
};
const GROUP_STATE_LABEL = {
  ocioso: 'Ocioso', abrindo: 'Abrindo válvulas', aguardando_partida: 'Aguardando partida',
  partindo_bomba: 'Partindo bomba', rodando: 'Em irrigação', transicao: 'Em transição',
  parando_bomba: 'Parando bomba', drenando: 'Drenando', fechando: 'Fechando válvulas',
  adiado: 'Adiado', desconhecido: '—',
};

function groupStatusById(list, id) {
  return (Array.isArray(list) ? list : []).find((s) => s && num(s.id) === num(id)) || {};
}

// Descrição curta de uma regra de intertravamento (uma linha). Nomes de estação/
// sensor quando disponíveis; cai para índice numérico caso contrário. Retorna HTML
// já escapado (mesma convenção de stationName/zoneName) — embutir sem re-escapar.
function interlockDesc(r, sts, sens) {
  if (num(r.tipo) !== 0) return `Máx. ${num(r.maxAbertas)} zonas abertas simultaneamente`;
  const est = stationName(sts, r.node);
  const l = stationSensors(sens, r.node);
  const x = l.find((se) => se && num(se.idx) === num(r.sensor));
  const sen = x && x.nome ? esc(x.nome) : ('sensor ' + num(r.sensor));
  const cond = esc(INTERLOCK_COND_LABEL[num(r.condicao)] || ('cond ' + num(r.condicao)));
  let d = `${est} · ${sen} · ${cond}`;
  if (num(r.condicao) >= 2) d += ` ${(num(r.valor) / 100).toFixed(2)} (hist. ${(num(r.histerese) / 100).toFixed(2)})`;
  return d;
}

async function renderGruposInterlocks() {
  const [groups, status, zones, weather, interlocks, stations, sensors] = await Promise.all([
    getJson('/groups'),
    getJson('/groups/status').catch(() => []),
    getJson('/zones').catch(() => []),
    getJson('/weather').catch(() => null),
    getJson('/interlocks').catch(() => []),
    getJson('/stations').catch(() => []),
    getJson('/sensors').catch(() => []),
  ]);
  const grows = Array.isArray(groups) ? groups : [];
  const gstat = Array.isArray(status) ? status : [];
  const zs = Array.isArray(zones) ? zones : [];
  const irows = Array.isArray(interlocks) ? interlocks : [];
  const sts = Array.isArray(stations) ? stations : [];
  const sens = Array.isArray(sensors) ? sensors : [];

  // groupId → mensagem das regras de meteorologia atualmente disparadas.
  const groupSupMap = {};
  if (weather && Array.isArray(weather.rules)) {
    weather.rules.filter((r) => r && r.triggered).forEach((r) => {
      if (Array.isArray(r.grupoIds)) {
        r.grupoIds.forEach((gid) => {
          if (!(gid in groupSupMap)) groupSupMap[gid] = r.mensagem || r.nome || '';
        });
      }
    });
  }

  const groupCards = grows.map((g) => {
    g = g || {};
    const s = groupStatusById(gstat, g.id);
    const estado = s.estado || 'ocioso';
    const cls = GROUP_STATE_CLASS[estado] || 'gray';
    const estLabel = GROUP_STATE_LABEL[estado] || estado;
    const bombaNome = num(g.bombaZoneId) ? zoneName(zs, g.bombaZoneId) : '—';
    const zonasNomes = (Array.isArray(g.zonas) ? g.zonas : []).map((id) => zoneName(zs, id)).join(', ') || '—';
    const maxTxt = num(g.maxOpen) ? num(g.maxOpen) : 'sem teto';
    const supMsg = groupSupMap[num(g.id)];
    const supLine = supMsg != null ? `<div class="gi-supress">Suprimido por meteorologia — ${esc(supMsg)}</div>` : '';
    return `<div class="gi-card" data-gedit="${num(g.id)}">
      <div class="gi-cardhdr">
        <div class="gi-name">${esc(g.nome || ('Grupo ' + num(g.id)))}</div>
        <span class="gi-editpill">Editar</span>
      </div>
      <div class="gi-line">Bomba: ${bombaNome} · Zonas: ${zonasNomes}</div>
      <div class="gi-estado ${cls}" data-gstate="${num(g.id)}">${esc(estLabel)}</div>
      <div class="gi-line gi-cfg">Sobreposição ${num(g.overlapS)}s · partida ${num(g.startAfterOpenS)}s após abrir · parada ${num(g.stopBeforeCloseS)}s antes de fechar<br>Mín. ${num(g.minOpen)} / máx. ${maxTxt} zonas abertas · máx. ${num(g.maxStartsHour)} partidas/hora</div>
      ${supLine}
    </div>`;
  }).join('');

  const itlCards = irows.map((r) => {
    r = r || {};
    const acao = INTERLOCK_ACAO_LABEL[num(r.acao)] || ('ação ' + num(r.acao));
    const tipo = INTERLOCK_TIPO_LABEL[num(r.tipo)] || ('tipo ' + num(r.tipo));
    const zonasStr = r.todas ? 'Todas as zonas' : ('Zonas: ' + ((Array.isArray(r.zonas) ? r.zonas : []).map((id) => zoneName(zs, id)).join(', ') || '—'));
    return `<div class="gi-card" data-itledit="${num(r.id)}">
      <div class="gi-cardhdr"><div class="gi-name">${esc(r.mensagem || tipo)}</div></div>
      <div class="gi-line">${interlockDesc(r, sts, sens)} · ${esc(acao)}</div>
      <div class="gi-line">${zonasStr}</div>
    </div>`;
  }).join('');

  view.innerHTML = `
    <div class="lvl-wrap">
      <div class="gi-sechdr"><div class="lvl-seclbl">Grupos hidráulicos</div></div>
      <div class="lvl-list">${groupCards || '<div class="lvl-empty">Nenhum grupo hidráulico.</div>'}</div>
      <button class="lvl-newbtn" data-gnew>+ Novo grupo</button>

      <div class="gi-sechdr" style="margin-top:6px;"><div class="lvl-seclbl">Intertravamentos</div></div>
      <button class="lvl-newbtn" data-itlnew>+ Novo intertravamento</button>
      <div class="lvl-list">${itlCards || '<div class="lvl-empty">Nenhuma regra de intertravamento.</div>'}</div>
    </div>`;

  view.querySelector('[data-gnew]').addEventListener('click', () => groupForm(null, zs));
  view.querySelectorAll('[data-gedit]').forEach((c) => c.addEventListener('click', () => {
    groupForm(grows.find((x) => x && num(x.id) === num(c.dataset.gedit)) || null, zs);
  }));
  view.querySelector('[data-itlnew]').addEventListener('click', () => interlockForm(null, sts, zs, sens, irows));
  view.querySelectorAll('[data-itledit]').forEach((c) => c.addEventListener('click', () => {
    interlockForm(irows.find((x) => x && num(x.id) === num(c.dataset.itledit)) || null, sts, zs, sens, irows);
  }));
}

// Atualiza só o rótulo de estado ao vivo (não recria a lista; no-op se a lista não está montada).
async function pollGroupStatus() {
  if (!document.querySelector('[data-gstate]')) return; // form aberto ou outra aba
  const st = await getJson('/groups/status').catch(() => []);
  (Array.isArray(st) ? st : []).forEach((s) => {
    if (!s) return;
    const el = view.querySelector(`[data-gstate="${num(s.id)}"]`);
    if (el) {
      const estado = s.estado || 'ocioso';
      el.className = 'gi-estado ' + (GROUP_STATE_CLASS[estado] || 'gray');
      el.textContent = GROUP_STATE_LABEL[estado] || estado;
    }
  });
}

// Formulário de grupo hidráulico — layout do mockup (classes lvl-*): pills com
// nomes de zona para bomba/zonas-membro, botões de transição, painéis de limites/
// tempos/proteção. Campos de texto/número atualizam `st` no input para sobreviver
// aos re-renders disparados pelas pills.
function groupForm(group, zones) {
  const editing = !!group;
  const zs = (Array.isArray(zones) ? zones : []).filter((z) => z && num(z.fonteInput ?? -1) < 0);
  const st = group ? JSON.parse(JSON.stringify(group)) : {
    id: 0, nome: '', bombaZoneId: 0, zonas: [], minOpen: 1, maxOpen: 1, transicao: 0,
    overlapS: 10, startAfterOpenS: 5, stopBeforeCloseS: 8, minRunMin: 5, maxStartsHour: 6,
  };
  st.zonas = Array.isArray(st.zonas) ? st.zonas : [];
  let deleteConfirm = false;

  function render() {
    const bombaPills = `<button class="lvl-pill${num(st.bombaZoneId) === 0 ? ' sel' : ''}" data-gbomba="0">— sem —</button>` +
      zs.map((z) => `<button class="lvl-pill${num(st.bombaZoneId) === num(z.id) ? ' sel' : ''}" data-gbomba="${num(z.id)}">${z.name ? esc(z.name) : ('zona ' + num(z.id))}</button>`).join('');
    const zonePills = zs.length
      ? zs.map((z) => `<button class="lvl-pill${st.zonas.includes(num(z.id)) ? ' sel' : ''}" data-gz="${num(z.id)}">${z.name ? esc(z.name) : ('zona ' + num(z.id))}</button>`).join('')
      : '<div class="lvl-muted">Sem zonas não-espelho.</div>';
    const transBtns = [['0', 'Abrir antes de fechar'], ['1', 'Fechar antes de abrir']]
      .map(([v, l]) => `<button class="lvl-polbtn${num(st.transicao) === num(v) ? ' sel' : ''}" data-gtrans="${v}">${l}</button>`).join('');
    const delBlock = editing
      ? (deleteConfirm
          ? `<div class="lvl-delbox">Excluir este grupo?<div class="lvl-delbtns"><button class="lvl-delcancel" data-gdelcancel>Cancelar</button><button class="lvl-delconfirm" data-gdelconfirm>Excluir</button></div></div>`
          : `<button class="lvl-delbtn" data-gdel>Excluir grupo</button>`)
      : '';

    view.innerHTML = `
      <div class="lvl-wrap">
        <div class="lvl-back" data-gback>‹ Grupos &amp; Intertravamentos</div>
        <div class="lvl-title">Grupo hidráulico</div>
        <div class="lvl-panel">
          <div><div class="lvl-seclbl">Nome</div><input class="lvl-input" id="g-nome" maxlength="15" value="${esc(st.nome || '')}"></div>
          <div><div class="lvl-seclbl">Bomba / válvula mestre</div><div class="lvl-pills">${bombaPills}</div></div>
          <div><div class="lvl-seclbl">Zonas do grupo</div><div class="lvl-pills">${zonePills}</div></div>
          <div><div class="lvl-seclbl">Transição</div><div class="lvl-pol2">${transBtns}</div></div>
        </div>
        <div class="lvl-panel">
          <div class="lvl-seclbl">Limites de válvulas abertas</div>
          <div class="lvl-advrow">
            <div class="lvl-advfld"><div class="lvl-advlbl">Mín. com bomba ligada</div><input class="lvl-input" id="g-min" type="number" min="1" value="${num(st.minOpen)}"></div>
            <div class="lvl-advfld"><div class="lvl-advlbl">Máx. simultâneas (0=sem teto)</div><input class="lvl-input" id="g-max" type="number" min="0" value="${num(st.maxOpen)}"></div>
          </div>
          <div class="lvl-seclbl">Tempos de comutação (s)</div>
          <div class="lvl-advrow">
            <div class="lvl-advfld"><div class="lvl-advlbl">Sobreposição</div><input class="lvl-input" id="g-ov" type="number" min="0" value="${num(st.overlapS)}"></div>
            <div class="lvl-advfld"><div class="lvl-advlbl">Partida após abrir</div><input class="lvl-input" id="g-sa" type="number" min="0" value="${num(st.startAfterOpenS)}"></div>
            <div class="lvl-advfld"><div class="lvl-advlbl">Parada antes de fechar</div><input class="lvl-input" id="g-sb" type="number" min="0" value="${num(st.stopBeforeCloseS)}"></div>
          </div>
          <div class="lvl-seclbl">Proteção do motor</div>
          <div class="lvl-advrow">
            <div class="lvl-advfld"><div class="lvl-advlbl">Funcionamento mín. (min)</div><input class="lvl-input" id="g-mr" type="number" min="0" value="${num(st.minRunMin)}"></div>
            <div class="lvl-advfld"><div class="lvl-advlbl">Máx. partidas/hora</div><input class="lvl-input" id="g-ms" type="number" min="0" value="${num(st.maxStartsHour)}"></div>
          </div>
        </div>
        <button class="lvl-save" data-gsave>Salvar grupo</button>
        ${delBlock}
      </div>`;

    view.querySelector('[data-gback]').addEventListener('click', () => renderGruposInterlocks().catch(() => {}));
    view.querySelectorAll('[data-gbomba]').forEach((b) => b.addEventListener('click', () => { st.bombaZoneId = num(b.dataset.gbomba); render(); }));
    view.querySelectorAll('[data-gz]').forEach((b) => b.addEventListener('click', () => {
      const zid = num(b.dataset.gz);
      const i = st.zonas.indexOf(zid);
      if (i >= 0) st.zonas.splice(i, 1); else st.zonas.push(zid);
      render();
    }));
    view.querySelectorAll('[data-gtrans]').forEach((b) => b.addEventListener('click', () => { st.transicao = num(b.dataset.gtrans); render(); }));

    const bind = (id, key) => { const el = view.querySelector(id); if (el) el.addEventListener('input', (e) => { st[key] = e.target.value; }); };
    bind('#g-nome', 'nome');
    bind('#g-min', 'minOpen'); bind('#g-max', 'maxOpen');
    bind('#g-ov', 'overlapS'); bind('#g-sa', 'startAfterOpenS'); bind('#g-sb', 'stopBeforeCloseS');
    bind('#g-mr', 'minRunMin'); bind('#g-ms', 'maxStartsHour');

    view.querySelector('[data-gsave]').addEventListener('click', save);
    const delBtn = view.querySelector('[data-gdel]');
    if (delBtn) delBtn.addEventListener('click', () => { deleteConfirm = true; render(); });
    const delCancel = view.querySelector('[data-gdelcancel]');
    if (delCancel) delCancel.addEventListener('click', () => { deleteConfirm = false; render(); });
    const delConfirm = view.querySelector('[data-gdelconfirm]');
    if (delConfirm) delConfirm.addEventListener('click', async () => {
      const r = await postJson('/groups/delete', { id: num(st.id) });
      if (r.ok) renderGruposInterlocks().catch(() => {});
      else alert('Falha ao excluir: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
    });
  }

  async function save() {
    const body = {
      id: num(st.id),
      nome: String(st.nome || '').slice(0, 15),
      bombaZoneId: num(st.bombaZoneId),
      zonas: st.zonas,
      minOpen: num(st.minOpen),
      maxOpen: num(st.maxOpen),
      transicao: num(st.transicao),
      overlapS: num(st.overlapS),
      startAfterOpenS: num(st.startAfterOpenS),
      stopBeforeCloseS: num(st.stopBeforeCloseS),
      minRunMin: num(st.minRunMin),
      maxStartsHour: num(st.maxStartsHour),
    };
    if (!body.zonas.length) { alert('Selecione ao menos uma zona.'); return; }
    const r = await postJson('/groups', body);
    if (r.ok) renderGruposInterlocks().catch(() => {});
    else alert('Falha: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
  }

  render();
}

// ===== Nível (boia) — modelo de UI do mockup "Irrigacao Mobile.dc.html" =====
// Telas separadas (lista ↔ edição), seleção por pills, polaridade em dois
// botões, exclusão inline, mini-tanque no card. Estado no módulo (nvData cache
// dos 4 endpoints + nvUI da tela de edição); interações re-renderizam de cache
// sem novo fetch; salvar/excluir fazem POST e recarregam.
const NV_MAX_RULES = 4;
let nvData = { stations: [], zones: [], sensors: [], rules: [] };
let nvUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };

function fmtDur(s) {
  s = num(s);
  if (s >= 3600 && s % 3600 === 0) return (s / 3600) + 'h';
  if (s >= 60 && s % 60 === 0) return (s / 60) + 'm';
  return s + 's';
}
function sensorName(sensors, node, idx) {
  const st = (Array.isArray(sensors) ? sensors : []).find((x) => x && num(x.node) === num(node));
  const s = st && Array.isArray(st.sensores) ? st.sensores.find((y) => y && num(y.idx) === num(idx)) : null;
  return s && s.nome ? esc(s.nome) : ('s' + num(idx));
}
function zoneName(zones, id) {
  const z = (Array.isArray(zones) ? zones : []).find((x) => x && num(x.id) === num(id));
  return z && z.name ? esc(z.name) : ('zona ' + num(id));
}
function polarityText(liga) {
  return (liga ? 'ATIVA' : 'INATIVA') + ' (= reservatório baixo)';
}
// Sensores digitais (tipo 0 = boia elegível) de um nó.
function nvDigitalSensors(sensors, node) {
  const st = (Array.isArray(sensors) ? sensors : []).find((x) => x && num(x.node) === num(node));
  return st && Array.isArray(st.sensores) ? st.sensores.filter((s) => s && num(s.tipo) === 0) : [];
}
// Card de uma regra na lista (mini-tanque + resumo). Puro.
function nvCardHtml(r, stations, zones, sensors) {
  r = r || {};
  const est = stationName(stations, r.sensorNode);
  const sen = sensorName(sensors, r.sensorNode, r.sensorIdx);
  const zon = zoneName(zones, r.targetZoneId);
  return `<div class="lvl-card" data-nedit="${num(r.id)}">
    <div class="lvl-tank"></div>
    <div class="lvl-cardbody">
      <div class="lvl-hdr">${est} · boia "${sen}"</div>
      <div class="lvl-line">liga bomba ▸ ${zon}</div>
      <div class="lvl-pol">quando boia ${polarityText(r.ligaQuandoAtivo)}</div>
      <div class="lvl-chips">
        <span class="lvl-pill-chip">min ${fmtDur(r.minOnS)}/${fmtDur(r.minOffS)}</span>
        <span class="lvl-pill-chip warn">sem sinal ${fmtDur(r.staleTimeoutS)} ⚠</span>
      </div>
    </div>
  </div>`;
}
// Tela-lista. `data` cai para nvData em produção; explícito nos testes. Puro.
function nvListHtml(data) {
  data = data || nvData;
  const { stations, zones, sensors, rules } = data;
  const atLimit = (rules || []).length >= NV_MAX_RULES;
  const limit = atLimit ? `<div class="lvl-note">Limite de ${NV_MAX_RULES} regras atingido.</div>` : '';
  const newBtn = atLimit ? '' : `<button class="lvl-newbtn" id="nv-new">+ Nova regra</button>`;
  const cards = (rules || []).length
    ? rules.map((r) => nvCardHtml(r, stations, zones, sensors)).join('')
    : `<div class="lvl-empty">Nenhuma regra de nível.</div>`;
  return `<div class="lvl-wrap">
    <div class="lvl-title">Controle de nível</div>
    <div class="lvl-sub">Liga/desliga a bomba conforme a boia do reservatório</div>
    ${limit}${newBtn}
    <div class="lvl-list">${cards}</div>
  </div>`;
}
// Tela-edição. `draft`/`data`/`ui` caem para o estado do módulo em produção;
// explícitos nos testes. Puro (não toca DOM).
function nvEditHtml(draft, data, ui) {
  const d = draft || nvUI.draft;
  data = data || nvData;
  ui = ui || nvUI;
  const stations = data.stations || [];
  const zones = data.zones || [];
  const sensors = data.sensors || [];
  const editing = num(d.id) > 0;
  const digitals = nvDigitalSensors(sensors, d.sensorNode);
  const errors = ui.errors || [];
  const errBox = errors.length
    ? `<div class="lvl-errbox">${errors.map((e) => `<div>${esc(e)}</div>`).join('')}</div>` : '';
  const stPills = stations.map((s) =>
    `<button class="lvl-pill${num(s.node) === num(d.sensorNode) ? ' sel' : ''}" data-nvst="${num(s.node)}">${s.name ? esc(s.name) : nodeHex(s.node)}</button>`).join('');
  const boiaPills = digitals.length
    ? digitals.map((s) => `<button class="lvl-pill${num(s.idx) === num(d.sensorIdx) ? ' sel' : ''}" data-nvboia="${num(s.idx)}">${s.nome ? esc(s.nome) : ('s' + num(s.idx))}</button>`).join('')
    : `<div class="lvl-muted">Nenhuma boia (sensor digital) nesta estação.</div>`;
  const bombaPills = zones.map((z) =>
    `<button class="lvl-pill${num(z.id) === num(d.targetZoneId) ? ' sel' : ''}" data-nvzone="${num(z.id)}">${z.name ? esc(z.name) : ('zona ' + num(z.id))}</button>`).join('');
  const advOpen = !!d.advancedOpen;
  const msg = d.mensagem || '';
  const delBlock = editing
    ? (ui.deleteConfirm
        ? `<div class="lvl-delbox">
      <div>Excluir esta regra de nível?</div>
      <div class="lvl-delbtns">
        <button class="lvl-delcancel" id="nv-delcancel">Cancelar</button>
        <button class="lvl-delconfirm" id="nv-delconfirm">Excluir</button>
      </div>
    </div>`
        : `<button class="lvl-delbtn" id="nv-delrequest">Excluir regra</button>`)
    : '';
  return `<div class="lvl-wrap">
    <div class="lvl-back" id="nv-back">‹ Controle de nível</div>
    <div class="lvl-title">${editing ? 'Editar regra' : 'Nova regra'}</div>
    ${errBox}
    <div class="lvl-panel">
      <div class="lvl-sec">
        <div class="lvl-seclbl">Estação</div>
        <div class="lvl-pills">${stPills}</div>
      </div>
      <div class="lvl-sec">
        <div class="lvl-seclbl">Boia</div>
        <div class="lvl-pills">${boiaPills}</div>
      </div>
      <div class="lvl-sec">
        <div class="lvl-seclbl">Polaridade — quando a bomba liga</div>
        <div class="lvl-pol2">
          <button class="lvl-polbtn${d.ligaQuandoAtivo ? ' sel' : ''}" data-nvpol="1">Boia ativa</button>
          <button class="lvl-polbtn${d.ligaQuandoAtivo ? '' : ' sel'}" data-nvpol="0">Boia inativa</button>
        </div>
        <div class="lvl-polhint">quando boia ${d.ligaQuandoAtivo ? 'ativa' : 'inativa'} → reservatório baixo</div>
      </div>
      <div class="lvl-sec">
        <div class="lvl-seclbl">Bomba a acionar</div>
        <div class="lvl-pills">${bombaPills}</div>
      </div>
    </div>
    <div class="lvl-advtoggle" id="nv-advtoggle">${advOpen ? '▾' : '▸'} Ajustes avançados</div>
    <div class="lvl-adv${advOpen ? '' : ' hidden'}">
      <div class="lvl-advrow">
        <div class="lvl-advfld">
          <div class="lvl-advlbl">Tempo mínimo ligada (s)</div>
          <input class="lvl-input" id="nv-minon" type="number" min="0" value="${num(d.minOnS)}">
        </div>
        <div class="lvl-advfld">
          <div class="lvl-advlbl">Tempo mínimo desligada (s)</div>
          <input class="lvl-input" id="nv-minoff" type="number" min="0" value="${num(d.minOffS)}">
        </div>
      </div>
      <div class="lvl-advfld">
        <div class="lvl-advlbl">Falha se sem sinal por (s)</div>
        <input class="lvl-input" id="nv-stale" type="number" min="1" value="${num(d.staleTimeoutS)}">
      </div>
      <div class="lvl-advfld">
        <div class="lvl-advlblrow"><span class="lvl-advlbl">Mensagem de alerta</span><span class="lvl-counter">${msg.length}/23</span></div>
        <input class="lvl-input" id="nv-msg" maxlength="23" placeholder="Ex.: Cisterna baixa" value="${esc(msg)}">
      </div>
    </div>
    <button class="lvl-save" id="nv-save">Salvar</button>
    ${delBlock}
  </div>`;
}

// ===== Controle de nível por boia — controller + wiring =====
// Entrada do roteador: carrega os 4 endpoints, cacheia e mostra a lista.
async function renderNiveis() {
  const [rules, stations, zones, sensors] = await Promise.all([
    getJson('/levels').catch(() => []),
    getJson('/stations').catch(() => []),
    getJson('/zones').catch(() => []),
    getJson('/sensors').catch(() => []),
  ]);
  nvData = {
    stations: Array.isArray(stations) ? stations : [],
    zones: Array.isArray(zones) ? zones : [],
    sensors: Array.isArray(sensors) ? sensors : [],
    rules: Array.isArray(rules) ? rules : [],
  };
  nvUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };
  nvRender();
}

function nvRender() {
  view.innerHTML = nvUI.screen === 'edit' ? nvEditHtml() : nvListHtml();
  nvWire();
}

// Copia os inputs avançados atuais para o draft antes de qualquer re-render
// disparado por pill/toggle (senão o que o usuário digitou se perde).
function nvSyncInputs() {
  const d = nvUI.draft;
  if (!d) return;
  const q = (sel) => view.querySelector(sel);
  const mo = q('#nv-minon'); if (mo) d.minOnS = num(mo.value);
  const mf = q('#nv-minoff'); if (mf) d.minOffS = num(mf.value);
  const st = q('#nv-stale'); if (st) d.staleTimeoutS = num(st.value);
  const mg = q('#nv-msg'); if (mg) d.mensagem = mg.value.slice(0, 23);
}

function nvOpenNew() {
  if (nvData.rules.length >= NV_MAX_RULES) return;
  const st = nvData.stations[0] || {};
  const digs = nvDigitalSensors(nvData.sensors, st.node);
  nvUI = {
    screen: 'edit', errors: [], deleteConfirm: false,
    draft: {
      id: 0, sensorNode: num(st.node), sensorIdx: digs.length ? num(digs[0].idx) : '',
      ligaQuandoAtivo: true, targetZoneId: null,
      minOnS: 60, minOffS: 60, staleTimeoutS: 600, mensagem: '', advancedOpen: false,
    },
  };
  nvRender();
}

function nvOpenEdit(id) {
  const r = nvData.rules.find((x) => x && num(x.id) === num(id));
  if (!r) return;
  nvUI = {
    screen: 'edit', errors: [], deleteConfirm: false,
    draft: {
      id: num(r.id), sensorNode: num(r.sensorNode), sensorIdx: num(r.sensorIdx),
      ligaQuandoAtivo: !!r.ligaQuandoAtivo, targetZoneId: num(r.targetZoneId),
      minOnS: num(r.minOnS), minOffS: num(r.minOffS), staleTimeoutS: num(r.staleTimeoutS),
      mensagem: r.mensagem || '', advancedOpen: false,
    },
  };
  nvRender();
}

function nvCancel() {
  nvUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };
  nvRender();
}

// Trocar estação: reposiciona a boia para o primeiro sensor digital do nó.
function nvSetStation(node) {
  nvSyncInputs();
  const d = nvUI.draft;
  d.sensorNode = node;
  const digs = nvDigitalSensors(nvData.sensors, node);
  d.sensorIdx = digs.length ? num(digs[0].idx) : '';
  nvRender();
}

function nvValidate(d) {
  const e = [];
  if (!d.sensorNode || d.sensorIdx === '' || d.sensorIdx == null) e.push('Selecione a estação e a boia.');
  if (!d.targetZoneId) e.push('Selecione a bomba a ser acionada.');
  return e;
}

async function nvSave() {
  nvSyncInputs();
  const d = nvUI.draft;
  const errors = nvValidate(d);
  if (errors.length) { nvUI.errors = errors; nvRender(); return; }
  const body = {
    id: num(d.id),
    sensorNode: num(d.sensorNode),
    sensorIdx: num(d.sensorIdx),
    ligaQuandoAtivo: !!d.ligaQuandoAtivo,
    targetZoneId: num(d.targetZoneId),
    minOnS: num(d.minOnS),
    minOffS: num(d.minOffS),
    staleTimeoutS: num(d.staleTimeoutS) || 1,
    mensagem: (d.mensagem || '').slice(0, 23),
  };
  const r = await postJson('/levels', body);
  if (r.ok) renderNiveis().catch(() => {});
  else { nvUI.errors = (r.body && Array.isArray(r.body.errors)) ? r.body.errors : ['Falha ao salvar.']; nvRender(); }
}

async function nvDelete() {
  const id = num(nvUI.draft.id);
  const r = await postJson('/levels/delete', { id });
  if (r.ok) renderNiveis().catch(() => {});
  else { nvUI.deleteConfirm = false; nvUI.errors = ['Falha ao excluir.']; nvRender(); }
}

function nvWire() {
  const q = (sel) => view.querySelector(sel);

  if (nvUI.screen !== 'edit') {
    const nb = q('#nv-new');
    if (nb) nb.addEventListener('click', () => nvOpenNew());
    view.querySelectorAll('[data-nedit]').forEach((c) =>
      c.addEventListener('click', () => nvOpenEdit(num(c.dataset.nedit))));
    return;
  }

  const back = q('#nv-back');
  if (back) back.addEventListener('click', () => nvCancel());

  view.querySelectorAll('[data-nvst]').forEach((b) =>
    b.addEventListener('click', () => nvSetStation(num(b.dataset.nvst))));
  view.querySelectorAll('[data-nvboia]').forEach((b) =>
    b.addEventListener('click', () => { nvSyncInputs(); nvUI.draft.sensorIdx = num(b.dataset.nvboia); nvRender(); }));
  view.querySelectorAll('[data-nvpol]').forEach((b) =>
    b.addEventListener('click', () => { nvSyncInputs(); nvUI.draft.ligaQuandoAtivo = b.dataset.nvpol === '1'; nvRender(); }));
  view.querySelectorAll('[data-nvzone]').forEach((b) =>
    b.addEventListener('click', () => { nvSyncInputs(); nvUI.draft.targetZoneId = num(b.dataset.nvzone); nvRender(); }));

  const advToggle = q('#nv-advtoggle');
  if (advToggle) advToggle.addEventListener('click', () => {
    nvSyncInputs();
    nvUI.draft.advancedOpen = !nvUI.draft.advancedOpen;
    nvRender();
  });

  const msgIn = q('#nv-msg');
  if (msgIn) msgIn.addEventListener('input', () => {
    const c = view.querySelector('.lvl-counter');
    if (c) c.textContent = msgIn.value.length + '/23';
  });

  const save = q('#nv-save');
  if (save) save.addEventListener('click', () => nvSave());

  const delReq = q('#nv-delrequest');
  if (delReq) delReq.addEventListener('click', () => { nvSyncInputs(); nvUI.deleteConfirm = true; nvRender(); });
  const delCancel = q('#nv-delcancel');
  if (delCancel) delCancel.addEventListener('click', () => { nvUI.deleteConfirm = false; nvRender(); });
  const delConfirm = q('#nv-delconfirm');
  if (delConfirm) delConfirm.addEventListener('click', () => nvDelete());
}

// ===== Modo Espelhamento =====
// Copia o estado das entradas físicas do gateway para as saídas de zona associadas nos nós.
// Modelo de UI do mockup "Irrigacao Mobile.dc.html" linhas 815–970.
// Estado do módulo: mrData cache do GET + mrUI da tela de edição.
let mrData = { enabled: false, ports: [] };
let mrUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };

// Nomes fixos das 4 portas de entrada física do gateway.
const MR_PORT_NAMES = ['Entrada 1', 'Entrada 2', 'Entrada 3', 'Entrada 4'];

// Cor do ponto de estado (ativo/inativo).
function mrDotColor(active) {
  return active ? 'oklch(0.47 0.1 150)' : 'oklch(0.75 0.006 100)';
}

// Tela-lista (pura, não toca DOM).
function mrListHtml() {
  const { enabled, ports } = mrData;
  const toggleBg = enabled ? 'oklch(0.47 0.1 150)' : 'oklch(0.85 0.006 100)';
  const knobLeft = enabled ? '18px' : '2px';
  const statusLabel = enabled ? 'Espelhamento ativo — entradas controlam saídas' : 'Desativado — cronograma e painel assumem o controle';

  const bypassBanner = enabled
    ? `<div style="background:oklch(0.65 0.15 75 / 0.1);border:1px solid oklch(0.65 0.15 75 / 0.4);border-radius:14px;padding:12px 14px;">
        <div style="font-size:12.5px;font-weight:700;color:oklch(0.22 0.008 100);">Bypass ativo</div>
        <div style="font-size:12px;color:oklch(0.22 0.008 100);margin-top:4px;line-height:1.5;">Cronogramas, grupos e intertravamentos são ignorados nas zonas associadas.</div>
      </div>`
    : '';

  // Estado atual das portas (read-only)
  const portRows = (Array.isArray(ports) ? ports : []).map((p) => {
    const nome = esc(MR_PORT_NAMES[num(p.i)] || ('Entrada ' + (num(p.i) + 1)));
    const active = !!p.active;
    const dotColor = mrDotColor(active);
    const label = active ? 'Ativa' : 'Inativa';
    return `<div style="display:flex;align-items:center;justify-content:space-between;gap:8px;padding:10px 0;border-bottom:1px solid oklch(0.94 0.004 100);">
      <div style="display:flex;align-items:center;gap:10px;min-width:0;">
        <div style="width:9px;height:9px;border-radius:999px;background:${dotColor};flex-shrink:0;"></div>
        <div style="font-size:13.5px;font-weight:600;color:oklch(0.22 0.008 100);">${nome}</div>
      </div>
      <div style="font-size:12px;font-weight:600;color:${dotColor};white-space:nowrap;">${label}</div>
    </div>`;
  }).join('');

  // Associações: portas que têm zoneId associado
  const mappings = (Array.isArray(ports) ? ports : []).filter((p) => p.zoneId != null && p.zoneId > 0);
  const freePorts = (Array.isArray(ports) ? ports : []).filter((p) => !(p.zoneId != null && p.zoneId > 0));
  const allUsed = freePorts.length === 0;
  const hasFree = freePorts.length > 0;
  const hasMappings = mappings.length > 0;

  const newBtn = hasFree
    ? `<button data-mrnew style="cursor:pointer;border:1.5px dashed oklch(0.47 0.1 150 / 0.5);background:transparent;color:oklch(0.47 0.1 150);font-size:13px;font-weight:700;padding:11px;border-radius:14px;width:100%;">+ Nova associação</button>`
    : '';
  const allUsedMsg = allUsed
    ? `<div style="font-size:12px;color:oklch(0.52 0.006 100);background:oklch(0.97 0.003 100);border-radius:10px;padding:9px 12px;">Todas as portas de entrada já estão associadas.</div>`
    : '';

  const mappingCards = mappings.map((p) => {
    const portaNome = esc(MR_PORT_NAMES[num(p.i)] || ('Entrada ' + (num(p.i) + 1)));
    const zonaNome = esc(p.zoneName || ('Zona ' + num(p.zoneId)));
    const invertido = !!p.invertido;
    const habilitado = !!p.habilitado;
    const driving = !!p.driving;
    const invBadge = invertido ? ` <span style="font-size:10.5px;font-weight:700;color:oklch(0.65 0.15 75);">INV</span>` : '';
    const habChip = habilitado
      ? `<span style="font-size:10.5px;font-weight:700;padding:3px 8px;border-radius:999px;background:oklch(0.47 0.1 150 / 0.12);color:oklch(0.47 0.1 150);white-space:nowrap;">Habilitada</span>`
      : `<span style="font-size:10.5px;font-weight:700;padding:3px 8px;border-radius:999px;background:oklch(0.9 0.006 100);color:oklch(0.52 0.006 100);white-space:nowrap;">Desativada</span>`;
    const habBg = habilitado ? 'oklch(0.47 0.1 150)' : 'oklch(0.85 0.006 100)';
    const habKnob = habilitado ? '16px' : '2px';
    const inActive = !!p.active;
    const inDotColor = mrDotColor(invertido ? !inActive : inActive);
    const outDotColor = mrDotColor(driving);
    const inLabel = inActive ? 'ativa' : 'inativa';
    const rawHigh = invertido ? !inActive : inActive;
    const inRawLabel = rawHigh ? 'nível alto' : 'nível baixo';
    const outLabel = driving ? 'aberta' : 'fechada';
    return `<div style="cursor:pointer;background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);box-shadow:0 1px 3px rgba(0,0,0,0.05);border-radius:16px;padding:14px 16px;" data-mredit="${num(p.i)}">
      <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;">
        <div style="font-size:14px;font-weight:600;color:oklch(0.22 0.008 100);min-width:0;">${portaNome} <span style="color:oklch(0.7 0.006 100);">→</span> ${zonaNome}${invBadge}</div>
        <div style="display:flex;align-items:center;gap:8px;flex-shrink:0;">
          ${habChip}
          <div data-mrtoghab="${num(p.i)}" style="cursor:pointer;width:34px;height:20px;border-radius:999px;background:${habBg};position:relative;">
            <div style="position:absolute;top:2px;left:${habKnob};width:16px;height:16px;border-radius:999px;background:#fff;"></div>
          </div>
        </div>
      </div>
      <div style="font-size:12px;color:oklch(0.52 0.006 100);margin-top:6px;">Polaridade ${invertido ? 'invertida' : 'normal'}</div>
      <div style="display:flex;align-items:center;gap:14px;margin-top:10px;">
        <div style="display:flex;align-items:center;gap:6px;">
          <div style="width:8px;height:8px;border-radius:999px;background:${inDotColor};"></div>
          <div style="font-size:11.5px;color:oklch(0.52 0.006 100);">entrada ${inLabel} (${inRawLabel})</div>
        </div>
        <span style="color:oklch(0.75 0.006 100);font-size:13px;">→</span>
        <div style="display:flex;align-items:center;gap:6px;">
          <div style="width:8px;height:8px;border-radius:999px;background:${outDotColor};"></div>
          <div style="font-size:11.5px;color:oklch(0.52 0.006 100);">saída ${outLabel}</div>
        </div>
      </div>
    </div>`;
  }).join('');

  const noMappingsMsg = !hasMappings
    ? `<div style="font-size:12.5px;color:oklch(0.52 0.006 100);">Nenhuma associação configurada.</div>`
    : '';

  return `
    <div style="font-size:18px;font-weight:700;color:oklch(0.22 0.008 100);">Modo Espelhamento</div>
    <div style="font-size:12px;color:oklch(0.52 0.006 100);margin-top:-6px;">Copia o estado das entradas físicas do gateway direto para saídas associadas nos nós</div>

    <div style="background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);box-shadow:0 1px 3px rgba(0,0,0,0.05);border-radius:16px;padding:14px 16px;display:flex;align-items:center;justify-content:space-between;gap:10px;">
      <div>
        <div style="font-size:14px;font-weight:600;color:oklch(0.22 0.008 100);">Ativar espelhamento</div>
        <div style="font-size:12px;color:oklch(0.52 0.006 100);margin-top:2px;">${esc(statusLabel)}</div>
      </div>
      <div data-mrtoggle style="cursor:pointer;width:38px;height:22px;border-radius:999px;background:${toggleBg};position:relative;flex-shrink:0;">
        <div style="position:absolute;top:2px;left:${knobLeft};width:18px;height:18px;border-radius:999px;background:#fff;"></div>
      </div>
    </div>

    ${bypassBanner}

    <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;margin-top:6px;">
      <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;">Estado atual das portas</div>
    </div>
    <div style="background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);box-shadow:0 1px 3px rgba(0,0,0,0.05);border-radius:16px;padding:6px 16px;display:flex;flex-direction:column;">
      ${portRows || '<div style="padding:10px 0;font-size:12px;color:oklch(0.6 0.006 100);">Sem dados de portas.</div>'}
    </div>

    <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;margin-top:6px;">
      <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;">Associações porta → zona</div>
    </div>
    ${allUsedMsg}
    ${newBtn}
    ${mappingCards}
    ${noMappingsMsg}
  `;
}

// Tela-edição do espelhamento (pura, não toca DOM).
function mrEditHtml() {
  const d = mrUI.draft;
  const { ports } = mrData;
  const editing = d.isEditing;
  const errors = mrUI.errors || [];
  const errBox = errors.length
    ? `<div style="background:oklch(0.55 0.16 30 / 0.08);border:1px solid oklch(0.55 0.16 30 / 0.3);border-radius:12px;padding:10px 12px;display:flex;flex-direction:column;gap:4px;">
        ${errors.map((e) => `<div style="font-size:12px;color:oklch(0.55 0.16 30);">${esc(e)}</div>`).join('')}
      </div>`
    : '';

  // Portas livres (sem zona) + a porta atual em edição
  const freePorts = (Array.isArray(ports) ? ports : []).filter(
    (p) => !(p.zoneId != null && p.zoneId > 0) || num(p.i) === num(d.input)
  );
  const portPills = freePorts.map((p) => {
    const nome = esc(MR_PORT_NAMES[num(p.i)] || ('Entrada ' + (num(p.i) + 1)));
    const sel = num(p.i) === num(d.input);
    const border = sel ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
    const bg = sel ? 'oklch(0.47 0.1 150)' : 'transparent';
    const color = sel ? '#fff' : 'oklch(0.4 0.006 100)';
    const shadow = sel ? '0 1px 4px oklch(0.47 0.1 150 / 0.3)' : 'none';
    return `<button data-mrport="${num(p.i)}" style="cursor:pointer;border:1.5px solid ${border};padding:7px 12px;border-radius:999px;background:${bg};color:${color};font-size:12.5px;font-weight:600;box-shadow:${shadow};">${nome}</button>`;
  }).join('');

  // Zonas disponíveis (todas as zonas do mrData)
  const zones = Array.isArray(mrData.zones) ? mrData.zones : [];
  const zonePills = zones.map((z) => {
    const nome = esc(z.name || ('Zona ' + num(z.id)));
    const sel = num(z.id) === num(d.zoneId);
    const border = sel ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
    const bg = sel ? 'oklch(0.47 0.1 150)' : 'transparent';
    const color = sel ? '#fff' : 'oklch(0.4 0.006 100)';
    const shadow = sel ? '0 1px 4px oklch(0.47 0.1 150 / 0.3)' : 'none';
    return `<button data-mrzone="${num(z.id)}" style="cursor:pointer;border:1.5px solid ${border};padding:7px 12px;border-radius:999px;background:${bg};color:${color};font-size:12.5px;font-weight:600;box-shadow:${shadow};">${nome}</button>`;
  }).join('');

  const normalBorder = !d.invertido ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
  const normalBg = !d.invertido ? 'oklch(0.47 0.1 150 / 0.1)' : 'transparent';
  const normalColor = !d.invertido ? 'oklch(0.47 0.1 150)' : 'oklch(0.4 0.006 100)';
  const invBorder = d.invertido ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
  const invBg = d.invertido ? 'oklch(0.47 0.1 150 / 0.1)' : 'transparent';
  const invColor = d.invertido ? 'oklch(0.47 0.1 150)' : 'oklch(0.4 0.006 100)';
  const polaridadeLabel = d.invertido
    ? 'Invertido: saída abre quando entrada estiver em 0 V.'
    : 'Normal: saída abre quando entrada estiver em 1 (ativa).';

  const habBg = d.habilitado ? 'oklch(0.47 0.1 150)' : 'oklch(0.85 0.006 100)';
  const habKnob = d.habilitado ? '18px' : '2px';

  let delBlock = '';
  if (editing) {
    if (mrUI.deleteConfirm) {
      delBlock = `<div style="background:oklch(0.55 0.16 30 / 0.08);border:1px solid oklch(0.55 0.16 30 / 0.3);border-radius:12px;padding:12px 14px;">
        <div style="font-size:12.5px;color:oklch(0.22 0.008 100);">Excluir esta associação?</div>
        <div style="display:flex;gap:8px;margin-top:8px;">
          <button data-mrdelcancel style="cursor:pointer;flex:1;border:1px solid oklch(0.9 0.006 100);padding:9px;border-radius:8px;background:transparent;color:oklch(0.4 0.006 100);font-size:12.5px;font-weight:600;">Cancelar</button>
          <button data-mrdelconfirm style="cursor:pointer;flex:1;border:none;padding:9px;border-radius:8px;background:oklch(0.55 0.16 30);color:#fff;font-size:12.5px;font-weight:700;">Excluir</button>
        </div>
      </div>`;
    }
    delBlock += `<button data-mrdelreq style="cursor:pointer;border:1px solid oklch(0.55 0.16 30 / 0.4);padding:11px;border-radius:12px;background:transparent;color:oklch(0.55 0.16 30);font-size:13px;font-weight:700;width:100%;">Excluir associação</button>`;
  }

  return `
    <div data-mrcancel style="cursor:pointer;font-size:13px;font-weight:600;color:oklch(0.47 0.1 150);">‹ Modo Espelhamento</div>
    <div style="font-size:18px;font-weight:700;color:oklch(0.22 0.008 100);">${editing ? 'Editar associação' : 'Nova associação'}</div>

    ${errBox}

    <div style="background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);border-radius:16px;padding:14px 16px;display:flex;flex-direction:column;gap:14px;">
      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Porta de entrada (gateway)</div>
        <div style="display:flex;flex-wrap:wrap;gap:6px;">
          ${portPills || '<div style="font-size:12px;color:oklch(0.6 0.006 100);">Todas as portas em uso.</div>'}
        </div>
      </div>

      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Zona associada (saída no nó)</div>
        <div style="display:flex;flex-wrap:wrap;gap:6px;">
          ${zonePills || '<div style="font-size:12px;color:oklch(0.6 0.006 100);">Nenhuma zona configurada.</div>'}
        </div>
      </div>

      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Polaridade da entrada</div>
        <div style="font-size:12px;color:oklch(0.52 0.006 100);margin-bottom:8px;line-height:1.4;">Use "Invertido" quando o hardware reporta a entrada como ativa em 0 V (lógica invertida).</div>
        <div style="display:flex;gap:8px;">
          <button data-mrnormal style="cursor:pointer;flex:1;border:1.5px solid ${normalBorder};padding:9px;border-radius:8px;background:${normalBg};color:${normalColor};font-size:12.5px;font-weight:700;">Normal</button>
          <button data-mrinvertido style="cursor:pointer;flex:1;border:1.5px solid ${invBorder};padding:9px;border-radius:8px;background:${invBg};color:${invColor};font-size:12.5px;font-weight:700;">Invertido</button>
        </div>
        <div style="font-size:12px;font-weight:600;color:oklch(0.47 0.1 150);margin-top:8px;">${esc(polaridadeLabel)}</div>
      </div>

      <div data-mrhab style="cursor:pointer;display:flex;align-items:center;justify-content:space-between;gap:8px;background:oklch(0.97 0.003 100);border-radius:10px;padding:10px 12px;">
        <div style="font-size:13px;font-weight:600;color:oklch(0.22 0.008 100);">Associação habilitada</div>
        <div style="width:38px;height:22px;border-radius:999px;background:${habBg};position:relative;flex-shrink:0;">
          <div style="position:absolute;top:2px;left:${habKnob};width:18px;height:18px;border-radius:999px;background:#fff;"></div>
        </div>
      </div>
    </div>

    <button data-mrsave style="cursor:pointer;border:none;padding:13px;border-radius:12px;background:oklch(0.47 0.1 150);color:#fff;font-size:15px;font-weight:700;width:100%;">Salvar associação</button>

    ${delBlock}
  `;
}

function mrRender() {
  view.innerHTML = mrUI.screen === 'edit' ? mrEditHtml() : mrListHtml();
  mrWire();
}

function mrWire() {
  const q = (sel) => view.querySelector(sel);

  if (mrUI.screen !== 'edit') {
    // Toggle espelhamento ativo/inativo
    const tog = q('[data-mrtoggle]');
    if (tog) tog.addEventListener('click', async () => {
      const r = await postJson('/mirror', { enabled: !mrData.enabled });
      if (r.ok) renderEspelhamento().catch(() => {});
    });

    // Nova associação
    const nb = q('[data-mrnew]');
    if (nb) nb.addEventListener('click', () => mrOpenNew());

    // Editar associação (click no card, não no toggle)
    view.querySelectorAll('[data-mredit]').forEach((el) => {
      el.addEventListener('click', (e) => {
        // Não abrir edit se clicou no toggle de habilitado
        if (e.target.closest('[data-mrtoghab]')) return;
        mrOpenEdit(num(el.dataset.mredit));
      });
    });

    // Toggle habilitado inline no card da lista
    view.querySelectorAll('[data-mrtoghab]').forEach((el) => {
      el.addEventListener('click', async (e) => {
        e.stopPropagation();
        const portIdx = num(el.dataset.mrtoghab);
        const p = (mrData.ports || []).find((x) => num(x.i) === portIdx);
        if (!p) return;
        const r = await postJson('/mirror/mapping', {
          input: portIdx,
          zoneId: num(p.zoneId),
          invertido: !!p.invertido,
          habilitado: !p.habilitado,
        });
        if (r.ok) renderEspelhamento().catch(() => {});
      });
    });
    return;
  }

  // Tela de edição
  const cancel = q('[data-mrcancel]');
  if (cancel) cancel.addEventListener('click', () => { mrUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false }; mrRender(); });

  view.querySelectorAll('[data-mrport]').forEach((b) =>
    b.addEventListener('click', () => { mrUI.draft.input = num(b.dataset.mrport); mrRender(); }));
  view.querySelectorAll('[data-mrzone]').forEach((b) =>
    b.addEventListener('click', () => { mrUI.draft.zoneId = num(b.dataset.mrzone); mrRender(); }));

  const norm = q('[data-mrnormal]');
  if (norm) norm.addEventListener('click', () => { mrUI.draft.invertido = false; mrRender(); });
  const inv = q('[data-mrinvertido]');
  if (inv) inv.addEventListener('click', () => { mrUI.draft.invertido = true; mrRender(); });

  const hab = q('[data-mrhab]');
  if (hab) hab.addEventListener('click', () => { mrUI.draft.habilitado = !mrUI.draft.habilitado; mrRender(); });

  const save = q('[data-mrsave]');
  if (save) save.addEventListener('click', () => mrSave());

  const delReq = q('[data-mrdelreq]');
  if (delReq) delReq.addEventListener('click', () => { mrUI.deleteConfirm = true; mrRender(); });
  const delCancel = q('[data-mrdelcancel]');
  if (delCancel) delCancel.addEventListener('click', () => { mrUI.deleteConfirm = false; mrRender(); });
  const delConfirm = q('[data-mrdelconfirm]');
  if (delConfirm) delConfirm.addEventListener('click', () => mrDelete());
}

function mrOpenNew() {
  const freePorts = (mrData.ports || []).filter((p) => !(p.zoneId != null && p.zoneId > 0));
  if (!freePorts.length) return;
  const firstPort = freePorts[0];
  const firstZone = (Array.isArray(mrData.zones) && mrData.zones[0]) ? num(mrData.zones[0].id) : null;
  mrUI = {
    screen: 'edit', errors: [], deleteConfirm: false,
    draft: { input: num(firstPort.i), zoneId: firstZone, invertido: false, habilitado: true, isEditing: false },
  };
  mrRender();
}

function mrOpenEdit(portIdx) {
  const p = (mrData.ports || []).find((x) => num(x.i) === portIdx);
  if (!p) return;
  mrUI = {
    screen: 'edit', errors: [], deleteConfirm: false,
    draft: {
      input: num(p.i),
      zoneId: num(p.zoneId),
      invertido: !!p.invertido,
      habilitado: !!p.habilitado,
      isEditing: true,
    },
  };
  mrRender();
}

async function mrSave() {
  const d = mrUI.draft;
  const errors = [];
  if (d.input == null || d.input < 0 || d.input > 3) errors.push('Selecione uma porta de entrada.');
  if (!d.zoneId) errors.push('Selecione uma zona.');
  if (errors.length) { mrUI.errors = errors; mrRender(); return; }
  const r = await postJson('/mirror/mapping', {
    input: num(d.input),
    zoneId: num(d.zoneId),
    invertido: !!d.invertido,
    habilitado: !!d.habilitado,
  });
  if (r.ok) { mrUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false }; renderEspelhamento().catch(() => {}); }
  else { mrUI.errors = (r.body && Array.isArray(r.body.errors)) ? r.body.errors : ['Falha ao salvar.']; mrRender(); }
}

async function mrDelete() {
  const r = await postJson('/mirror/mapping/delete', { input: num(mrUI.draft.input) });
  if (r.ok) { mrUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false }; renderEspelhamento().catch(() => {}); }
  else { mrUI.deleteConfirm = false; mrUI.errors = ['Falha ao excluir.']; mrRender(); }
}

// Entrada do roteador: carrega GET /mirror e cacheia em mrData.
async function renderEspelhamento() {
  const [mirror, zones] = await Promise.all([
    getJson('/mirror').catch(() => ({ enabled: false, ports: [] })),
    getJson('/zones').catch(() => []),
  ]);
  mrData = {
    enabled: !!mirror.enabled,
    ports: Array.isArray(mirror.ports) ? mirror.ports : [],
    zones: Array.isArray(zones) ? zones : [],
  };
  // Poll apenas actualiza o cache; não reconstrói o DOM da tela de edição
  // (evita flicker e perda de estado do draft durante edição activa).
  if (mrUI.screen === 'edit') return;
  mrUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };
  mrRender();
}

// ===== Meteorologia (supressão por previsão de chuva — Open-Meteo) =====

// Estado local (screen: 'list' | 'edit', draft, errors, deleteConfirm).
let mtData = { weather: null, zones: [], groups: [] };
let mtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };

// Formata quantos segundos/min/horas atrás foi 'epochSecs' (epoch UTC em segundos).
function fmtAgo(epochSecs) {
  epochSecs = num(epochSecs);
  if (!epochSecs) return '—';
  const diffS = Math.max(0, Math.floor(Date.now() / 1000) - epochSecs);
  if (diffS < 60) return 'há ' + diffS + ' s';
  if (diffS < 3600) return 'há ' + Math.floor(diffS / 60) + ' min';
  return 'há ' + Math.floor(diffS / 3600) + ' h';
}

// Converte valor em centi-unidades para display com 1 casa decimal, fallback '—'.
function centi(v, unit) {
  if (v == null || v === '' || !Number.isFinite(Number(v))) return '—';
  return (num(v) / 100).toFixed(1) + unit;
}

// Resolve o nome de uma zona ou grupo pelo id, com fallback.
function mtZoneName(id) {
  const z = (Array.isArray(mtData.zones) ? mtData.zones : []).find((x) => x && num(x.id) === num(id));
  return z ? (z.name || z.nome || ('Zona ' + num(id))) : ('Zona ' + num(id));
}
function mtGroupName(id) {
  const g = (Array.isArray(mtData.groups) ? mtData.groups : []).find((x) => x && num(x.id) === num(id));
  return g ? (g.name || g.nome || ('Grupo ' + num(id))) : ('Grupo ' + num(id));
}

// ---- Tela lista (pure HTML string) ----
function mtListHtml() {
  const w = mtData.weather || {};
  const m = w.metrics || {};
  const rules = Array.isArray(w.rules) ? w.rules : [];

  const location = esc(w.location || '—');
  const updatedLabel = fmtAgo(w.updatedEpoch);
  const isMock = !!w.isMock;
  const mockNote = isMock ? ' · estimativa offline' : '';
  const errorBox = isMock
    ? `<div style="font-size:11.5px;color:oklch(0.55 0.16 30);">Dados simulados — sem conexão com Open-Meteo.</div>`
    : '';

  const tempAtual = (m.tempAtualCenti != null && Number.isFinite(num(m.tempAtualCenti)))
    ? (num(m.tempAtualCenti) / 100).toFixed(1) + '°C' : '—';
  const umidadeRel = (m.umidadeRel != null && Number.isFinite(num(m.umidadeRel)))
    ? num(m.umidadeRel) + '%' : '—';

  const metrics = [
    { label: 'Chuva prevista (12h)', valueLabel: centi(m.chuvaPrevista12hCenti, 'mm') },
    { label: 'Probabilidade de chuva', valueLabel: (m.probChuva != null ? num(m.probChuva) + '%' : '—') },
    { label: 'Chuva acumulada (24h)', valueLabel: centi(m.chuvaAcum24hCenti, 'mm') },
    { label: 'Umidade do solo', valueLabel: (m.umidadeSolo != null ? num(m.umidadeSolo) + '%' : '—') },
    { label: 'Temperatura mínima', valueLabel: centi(m.tempMinCenti, '°C') },
    { label: 'Temperatura máxima', valueLabel: centi(m.tempMaxCenti, '°C') },
    { label: 'Vento (rajada)', valueLabel: centi(m.ventoRajadaCenti, 'km/h') },
    { label: 'Evapotranspiração (ET0)', valueLabel: centi(m.et0Centi, 'mm') },
  ];

  const metricGrid = metrics.map((mt) => `
    <div style="background:oklch(0.97 0.003 100);border-radius:10px;padding:9px 11px;">
      <div style="font-size:10.5px;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.03em;">${esc(mt.label)}</div>
      <div style="font-size:14px;font-weight:700;color:oklch(0.22 0.008 100);margin-top:2px;">${esc(mt.valueLabel)}</div>
    </div>`).join('');

  // Status strip
  const anySuppressed = !!w.anySuppressed;
  let suppressedNames = '';
  if (anySuppressed) {
    const names = [];
    rules.filter((r) => r && r.triggered).forEach((r) => {
      (Array.isArray(r.grupoIds) ? r.grupoIds : []).forEach((gid) => {
        const n = mtGroupName(gid); if (!names.includes(n)) names.push(n);
      });
      (Array.isArray(r.zonaIds) ? r.zonaIds : []).forEach((zid) => {
        const n = mtZoneName(zid); if (!names.includes(n)) names.push(n);
      });
    });
    suppressedNames = names.join(', ') || '—';
  }
  const statusLabel = anySuppressed ? ('Suprimindo: ' + suppressedNames) : 'Sem restrição meteorológica no momento';
  const statusColor = anySuppressed ? 'oklch(0.55 0.14 230)' : 'oklch(0.52 0.006 100)';
  const statusBg = anySuppressed ? 'oklch(0.55 0.14 230 / 0.08)' : 'oklch(0.97 0.003 100)';
  const statusBorder = anySuppressed ? 'oklch(0.55 0.14 230 / 0.3)' : 'oklch(0.9 0.006 100)';

  // Rule cards
  const ruleCards = rules.map((r) => {
    r = r || {};
    const habBg = r.enabled ? 'oklch(0.47 0.1 150)' : 'oklch(0.85 0.006 100)';
    const habKnob = r.enabled ? '16px' : '2px';
    const activeBadge = r.triggered
      ? `<span style="font-size:10.5px;font-weight:700;padding:3px 8px;border-radius:999px;background:oklch(0.55 0.14 230 / 0.14);color:oklch(0.55 0.14 230);white-space:nowrap;">Suprimindo</span>`
      : '';
    const limiarMm = (num(r.limiarMmCenti) / 100).toFixed(1);
    const detalhe = `Chuva prevista (12h) > ${limiarMm}mm E probabilidade > ${num(r.limiarPct)}%`;
    const zNomes = (Array.isArray(r.zonaIds) ? r.zonaIds : []).map((id) => mtZoneName(id));
    const gNomes = (Array.isArray(r.grupoIds) ? r.grupoIds : []).map((id) => mtGroupName(id));
    const alvosParts = [];
    if (zNomes.length) alvosParts.push('Zonas: ' + zNomes.join(', '));
    if (gNomes.length) alvosParts.push('Grupos: ' + gNomes.join(', '));
    const alvosLabel = alvosParts.join(' · ') || 'Nenhum alvo selecionado';
    const chuvaAtualStr = (r.chuvaAtualCenti != null && Number.isFinite(num(r.chuvaAtualCenti)))
      ? (num(r.chuvaAtualCenti) / 100).toFixed(1) + 'mm' : '—';
    const probAtualStr = (r.probAtual != null) ? num(r.probAtual) + '%' : '—';
    const valorAtualLabel = chuvaAtualStr + ' · ' + probAtualStr;
    return `<div style="cursor:pointer;background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);box-shadow:0 1px 3px rgba(0,0,0,0.05);border-radius:16px;padding:14px 16px;" data-mtedit="${num(r.id)}">
      <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;">
        <div style="font-size:14px;font-weight:600;color:oklch(0.22 0.008 100);min-width:0;">${esc(r.nome || '')}</div>
        <div style="display:flex;align-items:center;gap:8px;flex-shrink:0;">
          ${activeBadge}
          <div data-mttog="${num(r.id)}" style="cursor:pointer;width:34px;height:20px;border-radius:999px;background:${habBg};position:relative;">
            <div style="position:absolute;top:2px;left:${habKnob};width:16px;height:16px;border-radius:999px;background:#fff;"></div>
          </div>
        </div>
      </div>
      <div style="font-size:12px;color:oklch(0.52 0.006 100);margin-top:6px;line-height:1.5;">${esc(detalhe)}</div>
      <div style="font-size:11.5px;color:oklch(0.6 0.006 100);margin-top:4px;">${esc(alvosLabel)}</div>
      <div style="font-size:11.5px;color:oklch(0.6 0.006 100);margin-top:2px;">Leitura atual: ${esc(valorAtualLabel)}</div>
    </div>`;
  }).join('');

  const emptyRules = !rules.length
    ? `<div style="font-size:12.5px;color:oklch(0.52 0.006 100);">Nenhuma regra meteorológica.</div>`
    : '';

  return `
    <div style="font-size:18px;font-weight:700;color:oklch(0.22 0.008 100);">Meteorologia</div>
    <div style="font-size:12px;color:oklch(0.52 0.006 100);margin-top:-6px;">Dados da Open-Meteo — suprime programas de zonas e grupos quando a chuva prevista e a probabilidade ultrapassam os limiares da regra</div>

    <div style="background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);box-shadow:0 1px 3px rgba(0,0,0,0.05);border-radius:16px;padding:14px 16px;display:flex;flex-direction:column;gap:10px;">
      <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;">
        <div style="min-width:0;">
          <div style="font-size:14px;font-weight:600;color:oklch(0.22 0.008 100);">${location}</div>
          <div style="font-size:11.5px;color:oklch(0.52 0.006 100);margin-top:2px;">Atualizado ${esc(updatedLabel)}${esc(mockNote)}</div>
        </div>
        <button id="mt-refresh" style="cursor:pointer;font-size:11px;font-weight:600;color:oklch(0.47 0.1 150);border:1px solid oklch(0.47 0.1 150 / 0.4);border-radius:999px;padding:4px 10px;white-space:nowrap;flex-shrink:0;background:transparent;">Atualizar</button>
      </div>
      ${errorBox}
      <div style="display:flex;gap:14px;">
        <div><div style="font-size:22px;font-weight:700;color:oklch(0.22 0.008 100);">${esc(tempAtual)}</div><div style="font-size:11px;color:oklch(0.52 0.006 100);">agora</div></div>
        <div><div style="font-size:22px;font-weight:700;color:oklch(0.22 0.008 100);">${esc(umidadeRel)}</div><div style="font-size:11px;color:oklch(0.52 0.006 100);">umidade relativa</div></div>
      </div>
      <div style="display:grid;grid-template-columns:1fr 1fr;gap:10px;">
        ${metricGrid}
      </div>
    </div>

    <div id="mt-refresh-msg" style="font-size:12px;color:oklch(0.55 0.16 30);display:none;"></div>

    <div style="background:${statusBg};border:1px solid ${statusBorder};border-radius:14px;padding:12px 14px;">
      <div style="font-size:12.5px;font-weight:700;color:${statusColor};">${esc(statusLabel)}</div>
    </div>

    <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;margin-top:6px;">
      <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;">Regras</div>
    </div>
    <button id="mt-new" style="cursor:pointer;border:1.5px dashed oklch(0.47 0.1 150 / 0.5);background:transparent;color:oklch(0.47 0.1 150);font-size:13px;font-weight:700;padding:11px;border-radius:14px;width:100%;">+ Nova regra</button>
    ${ruleCards}
    ${emptyRules}
  `;
}

// ---- Tela edição (pure HTML string) ----
function mtEditHtml() {
  const d = mtUI.draft || {};
  const errors = mtUI.errors || [];
  const isNew = !d.id;
  const title = isNew ? 'Nova regra meteorológica' : 'Editar regra';

  const errBox = errors.length
    ? `<div style="background:oklch(0.55 0.16 30 / 0.08);border:1px solid oklch(0.55 0.16 30 / 0.3);border-radius:12px;padding:10px 12px;display:flex;flex-direction:column;gap:4px;">
        ${errors.map((e) => `<div style="font-size:12px;color:oklch(0.55 0.16 30);">${esc(e)}</div>`).join('')}
      </div>`
    : '';

  const zonePills = (Array.isArray(mtData.zones) ? mtData.zones : []).map((z) => {
    const zid = num(z.id);
    const sel = (Array.isArray(d.zonaIds) ? d.zonaIds : []).indexOf(zid) !== -1;
    const nome = esc(z.name || z.nome || ('Zona ' + zid));
    const border = sel ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
    const bg = sel ? 'oklch(0.47 0.1 150)' : 'transparent';
    const color = sel ? '#fff' : 'oklch(0.4 0.006 100)';
    const shadow = sel ? '0 1px 4px oklch(0.47 0.1 150 / 0.3)' : 'none';
    return `<button data-mtzchip="${zid}" style="cursor:pointer;border:1.5px solid ${border};padding:7px 12px;border-radius:999px;background:${bg};color:${color};font-size:12.5px;font-weight:600;box-shadow:${shadow};">${nome}</button>`;
  }).join('');

  const groupPills = (Array.isArray(mtData.groups) ? mtData.groups : []).map((g) => {
    const gid = num(g.id);
    const sel = (Array.isArray(d.grupoIds) ? d.grupoIds : []).indexOf(gid) !== -1;
    const nome = esc(g.name || g.nome || ('Grupo ' + gid));
    const border = sel ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
    const bg = sel ? 'oklch(0.47 0.1 150)' : 'transparent';
    const color = sel ? '#fff' : 'oklch(0.4 0.006 100)';
    const shadow = sel ? '0 1px 4px oklch(0.47 0.1 150 / 0.3)' : 'none';
    return `<button data-mtgchip="${gid}" style="cursor:pointer;border:1.5px solid ${border};padding:7px 12px;border-radius:999px;background:${bg};color:${color};font-size:12.5px;font-weight:600;box-shadow:${shadow};">${nome}</button>`;
  }).join('');

  const habBg = d.enabled ? 'oklch(0.47 0.1 150)' : 'oklch(0.85 0.006 100)';
  const habKnob = d.enabled ? '18px' : '2px';

  const deleteSection = !isNew
    ? (mtUI.deleteConfirm
      ? `<div style="background:oklch(0.55 0.16 30 / 0.08);border:1px solid oklch(0.55 0.16 30 / 0.3);border-radius:12px;padding:12px 14px;">
          <div style="font-size:12.5px;color:oklch(0.22 0.008 100);">Excluir esta regra meteorológica?</div>
          <div style="display:flex;gap:8px;margin-top:8px;">
            <button id="mt-delcancel" style="cursor:pointer;flex:1;border:1px solid oklch(0.9 0.006 100);padding:9px;border-radius:8px;background:transparent;color:oklch(0.52 0.006 100);font-size:12.5px;font-weight:600;">Cancelar</button>
            <button id="mt-delconfirm" style="cursor:pointer;flex:1;border:none;padding:9px;border-radius:8px;background:oklch(0.55 0.16 30);color:#fff;font-size:12.5px;font-weight:700;">Excluir</button>
          </div>
        </div>`
      : '')
    + `<button id="mt-delrequest" style="cursor:pointer;border:1px solid oklch(0.55 0.16 30 / 0.4);padding:11px;border-radius:12px;background:transparent;color:oklch(0.55 0.16 30);font-size:13.5px;font-weight:700;width:100%;">Excluir regra</button>`
    : '';

  return `
    <div id="mt-back" style="cursor:pointer;display:flex;align-items:center;gap:6px;margin-bottom:2px;">
      <span style="font-size:16px;color:oklch(0.47 0.1 150);">‹</span>
      <span style="font-size:13px;font-weight:600;color:oklch(0.47 0.1 150);">Meteorologia</span>
    </div>
    <div style="font-size:18px;font-weight:700;color:oklch(0.22 0.008 100);">${esc(title)}</div>

    ${errBox}

    <div style="background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);border-radius:16px;padding:14px 16px;display:flex;flex-direction:column;gap:12px;">
      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Nome da regra</div>
        <input id="mt-nome" value="${esc(d.nome || '')}" placeholder="Ex.: Chuva forte prevista" style="width:100%;box-sizing:border-box;border:1px solid oklch(0.9 0.006 100);border-radius:8px;padding:9px 10px;font-size:14px;background:oklch(0.97 0.003 100);color:oklch(0.22 0.008 100);">
      </div>
      <div style="display:flex;gap:10px;">
        <div style="flex:1;">
          <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Chuva prevista em 12h &gt; (mm)</div>
          <input id="mt-chuva" type="number" value="${esc(String(d.limiarMm != null ? d.limiarMm : ''))}" style="width:100%;box-sizing:border-box;border:1px solid oklch(0.9 0.006 100);border-radius:8px;padding:9px 10px;font-size:14px;background:oklch(0.97 0.003 100);color:oklch(0.22 0.008 100);">
        </div>
        <div style="flex:1;">
          <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Probabilidade de chuva &gt; (%)</div>
          <input id="mt-prob" type="number" value="${esc(String(d.limiarPct != null ? d.limiarPct : ''))}" style="width:100%;box-sizing:border-box;border:1px solid oklch(0.9 0.006 100);border-radius:8px;padding:9px 10px;font-size:14px;background:oklch(0.97 0.003 100);color:oklch(0.22 0.008 100);">
        </div>
      </div>
      <div style="font-size:11px;color:oklch(0.52 0.006 100);">Suprime quando <strong>ambas</strong> as condições forem satisfeitas na consulta (2 consultas diárias à Open-Meteo).</div>
      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Zonas afetadas</div>
        <div style="display:flex;flex-wrap:wrap;gap:6px;">
          ${zonePills || '<div style="font-size:12px;color:oklch(0.6 0.006 100);">Nenhuma zona disponível.</div>'}
        </div>
      </div>
      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Grupos hidráulicos afetados</div>
        <div style="display:flex;flex-wrap:wrap;gap:6px;">
          ${groupPills || '<div style="font-size:12px;color:oklch(0.6 0.006 100);">Nenhum grupo disponível.</div>'}
        </div>
      </div>
      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Mensagem de alerta</div>
        <input id="mt-msg" value="${esc(d.mensagem || '')}" placeholder="Ex.: Chuva prevista para as próximas horas" style="width:100%;box-sizing:border-box;border:1px solid oklch(0.9 0.006 100);border-radius:8px;padding:9px 10px;font-size:14px;background:oklch(0.97 0.003 100);color:oklch(0.22 0.008 100);">
      </div>
      <div id="mt-habrow" style="cursor:pointer;display:flex;align-items:center;justify-content:space-between;gap:8px;background:oklch(0.97 0.003 100);border-radius:10px;padding:10px 12px;">
        <div style="font-size:13px;font-weight:600;color:oklch(0.22 0.008 100);">Regra habilitada</div>
        <div style="width:38px;height:22px;border-radius:999px;background:${habBg};position:relative;flex-shrink:0;">
          <div style="position:absolute;top:2px;left:${habKnob};width:18px;height:18px;border-radius:999px;background:#fff;"></div>
        </div>
      </div>
    </div>

    <button id="mt-save" style="cursor:pointer;border:none;padding:13px;border-radius:12px;background:oklch(0.47 0.1 150);color:#fff;font-size:15px;font-weight:700;width:100%;">Salvar regra</button>

    ${deleteSection}
  `;
}

// ---- Wiring (liga eventos ao DOM depois de cada render) ----
function mtWire() {
  const q = (sel) => view.querySelector(sel);

  if (mtUI.screen !== 'edit') {
    // Lista
    const nb = q('#mt-new');
    if (nb) nb.addEventListener('click', () => mtOpenNew());

    const ref = q('#mt-refresh');
    if (ref) ref.addEventListener('click', async () => {
      const r = await postJson('/weather/refresh', {});
      if (r.ok) {
        renderMeteo().catch(() => {});
      } else {
        const msg = q('#mt-refresh-msg');
        if (msg) { msg.textContent = 'Falha: ' + (r.body && r.body.reason ? r.body.reason : 'sem WiFi'); msg.style.display = ''; }
      }
    });

    view.querySelectorAll('[data-mtedit]').forEach((c) => {
      c.addEventListener('click', (e) => {
        // Impede que o clique no toggle dispare a abertura do formulário
        if (e.target.closest('[data-mttog]')) return;
        const id = num(c.dataset.mtedit);
        const rule = (Array.isArray(mtData.weather && mtData.weather.rules) ? mtData.weather.rules : [])
          .find((x) => x && num(x.id) === id);
        if (rule) mtOpenEdit(rule);
      });
    });

    view.querySelectorAll('[data-mttog]').forEach((b) => {
      b.addEventListener('click', async (e) => {
        e.stopPropagation();
        const id = num(b.dataset.mttog);
        const rule = (Array.isArray(mtData.weather && mtData.weather.rules) ? mtData.weather.rules : [])
          .find((x) => x && num(x.id) === id);
        if (!rule) return;
        const body = {
          id: id,
          nome: rule.nome,
          limiarMm: num(rule.limiarMmCenti) / 100,
          limiarPct: num(rule.limiarPct),
          zonaIds: Array.isArray(rule.zonaIds) ? rule.zonaIds : [],
          grupoIds: Array.isArray(rule.grupoIds) ? rule.grupoIds : [],
          enabled: !rule.enabled,
          mensagem: rule.mensagem || '',
        };
        await postJson('/weather/rule', body);
        renderMeteo().catch(() => {});
      });
    });
    return;
  }

  // Edição
  const back = q('#mt-back');
  if (back) back.addEventListener('click', () => mtCancel());

  const habRow = q('#mt-habrow');
  if (habRow) habRow.addEventListener('click', () => {
    mtSyncInputs();
    mtUI.draft.enabled = !mtUI.draft.enabled;
    mtRender();
  });

  view.querySelectorAll('[data-mtzchip]').forEach((b) => {
    b.addEventListener('click', () => {
      mtSyncInputs();
      const zid = num(b.dataset.mtzchip);
      const idx = mtUI.draft.zonaIds.indexOf(zid);
      if (idx >= 0) mtUI.draft.zonaIds.splice(idx, 1); else mtUI.draft.zonaIds.push(zid);
      mtRender();
    });
  });

  view.querySelectorAll('[data-mtgchip]').forEach((b) => {
    b.addEventListener('click', () => {
      mtSyncInputs();
      const gid = num(b.dataset.mtgchip);
      const idx = mtUI.draft.grupoIds.indexOf(gid);
      if (idx >= 0) mtUI.draft.grupoIds.splice(idx, 1); else mtUI.draft.grupoIds.push(gid);
      mtRender();
    });
  });

  const save = q('#mt-save');
  if (save) save.addEventListener('click', () => mtSave());

  const delReq = q('#mt-delrequest');
  if (delReq) delReq.addEventListener('click', () => { mtSyncInputs(); mtUI.deleteConfirm = true; mtRender(); });
  const delCancel = q('#mt-delcancel');
  if (delCancel) delCancel.addEventListener('click', () => { mtUI.deleteConfirm = false; mtRender(); });
  const delConfirm = q('#mt-delconfirm');
  if (delConfirm) delConfirm.addEventListener('click', () => mtDelete());
}

// Copia inputs de texto do DOM para o draft (antes de re-renders por chip/toggle).
function mtSyncInputs() {
  const d = mtUI.draft;
  if (!d) return;
  const q = (sel) => view.querySelector(sel);
  const ni = q('#mt-nome'); if (ni) d.nome = ni.value;
  const ci = q('#mt-chuva'); if (ci) d.limiarMm = ci.value;
  const pi = q('#mt-prob'); if (pi) d.limiarPct = pi.value;
  const mi = q('#mt-msg'); if (mi) d.mensagem = mi.value;
}

function mtRender() {
  view.innerHTML = mtUI.screen === 'edit' ? mtEditHtml() : mtListHtml();
  mtWire();
}

function mtOpenNew() {
  mtUI = {
    screen: 'edit', errors: [], deleteConfirm: false,
    draft: { id: null, nome: '', limiarMm: 5, limiarPct: 60, zonaIds: [], grupoIds: [], enabled: true, mensagem: '' },
  };
  mtRender();
}

function mtOpenEdit(rule) {
  mtUI = {
    screen: 'edit', errors: [], deleteConfirm: false,
    draft: {
      id: num(rule.id),
      nome: rule.nome || '',
      limiarMm: (num(rule.limiarMmCenti) / 100),
      limiarPct: num(rule.limiarPct),
      zonaIds: Array.isArray(rule.zonaIds) ? rule.zonaIds.slice() : [],
      grupoIds: Array.isArray(rule.grupoIds) ? rule.grupoIds.slice() : [],
      enabled: !!rule.enabled,
      mensagem: rule.mensagem || '',
    },
  };
  mtRender();
}

function mtCancel() {
  mtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };
  mtRender();
}

async function mtSave() {
  mtSyncInputs();
  const d = mtUI.draft;
  const errors = [];
  if (!d.nome || !String(d.nome).trim()) errors.push('Dê um nome à regra.');
  const limiarMmNum = Number(d.limiarMm);
  if (d.limiarMm === '' || d.limiarMm === null || !Number.isFinite(limiarMmNum)) errors.push('Informe o limiar de chuva prevista (mm).');
  const limiarPctNum = Number(d.limiarPct);
  if (d.limiarPct === '' || d.limiarPct === null || !Number.isFinite(limiarPctNum)) errors.push('Informe o limiar de probabilidade (%).');
  if ((!Array.isArray(d.zonaIds) || !d.zonaIds.length) && (!Array.isArray(d.grupoIds) || !d.grupoIds.length)) errors.push('Selecione ao menos uma zona ou grupo.');
  if (errors.length) { mtUI.errors = errors; mtRender(); return; }
  const body = {
    nome: String(d.nome).trim(),
    limiarMm: limiarMmNum,
    limiarPct: limiarPctNum,
    zonaIds: d.zonaIds || [],
    grupoIds: d.grupoIds || [],
    enabled: !!d.enabled,
    mensagem: String(d.mensagem || '').trim(),
  };
  if (d.id != null) body.id = num(d.id);
  const r = await postJson('/weather/rule', body);
  if (r.ok) {
    mtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };
    renderMeteo().catch(() => {});
  } else {
    mtUI.errors = (r.body && Array.isArray(r.body.errors)) ? r.body.errors : ['Falha ao salvar.'];
    mtRender();
  }
}

async function mtDelete() {
  const id = num(mtUI.draft && mtUI.draft.id);
  const r = await postJson('/weather/rule/delete', { id });
  if (r.ok) {
    mtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };
    renderMeteo().catch(() => {});
  } else {
    mtUI.deleteConfirm = false;
    mtUI.errors = ['Falha ao excluir.'];
    mtRender();
  }
}

// Ponto de entrada do roteador: carrega dados e mostra a lista.
async function renderMeteo() {
  const [weather, zones, groups] = await Promise.all([
    getJson('/weather').catch(() => null),
    getJson('/zones').catch(() => []),
    getJson('/groups').catch(() => []),
  ]);
  mtData = {
    weather: weather || {},
    zones: Array.isArray(zones) ? zones : [],
    groups: Array.isArray(groups) ? groups : [],
  };
  // Se estiver na tela de edição, não reseta o estado da edição (evita perda de draft em poll).
  if (mtUI.screen === 'edit') return;
  mtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };
  mtRender();
}

// ===== Modo Remoto (botoeira em nó/gateway → saída em outro nó/gateway) =====

// Estado do módulo
let rmtData = { status: [], associations: [], zones: [], stations: [], selfNode: null };
let rmtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };

// Cor do ponto de estado vivo.
function rmtDotColor(on) {
  return on ? 'oklch(0.47 0.1 150)' : 'oklch(0.75 0.006 100)';
}

// Formata label de um nó (node id número).
function rmtNodeLabel(nodeId, stations, selfNode) {
  if (nodeId == null) return '—';
  if (num(nodeId) === num(selfNode)) return 'Gateway (local)';
  const s = (Array.isArray(stations) ? stations : []).find((x) => num(x.node) === num(nodeId));
  return s ? (s.name || ('Nó ' + nodeHex(nodeId))) : nodeHex(nodeId);
}

// Formata label de uma zona (para card de associação).
function rmtZoneLabel(zoneId, zones, stations, selfNode) {
  const z = (Array.isArray(zones) ? zones : []).find((x) => num(x.id) === num(zoneId));
  if (!z) return 'Zona ' + num(zoneId);
  const nome = z.name || z.nome || ('Zona ' + num(z.id));
  const nodeLabel = rmtNodeLabel(z.node, stations, selfNode);
  return nodeLabel + ' · ' + nome;
}

// ---- Tela lista (pura, não toca DOM) ----
function rmtListHtml() {
  const { status, associations, zones, stations, selfNode } = rmtData;

  // Card "Testar acionamento" — cada entrada do status[] é já deduplicada por saída.
  const testRows = (Array.isArray(status) ? status : []).map((s) => {
    const label = esc(s.name || ('Zona ' + num(s.targetZoneId)));
    const on = !!s.on;
    const dotColor = rmtDotColor(on);
    const stateLabel = on ? 'ligada' : 'desligada';
    return `<div style="display:flex;align-items:center;justify-content:space-between;gap:8px;padding:10px 0;border-bottom:1px solid oklch(0.94 0.004 100);">
      <div style="min-width:0;">
        <div style="font-size:13px;font-weight:600;color:oklch(0.22 0.008 100);">${label}</div>
        <div style="display:flex;align-items:center;gap:6px;margin-top:3px;">
          <div style="width:7px;height:7px;border-radius:999px;background:${dotColor};"></div>
          <div style="font-size:11px;color:oklch(0.52 0.006 100);">${stateLabel}</div>
        </div>
      </div>
      <div style="display:flex;align-items:center;gap:8px;flex-shrink:0;">
        <button data-rmtcmd="${num(s.targetZoneId)}" style="cursor:pointer;border:1.5px solid oklch(0.47 0.1 150 / 0.4);padding:6px 14px;border-radius:999px;background:transparent;color:oklch(0.47 0.1 150);font-size:12px;font-weight:700;white-space:nowrap;">Acionar</button>
      </div>
    </div>`;
  }).join('');

  const hasTestRows = testRows.length > 0;
  const testCard = hasTestRows
    ? `<div style="background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);box-shadow:0 1px 3px rgba(0,0,0,0.05);border-radius:16px;padding:6px 16px;display:flex;flex-direction:column;">${testRows}</div>`
    : `<div style="font-size:12.5px;color:oklch(0.52 0.006 100);">Nenhuma botoeira configurada ainda.</div>`;

  // Cards de associação
  const assocCards = (Array.isArray(associations) ? associations : []).map((a) => {
    const targetLabel = esc(rmtZoneLabel(a.targetZoneId, zones, stations, selfNode));
    const triggerNodeNames = (Array.isArray(a.triggers) ? a.triggers : [])
      .map((t) => rmtNodeLabel(t.node, stations, selfNode))
      .filter((v, i, arr) => arr.indexOf(v) === i)
      .join(', ') || '—';
    const habilitado = !!a.enabled;
    const habChip = habilitado
      ? `<span style="font-size:10.5px;font-weight:700;padding:3px 8px;border-radius:999px;background:oklch(0.47 0.1 150 / 0.12);color:oklch(0.47 0.1 150);white-space:nowrap;">Habilitada</span>`
      : `<span style="font-size:10.5px;font-weight:700;padding:3px 8px;border-radius:999px;background:oklch(0.9 0.006 100);color:oklch(0.52 0.006 100);white-space:nowrap;">Desativada</span>`;
    const habBg = habilitado ? 'oklch(0.47 0.1 150)' : 'oklch(0.85 0.006 100)';
    const habKnob = habilitado ? '16px' : '2px';
    const on = !!(Array.isArray(status) ? status : []).find((s) => num(s.targetZoneId) === num(a.targetZoneId) && s.on);
    const outDotColor = rmtDotColor(on);
    const outLabel = on ? 'ligada' : 'desligada';
    return `<div style="cursor:pointer;background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);box-shadow:0 1px 3px rgba(0,0,0,0.05);border-radius:16px;padding:14px 16px;" data-rmtedit="${esc(String(a.id))}">
      <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;">
        <div style="font-size:14px;font-weight:600;color:oklch(0.22 0.008 100);min-width:0;">${targetLabel}</div>
        <div style="display:flex;align-items:center;gap:8px;flex-shrink:0;">
          ${habChip}
          <div data-rmttoghab="${esc(String(a.id))}" style="cursor:pointer;width:34px;height:20px;border-radius:999px;background:${habBg};position:relative;">
            <div style="position:absolute;top:2px;left:${habKnob};width:16px;height:16px;border-radius:999px;background:#fff;"></div>
          </div>
        </div>
      </div>
      <div style="font-size:12px;color:oklch(0.52 0.006 100);margin-top:6px;">Acionado por: ${esc(triggerNodeNames)}</div>
      <div style="display:flex;align-items:center;gap:8px;margin-top:8px;">
        <div style="width:8px;height:8px;border-radius:999px;background:${outDotColor};"></div>
        <div style="font-size:11.5px;color:oklch(0.52 0.006 100);">saída ${outLabel}</div>
      </div>
    </div>`;
  }).join('');

  const hasAssocs = assocCards.length > 0;
  const noAssocsMsg = !hasAssocs
    ? `<div style="font-size:12.5px;color:oklch(0.52 0.006 100);">Nenhuma associação configurada.</div>`
    : '';

  return `
    <div style="font-size:18px;font-weight:700;color:oklch(0.22 0.008 100);">Modo Remoto</div>
    <div style="font-size:12px;color:oklch(0.52 0.006 100);margin-top:-6px;">O aperto de um botão em um nó ou no gateway aciona uma válvula/saída em outro nó ou no gateway. A ativação depende apenas das associações configuradas abaixo.</div>

    <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;margin-top:6px;">
      <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;">Testar acionamento</div>
    </div>
    ${testCard}

    <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;margin-top:6px;">
      <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;">Associações botão(ões) → saída</div>
    </div>
    <button data-rmtnew style="cursor:pointer;border:1.5px dashed oklch(0.47 0.1 150 / 0.5);background:transparent;color:oklch(0.47 0.1 150);font-size:13px;font-weight:700;padding:11px;border-radius:14px;width:100%;">+ Nova associação</button>
    ${assocCards}
    ${noAssocsMsg}
  `;
}

// ---- Tela edição (pura, não toca DOM) ----
function rmtEditHtml() {
  const d = rmtUI.draft || {};
  const { zones, stations, selfNode } = rmtData;
  const isEditing = !!d.isEditing;
  const errors = rmtUI.errors || [];

  const errBox = errors.length
    ? `<div style="background:oklch(0.55 0.16 30 / 0.08);border:1px solid oklch(0.55 0.16 30 / 0.3);border-radius:12px;padding:10px 12px;display:flex;flex-direction:column;gap:4px;">
        ${errors.map((e) => `<div style="font-size:12px;color:oklch(0.55 0.16 30);">${esc(e)}</div>`).join('')}
      </div>`
    : '';

  // Nós disponíveis para gatilho (gateway + todas as estações)
  const allNodes = [
    { node: num(selfNode), name: 'Gateway (local)' },
    ...(Array.isArray(stations) ? stations : []).map((s) => ({ node: num(s.node), name: s.name || nodeHex(s.node) })),
  ];

  // Chips de nó para seleção de gatilho
  const triggerNodePills = allNodes.map((n) => {
    const sel = num(n.node) === num(d.triggerPickNode);
    const border = sel ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
    const bg = sel ? 'oklch(0.47 0.1 150)' : 'transparent';
    const color = sel ? '#fff' : 'oklch(0.4 0.006 100)';
    const shadow = sel ? '0 1px 4px oklch(0.47 0.1 150 / 0.3)' : 'none';
    return `<button data-rmttrignode="${num(n.node)}" style="cursor:pointer;border:1.5px solid ${border};padding:6px 11px;border-radius:999px;background:${bg};color:${color};font-size:12px;font-weight:600;box-shadow:${shadow};">${esc(n.name)}</button>`;
  }).join('');

  // Entradas do nó selecionado para gatilho (0..3)
  const triggers = Array.isArray(d.triggers) ? d.triggers : [];
  const triggerForPickNode = triggers.find((t) => num(t.node) === num(d.triggerPickNode));
  const triggerInputPills = [0, 1, 2, 3].map((idx) => {
    const sel = triggerForPickNode ? num(triggerForPickNode.inputIdx) === idx : false;
    const border = sel ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
    const bg = sel ? 'oklch(0.47 0.1 150)' : 'transparent';
    const color = sel ? '#fff' : 'oklch(0.4 0.006 100)';
    const shadow = sel ? '0 1px 4px oklch(0.47 0.1 150 / 0.3)' : 'none';
    return `<button data-rmttriginput="${idx}" style="cursor:pointer;border:1.5px solid ${border};padding:7px 12px;border-radius:999px;background:${bg};color:${color};font-size:12.5px;font-weight:600;box-shadow:${shadow};">Entrada ${idx + 1}</button>`;
  }).join('');

  // Nós para saída-alvo (gateway + estações)
  const targetNodePills = allNodes.map((n) => {
    const sel = num(n.node) === num(d.targetPickNode);
    const border = sel ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
    const bg = sel ? 'oklch(0.47 0.1 150)' : 'transparent';
    const color = sel ? '#fff' : 'oklch(0.4 0.006 100)';
    const shadow = sel ? '0 1px 4px oklch(0.47 0.1 150 / 0.3)' : 'none';
    return `<button data-rmttargetnode="${num(n.node)}" style="cursor:pointer;border:1.5px solid ${border};padding:6px 11px;border-radius:999px;background:${bg};color:${color};font-size:12px;font-weight:600;box-shadow:${shadow};">${esc(n.name)}</button>`;
  }).join('');

  // Zonas do nó-alvo selecionado
  const targetZones = (Array.isArray(zones) ? zones : []).filter((z) => num(z.node) === num(d.targetPickNode));
  const targetZonePills = targetZones.length
    ? targetZones.map((z) => {
        const nome = esc(z.name || z.nome || ('Zona ' + num(z.id)));
        const sel = num(z.id) === num(d.targetZoneId);
        const border = sel ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
        const bg = sel ? 'oklch(0.47 0.1 150)' : 'transparent';
        const color = sel ? '#fff' : 'oklch(0.4 0.006 100)';
        const shadow = sel ? '0 1px 4px oklch(0.47 0.1 150 / 0.3)' : 'none';
        return `<button data-rmtzone="${num(z.id)}" style="cursor:pointer;border:1.5px solid ${border};padding:7px 12px;border-radius:999px;background:${bg};color:${color};font-size:12.5px;font-weight:600;box-shadow:${shadow};">${nome}</button>`;
      }).join('')
    : `<div style="font-size:12px;color:oklch(0.6 0.006 100);">Nenhuma zona neste nó.</div>`;

  // LED por gatilho — para cada trigger selecionado, picker LED 1 / LED 2 / nenhum
  const ledSections = triggers.length
    ? triggers.map((t) => {
        const nodeName = esc(rmtNodeLabel(t.node, stations, selfNode));
        const curSlot = t.ledSlot != null ? num(t.ledSlot) : 255;
        const ledOpts = [
          { label: 'Nenhum', slot: 255 },
          { label: 'LED 1', slot: 0 },
          { label: 'LED 2', slot: 1 },
        ];
        const pills = ledOpts.map((opt) => {
          const sel = curSlot === opt.slot;
          const border = sel ? 'oklch(0.47 0.1 150)' : 'oklch(0.88 0.006 100)';
          const bg = sel ? 'oklch(0.47 0.1 150)' : 'transparent';
          const color = sel ? '#fff' : 'oklch(0.4 0.006 100)';
          const shadow = sel ? '0 1px 4px oklch(0.47 0.1 150 / 0.3)' : 'none';
          return `<button data-rmtled="${num(t.node)}" data-rmtledslot="${opt.slot}" style="cursor:pointer;border:1.5px solid ${border};padding:7px 12px;border-radius:999px;background:${bg};color:${color};font-size:12.5px;font-weight:600;box-shadow:${shadow};">${esc(opt.label)}</button>`;
        }).join('');
        return `<div style="margin-bottom:8px;">
          <div style="font-size:11px;color:oklch(0.6 0.006 100);margin-bottom:5px;">${nodeName}</div>
          <div style="display:flex;flex-wrap:wrap;gap:6px;">${pills}</div>
        </div>`;
      }).join('')
    : `<div style="font-size:12px;color:oklch(0.6 0.006 100);">Selecione gatilho(s) acima.</div>`;

  const habBg = d.enabled ? 'oklch(0.47 0.1 150)' : 'oklch(0.85 0.006 100)';
  const habKnob = d.enabled ? '18px' : '2px';

  let delBlock = '';
  if (isEditing) {
    if (rmtUI.deleteConfirm) {
      delBlock = `<div style="background:oklch(0.55 0.16 30 / 0.08);border:1px solid oklch(0.55 0.16 30 / 0.3);border-radius:12px;padding:12px 14px;">
        <div style="font-size:12.5px;color:oklch(0.22 0.008 100);">Excluir esta associação?</div>
        <div style="display:flex;gap:8px;margin-top:8px;">
          <button data-rmtdelcancel style="cursor:pointer;flex:1;border:1px solid oklch(0.9 0.006 100);padding:9px;border-radius:8px;background:transparent;color:oklch(0.4 0.006 100);font-size:12.5px;font-weight:600;">Cancelar</button>
          <button data-rmtdelconfirm style="cursor:pointer;flex:1;border:none;padding:9px;border-radius:8px;background:oklch(0.55 0.16 30);color:#fff;font-size:12.5px;font-weight:700;">Excluir</button>
        </div>
      </div>`;
    }
    delBlock += `<button data-rmtdelreq style="cursor:pointer;border:1px solid oklch(0.55 0.16 30 / 0.4);padding:11px;border-radius:12px;background:transparent;color:oklch(0.55 0.16 30);font-size:13px;font-weight:700;width:100%;">Excluir associação</button>`;
  }

  return `
    <div data-rmtcancel style="cursor:pointer;font-size:13px;font-weight:600;color:oklch(0.47 0.1 150);">‹ Modo Remoto</div>
    <div style="font-size:18px;font-weight:700;color:oklch(0.22 0.008 100);">${isEditing ? 'Editar associação' : 'Nova associação'}</div>

    ${errBox}

    <div style="background:oklch(1 0 0);border:1px solid oklch(0.9 0.006 100);border-radius:16px;padding:14px 16px;display:flex;flex-direction:column;gap:14px;">
      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Botões — um por nó/gateway; pode adicionar mais de um nó</div>
        <div style="font-size:10px;font-weight:700;color:oklch(0.6 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:5px;">Nó</div>
        <div style="display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px;">
          ${triggerNodePills}
        </div>
        <div style="font-size:10px;font-weight:700;color:oklch(0.6 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:5px;">Entrada</div>
        <div style="display:flex;flex-wrap:wrap;gap:6px;">
          ${triggerInputPills}
        </div>
      </div>

      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">Saída a acionar</div>
        <div style="font-size:10px;font-weight:700;color:oklch(0.6 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:5px;">Nó</div>
        <div style="display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px;">
          ${targetNodePills}
        </div>
        <div style="font-size:10px;font-weight:700;color:oklch(0.6 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:5px;">Saída</div>
        <div style="display:flex;flex-wrap:wrap;gap:6px;">
          ${targetZonePills}
        </div>
      </div>

      <div>
        <div style="font-size:11px;font-weight:700;color:oklch(0.52 0.006 100);text-transform:uppercase;letter-spacing:0.04em;margin-bottom:6px;">LED de feedback por gatilho (opcional)</div>
        <div style="font-size:12px;color:oklch(0.52 0.006 100);margin-bottom:8px;line-height:1.4;">Acende junto com a saída acionada, indicando visualmente que a botoeira foi ativada — no nó da botoeira.</div>
        ${ledSections}
      </div>

      <div data-rmthab style="cursor:pointer;display:flex;align-items:center;justify-content:space-between;gap:8px;background:oklch(0.97 0.003 100);border-radius:10px;padding:10px 12px;">
        <div style="font-size:13px;font-weight:600;color:oklch(0.22 0.008 100);">Associação habilitada</div>
        <div style="width:38px;height:22px;border-radius:999px;background:${habBg};position:relative;flex-shrink:0;">
          <div style="position:absolute;top:2px;left:${habKnob};width:18px;height:18px;border-radius:999px;background:#fff;"></div>
        </div>
      </div>
    </div>

    <button data-rmtsave style="cursor:pointer;border:none;padding:13px;border-radius:12px;background:oklch(0.47 0.1 150);color:#fff;font-size:15px;font-weight:700;width:100%;">Salvar associação</button>

    ${delBlock}
  `;
}

function rmtRender() {
  view.innerHTML = rmtUI.screen === 'edit' ? rmtEditHtml() : rmtListHtml();
  rmtWire();
}

function rmtWire() {
  const q = (sel) => view.querySelector(sel);

  if (rmtUI.screen !== 'edit') {
    // Botões "Acionar" (testar acionamento)
    view.querySelectorAll('[data-rmtcmd]').forEach((btn) => {
      btn.addEventListener('click', async (e) => {
        e.stopPropagation();
        const zid = num(btn.dataset.rmtcmd);
        await postJson('/remote/command', { targetZoneId: zid });
        renderRemoto().catch(() => {});
      });
    });

    // Nova associação
    const nb = q('[data-rmtnew]');
    if (nb) nb.addEventListener('click', () => rmtOpenNew());

    // Editar associação (click no card, não no toggle)
    view.querySelectorAll('[data-rmtedit]').forEach((el) => {
      el.addEventListener('click', (e) => {
        if (e.target.closest('[data-rmttoghab]')) return;
        rmtOpenEdit(el.dataset.rmtedit);
      });
    });

    // Toggle habilitado inline
    view.querySelectorAll('[data-rmttoghab]').forEach((el) => {
      el.addEventListener('click', async (e) => {
        e.stopPropagation();
        const id = el.dataset.rmttoghab;
        const a = (rmtData.associations || []).find((x) => String(x.id) === String(id));
        if (!a) return;
        const r = await postJson('/remote', { id: a.id, enabled: !a.enabled, targetZoneId: num(a.targetZoneId), triggers: Array.isArray(a.triggers) ? a.triggers : [] });
        if (r.ok) renderRemoto().catch(() => {});
      });
    });
    return;
  }

  // Tela de edição
  const cancel = q('[data-rmtcancel]');
  if (cancel) cancel.addEventListener('click', () => { rmtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false }; rmtRender(); });

  // Seleção do nó de gatilho
  view.querySelectorAll('[data-rmttrignode]').forEach((b) => {
    b.addEventListener('click', () => {
      rmtUI.draft.triggerPickNode = num(b.dataset.rmttrignode);
      rmtRender();
    });
  });

  // Seleção de entrada do nó de gatilho (um por nó, substitui se já existir)
  view.querySelectorAll('[data-rmttriginput]').forEach((b) => {
    b.addEventListener('click', () => {
      const inputIdx = num(b.dataset.rmttriginput);
      const pickNode = num(rmtUI.draft.triggerPickNode);
      const triggers = (rmtUI.draft.triggers || []).filter((t) => num(t.node) !== pickNode);
      // Verifica se já estava selecionado (toggle off)
      const wasSelected = (rmtUI.draft.triggers || []).some((t) => num(t.node) === pickNode && num(t.inputIdx) === inputIdx);
      if (!wasSelected) {
        triggers.push({ node: pickNode, inputIdx, ledSlot: 255 });
      }
      rmtUI.draft.triggers = triggers;
      rmtRender();
    });
  });

  // Seleção do nó de saída-alvo
  view.querySelectorAll('[data-rmttargetnode]').forEach((b) => {
    b.addEventListener('click', () => {
      rmtUI.draft.targetPickNode = num(b.dataset.rmttargetnode);
      rmtUI.draft.targetZoneId = null;
      rmtRender();
    });
  });

  // Seleção de zona-alvo
  view.querySelectorAll('[data-rmtzone]').forEach((b) => {
    b.addEventListener('click', () => {
      rmtUI.draft.targetZoneId = num(b.dataset.rmtzone);
      rmtRender();
    });
  });

  // LED por gatilho
  view.querySelectorAll('[data-rmtled]').forEach((b) => {
    b.addEventListener('click', () => {
      const nodeId = num(b.dataset.rmtled);
      const slot = num(b.dataset.rmtledslot);
      rmtUI.draft.triggers = (rmtUI.draft.triggers || []).map((t) =>
        num(t.node) === nodeId ? { ...t, ledSlot: slot } : t
      );
      rmtRender();
    });
  });

  const hab = q('[data-rmthab]');
  if (hab) hab.addEventListener('click', () => { rmtUI.draft.enabled = !rmtUI.draft.enabled; rmtRender(); });

  const save = q('[data-rmtsave]');
  if (save) save.addEventListener('click', () => rmtSave());

  const delReq = q('[data-rmtdelreq]');
  if (delReq) delReq.addEventListener('click', () => { rmtUI.deleteConfirm = true; rmtRender(); });
  const delCancel = q('[data-rmtdelcancel]');
  if (delCancel) delCancel.addEventListener('click', () => { rmtUI.deleteConfirm = false; rmtRender(); });
  const delConfirm = q('[data-rmtdelconfirm]');
  if (delConfirm) delConfirm.addEventListener('click', () => rmtDelete());
}

function rmtOpenNew() {
  const selfNode = rmtData.selfNode;
  const firstNode = selfNode != null ? num(selfNode) : 0;
  rmtUI = {
    screen: 'edit', errors: [], deleteConfirm: false,
    draft: { id: null, enabled: true, triggers: [], targetZoneId: null, targetPickNode: firstNode, triggerPickNode: firstNode, isEditing: false },
  };
  rmtRender();
}

function rmtOpenEdit(id) {
  const a = (rmtData.associations || []).find((x) => String(x.id) === String(id));
  if (!a) return;
  const selfNode = rmtData.selfNode;
  const firstNode = selfNode != null ? num(selfNode) : 0;
  const triggers = Array.isArray(a.triggers) ? a.triggers : [];
  const triggerPickNode = triggers.length ? num(triggers[0].node) : firstNode;
  const targetZone = (rmtData.zones || []).find((z) => num(z.id) === num(a.targetZoneId));
  const targetPickNode = targetZone ? num(targetZone.node) : firstNode;
  rmtUI = {
    screen: 'edit', errors: [], deleteConfirm: false,
    draft: {
      id: a.id,
      enabled: !!a.enabled,
      triggers: triggers.map((t) => ({ node: num(t.node), inputIdx: num(t.inputIdx), ledSlot: t.ledSlot != null ? num(t.ledSlot) : 255 })),
      targetZoneId: num(a.targetZoneId),
      targetPickNode,
      triggerPickNode,
      isEditing: true,
    },
  };
  rmtRender();
}

async function rmtSave() {
  const d = rmtUI.draft;
  const errors = [];
  if (!d.triggers || d.triggers.length === 0) errors.push('Selecione ao menos um gatilho.');
  if (!d.targetZoneId) errors.push('Selecione a saída a ser acionada.');
  if (errors.length) { rmtUI.errors = errors; rmtRender(); return; }
  const payload = {
    enabled: !!d.enabled,
    targetZoneId: num(d.targetZoneId),
    triggers: d.triggers.map((t) => ({ node: num(t.node), inputIdx: num(t.inputIdx), ledSlot: t.ledSlot != null ? num(t.ledSlot) : 255 })),
  };
  if (d.id != null) payload.id = d.id;
  const r = await postJson('/remote', payload);
  if (r.ok) { rmtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false }; renderRemoto().catch(() => {}); }
  else { rmtUI.errors = (r.body && Array.isArray(r.body.errors)) ? r.body.errors : ['Falha ao salvar.']; rmtRender(); }
}

async function rmtDelete() {
  const r = await postJson('/remote/delete', { id: rmtUI.draft.id });
  if (r.ok) { rmtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false }; renderRemoto().catch(() => {}); }
  else { rmtUI.deleteConfirm = false; rmtUI.errors = ['Falha ao excluir.']; rmtRender(); }
}

// Entrada do roteador: carrega GET /remote + dados auxiliares e cacheia em rmtData.
async function renderRemoto() {
  const [remoteData, overview, zones, stations] = await Promise.all([
    getJson('/remote').catch(() => ({ status: [], associations: [] })),
    getJson('/overview').catch(() => ({})),
    getJson('/zones').catch(() => []),
    getJson('/stations').catch(() => []),
  ]);
  rmtData = {
    status: Array.isArray(remoteData.status) ? remoteData.status : [],
    associations: Array.isArray(remoteData.associations) ? remoteData.associations : [],
    zones: Array.isArray(zones) ? zones : [],
    stations: Array.isArray(stations) ? stations : [],
    selfNode: overview.selfNode != null ? num(overview.selfNode) : null,
  };
  // Poll apenas atualiza o cache; não reconstrói o DOM da tela de edição.
  if (rmtUI.screen === 'edit') return;
  rmtUI = { screen: 'list', draft: null, errors: [], deleteConfirm: false };
  rmtRender();
}

// ===== "Mais" (menu de telas secundárias) =====
function renderMais() {
  const items = [
    ['remoto', 'Modo Remoto', 'Botoeira em um nó/gateway aciona saída em outro'],
    ['espelhamento', 'Modo Espelhamento', 'Entradas físicas do gateway → saídas dos nós'],
    ['grupos', 'Grupos & Intertravamentos', 'Sequenciamento de bomba e regras de bloqueio'],
    ['niveis', 'Controle de nível', 'Controle de bomba por boia flutuante'],
    ['meteo', 'Meteorologia', 'Supressão por previsão de chuva (Open-Meteo)'],
    ['sensores', 'Sensores', 'Leituras e nomes por estação'],
    ['gpo', 'Saídas (GPO)', 'Relés/MOSFET: portão, bomba auxiliar, luz, sirene'],
    ['tamper', 'Tamper / manutenção', 'Violação de gabinete e janela de manutenção'],
    ['auditlog', 'Log de auditoria', 'Histórico completo de ações e eventos'],
    ['radio', 'Enlace / Cobertura', 'SNR/RSSI dos nós e cobertura de sinal'],
    ['wifi', 'Rede Wi-Fi', 'Conectar o gateway a uma rede Wi-Fi local'],
    ['horario', 'Horário', 'Fonte de hora, fuso e sincronização dos nós'],
    ['sistema', 'Sistema', 'Backup e chave da rede'],
  ];
  view.innerHTML = items
    .map(
      ([k, t, s]) => `<div class="card navrow" data-sub="${k}">
        <div class="navinfo"><div class="name">${esc(t)}</div><div class="sub">${esc(s)}</div></div>
        <span class="chev">›</span>
      </div>`
    )
    .join('');
  view.querySelectorAll('[data-sub]').forEach((c) => c.addEventListener('click', () => showSub(c.dataset.sub)));
}

// ===== Sistema (backup + restaurar) =====
// Baixa o backup via fetch→Blob (robusto: dá feedback de erro e não depende do
// comportamento de <a download> contra o webserver embarcado).
async function downloadBackup(btn, statusEl) {
  btn.disabled = true;
  statusEl.textContent = 'Gerando backup…';
  statusEl.className = 'sub';
  try {
    const r = await fetch(API + '/export');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const text = await r.text();
    if (!text || text[0] !== '{') throw new Error('resposta vazia/inválida');
    const url = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = 'irrigacao-backup.json';
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    statusEl.textContent = 'Backup baixado (' + text.length + ' bytes).';
  } catch (e) {
    statusEl.textContent = 'Falha ao gerar backup (' + esc(e.message) + ').';
    statusEl.className = 'sub err';
  } finally {
    btn.disabled = false;
  }
}

async function restoreBackup(btn, fileInput, statusEl) {
  const file = fileInput.files[0];
  if (!file) {
    statusEl.textContent = 'Selecione um arquivo .json antes de restaurar.';
    statusEl.className = 'sub err';
    return;
  }
  btn.disabled = true;
  statusEl.textContent = 'Enviando backup…';
  statusEl.className = 'sub';
  try {
    const text = await file.text();
    if (!text || text[0] !== '{') throw new Error('arquivo inválido (não é JSON)');
    const r = await fetch(API + '/import', { method: 'POST', body: text });
    const json = await r.json();
    if (!r.ok) throw new Error(json.error || json.errors?.[0] || 'HTTP ' + r.status);
    if (!json.ok) throw new Error(json.error || 'falha no servidor');
    statusEl.textContent =
      'Restaurado: ' + json.zonas + ' zonas, ' + json.programas + ' programas, ' +
      json.intertravamentos + ' intertravamentos, ' + json.grupos + ' grupos.';
    statusEl.className = 'sub';
  } catch (e) {
    statusEl.textContent = 'Falha ao restaurar (' + esc(e.message) + ').';
    statusEl.className = 'sub err';
  } finally {
    btn.disabled = false;
  }
}

function renderSistema() {
  view.innerHTML =
    `<div class="card">
       <div class="sens-hdr"><span class="name">Backup</span></div>
       <div class="sub maint-sub">Exporta a configuração completa (§5.5): PSK, estações, zonas, programas, intertravamentos e grupos — para restaurar num gateway substituto ou cadastrar no cofre do device de serviço.</div>
       <button class="btn solid big syslink" id="dlBackup">⬇ Baixar backup (.json)</button>
       <div class="sub" id="dlStatus"></div>
     </div>
     <div class="card">
       <div class="sens-hdr"><span class="name">Restaurar</span></div>
       <div class="sub maint-sub">Importa um backup e reaplica as tabelas de configuração (zonas, programas, intertravamentos, grupos). <b>Mantém a chave da rede</b> e não reinicia. As estações não são reconfiguradas (só metadados).</div>
       <input type="file" id="restoreFile" accept=".json,application/json" class="sub">
       <button class="btn solid big syslink" id="doRestore">⬆ Restaurar backup (.json)</button>
       <div class="sub" id="restoreStatus"></div>
     </div>`;

  const btn = view.querySelector('#dlBackup');
  const status = view.querySelector('#dlStatus');
  btn.addEventListener('click', () => downloadBackup(btn, status));

  const rBtn = view.querySelector('#doRestore');
  const rFile = view.querySelector('#restoreFile');
  const rStatus = view.querySelector('#restoreStatus');
  rBtn.addEventListener('click', () => restoreBackup(rBtn, rFile, rStatus));
}

// ===== Rádio: helpers puros de render (Enlace + Cobertura) =====
// Cards por estação conhecida (fonte /stations). Qualidade por SNR quarter-dB.
function malhaCardsHtml(list) {
  if (!list || !list.length) return `<div class="card"><div class="sub">Nenhuma estação conhecida.</div></div>`;
  return list
    .map((s) => {
      s = s || {};
      const snrQ = s.snrQuarterDb;
      const snr = snrQ != null ? (num(snrQ) / 4).toFixed(1) + ' dB' : '—';
      const rssi = s.rssiDbm != null ? num(s.rssiDbm) + ' dBm' : '—';
      const vbat = s.vbatCentiV != null ? fmtVolts(s.vbatCentiV) : '—';
      const q = snrQ == null ? 'gray' : num(snrQ) >= 24 ? 'green' : num(snrQ) >= 8 ? 'amber' : 'red';
      const name = s.name ? esc(s.name) : nodeHex(s.node);
      return `<div class="card">
        <div class="sens-hdr"><span class="name">${name}</span><span class="chip ${q}">${snr}</span></div>
        <div class="row3">
          <div class="card stat"><div class="lbl">SNR</div><div class="val">${snr}</div></div>
          <div class="card stat"><div class="lbl">RSSI</div><div class="val">${rssi}</div></div>
          <div class="card stat"><div class="lbl">Bateria</div><div class="val">${vbat}</div></div>
        </div>
      </div>`;
    })
    .join('');
}

// Tabela de beacons de site survey (fonte /survey) + botão Limpar.
function coberturaTableHtml(rows) {
  const COV_ROLES = ['Estação', 'Gateway', 'Repetidor', 'Serviço'];
  const list = Array.isArray(rows) ? rows : [];
  const clearRow = `<div class="log-export-row"><button class="btn ghost sm" id="cov-clear">Limpar</button></div>`;
  const body = list.length
    ? list
        .map((r) => {
          r = r || {};
          const coord = r.coord ? `${(num(r.lat) / 1e7).toFixed(5)}, ${(num(r.lon) / 1e7).toFixed(5)}` : '—';
          return `<tr>
          <td class="mono">${esc(nodeHex(r.no))}</td>
          <td>${esc(COV_ROLES[num(r.role)] || ('papel ' + num(r.role)))}</td>
          <td class="mono">${esc(coord)}</td>
          <td>${esc((num(r.snr) / 4).toFixed(0))}</td>
          <td>${esc(String(num(r.rssi)))}</td>
          <td>${esc(fmtSince(r.idadeS))}</td>
        </tr>`;
        })
        .join('')
    : '<tr><td colspan="6" class="empty">Sem beacons recebidos.</td></tr>';
  return (
    clearRow +
    `<div class="log-table-wrap"><table class="log-table">
    <thead><tr><th>Nó</th><th>Papel</th><th>Coordenada</th><th>SNR</th><th>RSSI</th><th>Idade</th></tr></thead>
    <tbody>${body}</tbody></table></div>`
  );
}

// Toggle segmentado Enlace|Cobertura. seg: 'enlace' | 'cobertura'.
function radioSegHtml(seg) {
  const eAct = seg === 'cobertura' ? '' : ' active';
  const cAct = seg === 'cobertura' ? ' active' : '';
  return `<div class="segrow">
    <button class="seg wide${eAct}" data-radioseg="enlace">Enlace</button>
    <button class="seg wide${cAct}" data-radioseg="cobertura">Cobertura</button>
  </div>`;
}

// ===== Rádio: tela unificada (Enlace + Cobertura) =====
// Estado da aba ativa; sobrevive aos re-renders de poll (3 s).
let signalSeg = 'enlace'; // 'enlace' | 'cobertura'
async function renderRadio() {
  const seg = signalSeg === 'cobertura' ? 'cobertura' : 'enlace';
  let body;
  if (seg === 'cobertura') {
    const rows = (await getJson('/survey')) || [];
    body = coberturaTableHtml(rows);
  } else {
    const list = await getJson('/stations').catch(() => []);
    body = malhaCardsHtml(list);
  }
  view.innerHTML = radioSegHtml(seg) + body;
  view.querySelectorAll('[data-radioseg]').forEach((b) =>
    b.addEventListener('click', () => {
      signalSeg = b.dataset.radioseg;
      renderRadio().catch(() => {});
    })
  );
  if (seg === 'cobertura') {
    const cb = view.querySelector('#cov-clear');
    if (cb)
      cb.addEventListener('click', async () => {
        await postJson('/survey/clear', {});
        renderRadio().catch(() => {});
      });
  }
}

// ===== Rede Wi-Fi (gateway) — endpoints /api/portal/wifi/* =====
// Mock networks para testes de UI (descomente para ativar).
const WF_MOCK_NETWORKS = [
  { ssid: 'Casa', rssi: -45, secure: 1 },
  { ssid: 'Vizinhos', rssi: -62, secure: 1 },
  { ssid: 'Wifi Publico', rssi: -75, secure: 0 },
  { ssid: 'Irrigacao-IOT', rssi: -55, secure: 1 },
];
let wfUseMock = false;

// Fetch helpers exclusivos (não usam API = /api/irrigation).
async function wfGet(path) {
  if (wfUseMock) return wfMockGet(path);
  const r = await fetch(path);
  const j = await r.json().catch(() => ({}));
  return { ok: r.ok, status: r.status, body: j };
}
async function wfPost(path, body) {
  if (wfUseMock) return wfMockPost(path, body);
  const r = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  const j = await r.json().catch(() => ({}));
  return { ok: r.ok, status: r.status, body: j };
}

// Mock responses para testes (simula endpoints WiFi).
let wfMockState = { enabled: false, connectedSsid: null, scanInProgress: false, connectState: 'idle' };
async function wfMockGet(path) {
  await new Promise(r => setTimeout(r, 100)); // simula latência
  if (path === '/api/portal/wifi') {
    return { ok: true, status: 200, body: { enabled: wfMockState.enabled, connectedSsid: wfMockState.connectedSsid } };
  }
  if (path === '/api/portal/wifi/scan') {
    return { ok: true, status: 200, body: { scanning: wfMockState.scanInProgress, networks: wfMockState.scanInProgress ? [] : WF_MOCK_NETWORKS } };
  }
  if (path === '/api/portal/wifi/connect') {
    return { ok: true, status: 200, body: { state: wfMockState.connectState, ssid: wfMockState.connectedSsid, error: null } };
  }
  return { ok: false, status: 404, body: {} };
}
async function wfMockPost(path, body) {
  await new Promise(r => setTimeout(r, 100));
  if (path === '/api/portal/wifi/toggle') {
    wfMockState.enabled = body.enabled;
    wfMockState.scanInProgress = body.enabled;
    return { ok: true, status: 200, body: {} };
  }
  if (path === '/api/portal/wifi/scan') {
    wfMockState.scanInProgress = true;
    setTimeout(() => { wfMockState.scanInProgress = false; }, 2000);
    return { ok: true, status: 200, body: {} };
  }
  if (path === '/api/portal/wifi/connect') {
    wfMockState.connectState = 'connecting';
    wfMockState.connectedSsid = body.ssid;
    setTimeout(() => { wfMockState.connectState = 'success'; }, 2000);
    return { ok: true, status: 200, body: {} };
  }
  if (path === '/api/portal/wifi/forget') {
    wfMockState.connectedSsid = null;
    return { ok: true, status: 200, body: {} };
  }
  return { ok: false, status: 404, body: {} };
}

// Estado local do módulo Wi-Fi do painel (reiniciado a cada entrada na tela).
let wfEnabled = false;
let wfSelectedNet = null;
let wfScanTimer = null;
let wfConnectTimer = null;
let wfPollGen = 0;
let wfConnectFails = 0;

function wfClearTimers() {
  wfPollGen++;
  if (wfScanTimer)    { clearTimeout(wfScanTimer);    wfScanTimer    = null; }
  if (wfConnectTimer) { clearTimeout(wfConnectTimer); wfConnectTimer = null; }
}

// Renderiza toda a tela Wi-Fi no #view (padrão sub-screen do painel: re-render em cada transição).
// vista: 'list' | 'password' | 'connecting' | 'success' | 'error'
function wfRender(vista, extra) {
  extra = extra || {};
  // Barra de cabeçalho do toggle (comum ao estado 'list')
  const trackBg = wfEnabled ? 'var(--green)' : 'oklch(0.85 0.006 100)';
  const knobLeft = wfEnabled ? '18px' : '2px';
  const toggleHtml = `
    <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:4px;">
      <div style="font-size:18px;font-weight:700;color:var(--text);">Rede Wi-Fi</div>
      <div id="wfToggle" style="cursor:pointer;width:44px;height:26px;border-radius:999px;background:${trackBg};position:relative;flex-shrink:0;">
        <div style="position:absolute;top:2px;left:${knobLeft};width:22px;height:22px;border-radius:999px;background:#fff;box-shadow:0 1px 2px rgba(0,0,0,0.25);transition:left 0.15s;"></div>
      </div>
    </div>`;

  let body = '';
  if (vista === 'list') {
    if (!wfEnabled) {
      body = toggleHtml + `
        <div class="card" style="padding:28px 16px;text-align:center;margin-top:4px;">
          <div style="font-size:13px;color:var(--muted);">Wi-Fi desativado. Ative para buscar redes próximas.</div>
        </div>`;
    } else {
      const nets = extra.networks || [];
      const connSsid = extra.connectedSsid || null;
      const scanning = !!extra.scanning;

      let networksHtml = '';
      if (scanning) {
        networksHtml = `
          <div class="card" style="padding:24px 16px;text-align:center;margin-top:4px;">
            <div style="width:22px;height:22px;border-radius:999px;border:2.5px solid var(--border);border-top-color:var(--green);margin:0 auto 10px;animation:wifiSpin 0.8s linear infinite;"></div>
            <div style="font-size:12.5px;color:var(--muted);">Buscando redes próximas…</div>
          </div>`;
      } else if (!nets.length) {
        networksHtml = `
          <div class="card" style="padding:24px 16px;text-align:center;margin-top:4px;">
            <div style="font-size:13px;font-weight:600;color:var(--text);">Nenhuma rede encontrada</div>
            <div style="font-size:12px;color:var(--muted);margin-top:4px;">Aproxime o gateway do roteador e tente novamente.</div>
            <button id="wfRescan" style="margin-top:12px;border:1px solid var(--border);padding:9px 14px;border-radius:8px;background:transparent;color:var(--text);font-size:12.5px;font-weight:600;">Buscar novamente</button>
          </div>`;
      } else {
        const hasConn = nets.some((n) => n.ssid === connSsid);
        const hint = hasConn
          ? `<div style="font-size:11px;color:var(--muted);padding:4px 2px 0;">Toque em uma rede conectada para esquecê-la.</div>`
          : '';
        const rows = nets.map((net) => {
          const connected = net.ssid === connSsid;
          const border = connected ? 'oklch(0.47 0.1 150 / 0.35)' : 'var(--border)';
          const sig = net.rssi >= -65 ? 3 : net.rssi >= -80 ? 2 : 1;
          const lit = 'var(--green)', dim = 'oklch(0.85 0.006 100)';
          const bars = `<div style="display:flex;align-items:flex-end;gap:2px;height:14px;flex-shrink:0;">
            <div style="width:3px;height:5px;border-radius:1px;background:${sig >= 1 ? lit : dim};"></div>
            <div style="width:3px;height:9px;border-radius:1px;background:${sig >= 2 ? lit : dim};"></div>
            <div style="width:3px;height:14px;border-radius:1px;background:${sig >= 3 ? lit : dim};"></div>
          </div>`;
          const badge = connected
            ? `<span style="font-size:10.5px;font-weight:700;padding:3px 8px;border-radius:999px;background:oklch(0.47 0.1 150 / 0.12);color:var(--green);white-space:nowrap;flex-shrink:0;">Conectado</span>`
            : '';
          return `<div data-wfssid="${esc(net.ssid)}" data-wfsec="${net.secure ? 1 : 0}" data-wfconn="${connected ? 1 : 0}"
            style="cursor:pointer;background:var(--card);border:1px solid ${border};box-shadow:var(--shadow-card);border-radius:14px;padding:12px 14px;display:flex;align-items:center;justify-content:space-between;gap:10px;">
            <div style="display:flex;align-items:center;gap:10px;min-width:0;">
              ${bars}
              <div style="font-size:14px;font-weight:600;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;">${esc(net.ssid)}</div>
            </div>
            ${badge}
          </div>`;
        }).join('');
        networksHtml = `<div style="display:flex;flex-direction:column;gap:8px;margin-top:4px;">${rows}</div>${hint}`;
      }

      body = toggleHtml + `
        <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;margin:8px 0 4px;">
          <div style="font-size:11px;font-weight:700;color:var(--muted);text-transform:uppercase;letter-spacing:0.04em;">Redes disponíveis</div>
          <span id="wfRefresh" style="cursor:pointer;font-size:11px;font-weight:600;color:var(--green);">Atualizar</span>
        </div>
        ${networksHtml}`;
    }
  } else if (vista === 'password') {
    body = `
      <div style="font-size:18px;font-weight:700;color:var(--text);margin-bottom:2px;">${esc((wfSelectedNet || {}).ssid || '')}</div>
      <div style="font-size:12px;color:var(--muted);margin-bottom:12px;">Digite a senha da rede para conectar o gateway</div>
      <div class="card" style="padding:14px 16px;display:flex;flex-direction:column;gap:10px;">
        <input type="password" id="wfPskInput" placeholder="Senha (mín. 8 caracteres)"
          style="border:1px solid var(--border);border-radius:8px;padding:10px 11px;font-size:14px;background:oklch(0.97 0.003 100);color:var(--text);font-family:inherit;width:100%;box-sizing:border-box;" />
        <div style="display:flex;gap:8px;">
          <button id="wfPskCancel" style="flex:1;border:1px solid var(--border);padding:10px;border-radius:8px;background:transparent;color:var(--muted);font-size:13px;font-weight:600;">Cancelar</button>
          <button id="wfPskConnect" style="flex:1;border:none;padding:10px;border-radius:8px;background:var(--green);color:#fff;font-size:13px;font-weight:700;">Conectar</button>
        </div>
      </div>`;
  } else if (vista === 'connecting') {
    body = `
      <div class="card" style="padding:40px 16px;text-align:center;margin-top:40px;">
        <div style="width:28px;height:28px;border-radius:999px;border:3px solid var(--border);border-top-color:var(--green);margin:0 auto 14px;animation:wifiSpin 0.8s linear infinite;"></div>
        <div style="font-size:14px;font-weight:600;color:var(--text);">Conectando a ${esc((wfSelectedNet || {}).ssid || '')}…</div>
      </div>`;
  } else if (vista === 'success') {
    body = `
      <div class="card" style="border-color:oklch(0.47 0.1 150 / 0.35);padding:32px 20px;text-align:center;margin-top:24px;">
        <div style="width:44px;height:44px;border-radius:999px;background:oklch(0.47 0.1 150 / 0.12);display:flex;align-items:center;justify-content:center;margin:0 auto 14px;">
          <span style="color:var(--green);font-size:22px;font-weight:700;">✓</span>
        </div>
        <div style="font-size:15px;font-weight:700;color:var(--text);">Conectado a ${esc(extra.ssid || '')}</div>
        <div style="font-size:12px;color:var(--muted);margin-top:4px;">O gateway já está na rede local.</div>
        <button id="wfSuccessBtn" style="margin-top:18px;border:none;padding:11px 20px;border-radius:8px;background:var(--green);color:#fff;font-size:13px;font-weight:700;">Concluído</button>
      </div>`;
  } else if (vista === 'error') {
    const msg = esc(extra.msg || 'Falha ao conectar. Verifique a senha e tente novamente.');
    body = `
      <div class="card redbox" style="margin-top:16px;">
        <div style="font-size:14px;font-weight:700;color:var(--red);">Falha ao conectar</div>
        <div style="font-size:12.5px;color:var(--text);margin-top:4px;">${msg}</div>
        <div style="display:flex;gap:8px;margin-top:14px;">
          <button id="wfErrCancel" style="flex:1;border:1px solid var(--border);padding:10px;border-radius:8px;background:transparent;color:var(--muted);font-size:13px;font-weight:600;">Cancelar</button>
          <button id="wfErrRetry" style="flex:1;border:none;padding:10px;border-radius:8px;background:var(--red);color:#fff;font-size:13px;font-weight:700;">Tentar novamente</button>
        </div>
      </div>`;
  }

  view.innerHTML = `<div style="display:flex;flex-direction:column;gap:12px;">${body}</div>`;
  wfWire(vista, extra);
}

function wfWire(vista, extra) {
  extra = extra || {};
  const q = (id) => view.querySelector('#' + id);

  if (vista === 'list') {
    const toggle = q('wfToggle');
    if (toggle) toggle.addEventListener('click', () => wfDoToggle());
    const refresh = q('wfRefresh');
    if (refresh) refresh.addEventListener('click', () => wfStartScan());
    const rescan = q('wfRescan');
    if (rescan) rescan.addEventListener('click', () => wfStartScan());
    view.querySelectorAll('[data-wfssid]').forEach((el) => {
      el.addEventListener('click', () => {
        if (+el.dataset.wfconn) { wfDoForget(); return; }
        const ssid = el.dataset.wfssid;
        const secure = +el.dataset.wfsec;
        wfSelectedNet = (extra.networks || []).find((n) => n.ssid === ssid) || { ssid, rssi: 0, secure: !!secure };
        if (!secure) {
          wfDoConnect('');
        } else {
          wfRender('password');
        }
      });
    });
  } else if (vista === 'password') {
    const cancel = q('wfPskCancel');
    if (cancel) cancel.addEventListener('click', () => { wfSelectedNet = null; wfStartScan(); });
    const connect = q('wfPskConnect');
    if (connect) connect.addEventListener('click', () => {
      const psk = (q('wfPskInput') || {}).value || '';
      wfDoConnect(psk);
    });
  } else if (vista === 'success') {
    const btn = q('wfSuccessBtn');
    if (btn) btn.addEventListener('click', () => { wfSelectedNet = null; wfStartScan(); });
  } else if (vista === 'error') {
    const cancel = q('wfErrCancel');
    if (cancel) cancel.addEventListener('click', () => { wfSelectedNet = null; wfStartScan(); });
    const retry = q('wfErrRetry');
    if (retry) retry.addEventListener('click', () => wfRender('password'));
  }
}

async function wfDoToggle() {
  wfClearTimers();
  wfEnabled = !wfEnabled;
  // Render optimista imediato antes do POST
  wfRender('list', { scanning: wfEnabled, networks: [], connectedSsid: null });
  const r = await wfPost('/api/portal/wifi/toggle', { enabled: wfEnabled });
  if (!r.ok) {
    const msg = r.status === 403
      ? ((r.body.errors || []).join('; ') || 'portal fechado — pressione o botao do gateway')
      : 'Falha ao alterar estado Wi-Fi.';
    wfEnabled = !wfEnabled; // reverte
    wfRender('error', { msg });
    return;
  }
  if (wfEnabled) {
    wfStartScan();
  } else {
    wfRender('list', { networks: [], connectedSsid: null });
  }
}

async function wfDoForget() {
  const r = await wfPost('/api/portal/wifi/forget', {});
  if (!r.ok) {
    const msg = r.status === 403
      ? ((r.body.errors || []).join('; ') || 'portal fechado — pressione o botao do gateway')
      : 'Falha ao esquecer a rede.';
    wfRender('error', { msg });
    return;
  }
  wfStartScan();
}

async function wfStartScan() {
  wfClearTimers();
  const myGen = wfPollGen;
  wfRender('list', { scanning: true, networks: [], connectedSsid: null });
  await wfPost('/api/portal/wifi/scan', {});
  wfPollScan(myGen);
}

async function wfPollScan(myGen) {
  const r = await wfGet('/api/portal/wifi/scan');
  if (myGen !== wfPollGen) return;
  if (!r.ok) { wfRender('list', { networks: [], connectedSsid: null }); return; }
  if (r.body.scanning) {
    wfScanTimer = setTimeout(() => wfPollScan(myGen), 1500);
    return;
  }
  // Scan concluído — busca SSID conectado
  const st = await wfGet('/api/portal/wifi');
  if (myGen !== wfPollGen) return;
  const connSsid = (st.ok && st.body.connectedSsid) || null;
  wfRender('list', { networks: r.body.networks || [], connectedSsid: connSsid });
}

async function wfDoConnect(psk) {
  wfClearTimers();
  wfConnectFails = 0;
  const ssid = (wfSelectedNet || {}).ssid || '';
  const myGen = wfPollGen;
  wfRender('connecting');
  await wfPost('/api/portal/wifi/connect', { ssid, psk });
  wfPollConnect(ssid, myGen);
}

async function wfPollConnect(ssid, myGen) {
  const r = await wfGet('/api/portal/wifi/connect');
  if (myGen !== wfPollGen) return;
  if (!r.ok) {
    wfConnectFails++;
    if (wfConnectFails >= 20) {
      wfRender('error', { msg: 'Sem resposta do dispositivo.' });
      return;
    }
    wfConnectTimer = setTimeout(() => wfPollConnect(ssid, myGen), 1500);
    return;
  }
  wfConnectFails = 0;
  const state = r.body.state;
  if (state === 'connecting' || state === 'idle') {
    wfConnectTimer = setTimeout(() => wfPollConnect(ssid, myGen), 1500);
    return;
  }
  if (state === 'success') {
    wfRender('success', { ssid: r.body.ssid || ssid });
  } else {
    wfRender('error', { msg: r.body.error || 'Falha ao conectar. Verifique a senha e tente novamente.' });
  }
}

// Entrada principal do roteador: carrega estado inicial e decide a vista.
async function renderWifi() {
  wfClearTimers();
  wfSelectedNet = null;
  wfConnectFails = 0;
  const r = await wfGet('/api/portal/wifi');
  if (!r.ok) {
    wfEnabled = false;
    wfRender('list', { networks: [], connectedSsid: null });
    return;
  }
  wfEnabled = !!r.body.enabled;
  if (wfEnabled) {
    await wfStartScan();
  } else {
    wfRender('list', { networks: [], connectedSsid: null });
  }
}

// ===== Horário (relógio do gateway) — endpoints /api/irrigation/time* =====
const TZ_PRESETS = [
  ['America/Sao_Paulo', '<-03>3'],
  ['America/Manaus', '<-04>4'],
  ['America/Rio_Branco', '<-05>5'],
  ['America/Noronha', '<-02>2'],
  ['UTC', 'GMT0'],
];

function fmtEpochLocal(epoch) {
  if (!epoch) return '—';
  const d = new Date(epoch * 1000);
  const p = (n) => String(n).padStart(2, '0');
  return `${p(d.getDate())}/${p(d.getMonth() + 1)}/${d.getFullYear()} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

// POSIX TZ → rótulo GMT±X. No POSIX o offset é positivo a oeste de UTC
// (`<-03>3` = 3h a oeste), então o GMT real é o sinal invertido → GMT-3.
function gmtFromPosix(posix) {
  if (!posix || posix === 'GMT0') return 'GMT+0';
  const m = posix.match(/>([+-]?\d{1,2})/);
  if (!m) return '';
  const off = -parseInt(m[1], 10);
  return 'GMT' + (off >= 0 ? '+' : '') + off;
}

async function renderHorario() {
  const t = (await getJson('/time').catch(() => ({}))) || {};
  const stations = (await getJson('/stations').catch(() => [])) || [];
  const isNtp = t.source === 'ntp';
  const lastSync =
    t.lastSyncS == null || t.lastSyncS < 0
      ? 'nunca'
      : t.lastSyncS === 0
      ? 'agora mesmo'
      : fmtSince(t.lastSyncS);

  const tzRows = TZ_PRESETS.map(
    ([label, posix]) =>
      `<button class="tzrow${t.tz === posix ? ' sel' : ''}" data-tz="${esc(posix)}"><span>${esc(label)}</span><span class="gmt">${gmtFromPosix(posix)}</span></button>`
  ).join('');

  const devRows =
    (Array.isArray(stations) ? stations : [])
      .map((s) => {
        const [cls, lbl] = stationSyncLabel(s.sync);
        const nm = s.name ? esc(s.name) : nodeHex(s.node);
        return `<div class="hr-dev">
          <div class="hr-dev-main">
            <div class="nm">${nm}</div>
            <div class="mt">Estação · ${nodeHex(s.node)}</div>
          </div>
          <span class="chip ${cls}">${esc(lbl)}</span>
        </div>`;
      })
      .join('') || '<div class="sub">Nenhuma estação conhecida.</div>';

  view.innerHTML = `
    <div class="card stack">
      <div>
        <div class="sens-hdr"><span class="name">Relógio do gateway</span></div>
        <div class="sub">Agora: ${fmtEpochLocal(t.nowEpoch)} · ${esc(t.tzLabel || '—')} · ${gmtFromPosix(t.tz)}</div>
      </div>
      <div>
        <div class="fp-lbl">Fonte de hora</div>
        <div class="fp-seg" id="srcSeg">
          <button data-src="ntp" class="${isNtp ? 'sel' : ''}">NTP (automática)</button>
          <button data-src="manual" class="${!isNtp ? 'sel' : ''}">Manual</button>
        </div>
      </div>
      <div id="srcBody"></div>
      <span id="timeMsg" class="sub"></span>
    </div>
    <div class="card">
      <div class="sens-hdr"><span class="name">Fuso horário</span></div>
      <div class="tzlist">${tzRows}</div>
    </div>
    <div class="card">
      <div class="sens-hdr"><span class="name">Sincronização por dispositivo</span></div>
      <div class="sub hr-sync-note">Epoch de config de cada estação — reflete se recebeu o horário/config mais recente.</div>
      <div class="hr-devs">${devRows}</div>
    </div>`;

  const srcBody = view.querySelector('#srcBody');
  const msg = view.querySelector('#timeMsg');
  function paintSource(src) {
    if (src === 'ntp') {
      srcBody.innerHTML = `
        <div class="hr-box">
          <div class="fp-kv"><span class="k">Servidor</span><span class="v">${esc(t.ntpServer || '—')}</span></div>
          <div class="fp-kv"><span class="k">Última sincronização</span><span class="v">${esc(lastSync)}</span></div>
        </div>
        ${t.staUp ? '<button class="btn ghost sm" id="syncNow">Sincronizar agora</button>' : '<div class="sub">Sem WiFi — NTP indisponível.</div>'}`;
      const sn = srcBody.querySelector('#syncNow');
      if (sn)
        sn.addEventListener('click', async () => {
          const r = await postJson('/time/sync', {});
          msg.textContent = r.ok ? 'Sincronização NTP disparada.' : 'Falha: ' + (r.body.reason || 'sem WiFi');
          setTimeout(() => renderHorario().catch(() => {}), 1500);
        });
    } else {
      const nowIso = t.nowEpoch ? new Date(t.nowEpoch * 1000) : new Date();
      const p = (n) => String(n).padStart(2, '0');
      const dv = `${nowIso.getFullYear()}-${p(nowIso.getMonth() + 1)}-${p(nowIso.getDate())}`;
      const tv = `${p(nowIso.getHours())}:${p(nowIso.getMinutes())}`;
      srcBody.innerHTML = `
        <div class="hr-manual">
          <label>Data<input type="date" id="mDate" value="${dv}"></label>
          <label>Hora<input type="time" id="mTime" value="${tv}"></label>
        </div>
        <button class="btn solid sm" id="mSet">Definir data e hora</button>`;
      srcBody.querySelector('#mSet').addEventListener('click', async () => {
        const d = srcBody.querySelector('#mDate').value;
        const h = srcBody.querySelector('#mTime').value;
        if (!d || !h) {
          msg.textContent = 'Preencha data e hora.';
          return;
        }
        const epoch = Math.floor(new Date(`${d}T${h}:00`).getTime() / 1000);
        const r = await postJson('/time', { epoch });
        msg.textContent = r.ok ? 'Data e hora aplicadas.' : 'Falha: ' + (r.body.reason || 'erro');
        setTimeout(() => renderHorario().catch(() => {}), 1500);
      });
    }
  }
  paintSource(isNtp ? 'ntp' : 'manual');
  view.querySelectorAll('#srcSeg button').forEach((b) =>
    b.addEventListener('click', () => {
      view.querySelectorAll('#srcSeg button').forEach((x) => x.classList.toggle('sel', x === b));
      paintSource(b.dataset.src);
    })
  );
  view.querySelectorAll('.tzrow').forEach((b) =>
    b.addEventListener('click', async () => {
      const r = await postJson('/timezone', { tz: b.dataset.tz });
      msg.textContent = r.ok ? 'Fuso atualizado.' : 'Falha ao definir fuso.';
      setTimeout(() => renderHorario().catch(() => {}), 800);
    })
  );
}

// ===== Roteamento =====
const RENDER = {
  overview: renderOverview,
  stations: renderStations,
  zones: renderZones,
  programs: renderPrograms,
  sensores: renderSensores,
  gpo: renderGpo,
  grupos: renderGruposInterlocks,
  niveis: renderNiveis,
  remoto: renderRemoto,
  espelhamento: renderEspelhamento,
  auditlog: renderAuditLog,
  tamper: renderTamper,
  radio: renderRadio,
  wifi: renderWifi,
  horario: renderHorario,
  meteo: renderMeteo,
  mais: renderMais,
  sistema: renderSistema,
};

// Rótulo mostrado na barra de volta ao entrar numa tela secundária via "Mais".
const SECTION_LABELS = {
  remoto: 'Modo Remoto',
  espelhamento: 'Espelhamento', grupos: 'Grupos & Intertravamentos', niveis: 'Nível',
  sensores: 'Sensores', gpo: 'Saídas (GPO)', tamper: 'Tamper', auditlog: 'Log', radio: 'Enlace / Cobertura',
  wifi: 'Rede Wi-Fi', horario: 'Horário', meteo: 'Meteorologia', sistema: 'Sistema',
};
// Telas que se auto-atualizam (poll 3 s) via re-render completo.
const POLLED = { overview: 1, stations: 1, sensores: 1, radio: 1, remoto: 1, espelhamento: 1, meteo: 1 };

const subbar = document.getElementById('subbar');
const subTitle = document.getElementById('subTitle');
document.getElementById('subBack').addEventListener('click', () => show('mais'));

function setActiveTab(tab) {
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.dataset.tab === tab));
}
function clearTimer() {
  if (timer) {
    clearInterval(timer);
    timer = null;
  }
  // Cancela polls Wi-Fi em voo ao sair de qualquer tela (gen guard invalida callbacks pendentes).
  wfClearTimers();
}

// Rota primária (botão da barra inferior).
async function show(tab) {
  current = tab;
  setActiveTab(tab);
  subbar.classList.add('hidden');
  clearTimer();
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
  if (POLLED[tab]) timer = setInterval(() => fn().catch(() => {}), 3000);
}

// Sub-rota (item dentro de "Mais"). Mantém a aba "Mais" ativa e mostra a barra de volta.
// O #subbar é irmão do #view, então sobrevive aos re-renders de poll.
async function showSub(name) {
  current = name;
  setActiveTab('mais');
  subTitle.textContent = SECTION_LABELS[name] || '';
  subbar.classList.remove('hidden');
  clearTimer();
  const fn = RENDER[name];
  if (!fn) {
    view.innerHTML = '<div class="empty">Em breve.</div>';
    return;
  }
  try {
    await fn();
  } catch (e) {
    view.innerHTML = '<div class="empty">Erro ao carregar (' + esc(e.message) + ').</div>';
  }
  if (name === 'grupos') timer = setInterval(() => pollGroupStatus().catch(() => {}), 3000);
  else if (POLLED[name]) timer = setInterval(() => fn().catch(() => {}), 3000);
}

document.querySelectorAll('.tab').forEach((t) => {
  if (t.disabled) return;
  t.addEventListener('click', () => show(t.dataset.tab));
});

const timeBadgeEl = document.getElementById('timeBadge');
if (timeBadgeEl) timeBadgeEl.addEventListener('click', () => showSub('horario'));

// Dev: ativar mock WiFi via console ou URL. Ex: wfToggleMock() ou ?wf-mock=1
function wfToggleMock() {
  wfUseMock = !wfUseMock;
  console.log(`WiFi mock ${wfUseMock ? 'ativado' : 'desativado'}`);
  wfMockState = { enabled: false, connectedSsid: null, scanInProgress: false, connectState: 'idle' };
}
if (new URLSearchParams(window.location.search).has('wf-mock')) {
  wfToggleMock();
}

show('overview');
