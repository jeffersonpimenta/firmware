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

// ===== Zonas =====
const TIPO_LABEL = { 0: 'Válvula', 1: 'GPO' };

// Nome amigável de uma estação a partir do nó (usa /stations; fallback hex).
function stationName(stations, node) {
  const s = (Array.isArray(stations) ? stations : []).find((x) => x && num(x.node) === num(node));
  return s && s.name ? esc(s.name) : nodeHex(node);
}

async function renderZones() {
  const [zones, stations] = await Promise.all([getJson('/zones'), getJson('/stations').catch(() => [])]);
  const rows = Array.isArray(zones) ? zones : [];

  const cards = rows
    .map((z) => {
      z = z || {};
      const meta =
        stationName(stations, z.node) + ' · saída ' + num(z.index) + ' · ' + (TIPO_LABEL[num(z.tipo)] || '—');
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
    </div>`;
    })
    .join('');

  view.innerHTML =
    `<button class="btn dashed" data-znew>+ Nova zona</button>` +
    (cards || '<div class="empty">Nenhuma zona configurada.</div>');

  view.querySelector('[data-znew]').addEventListener('click', () => zoneEditForm(null, rows, stations));
  view.querySelectorAll('[data-zedit]').forEach((b) => {
    b.addEventListener('click', () => {
      const z = rows.find((x) => x && num(x.id) === num(b.dataset.zedit));
      zoneEditForm(z || null, rows, stations);
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
function zoneEditForm(z, rows, stations) {
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
  let errors = [];
  let confirmDel = false;

  function render() {
    const errBox = errors.length
      ? `<div class="card redbox">${errors.map((e) => `<div>${esc(e)}</div>`).join('')}</div>`
      : '';

    const stationChips = sts.length
      ? sts
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
             <button class="seg wide${st.tipo === 1 ? ' active' : ''}" data-tipo="1">GPO biestável</button>
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

// Mapa extensível: programs adicionado na Task 13.
const RENDER = {
  overview: renderOverview,
  stations: renderStations,
  zones: renderZones,
  programs: renderPrograms,
};

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
