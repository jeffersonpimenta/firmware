const fs = require('fs');
let mockCode = fs.readFileSync('mock.js', 'utf-8');

// Simula ambiente browser
global.window = { fetch: null };
global.Response = class Response {
  constructor(body, opts = {}) {
    this.body = body;
    this.status = opts.status || 200;
    this.headers = opts.headers || {};
    this.ok = this.status >= 200 && this.status < 300;
  }
  json() {
    try {
      return Promise.resolve(JSON.parse(this.body));
    } catch (e) {
      return Promise.reject(e);
    }
  }
};
global.location = { search: '' };

// Executa mock.js using Function with global scope
// Change 'let STATE' to 'global.STATE' to make it globally accessible
mockCode = mockCode.replace('let STATE = ', 'global.STATE = ');
new Function(mockCode)();

// Testa consistência de STATE.levels com stations/zones/sensors
const STATE = global.STATE;

function digitalSensor(node, idx) {
  const st = STATE.sensors.find((s) => s.node === node);
  if (!st) return false;
  const sen = (st.sensores || []).find((x) => x.idx === idx);
  return !!sen && sen.tipo === 0;
}
function zoneExists(id) { return STATE.zones.some((z) => z.id === id); }

let fail = 0;
function check(cond, msg) { if (!cond) { console.error('FAIL:', msg); fail++; } }

check(Array.isArray(STATE.levels) && STATE.levels.length >= 2, 'levels deve ter >= 2 regras');
const polarities = new Set();
STATE.levels.forEach((r, i) => {
  check(digitalSensor(r.sensorNode, r.sensorIdx), `regra ${i}: sensor ${r.sensorIdx}@${r.sensorNode.toString(16)} deve ser digital existente`);
  check(zoneExists(r.targetZoneId), `regra ${i}: targetZoneId ${r.targetZoneId} deve existir em zones`);
  check((r.mensagem || '').length <= 23, `regra ${i}: mensagem <= 23 chars`);
  polarities.add(!!r.ligaQuandoAtivo);
});
check(polarities.size === 2, 'levels deve cobrir ambas polaridades (true e false)');

// Fase 9: mini-log da estação = /audit filtrado por node. Estação 0 precisa de >=1 evento.
check(
  Array.isArray(STATE.audit) && STATE.audit.some((a) => a.node === STATE.stations[0].node),
  'audit deve ter >=1 entrada do node da estação 0 (mini-log do sheet)'
);

// Fase 9: overview expõe estado de pareamento; quando pendente, tem nó válido.
check(typeof STATE.overview.pairingPending === 'boolean', 'overview.pairingPending deve ser boolean');
if (STATE.overview.pairingPending) {
  check(STATE.overview.pairingNodeId > 0, 'pairingPending=true exige pairingNodeId > 0 (card mostra o nó)');
}

// Fase 9: sheet de estação usa campos ricos + saídas p/ o teste de pulso.
STATE.stations.forEach((s, i) => {
  check(typeof s.hbMinutes === 'number', `estação ${i}: hbMinutes numérico`);
  check(typeof s.rssiDbm === 'number', `estação ${i}: rssiDbm numérico`);
  check(
    s.vbatAvisoCentiV > s.vbatCriticaCentiV,
    `estação ${i}: aviso (${s.vbatAvisoCentiV}) deve ser > crítica (${s.vbatCriticaCentiV})`
  );
  check(
    Array.isArray(s.outputs) && s.outputs.length >= 1 && s.outputs.every((o) => typeof o.index === 'number'),
    `estação ${i}: outputs[] com pelo menos 1 saída p/ o teste de pulso`
  );
});

if (fail) { console.error(`\n${fail} verificação(ões) falharam`); process.exit(1); }
console.log('OK: mock levels consistente');
