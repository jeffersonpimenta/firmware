const fs = require('fs');
const appCode = fs.readFileSync('app.js', 'utf-8');

// Stub mínimo de DOM/browser para permitir eval de app.js (que tem wiring top-level).
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
global.window = {}; global.location = { search: '' };
global.confirm = () => true; global.alert = () => {};

// Captura as funções puras (eval estrito não vaza declarações; Object.assign no fim, mesmo escopo).
global.L = {};
eval(appCode + '\nObject.assign(global.L, { fmtDur, sensorName, zoneName, polarityText, stationOptionsHtml, sensorOptionsHtml, zoneOptionsHtml, niveisCardHtml, niveisFormHtml });');

const stations = [{ node: 0xa1b2c3d4, name: 'Horta Norte' }, { node: 0xe5f6a7b8, name: 'Pomar' }];
const zones = [{ id: 1, name: 'Horta' }, { id: 3, name: 'Pastagem' }];
const sensors = [
  { node: 0xa1b2c3d4, sensores: [{ idx: 0, nome: 'Pressão', tipo: 1 }, { idx: 1, nome: 'Nível', tipo: 0 }] },
  { node: 0xe5f6a7b8, sensores: [{ idx: 1, nome: 'Boia caixa', tipo: 0 }] },
];
const rule = { id: 1, sensorNode: 0xa1b2c3d4, sensorIdx: 1, ligaQuandoAtivo: true, targetZoneId: 1, minOnS: 300, minOffS: 600, staleTimeoutS: 3600, mensagem: 'Cisterna baixa' };

let fail = 0;
const t = (cond, msg) => { if (!cond) { console.error('FAIL:', msg); fail++; } };

t(L.fmtDur(300) === '5m', 'fmtDur 300 -> 5m');
t(L.fmtDur(90) === '90s', 'fmtDur 90 -> 90s');
t(L.fmtDur(3600) === '1h', 'fmtDur 3600 -> 1h');
t(L.sensorName(sensors, 0xa1b2c3d4, 1) === 'Nível', 'sensorName resolve nome');
t(L.sensorName(sensors, 0xa1b2c3d4, 9) === 's9', 'sensorName fallback s{idx}');
t(L.zoneName(zones, 3) === 'Pastagem', 'zoneName resolve nome');
t(L.zoneName(zones, 99) === 'zona 99', 'zoneName fallback');
t(L.polarityText(true).startsWith('ATIVA'), 'polarityText true -> ATIVA');
t(L.polarityText(false).startsWith('INATIVA'), 'polarityText false -> INATIVA');

const sopts = L.sensorOptionsHtml(sensors, 0xa1b2c3d4, 1);
t(sopts.includes('Nível') && !sopts.includes('Pressão'), 'sensorOptions só digitais (tipo 0)');
t(sopts.includes('value="1" selected') || sopts.includes('value="1"  selected') || /value="1"[^>]*selected/.test(sopts), 'sensorOptions marca idx selecionado');

const card = L.niveisCardHtml(rule, stations, zones, sensors);
t(card.includes('Horta Norte') && card.includes('Nível'), 'card mostra nomes de estação/sensor');
t(card.includes('Horta') && card.includes('ATIVA'), 'card mostra zona e polaridade');
t(card.includes('5m') && card.includes('10m') && card.includes('1h'), 'card mostra durações formatadas');
t(!/0x[0-9a-f]+/.test(card), 'card não mostra hex cru quando há nomes');
t(card.includes('data-nedit="1"') && card.includes('data-ndel="1"'), 'card tem botões editar/excluir com id');

const form = L.niveisFormHtml(stations, zones, sensors, null);
t(form.includes('Nova regra') && form.includes('id="nv-zone"') && form.includes('id="nv-node"') && form.includes('id="nv-sidx"'), 'form nova tem selects');
t(form.includes('reservatório baixo'), 'form tem a frase de polaridade');
const formEdit = L.niveisFormHtml(stations, zones, sensors, rule);
t(formEdit.includes('Editar regra') && formEdit.includes('id="nv-id" value="1"'), 'form edição preenche id do nv-id');

// XSS / escaping (security constraint esc())
const xssSensors = [{ node: 0xa1b2c3d4, sensores: [{ idx: 1, nome: '<img onerror=1>', tipo: 0 }] }];
const xssCard = L.niveisCardHtml({ id: 9, sensorNode: 0xa1b2c3d4, sensorIdx: 1, ligaQuandoAtivo: true, targetZoneId: 1, minOnS: 30, minOffS: 30, staleTimeoutS: 90, mensagem: '' }, stations, zones, xssSensors);
t(xssCard.includes('&lt;img onerror=1&gt;') && !xssCard.includes('<img onerror=1>'), 'card escapa nome de sensor (XSS)');

// fmtDur(0) pins zero behavior
t(L.fmtDur(0) === '0s', 'fmtDur 0 -> 0s');

if (fail) { console.error(`\n${fail} verificação(ões) falharam`); process.exit(1); }
console.log('OK: helpers de nível');
// eval de app.js agenda setInterval (poll da Visão Geral) que mantém o event loop vivo;
// saída explícita p/ o teste terminar em CI em vez de pendurar.
process.exit(0);
