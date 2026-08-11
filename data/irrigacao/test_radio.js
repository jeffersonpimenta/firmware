const fs = require('fs');
const appCode = fs.readFileSync('app.js', 'utf-8');

// Stub mínimo de DOM/browser p/ eval do app.js (mesmo padrão de test_niveis.js,
// mas com window.location.search, que o app.js lê no topo).
const elStub = () => ({
  addEventListener() {}, removeEventListener() {},
  classList: { add() {}, remove() {}, toggle() { return false; } },
  querySelectorAll() { return []; }, querySelector() { return null; },
  appendChild() {}, replaceWith() {}, scrollIntoView() {},
  set innerHTML(_) {}, get innerHTML() { return ''; },
  dataset: {}, textContent: '', firstElementChild: null,
});
global.document = { getElementById: elStub, querySelectorAll: () => [], createElement: elStub };
global.fetch = () => Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}) });
global.window = { location: { search: '' } };
global.location = { search: '' };
global.confirm = () => true; global.alert = () => {};

global.L = {};
eval(appCode + '\nObject.assign(global.L, { malhaCardsHtml, coberturaTableHtml, radioSegHtml });');

let fail = 0;
const t = (cond, msg) => { if (!cond) { console.error('FAIL:', msg); fail++; } };

// ---- malhaCardsHtml ----
const stations = [
  { node: 0xa1b2c3d4, name: 'Horta Norte', snrQuarterDb: 28, rssiDbm: -80, vbatCentiV: 410 }, // 7 dB -> green
  { node: 0xe5f6a7b8, name: 'Pomar',       snrQuarterDb: 12, rssiDbm: -95, vbatCentiV: 390 }, // 3 dB -> amber
  { node: 0xd1e2f3a4, name: 'Represa',     snrQuarterDb: 4,  rssiDbm: -110, vbatCentiV: 370 }, // 1 dB -> red
];
const cards = L.malhaCardsHtml(stations);
t(cards.includes('Horta Norte') && cards.includes('Pomar'), 'malha: mostra nomes das estações');
t(cards.includes('7.0 dB') && cards.includes('-80 dBm'), 'malha: SNR em dB e RSSI em dBm');
t(cards.includes('chip green') && cards.includes('chip amber') && cards.includes('chip red'), 'malha: chip de qualidade por SNR');
t(cards.includes('>SNR<') && cards.includes('>RSSI<') && cards.includes('>Bateria<'), 'malha: rótulos das 3 stats');

const cardsGray = L.malhaCardsHtml([{ node: 0x1, name: 'Sem SNR' }]);
t(cardsGray.includes('chip gray') && cardsGray.includes('—'), 'malha: sem snrQuarterDb -> chip gray e travessão');

t(L.malhaCardsHtml([]).includes('Nenhuma estação conhecida.'), 'malha: lista vazia');
t(L.malhaCardsHtml(null).includes('Nenhuma estação conhecida.'), 'malha: null tratado como vazio');

const cardsXss = L.malhaCardsHtml([{ node: 0x1, name: '<img onerror=1>', snrQuarterDb: 28, rssiDbm: -80, vbatCentiV: 400 }]);
t(cardsXss.includes('&lt;img onerror=1&gt;') && !cardsXss.includes('<img onerror=1>'), 'malha: escapa nome (XSS)');

// ---- coberturaTableHtml ----
const rows = [
  { no: 0xa1b2c3d4, role: 0, coord: 1, lat: 234500000, lon: -467800000, snr: 24, rssi: -90, idadeS: 12 },
  { no: 0xe5f6a7b8, role: 9, coord: 0, snr: 8, rssi: -100, idadeS: 300 },
];
const tbl = L.coberturaTableHtml(rows);
t(tbl.includes('id="cov-clear"'), 'cobertura: botão Limpar presente');
t(tbl.includes('>Nó<') && tbl.includes('>Papel<') && tbl.includes('>Coordenada<') && tbl.includes('>Idade<'), 'cobertura: cabeçalho da tabela');
t(tbl.includes('Estação'), 'cobertura: papel 0 -> Estação');
t(tbl.includes('papel 9'), 'cobertura: papel desconhecido -> fallback "papel N"');
t(tbl.includes('23.45000, -46.78000'), 'cobertura: coordenada formatada quando coord=1');
t(/<td[^>]*>—<\/td>/.test(tbl), 'cobertura: coord ausente -> travessão');

t(L.coberturaTableHtml([]).includes('Sem beacons recebidos.'), 'cobertura: lista vazia');
t(L.coberturaTableHtml(null).includes('Sem beacons recebidos.'), 'cobertura: null tratado como vazio');

// ---- radioSegHtml ----
const segE = L.radioSegHtml('enlace');
t(segE.includes('data-radioseg="enlace"') && segE.includes('data-radioseg="cobertura"'), 'seg: ambos os botões presentes');
t(/seg wide active[^>]*data-radioseg="enlace"/.test(segE), 'seg: enlace ativo em signalSeg=enlace');
t(!/seg wide active[^>]*data-radioseg="cobertura"/.test(segE), 'seg: cobertura não-ativo em signalSeg=enlace');
const segC = L.radioSegHtml('cobertura');
t(/seg wide active[^>]*data-radioseg="cobertura"/.test(segC), 'seg: cobertura ativo em signalSeg=cobertura');
t(segE.includes('>Enlace<') && segE.includes('>Cobertura<'), 'seg: rótulos Enlace/Cobertura');

if (fail) { console.error(`\n${fail} verificação(ões) falharam`); process.exit(1); }
console.log('OK: helpers de rádio (enlace/cobertura)');
process.exit(0);
