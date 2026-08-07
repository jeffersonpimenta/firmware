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
    alertCount: 2,
    pairingPending: true,
    pairingNodeId: 0xc0ffee01,
    pairingSecondsLeft: 90,
    selfNode: 0x0a0a0a0a, // "Gateway (local)" — saída na própria placa do gateway
  },
  stations: [
    {
      node: 0xa1b2c3d4,
      name: 'Horta Norte',
      sync: 'sincronizada',
      secsSinceHeard: 45,
      vbatCentiV: 1250,
      vpanelCentiV: 1380,
      snrQuarterDb: 33,
      rssiDbm: -72,
      rebootCount: 2,
      flags: 0,
      lat: -2290680,
      lon: -4706160,
      hbMinutes: 15,
      vbatAvisoCentiV: 1220,
      vbatCriticaCentiV: 1180,
      outputs: [
        { tipo: 0, index: 0, label: 'Válvula 1' },
        { tipo: 0, index: 1, label: 'Válvula 2' },
      ],
    },
    {
      node: 0xe5f6a7b8,
      name: 'Pomar',
      sync: 'pendente',
      secsSinceHeard: 180,
      vbatCentiV: 1100,
      vpanelCentiV: 1210,
      snrQuarterDb: 10,
      rssiDbm: -94,
      rebootCount: 0,
      flags: 0,
      lat: -2212350,
      lon: -4765430,
      hbMinutes: 30,
      vbatAvisoCentiV: 1220,
      vbatCriticaCentiV: 1180,
      outputs: [
        { tipo: 0, index: 0, label: 'Válvula 1' },
        { tipo: 1, index: 0, label: 'GPO 1' },
      ],
    },
    {
      node: 0x12345678,
      name: 'Pastagem',
      sync: 'sincronizada',
      secsSinceHeard: 12,
      vbatCentiV: 1320,
      vpanelCentiV: 1400,
      snrQuarterDb: 20,
      rssiDbm: -85,
      rebootCount: 1,
      flags: 0,
      lat: 0,
      lon: 0,
      hbMinutes: 30,
      vbatAvisoCentiV: 1220,
      vbatCriticaCentiV: 1180,
      outputs: [{ tipo: 1, index: 0, label: 'GPO 1' }],
    },
  ],
  zones: [
    {
      id: 9,
      name: 'Motor terreno',
      node: 0x0a0a0a0a, // Gateway (local) — motor irriga todo o terreno, sem válvula
      tipo: 1,
      index: 0,
      maxMin: 120,
      padraoMin: 30,
      fonteInput: -1,
    },
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
        { idx: 1, nome: 'Boia caixa', tipo: 0, valor: 0 },
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
  levels: [
    {
      id: 1,
      sensorNode: 0xa1b2c3d4,
      sensorIdx: 1,
      ligaQuandoAtivo: true,
      targetZoneId: 1,
      minOnS: 300,
      minOffS: 600,
      staleTimeoutS: 3600,
      mensagem: 'Cisterna baixa',
    },
    {
      id: 2,
      sensorNode: 0xe5f6a7b8,
      sensorIdx: 1,
      ligaQuandoAtivo: false,
      targetZoneId: 3,
      minOnS: 60,
      minOffS: 120,
      staleTimeoutS: 90,
      mensagem: 'Caixa cheia',
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
  // Modo Espelhamento — espelha entradas físicas do gateway para saídas de zona nos nós.
  // enabled: flag global. Cada zona com fonteInput >= 0 tem uma associação (porta → zona).
  // ports[i].active: estado simulado da entrada física (lido pelo firmware via GPIO).
  mirrorEnabled: false,
  mirrorPortsActive: [false, false, false, false], // estado ao vivo simulado das 4 portas

  // Alertas não-reconhecidos (§8.1–§8.3). type = AlertType (StationMonitor.h).
  alerts: [
    { type: 5, node: 0xe5f6a7b8, arg: 0, atMs: 1000, ageS: 2400 }, // Estação silenciosa
    { type: 1, node: 0xa1b2c3d4, arg: 0, atMs: 2000, ageS: 300 },  // Bateria em aviso
  ],
  // Portal do nó (/api/portal/*). provisioned é ajustado abaixo por ?wizard=1.
  portalNode: {
    role: 0, name: 'Horta Norte', boundGateway: 0xa1b2c3d4, configEpoch: 17,
    safeMode: false, provisioned: true, numValves: 2, numGpos: 1,
    valveStates: 1, gpoStates: 0, vbatCentiV: 1250, vpanelCentiV: 1350,
    flags: 0, apSecondsLeft: 540, latE7: -221234567, lonE7: -476543210,
    uptimeS: 1234567,
    nowEpoch: Math.floor(Date.now() / 1000),
    hasTime: true,
  },
  portalSensors: {
    sensors: [
      { id: 0, tipo: 1, unidade: 'bar', valor: 152 },
      { id: 1, tipo: 0, unidade: '', valor: 100 },
    ],
  },
  portalLog: {
    log: [
      { ts: Math.floor(Date.now() / 1000) - 120, origem: 4, acao: 0, alvo: 1, res: 0, no: 0xa1b2c3d4, seq: 7 },
      { ts: 0, origem: 3, acao: 2, alvo: 0, res: 0, no: 0xa1b2c3d4, seq: 8 },
    ],
  },
  portalRoster: [
    { id: 1, name: 'Horta', padraoMin: 20 },
    { id: 2, name: 'Pomar', padraoMin: 30 },
    { id: 3, name: 'Pastagem', padraoMin: 45 },
  ],
  // WiFi — mock networks para testes de UI
  wifiNetworks: [
    { ssid: 'Casa', rssi: -45, secure: 1 },
    { ssid: 'Vizinhos', rssi: -62, secure: 1 },
    { ssid: 'Wifi Publico', rssi: -75, secure: 0 },
    { ssid: 'Irrigacao-IOT', rssi: -55, secure: 1 },
  ],
};

// Estado mutável (alterações de UI persiste até reload)
let STATE = JSON.parse(JSON.stringify(MOCK_DATA));
// ?wizard=1 → nó não-provisionado (wizard de 1º boot §6).
STATE.portalNode.provisioned = !/[?&]wizard(=1)?/.test(location.search);
// ?role=N → força papel do nó (ex.: ?role=3 para device SERVIÇO §11.8).
{ const m = location.search.match(/[?&]role=(\d)/); if (m) STATE.portalNode.role = +m[1]; }

// WiFi state — gerenciado pelos endpoints mock
let wifiState = {
  enabled: false,
  connectedSsid: null,
  scanInProgress: false,
  connectState: 'idle', // idle | connecting | success | failed
  connectSsid: null,
};

// Estado do relógio do gateway — endpoints /time, /timezone, /time/sync (página Horário).
// posix → rótulo (espelha tzLabelFor do backend: nome IANA, 'Personalizado' se desconhecido).
const TZ_LABELS = {
  '<-03>3': 'America/Sao_Paulo',
  '<-04>4': 'America/Manaus',
  '<-05>5': 'America/Rio_Branco',
  '<-02>2': 'America/Noronha',
  'GMT0': 'UTC',
};
let timeState = {
  tz: '<-03>3',
  source: 'ntp', // ntp | manual
  ntpServer: 'pool.ntp.org',
  lastSyncS: 0, // segundos desde o último NTP (0 = agora); só usado quando source==ntp
  manualEpoch: null,
  manualSetAtMs: null,
};

// Espelha buildTimeStatus() do backend (IrrigationWebApi.cpp).
function mockTimeStatus() {
  const nowEpoch =
    timeState.source === 'manual' && timeState.manualEpoch
      ? timeState.manualEpoch + Math.floor((Date.now() - timeState.manualSetAtMs) / 1000)
      : Math.floor(Date.now() / 1000);
  return {
    nowEpoch,
    hasRtc: true,
    source: timeState.source,
    quality: timeState.source === 'ntp' ? 4 : 2,
    ntpServer: timeState.ntpServer,
    lastSyncS: timeState.source === 'ntp' ? timeState.lastSyncS : -1,
    tz: timeState.tz,
    tzLabel: TZ_LABELS[timeState.tz] || 'Personalizado',
    staUp: !!wifiState.connectedSsid,
  };
}

// Substitui fetch global
const origFetch = window.fetch;
window.fetch = async function (url, opts) {
  const method = (opts && opts.method) || 'GET';
  const path = url.replace(/.*\/api\/irrigation/, '');

  // Latência artificial
  await new Promise(r => setTimeout(r, Math.random() * 300 + 100));

  // Portal do nó (/api/portal/*) — para preview do portal, inclua este mock em portal/index.html.
  const clean = url.split('?')[0];
  if (clean.includes('/api/portal')) {
    const ppath = clean.replace(/.*\/api\/portal/, '');
    if (method === 'POST') {
      if (ppath === '/provision') { STATE.portalNode.provisioned = true; return mockResponse({ ok: true, reboot: true }); }
      // WiFi toggle
      if (ppath === '/wifi/toggle') {
        const body = opts.body ? JSON.parse(opts.body) : {};
        wifiState.enabled = body.enabled;
        if (body.enabled) wifiState.scanInProgress = true;
        return mockResponse({ ok: true });
      }
      // Demais POSTs (/wifi/scan, /wifi/connect, /wifi/forget) caem nos handlers
      // dedicados abaixo, que tratam método — não retornar aqui.
    }
    if (ppath.startsWith('/node')) return mockResponse(STATE.portalNode);
    if (ppath.startsWith('/sensors')) return mockResponse(STATE.portalSensors);
    if (ppath.startsWith('/log')) return mockResponse(STATE.portalLog);
    if (ppath.startsWith('/coords')) return mockResponse({ latE7: STATE.portalNode.latE7, lonE7: STATE.portalNode.lonE7 });
    if (ppath.startsWith('/net/roster')) return mockResponse(STATE.portalRoster);
    if (ppath.startsWith('/link')) return mockResponse({
      snrQuarterDb: 33, rssiDbm: -72,
      history: [40, 55, 50, 65, 70, 60, 75, 80, 72, 78, 85, 82],
      neighbors: [
        { node: 0xa1b2c3d4, snrQuarterDb: 33, hops: 1, name: 'GW' },
        { node: 0x12345678, snrQuarterDb: 20, hops: 2, name: 'EST-2' },
      ],
    });
    if (ppath.startsWith('/service/clients')) return mockResponse({
      clients: [
        { id: 'fazenda-bela-vista', nome: 'Fazenda Bela Vista', canal: 'farm', estacoes: 4, active: true },
        { id: 'sitio-sao-joao',    nome: 'Sítio São João',    canal: 'sitio', estacoes: 2, active: false },
      ],
    });
    if (ppath.startsWith('/service/log')) return mockResponse({
      log: [
        { up: 3720, ts: Math.floor(Date.now() / 1000) - 300, ev: 'client_select', node: 0xa1b2c3d4 },
        { up: 120,  ts: 0,                                    ev: 'boot' },
      ],
    });
    if (ppath.startsWith('/service/scan')) return mockResponse({
      nodes: [
        { node: 0xa1b2c3d4, role: 1, epoch: 17, vbat: 1250, fw: 0x20301, snr: 12 },
        { node: 0xe5f6a7b8, role: 0, epoch: 17, vbat: 1180, fw: 0x20301, snr: 8  },
      ],
    });
    // WiFi endpoints
    if (ppath.startsWith('/wifi/toggle')) return mockResponse({ ok: true });
    if (ppath.startsWith('/wifi/scan') && method === 'POST') {
      wifiState.scanInProgress = true;
      setTimeout(() => { wifiState.scanInProgress = false; }, 1500);
      return mockResponse({ ok: true });
    }
    if (ppath.startsWith('/wifi/scan') && method === 'GET') {
      return mockResponse({ scanning: wifiState.scanInProgress, networks: wifiState.scanInProgress ? [] : MOCK_DATA.wifiNetworks });
    }
    if (ppath.startsWith('/wifi/connect') && method === 'POST') {
      wifiState.connectState = 'connecting';
      wifiState.connectSsid = (opts.body ? JSON.parse(opts.body) : {}).ssid;
      setTimeout(() => { wifiState.connectState = 'success'; wifiState.connectedSsid = wifiState.connectSsid; }, 1500);
      return mockResponse({ ok: true });
    }
    if (ppath.startsWith('/wifi/connect') && method === 'GET') {
      return mockResponse({ state: wifiState.connectState, ssid: wifiState.connectSsid, error: null });
    }
    if (ppath.startsWith('/wifi/forget')) {
      wifiState.connectedSsid = null;
      wifiState.connectSsid = null;
      return mockResponse({ ok: true });
    }
    if (ppath.startsWith('/wifi') && method === 'GET') {
      return mockResponse({ enabled: wifiState.enabled, connectedSsid: wifiState.connectedSsid });
    }
    return mockResponse({ ok: true });
  }

  // Intercepta POST + DELETE
  if (method === 'POST') {
    const body = opts.body ? JSON.parse(opts.body) : {};
    return handlePost(path, body);
  }

  // GET
  if (path.startsWith('/time')) return mockResponse(mockTimeStatus());
  if (path.startsWith('/zones')) return mockResponse(STATE.zones);
  if (path.startsWith('/stations')) return mockResponse(STATE.stations);
  if (path.startsWith('/overview')) return mockResponse(STATE.overview);
  if (path.startsWith('/alerts')) return mockResponse(STATE.alerts);
  if (path.startsWith('/programs')) return mockResponse(STATE.programs);
  if (path.startsWith('/sensors')) return mockResponse(STATE.sensors);
  if (path.startsWith('/interlocks')) return mockResponse(STATE.interlocks);
  if (path.startsWith('/audit')) return mockResponse(STATE.audit);
  if (path.startsWith('/groups/status')) return mockResponse(STATE.groupsStatus);
  if (path.startsWith('/groups')) return mockResponse(STATE.groups);
  if (path.startsWith('/export'))
    return mockResponse({
      fmt: 'irrig-vault',
      version: 1,
      clients: [
        {
          id: 'gateway',
          nome: 'Fazenda Bela Vista',
          canalNome: 'farm',
          psk_base64: 'QWJjREVGR0hJSktMTW4v',
          preset: 0,
          gateway: STATE.stations[0] ? STATE.stations[0].node : 0,
          estacoes: STATE.stations.map((s) => ({
            no: '!' + (s.node >>> 0).toString(16).padStart(8, '0'),
            nome: s.name,
            lat: s.lat,
            lon: s.lon,
            hbMinutes: s.hbMinutes,
            vbatAvisoCentiV: s.vbatAvisoCentiV,
            vbatCriticaCentiV: s.vbatCriticaCentiV,
          })),
          zonas: STATE.zones,
          programas: STATE.programs,
          grupos: STATE.groups,
          niveis: STATE.levels,
        },
      ],
    });
  if (path.startsWith('/survey')) return mockResponse(STATE.survey);
  if (path.startsWith('/levels')) return mockResponse(STATE.levels);
  if (path.startsWith('/mirror')) {
    // Constrói a resposta a partir de STATE.zones (fonteInput >= 0 → associação ativa).
    const ports = [0, 1, 2, 3].map((i) => {
      const zone = STATE.zones.find((z) => z.fonteInput === i);
      const active = STATE.mirrorPortsActive[i] || false;
      if (zone) {
        const invertido = !!zone.fonteInvertido;
        const habilitado = zone.fonteEnabled !== false; // default true
        // Se invertido, o driving é o inverso de active; senão direto.
        const driving = habilitado && (invertido ? !active : active);
        return { i, active, invertido, zoneId: zone.id, zoneName: zone.name || ('Zona ' + zone.id), habilitado, driving };
      }
      return { i, active, invertido: false, zoneId: null, zoneName: null, habilitado: false, driving: false };
    });
    return mockResponse({ enabled: STATE.mirrorEnabled, ports });
  }

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
  // Horário (relógio do gateway) — espelha hTimeSet/hTimezone/hTimeSync do backend.
  if (path === '/time/sync') {
    if (!wifiState.connectedSsid) return mockResponse({ ok: false, reason: 'sem WiFi' }, 409);
    timeState.source = 'ntp';
    timeState.lastSyncS = 0;
    return mockResponse({ ok: true });
  }
  if (path === '/time') {
    if (!body.epoch || body.epoch < 1600000000) return mockResponse({ ok: false, reason: 'epoch inválido' }, 400);
    timeState.source = 'manual';
    timeState.manualEpoch = body.epoch;
    timeState.manualSetAtMs = Date.now();
    return mockResponse(mockTimeStatus()); // backend devolve o status completo no sucesso
  }
  if (path === '/timezone') {
    if (!TZ_LABELS[body.tz]) return mockResponse({ ok: false, reason: 'fuso desconhecido' }, 400);
    timeState.tz = body.tz;
    return mockResponse({ ok: true });
  }
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
    if (body.kind === 'approve_pairing') {
      // §6: aprova → abre janela; simula o nó pareando (vira estação nova "pendente").
      const node = STATE.overview.pairingNodeId;
      STATE.overview.pairingPending = false;
      STATE.overview.pairingSecondsLeft = 0;
      if (node && !STATE.stations.some((s) => s.node === node)) {
        STATE.stations.push({
          node,
          name: 'Nova estação',
          sync: 'pendente',
          secsSinceHeard: 0,
          vbatCentiV: 0,
          vpanelCentiV: 0,
          snrQuarterDb: 0,
          rssiDbm: 0,
          rebootCount: 0,
          flags: 0,
          lat: 0,
          lon: 0,
          hbMinutes: 10,
          vbatAvisoCentiV: 1220,
          vbatCriticaCentiV: 1180,
          outputs: [],
        });
        STATE.overview.stationCount = STATE.stations.length;
      }
      return mockResponse({ ok: true });
    }
    if (body.kind === 'ack') {
      if (body.atMs) {
        // ack por-alerta: remove só o alerta cuja identidade (node+type+arg+atMs) casa.
        STATE.alerts = STATE.alerts.filter(
          (a) => !(a.node === body.node && a.type === body.type && a.arg === body.arg && a.atMs === body.atMs)
        );
      } else {
        STATE.alerts = []; // sem identidade ⇒ reconhecer todos (legado)
      }
      STATE.overview.alertCount = STATE.alerts.length;
      return mockResponse({ ok: true });
    }
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

  // Estações (Fase 9)
  if (path === '/stations/config') {
    const st = STATE.stations.find((s) => s.node === body.node);
    if (!st) return mockResponse({ errors: ['estação inexistente'] }, 400);
    const aviso = Number(body.vbatAvisoCentiV);
    const critica = Number(body.vbatCriticaCentiV);
    const hb = Number(body.hbMinutes);
    if (!(hb >= 1 && hb <= 1440)) return mockResponse({ errors: ['Heartbeat deve ser 1..1440 min.'] }, 400);
    if (!(aviso > critica)) return mockResponse({ errors: ['Limiar de aviso deve ser maior que o crítico.'] }, 400);
    st.hbMinutes = hb;
    st.vbatAvisoCentiV = aviso;
    st.vbatCriticaCentiV = critica;
    st.lat = Math.round(Number(body.latE7) / 100); // ×1e7 → ×1e5 (escala do /stations)
    st.lon = Math.round(Number(body.lonE7) / 100);
    st.sync = 'pendente'; // re-push muda o epoch → estação aguarda ACK do nó
    return mockResponse({ ok: true });
  }
  if (path === '/stations/delete') {
    const deps = STATE.zones.filter((z) => z.node === body.node);
    if (deps.length) {
      return mockResponse({ errors: ['zonas vinculadas: ' + deps.map((z) => z.name).join(', ')] }, 400);
    }
    STATE.stations = STATE.stations.filter((s) => s.node !== body.node);
    return mockResponse({ ok: true });
  }
  if (path === '/stations/pulse') {
    console.log('[MOCK] Pulso de teste:', body);
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

  // Controle de nível (boia)
  if (path === '/levels') {
    body.id = body.id || (Math.max(...STATE.levels.map(l => l.id), 0) + 1);
    const idx = STATE.levels.findIndex(l => l.id === body.id);
    if (idx >= 0) STATE.levels[idx] = body;
    else STATE.levels.push(body);
    return mockResponse({ ok: true });
  }
  if (path === '/levels/delete') {
    STATE.levels = STATE.levels.filter(l => l.id !== body.id);
    return mockResponse({ ok: true });
  }

  // Survey
  if (path === '/survey/clear') {
    STATE.survey = [];
    return mockResponse({ ok: true });
  }

  // Modo Espelhamento
  if (path === '/mirror') {
    // Toggle do flag global enabled.
    if (body.enabled !== undefined) STATE.mirrorEnabled = !!body.enabled;
    return mockResponse({ ok: true });
  }
  if (path === '/mirror/mapping') {
    // Valida: input 0..3, zoneId existente.
    const input = Number(body.input);
    const zoneId = Number(body.zoneId);
    if (input < 0 || input > 3 || !Number.isInteger(input))
      return mockResponse({ errors: ['Porta inválida (0..3).'] }, 400);
    const zone = STATE.zones.find((z) => z.id === zoneId);
    if (!zone) return mockResponse({ errors: ['Zona não encontrada.'] }, 400);
    // Limpa qualquer associação anterior nesta mesma zona ou porta.
    STATE.zones.forEach((z) => {
      if (z.fonteInput === input && z.id !== zoneId) {
        z.fonteInput = -1;
        z.fonteInvertido = false;
        z.fonteEnabled = false;
      }
    });
    zone.fonteInput = input;
    zone.fonteInvertido = !!body.invertido;
    zone.fonteEnabled = body.habilitado !== false;
    return mockResponse({ ok: true });
  }
  if (path === '/mirror/mapping/delete') {
    const input = Number(body.input);
    const zone = STATE.zones.find((z) => z.fonteInput === input);
    if (zone) {
      zone.fonteInput = -1;
      zone.fonteInvertido = false;
      zone.fonteEnabled = false;
    }
    return mockResponse({ ok: true });
  }

  // Importar backup (restaurar)
  if (path === '/import') {
    // body já é o objeto parseado (o fetch intercept faz JSON.parse(opts.body))
    const clients = Array.isArray(body.clients) ? body.clients : [];
    const client = clients[0] || {};
    // Backups reais aninham os arrays sob client.config (como buildClientBackup gera).
    // Mantém fallback para client direto caso config esteja ausente (formato legado).
    const cfg = (client.config && typeof client.config === 'object') ? client.config : client;
    const zonas = Array.isArray(cfg.zonas) ? cfg.zonas : [];
    const programas = Array.isArray(cfg.programas) ? cfg.programas : [];
    const interlocks = Array.isArray(cfg.intertravamentos) ? cfg.intertravamentos : [];
    const grupos = Array.isArray(cfg.grupos) ? cfg.grupos : [];
    // Aplica ao STATE para que os painéis reflitam o import
    if (zonas.length) STATE.zones = zonas;
    if (programas.length) STATE.programs = programas;
    if (interlocks.length) STATE.interlocks = interlocks;
    if (grupos.length) STATE.groups = grupos;
    return mockResponse({
      ok: true,
      zonas: zonas.length,
      programas: programas.length,
      intertravamentos: interlocks.length,
      grupos: grupos.length,
    });
  }

  // Fallback 404
  return mockResponse({ error: 'Endpoint não mockado: ' + path }, 404);
}

console.log('[MOCK] API mockada ativada. Dados fake em STATE global.');
