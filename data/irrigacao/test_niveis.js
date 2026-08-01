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
eval(appCode + '\nObject.assign(global.L, { fmtDur, sensorName, zoneName, polarityText, nvDigitalSensors, nvCardHtml, nvListHtml, nvEditHtml });');

const stations = [
  { node: 0xa1b2c3d4, name: 'Horta Norte' },
  { node: 0xe5f6a7b8, name: 'Pomar' },
  { node: 0xd1e2f3a4, name: 'Represa' }, // sem sensores → sem boia
];
const zones = [{ id: 1, name: 'Horta' }, { id: 3, name: 'Pastagem' }];
const sensors = [
  { node: 0xa1b2c3d4, sensores: [{ idx: 0, nome: 'Pressão', tipo: 1 }, { idx: 1, nome: 'Nível', tipo: 0 }] },
  { node: 0xe5f6a7b8, sensores: [{ idx: 1, nome: 'Boia caixa', tipo: 0 }] },
];
const rule = { id: 1, sensorNode: 0xa1b2c3d4, sensorIdx: 1, ligaQuandoAtivo: true, targetZoneId: 1, minOnS: 300, minOffS: 600, staleTimeoutS: 3600, mensagem: 'Cisterna baixa' };
const A = String(0xa1b2c3d4); // node como decimal (num()), usado nos data-attrs

let fail = 0;
const t = (cond, msg) => { if (!cond) { console.error('FAIL:', msg); fail++; } };

// ---- helpers puros de valor ----
t(L.fmtDur(300) === '5m', 'fmtDur 300 -> 5m');
t(L.fmtDur(90) === '90s', 'fmtDur 90 -> 90s');
t(L.fmtDur(3600) === '1h', 'fmtDur 3600 -> 1h');
t(L.fmtDur(0) === '0s', 'fmtDur 0 -> 0s');
t(L.sensorName(sensors, 0xa1b2c3d4, 1) === 'Nível', 'sensorName resolve nome');
t(L.sensorName(sensors, 0xa1b2c3d4, 9) === 's9', 'sensorName fallback s{idx}');
t(L.zoneName(zones, 3) === 'Pastagem', 'zoneName resolve nome');
t(L.zoneName(zones, 99) === 'zona 99', 'zoneName fallback');
t(L.polarityText(true).startsWith('ATIVA'), 'polarityText true -> ATIVA');
t(L.polarityText(false).startsWith('INATIVA'), 'polarityText false -> INATIVA');

// ---- nvDigitalSensors: só tipo 0 ----
const digs = L.nvDigitalSensors(sensors, 0xa1b2c3d4);
t(digs.length === 1 && digs[0].nome === 'Nível', 'nvDigitalSensors filtra só digitais (tipo 0)');
t(L.nvDigitalSensors(sensors, 0xd1e2f3a4).length === 0, 'nvDigitalSensors vazio p/ nó sem sensores');

// ---- card da lista ----
const card = L.nvCardHtml(rule, stations, zones, sensors);
t(card.includes('Horta Norte') && card.includes('Nível'), 'card mostra estação/boia');
t(card.includes('Horta') && card.includes('ATIVA'), 'card mostra bomba e polaridade');
t(card.includes('min 5m/10m') && card.includes('sem sinal 1h'), 'card mostra durações formatadas');
t(card.includes('lvl-tank'), 'card tem mini-tanque');
t(card.includes('data-nedit="1"'), 'card é clicável p/ editar (data-nedit)');
t(!/0x[0-9a-f]+/.test(card), 'card não mostra hex cru quando há nomes');

// XSS / escaping (constraint esc())
const xssSensors = [{ node: 0xa1b2c3d4, sensores: [{ idx: 1, nome: '<img onerror=1>', tipo: 0 }] }];
const xssCard = L.nvCardHtml({ id: 9, sensorNode: 0xa1b2c3d4, sensorIdx: 1, ligaQuandoAtivo: true, targetZoneId: 1, minOnS: 30, minOffS: 30, staleTimeoutS: 90, mensagem: '' }, stations, zones, xssSensors);
t(xssCard.includes('&lt;img onerror=1&gt;') && !xssCard.includes('<img onerror=1>'), 'card escapa nome de sensor (XSS)');

// ---- tela lista ----
const listHtml = L.nvListHtml({ stations, zones, sensors, rules: [rule] });
t(listHtml.includes('Controle de nível') && listHtml.includes('+ Nova regra'), 'lista tem título + botão nova');
t(listHtml.includes('data-nedit="1"'), 'lista renderiza o card da regra');
const listEmpty = L.nvListHtml({ stations, zones, sensors, rules: [] });
t(listEmpty.includes('Nenhuma regra de nível.'), 'lista vazia');
const listFull = L.nvListHtml({ stations, zones, sensors, rules: [rule, rule, rule, rule] });
t(listFull.includes('Limite de 4 regras atingido.') && !listFull.includes('+ Nova regra'), 'lista no limite esconde nova regra');

// ---- tela edição (editando) ----
const draftEdit = { id: 1, sensorNode: 0xa1b2c3d4, sensorIdx: 1, ligaQuandoAtivo: true, targetZoneId: 1, minOnS: 300, minOffS: 600, staleTimeoutS: 600, mensagem: 'oi', advancedOpen: true };
const uiPlain = { errors: [], deleteConfirm: false };
const editHtml = L.nvEditHtml(draftEdit, { stations, zones, sensors }, uiPlain);
t(editHtml.includes('Editar regra'), 'edição: título editar');
t(editHtml.includes(`lvl-pill sel" data-nvst="${A}"`), 'edição: estação selecionada marcada');
t(editHtml.includes('lvl-pill sel" data-nvboia="1"'), 'edição: boia selecionada marcada');
t(editHtml.includes('Nível') && !editHtml.includes('Pressão'), 'edição: boia lista só digitais');
t(editHtml.includes('lvl-polbtn sel" data-nvpol="1"'), 'edição: polaridade ativa marcada');
t(editHtml.includes('lvl-pill sel" data-nvzone="1"'), 'edição: bomba selecionada marcada');
t(editHtml.includes('reservatório baixo'), 'edição: hint de polaridade');
t(editHtml.includes('2/23'), 'edição: contador de mensagem');
t(editHtml.includes('Excluir regra'), 'edição de regra existente tem excluir');

// polaridade inativa
const editInativa = L.nvEditHtml({ ...draftEdit, ligaQuandoAtivo: false }, { stations, zones, sensors }, uiPlain);
t(editInativa.includes('lvl-polbtn sel" data-nvpol="0"'), 'edição: polaridade inativa marcada');

// boia inexistente (nó sem sensores digitais)
const editSemBoia = L.nvEditHtml({ ...draftEdit, sensorNode: 0xd1e2f3a4, sensorIdx: '' }, { stations, zones, sensors }, uiPlain);
t(editSemBoia.includes('Nenhuma boia (sensor digital) nesta estação.'), 'edição: estado sem boia');

// caixa de erros
const editErro = L.nvEditHtml(draftEdit, { stations, zones, sensors }, { errors: ['Selecione a bomba a ser acionada.'], deleteConfirm: false });
t(editErro.includes('lvl-errbox') && editErro.includes('Selecione a bomba a ser acionada.'), 'edição: caixa de erros');

// confirmação de exclusão inline
const editDel = L.nvEditHtml(draftEdit, { stations, zones, sensors }, { errors: [], deleteConfirm: true });
t(editDel.includes('Excluir esta regra de nível?'), 'edição: confirmação de exclusão inline');

// nova regra
const draftNew = { id: 0, sensorNode: 0xa1b2c3d4, sensorIdx: 1, ligaQuandoAtivo: true, targetZoneId: null, minOnS: 60, minOffS: 60, staleTimeoutS: 600, mensagem: '', advancedOpen: false };
const editNew = L.nvEditHtml(draftNew, { stations, zones, sensors }, uiPlain);
t(editNew.includes('Nova regra') && !editNew.includes('Excluir regra'), 'nova regra: título + sem excluir');

if (fail) { console.error(`\n${fail} verificação(ões) falharam`); process.exit(1); }
console.log('OK: helpers de nível');
// eval de app.js agenda setInterval (poll da Visão Geral) que mantém o event loop vivo;
// saída explícita p/ o teste terminar em CI em vez de pendurar.
process.exit(0);
