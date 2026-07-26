// mock.js — API mockada para testes de UI offline
// Inclua antes de app.js: <script src="mock.js"></script>
'use strict';

const MOCK_DATA = {
  overview: {
    hasRtc: true,
    stationCount: 3,
    running: true,
    runningZoneId: 1,
    runningRemainMin: 25,
    alertCount: 1,
    pairingPending: false,
    pairingNodeId: 0,
    pairingSecondsLeft: 0,
  },
  stations: [
    {
      node: 0xa1b2c3d4,
      name: 'Horta Norte',
      sync: 'sincronizada',
      secsSinceHeard: 45,
      vbatCentiV: 1250,
    },
    {
      node: 0xe5f6a7b8,
      name: 'Pomar',
      sync: 'pendente',
      secsSinceHeard: 180,
      vbatCentiV: 1100,
    },
    {
      node: 0x12345678,
      name: 'Pastagem',
      sync: 'sincronizada',
      secsSinceHeard: 12,
      vbatCentiV: 1320,
    },
  ],
  zones: [
    {
      id: 1,
      name: 'Horta',
      node: 0xa1b2c3d4,
      tipo: 0,
      index: 0,
      maxMin: 45,
      padraoMin: 20,
      fonteInput: -1,
    },
    {
      id: 2,
      name: 'Pomar',
      node: 0xa1b2c3d4,
      tipo: 0,
      index: 1,
      maxMin: 60,
      padraoMin: 30,
      fonteInput: -1,
    },
    {
      id: 3,
      name: 'Pastagem',
      node: 0xe5f6a7b8,
      tipo: 0,
      index: 0,
      maxMin: 90,
      padraoMin: 45,
      fonteInput: -1,
    },
    {
      id: 4,
      name: 'Portão Curral',
      node: 0xe5f6a7b8,
      tipo: 1,
      index: 0,
      maxMin: 1,
      padraoMin: 0,
      fonteInput: -1,
    },
  ],
  programs: [
    {
      id: 1,
      enabled: true,
      daysMask: 127,
      startMinute: 360,
      steps: [
        { zoneId: 1, durationMin: 20 },
        { zoneId: 2, durationMin: 30 },
      ],
    },
    {
      id: 2,
      enabled: true,
      daysMask: 31,
      startMinute: 720,
      steps: [
        { zoneId: 3, durationMin: 45 },
      ],
    },
  ],
  sensors: [
    {
      node: 0xa1b2c3d4,
      nome: 'Estação Horta',
      tamper: false,
      sensores: [
        { idx: 0, nome: 'Pressão linha', tipo: 1, valor: 750 },
        { idx: 1, nome: 'Nível', tipo: 0, valor: 1 },
      ],
    },
    {
      node: 0xe5f6a7b8,
      nome: 'Estação Pomar',
      tamper: false,
      sensores: [
        { idx: 0, nome: 'Pressão', tipo: 1, valor: 650 },
      ],
    },
  ],
  interlocks: [
    {
      id: 1,
      tipo: 0,
      node: 0xa1b2c3d4,
      sensor: 0,
      condicao: 2,
      valor: 500,
      histerese: 50,
      acao: 0,
      zonas: [1, 2],
      todas: false,
      mensagem: 'Pressão baixa — bloqueio',
      maxAbertas: 0,
    },
  ],
  audit: [
    {
      ts: Math.floor(Date.now() / 1000) - 600,
      origem: 1,
      acao: 0,
      alvo: 1,
      resultado: 0,
      node: 0xa1b2c3d4,
      seq: 42,
    },
    {
      ts: Math.floor(Date.now() / 1000) - 300,
      origem: 2,
      acao: 0,
      alvo: 2,
      resultado: 0,
      node: 0xa1b2c3d4,
      seq: 43,
    },
  ],
  groups: [
    {
      id: 1,
      nome: 'Grupo Principal',
      bombaZoneId: 4,
      zonas: [1, 2, 3],
      minOpen: 1,
      maxOpen: 2,
      transicao: 0,
      overlapS: 10,
      startAfterOpenS: 5,
      stopBeforeCloseS: 8,
      minRunMin: 5,
      maxStartsHour: 6,
    },
  ],
  groupsStatus: [
    {
      id: 1,
      estado: 'rodando',
      bomba: true,
      abertas: 2,
    },
  ],
  survey: [
    {
      no: 0xa1b2c3d4,
      role: 0,
      lat: -221234567,
      lon: -476543210,
      coord: true,
      snr: 120,
      rssi: -95,
      idadeS: 45,
    },
    {
      no: 0xe5f6a7b8,
      role: 0,
      lat: 0,
      lon: 0,
      coord: false,
      snr: 90,
      rssi: -110,
      idadeS: 120,
    },
  ],
};

// Estado mutável (alterações de UI persiste até reload)
let STATE = JSON.parse(JSON.stringify(MOCK_DATA));

// Substitui fetch global
const origFetch = window.fetch;
window.fetch = async function (url, opts) {
  const method = (opts && opts.method) || 'GET';
  const path = url.replace(/.*\/api\/irrigation/, '');

  // Latência artificial
  await new Promise(r => setTimeout(r, Math.random() * 300 + 100));

  // Intercepta POST + DELETE
  if (method === 'POST') {
    const body = opts.body ? JSON.parse(opts.body) : {};
    return handlePost(path, body);
  }

  // GET
  if (path.startsWith('/zones')) return mockResponse(STATE.zones);
  if (path.startsWith('/stations')) return mockResponse(STATE.stations);
  if (path.startsWith('/overview')) return mockResponse(STATE.overview);
  if (path.startsWith('/programs')) return mockResponse(STATE.programs);
  if (path.startsWith('/sensors')) return mockResponse(STATE.sensors);
  if (path.startsWith('/interlocks')) return mockResponse(STATE.interlocks);
  if (path.startsWith('/audit')) return mockResponse(STATE.audit);
  if (path.startsWith('/groups/status')) return mockResponse(STATE.groupsStatus);
  if (path.startsWith('/groups')) return mockResponse(STATE.groups);
  if (path.startsWith('/export')) return mockResponse({
    psk_base64: 'QWJjREVGR0hJSktMTW4v',
    zones: STATE.zones,
    programs: STATE.programs,
    stations: STATE.stations,
    groups: STATE.groups,
  });
  if (path.startsWith('/survey')) return mockResponse(STATE.survey);

  // Fallback API real
  return origFetch(url, opts);
};

function mockResponse(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

function handlePost(path, body) {
  // Zonas
  if (path === '/zones') {
    if (!body.name || !body.name.trim()) return mockResponse({ errors: ['Nome obrigatório.'] }, 400);
    body.id = body.id || (Math.max(...STATE.zones.map(z => z.id), 0) + 1);
    const idx = STATE.zones.findIndex(z => z.id === body.id);
    if (idx >= 0) STATE.zones[idx] = body;
    else STATE.zones.push(body);
    return mockResponse({ ok: true });
  }
  if (path === '/zones/delete') {
    STATE.zones = STATE.zones.filter(z => z.id !== body.id);
    return mockResponse({ ok: true });
  }

  // Programas
  if (path === '/programs') {
    if (!body.steps || !Array.isArray(body.steps) || body.steps.length === 0) {
      return mockResponse({ errors: ['Programa precisa de etapas.'] }, 400);
    }
    body.id = body.id || (Math.max(...STATE.programs.map(p => p.id), 0) + 1);
    const idx = STATE.programs.findIndex(p => p.id === body.id);
    if (idx >= 0) STATE.programs[idx] = body;
    else STATE.programs.push(body);
    return mockResponse({ ok: true });
  }
  if (path === '/programs/delete') {
    STATE.programs = STATE.programs.filter(p => p.id !== body.id);
    return mockResponse({ ok: true });
  }
  if (path === '/programs/toggle') {
    const p = STATE.programs.find(x => x.id === body.id);
    if (p) p.enabled = body.enabled;
    return mockResponse({ ok: true });
  }

  // Comando (abrir/fechar zona)
  if (path === '/command') {
    console.log('[MOCK] Comando:', body);
    STATE.overview.running = body.kind === 'open';
    if (body.kind === 'open') {
      STATE.overview.runningZoneId = body.zoneId;
      STATE.overview.runningRemainMin = Math.ceil(body.durationS / 60);
    }
    return mockResponse({ ok: true });
  }

  // Sensores
  if (path === '/sensors/name') {
    const st = STATE.sensors.find(s => s.node === body.node);
    if (st) {
      const sen = st.sensores.find(s => s.idx === body.sensor);
      if (sen) sen.nome = body.nome;
    }
    return mockResponse({ ok: true });
  }

  // Intertravamentos
  if (path === '/interlocks') {
    body.id = body.id || (Math.max(...STATE.interlocks.map(i => i.id), 0) + 1);
    const idx = STATE.interlocks.findIndex(i => i.id === body.id);
    if (idx >= 0) STATE.interlocks[idx] = body;
    else STATE.interlocks.push(body);
    return mockResponse({ ok: true });
  }
  if (path === '/interlocks/delete') {
    STATE.interlocks = STATE.interlocks.filter(i => i.id !== body.id);
    return mockResponse({ ok: true });
  }

  // Manutenção (tamper)
  if (path === '/maint') {
    console.log('[MOCK] Manutenção:', body);
    return mockResponse({ ok: true });
  }

  // Grupos
  if (path === '/groups') {
    body.id = body.id || (Math.max(...STATE.groups.map(g => g.id), 0) + 1);
    const idx = STATE.groups.findIndex(g => g.id === body.id);
    if (idx >= 0) STATE.groups[idx] = body;
    else STATE.groups.push(body);
    return mockResponse({ ok: true });
  }
  if (path === '/groups/delete') {
    STATE.groups = STATE.groups.filter(g => g.id !== body.id);
    return mockResponse({ ok: true });
  }
  if (path === '/groups/command') {
    const g = STATE.groupsStatus.find(s => s.id === body.id);
    if (g) {
      if (body.acao === 'abrir') {
        g.estado = 'rodando';
        g.bomba = true;
        g.abertas = 2;
      } else {
        g.estado = 'fechando';
        g.bomba = false;
        g.abertas = 0;
      }
    }
    return mockResponse({ ok: true });
  }

  // Survey
  if (path === '/survey/clear') {
    STATE.survey = [];
    return mockResponse({ ok: true });
  }

  // Fallback 404
  return mockResponse({ error: 'Endpoint não mockado: ' + path }, 404);
}

console.log('[MOCK] API mockada ativada. Dados fake em STATE global.');
