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

// ===== Labels de auditoria — contrato 3-vias com AuditLog.h (append-only; não reordenar) =====
// AuditOrigin: SISTEMA=0,CRONOGRAMA=1,PAINEL=2,PORTAL_CAMPO=3,BOTAO_FISICO=4,ENTRADA_FISICA=5,INTERTRAVAMENTO=6,FAILSAFE_TIMER=7,SERVICO=8
const ORIGENS_LABEL = ['Sistema','Cronograma','Painel','Portal campo','Botão físico','Entrada física','Intertravamento','Failsafe timer','Serviço'];
// AuditAction: ABRIR=0,FECHAR=1,PULSO=2,GPO_ON=3,GPO_OFF=4,PAREAR=5,FACTORY_RESET=6,CONFIG_EPOCH=7,SAFE_MODE_IN=8,SAFE_MODE_OUT=9,TAMPER=10,REBOOT=11,HIBERNA_IN=12,HIBERNA_OUT=13,CMD_REJEITADO=14
const ACOES_LABEL = ['Abrir','Fechar','Pulso','GPO ligar','GPO desligar','Parear','Factory reset','Config epoch','Safe mode in','Safe mode out','Tamper','Reboot','Hibernar in','Hibernar out','Cmd rejeitado'];
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
async function renderIntertravamentos() {
  const [rules, stations, zones] = await Promise.all([
    getJson('/interlocks'),
    getJson('/stations').catch(() => []),
    getJson('/zones').catch(() => []),
  ]);
  const rows = Array.isArray(rules) ? rules : [];
  const sts = Array.isArray(stations) ? stations : [];
  const zs = Array.isArray(zones) ? zones : [];

  const ruleCards = rows.map((r) => {
    r = r || {};
    const tipo = INTERLOCK_TIPO_LABEL[num(r.tipo)] || ('tipo ' + num(r.tipo));
    const acao = INTERLOCK_ACAO_LABEL[num(r.acao)] || ('ação ' + num(r.acao));
    let desc = '';
    if (num(r.tipo) === 0) {
      const cond = INTERLOCK_COND_LABEL[num(r.condicao)] || ('cond ' + num(r.condicao));
      desc = `${stationName(sts, r.node)} · sensor ${num(r.sensor)} · ${cond}`;
      if (num(r.condicao) >= 2) desc += ` ${(num(r.valor) / 100).toFixed(2)} (hist. ${(num(r.histerese) / 100).toFixed(2)})`;
    } else {
      desc = `máx. ${num(r.maxAbertas)} abertas`;
    }
    const zonasStr = r.todas ? 'Todas as zonas' : ('Zonas: ' + (Array.isArray(r.zonas) ? r.zonas.join(', ') : ''));
    return `<div class="card itl-card">
      <div class="itl-row">
        <div class="itl-info">
          <div class="name">${esc(r.mensagem || tipo)}</div>
          <div class="sub">${esc(desc)} · ${esc(acao)}</div>
          <div class="sub">${esc(zonasStr)}</div>
        </div>
        <div class="zbtns">
          <button class="btn ghost sm" data-itledit="${num(r.id)}">Editar</button>
          <button class="btn dangerline sm" data-itldel="${num(r.id)}">Excluir</button>
        </div>
      </div>
    </div>`;
  }).join('');

  view.innerHTML =
    `<button class="btn dashed" data-itlnew>+ Nova regra</button>` +
    (ruleCards || '<div class="empty">Nenhuma regra de intertravamento.</div>');

  view.querySelector('[data-itlnew]').addEventListener('click', () => interlockForm(null, sts, zs, rows));

  view.querySelectorAll('[data-itledit]').forEach((b) => {
    b.addEventListener('click', () => {
      const rule = rows.find((x) => x && num(x.id) === num(b.dataset.itledit));
      interlockForm(rule || null, sts, zs, rows);
    });
  });

  view.querySelectorAll('[data-itldel]').forEach((b) => {
    b.addEventListener('click', async () => {
      if (!confirm('Excluir esta regra?')) return;
      const r = await postJson('/interlocks/delete', { id: num(b.dataset.itldel) });
      if (r.ok) {
        renderIntertravamentos().catch(() => {});
      } else {
        alert('Falha ao excluir: ' + (Array.isArray(r.body && r.body.errors) ? r.body.errors.join(', ') : 'erro'));
      }
    });
  });
}

// Menor id livre em 1..255 (backend exige id != 0; regras novas não têm id).
function nextInterlockId(allRules) {
  const used = new Set((Array.isArray(allRules) ? allRules : []).map((r) => num(r && r.id)));
  for (let i = 1; i <= 255; i++) if (!used.has(i)) return i;
  return 0; // tabela cheia
}

function interlockForm(rule, stations, zones, allRules) {
  const editing = !!rule;
  const sts = Array.isArray(stations) ? stations : [];
  const zs = Array.isArray(zones) ? zones : [];
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

  function render() {
    const errBox = errors.length
      ? `<div class="card redbox">${errors.map((e) => `<div>${esc(e)}</div>`).join('')}</div>`
      : '';

    const stationChips = sts.length
      ? sts.map((s) => {
          s = s || {};
          const active = num(s.node) === st.node ? ' active' : '';
          return `<button class="pill${active}" data-inode="${num(s.node)}">${s.name ? esc(s.name) : nodeHex(s.node)}</button>`;
        }).join('')
      : '<div class="empty">Nenhuma estação.</div>';

    const tipoSegs = [0, 1].map((t) =>
      `<button class="seg wide${st.tipo === t ? ' active' : ''}" data-itipo="${t}">${INTERLOCK_TIPO_LABEL[t]}</button>`
    ).join('');

    const condSegs = [0, 1, 2, 3].map((c) =>
      `<button class="seg${st.condicao === c ? ' active' : ''}" data-icond="${c}">${INTERLOCK_COND_LABEL[c]}</button>`
    ).join('');

    const acaoSegs = [0, 1].map((a) =>
      `<button class="seg wide${st.acao === a ? ' active' : ''}" data-iacao="${a}">${INTERLOCK_ACAO_LABEL[a]}</button>`
    ).join('');

    const sensorBlock = st.tipo === 0 ? `
      <div class="fld">
        <span class="flbl">Estação</span>
        <div class="pills">${stationChips}</div>
      </div>
      <div class="frow">
        <label class="fld">
          <span class="flbl">Sensor (0-3)</span>
          <input class="finput" id="itl-sensor" type="number" min="0" max="3" value="${st.sensor}">
        </label>
        <label class="fld">
          <span class="flbl">Condição</span>
          <div class="segrow">${condSegs}</div>
        </label>
      </div>
      ${st.condicao >= 2 ? `
      <div class="frow">
        <label class="fld">
          <span class="flbl">Valor (centi-unid.)</span>
          <input class="finput" id="itl-valor" type="number" value="${st.valor}">
        </label>
        <label class="fld">
          <span class="flbl">Histerese (centi-unid.)</span>
          <input class="finput" id="itl-histerese" type="number" value="${st.histerese}">
        </label>
      </div>` : ''}
    ` : `
      <label class="fld">
        <span class="flbl">Máx. zonas abertas</span>
        <input class="finput" id="itl-maxAbertas" type="number" min="1" max="255" value="${st.maxAbertas}">
      </label>
    `;

    view.innerHTML =
      `<div class="backlink" data-itlback>‹ Intertravamentos</div>
       <div class="ztitle">${editing ? 'Editar regra' : 'Nova regra'}</div>
       ${errBox}
       <div class="card form">
         <div class="fld">
           <span class="flbl">Tipo</span>
           <div class="segrow">${tipoSegs}</div>
         </div>
         ${sensorBlock}
         <div class="fld">
           <span class="flbl">Ação</span>
           <div class="segrow">${acaoSegs}</div>
         </div>
         <div class="fld">
           <span class="flbl">Zonas afetadas (ids separados por vírgula)</span>
           <input class="finput" id="itl-zonas" placeholder="1,2,3" value="${esc(st.zonas.join(','))}">
           <div class="toggle-row" data-itltodas>
             <span class="tlbl">Todas as zonas</span>
             <span class="switch${st.todas ? ' on' : ''}"><span class="knob"></span></span>
           </div>
         </div>
         <label class="fld">
           <span class="flbl">Mensagem (opcional)</span>
           <input class="finput" id="itl-mensagem" maxlength="40" placeholder="Ex.: Sensor de chuva ativo" value="${esc(st.mensagem)}">
         </label>
       </div>
       <button class="btn solid big" data-itlsave>Salvar regra</button>`;

    view.querySelector('[data-itlback]').addEventListener('click', () => renderIntertravamentos().catch(() => {}));

    view.querySelectorAll('[data-itipo]').forEach((b) => {
      b.addEventListener('click', () => { st.tipo = num(b.dataset.itipo); render(); });
    });
    view.querySelectorAll('[data-inode]').forEach((b) => {
      b.addEventListener('click', () => { st.node = num(b.dataset.inode); render(); });
    });
    view.querySelectorAll('[data-icond]').forEach((b) => {
      b.addEventListener('click', () => { st.condicao = num(b.dataset.icond); render(); });
    });
    view.querySelectorAll('[data-iacao]').forEach((b) => {
      b.addEventListener('click', () => { st.acao = num(b.dataset.iacao); render(); });
    });

    const todaBtn = view.querySelector('[data-itltodas]');
    if (todaBtn) todaBtn.addEventListener('click', () => { st.todas = !st.todas; render(); });

    const snInp = view.querySelector('#itl-sensor');
    if (snInp) snInp.addEventListener('input', (e) => { st.sensor = num(e.target.value); });
    const condInp = view.querySelector('#itl-valor');
    if (condInp) condInp.addEventListener('input', (e) => { st.valor = num(e.target.value); });
    const histInp = view.querySelector('#itl-histerese');
    if (histInp) histInp.addEventListener('input', (e) => { st.histerese = num(e.target.value); });
    const maxInp = view.querySelector('#itl-maxAbertas');
    if (maxInp) maxInp.addEventListener('input', (e) => { st.maxAbertas = num(e.target.value); });

    view.querySelector('#itl-zonas').addEventListener('input', (e) => {
      st.zonas = e.target.value.split(',').map((v) => num(v.trim())).filter((v) => v > 0);
    });
    view.querySelector('#itl-mensagem').addEventListener('input', (e) => { st.mensagem = e.target.value; });

    view.querySelector('[data-itlsave]').addEventListener('click', save);
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
      renderIntertravamentos().catch(() => {});
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

// ===== Grupos hidráulicos =====
const GROUP_STATE_CLASS = {
  ocioso: 'gray', abrindo: 'amber', aguardando_partida: 'amber', partindo_bomba: 'amber',
  rodando: 'green', transicao: 'amber', parando_bomba: 'amber', drenando: 'amber',
  fechando: 'amber', adiado: 'red', desconhecido: 'gray',
};

function groupStatusById(list, id) {
  return (Array.isArray(list) ? list : []).find((s) => s && num(s.id) === num(id)) || {};
}

async function renderGrupos() {
  const [groups, status, zones] = await Promise.all([
    getJson('/groups'),
    getJson('/groups/status').catch(() => []),
    getJson('/zones').catch(() => []),
  ]);
  const rows = Array.isArray(groups) ? groups : [];
  const st = Array.isArray(status) ? status : [];
  const zs = Array.isArray(zones) ? zones : [];

  const cards = rows.map((g) => {
    g = g || {};
    const s = groupStatusById(st, g.id);
    const estado = s.estado || 'ocioso';
    const cls = GROUP_STATE_CLASS[estado] || 'gray';
    const bomba = s.bomba ? '<span class="chip green">bomba on</span>' : '<span class="chip gray">bomba off</span>';
    const membros = Array.isArray(g.zonas) ? g.zonas.join(', ') : '';
    return `<div class="card">
      <div class="itl-row">
        <div class="itl-info">
          <div class="name">${esc(g.nome || ('Grupo ' + num(g.id)))}
            <span class="chip ${cls}" data-gstate="${num(g.id)}">${esc(estado)}</span></div>
          <div class="sub" data-gbomba="${num(g.id)}">${bomba} · abertas <span data-gopen="${num(g.id)}">${num(s.abertas)}</span></div>
          <div class="sub">Bomba zona ${num(g.bombaZoneId)} · Zonas: ${esc(membros)} · min ${num(g.minOpen)}/max ${num(g.maxOpen)}</div>
        </div>
        <div class="zbtns">
          <button class="btn outline sm" data-gopenbtn="${num(g.id)}">Abrir</button>
          <button class="btn ghost sm" data-gclosebtn="${num(g.id)}">Fechar</button>
          <button class="btn ghost sm" data-gedit="${num(g.id)}">Editar</button>
          <button class="btn dangerline sm" data-gdel="${num(g.id)}">Excluir</button>
        </div>
      </div>
    </div>`;
  }).join('');

  view.innerHTML =
    `<button class="btn dashed" data-gnew>+ Novo grupo</button>` +
    (cards || '<div class="empty">Nenhum grupo hidráulico.</div>');

  view.querySelector('[data-gnew]').addEventListener('click', () => groupForm(null, zs));
  view.querySelectorAll('[data-gedit]').forEach((b) => b.addEventListener('click', () => {
    groupForm(rows.find((x) => x && num(x.id) === num(b.dataset.gedit)) || null, zs);
  }));
  view.querySelectorAll('[data-gdel]').forEach((b) => b.addEventListener('click', async () => {
    if (!confirm('Excluir este grupo?')) return;
    const r = await postJson('/groups/delete', { id: num(b.dataset.gdel) });
    if (r.ok) renderGrupos().catch(() => {});
    else alert('Falha ao excluir: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
  }));
  view.querySelectorAll('[data-gopenbtn]').forEach((b) => b.addEventListener('click', async () => {
    const r = await postJson('/groups/command', { id: num(b.dataset.gopenbtn), acao: 'abrir' });
    if (!r.ok) alert('Falha: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
  }));
  view.querySelectorAll('[data-gclosebtn]').forEach((b) => b.addEventListener('click', async () => {
    const r = await postJson('/groups/command', { id: num(b.dataset.gclosebtn), acao: 'fechar' });
    if (!r.ok) alert('Falha: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
  }));
}

// Atualiza só os badges/contadores ao vivo (não recria a lista; no-op se a aba/lista não está montada).
async function pollGroupStatus() {
  if (!document.querySelector('[data-gstate]')) return; // form aberto ou outra aba
  const st = await getJson('/groups/status').catch(() => []);
  (Array.isArray(st) ? st : []).forEach((s) => {
    if (!s) return;
    const badge = view.querySelector(`[data-gstate="${num(s.id)}"]`);
    if (badge) {
      const cls = GROUP_STATE_CLASS[s.estado] || 'gray';
      badge.className = 'chip ' + cls;
      badge.textContent = s.estado || 'ocioso';
    }
    const open = view.querySelector(`[data-gopen="${num(s.id)}"]`);
    if (open) open.textContent = num(s.abertas);
    const bombaWrap = view.querySelector(`[data-gbomba="${num(s.id)}"]`);
    if (bombaWrap) {
      const bombaChip = bombaWrap.querySelector('.chip');
      if (bombaChip) {
        bombaChip.className = s.bomba ? 'chip green' : 'chip gray';
        bombaChip.textContent = s.bomba ? 'bomba on' : 'bomba off';
      }
    }
  });
}

function groupForm(group, zones) {
  const editing = !!group;
  const zs = (Array.isArray(zones) ? zones : []).filter((z) => z && num(z.fonteInput ?? -1) < 0);
  const st = group ? JSON.parse(JSON.stringify(group)) : {
    id: 0, nome: '', bombaZoneId: 0, zonas: [], minOpen: 1, maxOpen: 1, transicao: 0,
    overlapS: 10, startAfterOpenS: 5, stopBeforeCloseS: 8, minRunMin: 5, maxStartsHour: 6,
  };
  st.zonas = Array.isArray(st.zonas) ? st.zonas : [];

  function render() {
    const zoneChips = zs.map((z) => {
      const on = st.zonas.includes(num(z.id));
      return `<button class="chip ${on ? 'green' : 'gray'}" data-gz="${num(z.id)}">${num(z.id)}</button>`;
    }).join(' ');
    const bombaOpts = `<option value="0">— sem bomba —</option>` +
      zs.map((z) => `<option value="${num(z.id)}" ${num(st.bombaZoneId) === num(z.id) ? 'selected' : ''}>zona ${num(z.id)}</option>`).join('');

    view.innerHTML = `
      <div class="form">
        <label class="fld"><span class="flbl">Nome</span>
          <input class="finput" id="g-nome" maxlength="15" value="${esc(st.nome || '')}"></label>
        <label class="fld"><span class="flbl">Bomba (zona GPO)</span>
          <select class="finput" id="g-bomba">${bombaOpts}</select></label>
        <div class="fld"><span class="flbl">Zonas membro</span><div class="chips">${zoneChips || '<span class="sub">Sem zonas não-espelho.</span>'}</div></div>
        <div class="frow">
          <label class="fld"><span class="flbl">Mín. abertas</span><input class="finput" id="g-min" type="number" min="1" value="${num(st.minOpen)}"></label>
          <label class="fld"><span class="flbl">Máx. abertas (0=sem teto)</span><input class="finput" id="g-max" type="number" min="0" value="${num(st.maxOpen)}"></label>
        </div>
        <label class="fld"><span class="flbl">Transição</span>
          <select class="finput" id="g-trans">
            <option value="0" ${num(st.transicao) === 0 ? 'selected' : ''}>abrir antes de fechar</option>
            <option value="1" ${num(st.transicao) === 1 ? 'selected' : ''}>fechar antes de abrir</option>
          </select></label>
        <div class="frow">
          <label class="fld"><span class="flbl">Sobrepos. (s)</span><input class="finput" id="g-ov" type="number" min="0" value="${num(st.overlapS)}"></label>
          <label class="fld"><span class="flbl">Partida após abrir (s)</span><input class="finput" id="g-sa" type="number" min="0" value="${num(st.startAfterOpenS)}"></label>
        </div>
        <div class="frow">
          <label class="fld"><span class="flbl">Parar antes fechar (s)</span><input class="finput" id="g-sb" type="number" min="0" value="${num(st.stopBeforeCloseS)}"></label>
          <label class="fld"><span class="flbl">Func. mín. (min)</span><input class="finput" id="g-mr" type="number" min="0" value="${num(st.minRunMin)}"></label>
        </div>
        <label class="fld"><span class="flbl">Máx. partidas/hora</span><input class="finput" id="g-ms" type="number" min="0" value="${num(st.maxStartsHour)}"></label>
        <div class="frow">
          <button class="btn" data-gsave>${editing ? 'Salvar' : 'Criar'}</button>
          <button class="btn ghost" data-gback>Cancelar</button>
        </div>
      </div>`;

    view.querySelectorAll('[data-gz]').forEach((b) => b.addEventListener('click', () => {
      const zid = num(b.dataset.gz);
      const i = st.zonas.indexOf(zid);
      if (i >= 0) st.zonas.splice(i, 1); else st.zonas.push(zid);
      render();
    }));
    view.querySelector('[data-gback]').addEventListener('click', () => renderGrupos().catch(() => {}));
    view.querySelector('[data-gsave]').addEventListener('click', save);
  }

  async function save() {
    const body = {
      id: num(st.id),
      nome: view.querySelector('#g-nome').value.slice(0, 15),
      bombaZoneId: num(view.querySelector('#g-bomba').value),
      zonas: st.zonas,
      minOpen: num(view.querySelector('#g-min').value),
      maxOpen: num(view.querySelector('#g-max').value),
      transicao: num(view.querySelector('#g-trans').value),
      overlapS: num(view.querySelector('#g-ov').value),
      startAfterOpenS: num(view.querySelector('#g-sa').value),
      stopBeforeCloseS: num(view.querySelector('#g-sb').value),
      minRunMin: num(view.querySelector('#g-mr').value),
      maxStartsHour: num(view.querySelector('#g-ms').value),
    };
    if (!body.zonas.length) { alert('Selecione ao menos uma zona.'); return; }
    const r = await postJson('/groups', body);
    if (r.ok) renderGrupos().catch(() => {});
    else alert('Falha: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
  }

  render();
}

// Mapa extensível: programs adicionado na Task 13.
const RENDER = {
  overview: renderOverview,
  stations: renderStations,
  zones: renderZones,
  programs: renderPrograms,
  sensores: renderSensores,
  gpo: renderGpo,
  grupos: renderGrupos,
  intertravamentos: renderIntertravamentos,
  auditlog: renderAuditLog,
  tamper: renderTamper,
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
  if (tab === 'overview' || tab === 'stations' || tab === 'sensores') {
    timer = setInterval(() => fn().catch(() => {}), 3000);
  } else if (tab === 'grupos') {
    timer = setInterval(() => pollGroupStatus().catch(() => {}), 3000);
  }
}

document.querySelectorAll('.tab').forEach((t) => {
  if (t.disabled) return;
  t.addEventListener('click', () => show(t.dataset.tab));
});

show('overview');
