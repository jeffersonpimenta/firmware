# UI de nível (boia) — cards + form em frase — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reescrever o painel "Controle de nível" da UI de irrigação para cards + formulário em linguagem natural, com dropdowns amigáveis no lugar de hex/índices crus.

**Architecture:** Toda a lógica de montagem de HTML vira funções puras testáveis em Node (`niveisCardHtml`, `niveisFormHtml`, helpers de nome/opções). `renderNiveis()` só orquestra fetch + composição + wiring de eventos. Sem mudança de backend: reusa `/levels`, `/levels/delete`, `/stations`, `/zones`, `/sensors`. Mock ganha dados consistentes para exercitar cards e dropdowns.

**Tech Stack:** Vanilla JS (sem dependências, dispositivo offline), CSS com tokens oklch, testes Node via `eval` (padrão de `test_mock.js`), suíte nativa C++ (`./bin/run-tests.sh`) para regressão do firmware.

## Global Constraints

- Sem dependências JS externas (dispositivo offline serve arquivos estáticos). — copiado do cabeçalho de `app.js:1`.
- Sensor digital = `tipo === 0`; analógico = `tipo === 1`. Boia é sempre digital. — de `app.js:770`.
- Escapar todo texto vindo do usuário com `esc()` antes de inserir em HTML. — de `app.js:28`.
- Campo `mensagem` ≤ 23 chars. — de `LevelRule` / `IrrigationWebApi.cpp`.
- Não tocar em `src/` (backend). A suíte C++ deve permanecer GREEN (`./bin/run-tests.sh` → exit 0).
- Polaridade: o estado escolhido em `ligaQuandoAtivo` corresponde fisicamente a "reservatório baixo" (é quando a bomba enche). A frase sempre termina `→ reservatório baixo.`
- Node hex só aparece como **fallback de exibição** quando a estação não tem nome; nunca como campo de entrada.

## File Structure

- `data/irrigacao/mock.js` — Modify: adiciona 2ª regra em `STATE.levels` + 1 sensor digital em `STATE.sensors` para consistência. Responsável pelos dados de exemplo servidos aos 4 endpoints.
- `data/irrigacao/app.js` — Modify: substitui `renderNiveis()` (`app.js:1434-1515`); adiciona helpers puros de HTML e `wireNiveis()`. Responsável pela UI do painel de nível.
- `data/irrigacao/style.css` — Modify: adiciona bloco `.lvl-*` (cards + form em frase + avançado recolhível). Responsável pela aparência.
- `data/irrigacao/test_niveis.js` — Create: teste Node das funções puras de HTML (harness por `eval` com stubs de DOM/fetch).
- `data/irrigacao/test_mock.js` — Modify: estende o smoke test para asserir forma e consistência de `STATE.levels`.

---

### Task 1: Dados de mock consistentes + smoke test

Adiciona uma 2ª regra de nível e o sensor digital que ela referencia, cobrindo ambas polaridades e um `staleTimeoutS` distinto. Sem isso os cards/dropdowns novos não teriam variedade para exercitar.

**Files:**
- Modify: `data/irrigacao/mock.js` — editar os literais em `MOCK_DATA` (linha 5); `MOCK_DATA.sensors` node `0xe5f6a7b8` ~linha 114-120; `MOCK_DATA.levels` ~linha 182-194. `STATE` é cópia profunda (`let STATE = JSON.parse(JSON.stringify(MOCK_DATA))`, linha 249), então reflete as edições.
- Modify/Test: `data/irrigacao/test_mock.js`

**Interfaces:**
- Consumes: nada (primeira task).
- Produces: `STATE.levels` com ≥2 regras; cada regra tem `sensorNode`+`sensorIdx` apontando para um sensor `tipo===0` existente em `STATE.sensors`, e `targetZoneId` existente em `STATE.zones`. Estação `0xe5f6a7b8` ("Pomar") passa a ter sensor digital idx 1 "Boia caixa".

- [ ] **Step 1: Escrever o teste que falha**

Reescrever `data/irrigacao/test_niveis_mock.js`? Não — estender `data/irrigacao/test_mock.js`. Substituir o conteúdo a partir da linha 26 (`// Testa`) por asserts reais:

```js
// Testa consistência de STATE.levels com stations/zones/sensors
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

if (fail) { console.error(`\n${fail} verificação(ões) falharam`); process.exit(1); }
console.log('OK: mock levels consistente');
```

- [ ] **Step 2: Rodar o teste e confirmar que falha**

Run: `node data/irrigacao/test_mock.js` (a partir de `data/irrigacao/`: `cd data/irrigacao; node test_mock.js`)
Expected: FAIL — hoje só existe 1 regra (`polarities.size === 1`) e a estação Pomar não tem sensor digital, então `levels deve ter >= 2 regras` e/ou a checagem de polaridade falham; exit 1.

- [ ] **Step 3: Adicionar o sensor digital na estação Pomar**

Em `data/irrigacao/mock.js`, no objeto de `STATE.sensors` com `node: 0xe5f6a7b8`, trocar o array `sensores` para incluir a boia:

```js
    {
      node: 0xe5f6a7b8,
      nome: 'Estação Pomar',
      tamper: false,
      sensores: [
        { idx: 0, nome: 'Pressão', tipo: 1, valor: 650 },
        { idx: 1, nome: 'Boia caixa', tipo: 0, valor: 0 },
      ],
    },
```

- [ ] **Step 4: Adicionar a 2ª regra em STATE.levels**

Em `data/irrigacao/mock.js`, substituir o array `levels` por duas regras (polaridades opostas, timeouts distintos):

```js
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
```

- [ ] **Step 5: Rodar o teste e confirmar que passa**

Run: `cd data/irrigacao; node test_mock.js`
Expected: PASS — imprime `OK: mock levels consistente`; exit 0.

- [ ] **Step 6: Commit**

```bash
git add data/irrigacao/mock.js data/irrigacao/test_mock.js
git commit -m "test(irrigation): nível — mock com 2 regras consistentes + smoke test"
```

---

### Task 2: Funções puras de HTML + testes Node

Extrai toda a montagem de HTML do painel para funções puras, cobertas por teste Node. É a parte de risco (nomes resolvidos, polaridade, filtro digital, escaping) — TDD real aqui.

**Files:**
- Modify: `data/irrigacao/app.js` (inserir as funções logo antes de `async function renderNiveis()` em `app.js:1435`)
- Create: `data/irrigacao/test_niveis.js`

**Interfaces:**
- Consumes: helpers existentes `esc`, `num`, `nodeHex`, `stationName` (`app.js:29,39,55,204`).
- Produces (assinaturas que a Task 3 usa):
  - `fmtDur(s) -> string` (ex.: 300→"5m", 90→"90s", 3600→"1h")
  - `sensorName(sensors, node, idx) -> string` (escapado; fallback `"s{idx}"`)
  - `zoneName(zones, id) -> string` (escapado; fallback `"zona {id}"`)
  - `polarityText(liga) -> string` ("ATIVA (= reservatório baixo)" | "INATIVA (= reservatório baixo)")
  - `stationOptionsHtml(stations, selNode) -> string`
  - `sensorOptionsHtml(sensors, node, selIdx) -> string` (só `tipo===0`)
  - `zoneOptionsHtml(zones, selId) -> string`
  - `niveisCardHtml(rule, stations, zones, sensors) -> string`
  - `niveisFormHtml(stations, zones, sensors, rule|null) -> string`

- [ ] **Step 1: Escrever o teste que falha**

Criar `data/irrigacao/test_niveis.js` (harness por eval com stubs, à la `test_mock.js`; `eval` de `app.js` é estrito, então capturamos as funções via objeto global no fim do eval):

```js
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
t(formEdit.includes('Editar regra') && formEdit.includes('value="1"'), 'form edição preenche id/valores');

if (fail) { console.error(`\n${fail} verificação(ões) falharam`); process.exit(1); }
console.log('OK: helpers de nível');
```

- [ ] **Step 2: Rodar o teste e confirmar que falha**

Run: `cd data/irrigacao; node test_niveis.js`
Expected: FAIL — `ReferenceError: fmtDur is not defined` (ou similar) na linha do `Object.assign`, pois as funções ainda não existem em `app.js`.

- [ ] **Step 3: Implementar as funções puras**

Em `data/irrigacao/app.js`, inserir imediatamente antes de `// ===== Controle de nível por boia =====` (`app.js:1434`):

```js
// ===== Nível (boia) — helpers puros de HTML =====
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
function stationOptionsHtml(stations, selNode) {
  return (Array.isArray(stations) ? stations : []).map((s) =>
    `<option value="${num(s.node)}"${num(s.node) === num(selNode) ? ' selected' : ''}>${s.name ? esc(s.name) : nodeHex(s.node)}</option>`).join('');
}
function sensorOptionsHtml(sensors, node, selIdx) {
  const st = (Array.isArray(sensors) ? sensors : []).find((x) => x && num(x.node) === num(node));
  const list = st && Array.isArray(st.sensores) ? st.sensores.filter((s) => s && num(s.tipo) === 0) : [];
  if (!list.length) return '<option value="">(sem boia digital)</option>';
  return list.map((s) =>
    `<option value="${num(s.idx)}"${num(s.idx) === num(selIdx) ? ' selected' : ''}>${s.nome ? esc(s.nome) : ('s' + num(s.idx))}</option>`).join('');
}
function zoneOptionsHtml(zones, selId) {
  return (Array.isArray(zones) ? zones : []).map((z) =>
    `<option value="${num(z.id)}"${num(z.id) === num(selId) ? ' selected' : ''}>${z.name ? esc(z.name) : ('zona ' + num(z.id))}</option>`).join('');
}
function niveisCardHtml(r, stations, zones, sensors) {
  r = r || {};
  const est = stationName(stations, r.sensorNode);
  const sen = sensorName(sensors, r.sensorNode, r.sensorIdx);
  const zon = zoneName(zones, r.targetZoneId);
  const msg = r.mensagem ? `<div class="lvl-msg muted">“${esc(r.mensagem)}”</div>` : '';
  return `<div class="card lvl-card" data-nid="${num(r.id)}">
    <div class="lvl-hdr">${est} · boia “${sen}”</div>
    <div class="lvl-line">liga bomba <span class="chev">▸</span> <b>${zon}</b></div>
    <div class="lvl-line">quando boia ${polarityText(r.ligaQuandoAtivo)}</div>
    <div class="lvl-chips">
      <span class="chip gray">min ${fmtDur(r.minOnS)} on / ${fmtDur(r.minOffS)} off</span>
      <span class="chip amber">sem sinal ${fmtDur(r.staleTimeoutS)} ⚠</span>
    </div>${msg}
    <div class="lvl-btns">
      <button class="btn ghost sm" data-nedit="${num(r.id)}">Editar</button>
      <button class="btn dangerline sm" data-ndel="${num(r.id)}">Excluir</button>
    </div>
  </div>`;
}
function niveisFormHtml(stations, zones, sensors, r) {
  const firstNode = (Array.isArray(stations) && stations[0]) ? num(stations[0].node) : 0;
  const firstZone = (Array.isArray(zones) && zones[0]) ? num(zones[0].id) : 1;
  r = r || { id: 0, sensorNode: firstNode, sensorIdx: 0, ligaQuandoAtivo: true, targetZoneId: firstZone, minOnS: 30, minOffS: 30, staleTimeoutS: 90, mensagem: '' };
  const advOpen = num(r.minOnS) !== 30 || num(r.minOffS) !== 30 || num(r.staleTimeoutS) !== 90 || (r.mensagem || '') !== '';
  const editing = num(r.id) > 0;
  return `<div class="card form lvl-form">
    <div class="sec-title">${editing ? 'Editar regra' : 'Nova regra'}</div>
    <input type="hidden" id="nv-id" value="${num(r.id)}">
    <div class="lvl-sentence">
      A bomba
      <select class="finput" id="nv-zone">${zoneOptionsHtml(zones, r.targetZoneId)}</select>
      liga quando a boia
      <select class="finput" id="nv-node">${stationOptionsHtml(stations, r.sensorNode)}</select>
      <select class="finput" id="nv-sidx">${sensorOptionsHtml(sensors, r.sensorNode, r.sensorIdx)}</select>
      estiver
      <select class="finput" id="nv-liga">
        <option value="1"${r.ligaQuandoAtivo ? ' selected' : ''}>ativa</option>
        <option value="0"${r.ligaQuandoAtivo ? '' : ' selected'}>inativa</option>
      </select>
      <span class="lvl-hint">→ reservatório baixo.</span>
    </div>
    <button class="btn ghost sm lvl-adv-toggle" id="nv-adv-toggle" type="button">${advOpen ? '▾' : '▸'} Ajustes avançados</button>
    <div class="lvl-adv${advOpen ? '' : ' hidden'}" id="nv-adv">
      <div class="frow">
        <label class="fld"><span class="flbl">Min ligada (s)</span>
          <input class="finput" id="nv-minon" type="number" min="0" value="${num(r.minOnS)}"></label>
        <label class="fld"><span class="flbl">Min desligada (s)</span>
          <input class="finput" id="nv-minoff" type="number" min="0" value="${num(r.minOffS)}"></label>
      </div>
      <label class="fld"><span class="flbl">Falha se sem sinal (s)</span>
        <input class="finput" id="nv-stale" type="number" min="1" value="${num(r.staleTimeoutS)}"></label>
      <label class="fld"><span class="flbl">Mensagem de alerta (opcional)</span>
        <input class="finput" id="nv-msg" maxlength="23" value="${esc(r.mensagem || '')}"></label>
    </div>
    <div class="frow lvl-form-btns">
      <button class="btn" id="nv-save">Salvar</button>
      <button class="btn ghost${editing ? '' : ' hidden'}" id="nv-cancel" type="button">Cancelar</button>
    </div>
  </div>`;
}
```

- [ ] **Step 4: Rodar o teste e confirmar que passa**

Run: `cd data/irrigacao; node test_niveis.js`
Expected: PASS — imprime `OK: helpers de nível`; exit 0.

- [ ] **Step 5: Commit**

```bash
git add data/irrigacao/app.js data/irrigacao/test_niveis.js
git commit -m "feat(irrigation): nível — helpers puros de HTML (cards/form) + testes"
```

---

### Task 3: renderNiveis + wiring de eventos

Substitui o corpo antigo de `renderNiveis()` para compor via helpers e liga os eventos (troca de estação recarrega boias, avançado recolhível, editar/cancelar/salvar/excluir). Verificação manual no browser via mock.

**Files:**
- Modify: `data/irrigacao/app.js` — substituir `renderNiveis()` inteiro (`app.js:1435-1515`) e adicionar `wireNiveis()` + `errText()`.

**Interfaces:**
- Consumes: todos os helpers da Task 2; `getJson`, `postJson`, `view`, `num`, `confirm`, `alert`.
- Produces: `renderNiveis(editId?)` — assinatura compatível com o router (`RENDER.niveis = renderNiveis`, chamado sem args em `app.js:1595` região do mapa RENDER). `editId` opcional abre o form já preenchido para aquela regra.

- [ ] **Step 1: Substituir renderNiveis e adicionar wiring**

Em `data/irrigacao/app.js`, remover o corpo antigo (de `async function renderNiveis() {` até o `}` que fecha em `app.js:1515`) e colocar:

```js
async function renderNiveis(editId) {
  const [rules, stations, zones, sensors] = await Promise.all([
    getJson('/levels').catch(() => []),
    getJson('/stations').catch(() => []),
    getJson('/zones').catch(() => []),
    getJson('/sensors').catch(() => []),
  ]);
  const list = Array.isArray(rules) ? rules : [];
  const editRule = editId != null ? list.find((x) => x && num(x.id) === num(editId)) : null;
  const cards = list.length
    ? list.map((r) => niveisCardHtml(r, stations, zones, sensors)).join('')
    : '<div class="empty">Nenhuma regra de nível.</div>';
  view.innerHTML = `<div class="lvl-list">${cards}</div>` + niveisFormHtml(stations, zones, sensors, editRule);
  wireNiveis(stations, zones, sensors, list);
  if (editRule) {
    const f = view.querySelector('.lvl-form');
    if (f) f.scrollIntoView({ behavior: 'smooth', block: 'center' });
  }
}

function errText(r) {
  return (r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro';
}

function wireNiveis(stations, zones, sensors, list) {
  const q = (sel) => view.querySelector(sel);

  const nodeSel = q('#nv-node');
  if (nodeSel) nodeSel.addEventListener('change', () => {
    const sidx = q('#nv-sidx');
    if (sidx) sidx.innerHTML = sensorOptionsHtml(sensors, num(nodeSel.value), -1);
  });

  const advToggle = q('#nv-adv-toggle');
  if (advToggle) advToggle.addEventListener('click', () => {
    const box = q('#nv-adv');
    if (!box) return;
    const nowHidden = box.classList.toggle('hidden');
    advToggle.textContent = (nowHidden ? '▸' : '▾') + ' Ajustes avançados';
  });

  view.querySelectorAll('[data-ndel]').forEach((b) => b.addEventListener('click', async () => {
    if (!confirm('Excluir esta regra?')) return;
    const r = await postJson('/levels/delete', { id: num(b.dataset.ndel) });
    if (r.ok) renderNiveis().catch(() => {});
    else alert('Falha ao excluir: ' + errText(r));
  }));

  view.querySelectorAll('[data-nedit]').forEach((b) => b.addEventListener('click', () => {
    renderNiveis(num(b.dataset.nedit)).catch(() => {});
  }));

  const cancel = q('#nv-cancel');
  if (cancel) cancel.addEventListener('click', () => renderNiveis().catch(() => {}));

  const save = q('#nv-save');
  if (save) save.addEventListener('click', async () => {
    const node = num(q('#nv-node').value);
    const sidxRaw = q('#nv-sidx').value;
    if (!node || sidxRaw === '') { alert('Selecione a estação e a boia.'); return; }
    const body = {
      id: num(q('#nv-id').value),
      sensorNode: node,
      sensorIdx: num(sidxRaw),
      ligaQuandoAtivo: num(q('#nv-liga').value) === 1,
      targetZoneId: num(q('#nv-zone').value),
      minOnS: num(q('#nv-minon').value),
      minOffS: num(q('#nv-minoff').value),
      staleTimeoutS: num(q('#nv-stale').value),
      mensagem: q('#nv-msg').value.slice(0, 23),
    };
    const r = await postJson('/levels', body);
    if (r.ok) renderNiveis().catch(() => {});
    else alert('Falha: ' + errText(r));
  });
}
```

- [ ] **Step 2: Rodar o teste de helpers (regressão) e confirmar verde**

Run: `cd data/irrigacao; node test_niveis.js`
Expected: PASS — `OK: helpers de nível`. (O eval de `app.js` agora inclui `renderNiveis`/`wireNiveis`; os stubs de DOM/fetch cobrem o wiring top-level.)

- [ ] **Step 3: Verificação manual no browser (mock)**

Servir o painel com mock. Em `data/irrigacao/`:

```bash
python -m http.server 8080
```

Garantir que `data/irrigacao/index.html` tem `<script src="mock.js"></script>` **descomentado** antes de `app.js` (linha ~30; hoje no working tree está ativo — é o `M index.html` do git status). Abrir `http://localhost:8080/index.html`. O `mock.js` substitui `window.fetch` no load (não precisa de query param). Ir em "Mais" → "Controle de nível" e confirmar, um a um:

- [ ] Aparecem **2 cards**: "Horta Norte · boia "Nível"" (ATIVA) e "Pomar · boia "Boia caixa"" (INATIVA). Sem `0x...` visível.
- [ ] Chips mostram `min 5m on / 10m off` + `sem sinal 1h ⚠` no 1º; `min 1m on / 2m off` + `sem sinal 90s ⚠` no 2º.
- [ ] Form em frase: selects de Zona/Estação/Sensor/polaridade preenchidos; "→ reservatório baixo." ao final.
- [ ] Trocar a **Estação** no form recarrega o select de boia (ex.: Horta Norte → só "Nível"; Pomar → só "Boia caixa"; nunca sensores analógicos como "Pressão").
- [ ] "▸ Ajustes avançados" expande/recolhe e o triângulo vira ▾/▸.
- [ ] "Editar" num card abre o form preenchido ("Editar regra"), rola até ele, mostra "Cancelar"; "Cancelar" volta pra "Nova regra".
- [ ] "Salvar" sem estação/boia dispara o alerta "Selecione a estação e a boia."

- [ ] **Step 4: Rodar a suíte nativa e confirmar GREEN (nada em src/ foi tocado)**

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN). (No Windows: rodar via Docker conforme memória do projeto.)

- [ ] **Step 5: Commit**

```bash
git add data/irrigacao/app.js
git commit -m "feat(irrigation): nível — renderNiveis em cards + form em frase com dropdowns"
```

---

### Task 4: CSS do painel de nível

Estiliza os cards e o form em frase reusando os tokens existentes.

**Files:**
- Modify: `data/irrigacao/style.css` (adicionar bloco ao final da seção de cards; antes de `.hidden`/utilitários está OK).

**Interfaces:**
- Consumes: vars `--muted`, `--text`, `--border`, `--amber`, `--card`, `--radius-card` (`style.css:3-18`); classes `.card`, `.chip`, `.chip.gray`, `.chip.amber`, `.finput`, `.hidden`.
- Produces: classes `.lvl-list`, `.lvl-card`, `.lvl-hdr`, `.lvl-line`, `.lvl-chips`, `.lvl-msg`, `.lvl-btns`, `.lvl-sentence`, `.lvl-hint`, `.lvl-adv`, `.lvl-adv-toggle`, `.lvl-form-btns`.

- [ ] **Step 1: Adicionar o bloco CSS**

Em `data/irrigacao/style.css`, adicionar:

```css
/* ===== Controle de nível (boia) ===== */
.lvl-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
  margin-bottom: 12px;
}
.lvl-card .lvl-hdr {
  font-weight: 600;
  margin-bottom: 4px;
}
.lvl-card .lvl-line {
  font-size: 0.9em;
  color: var(--muted);
}
.lvl-card .lvl-line b {
  color: var(--text);
  font-weight: 600;
}
.lvl-card .lvl-chips {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 8px;
}
.lvl-card .lvl-msg {
  margin-top: 6px;
  font-style: italic;
  font-size: 0.85em;
}
.lvl-card .lvl-btns {
  display: flex;
  gap: 8px;
  justify-content: flex-end;
  margin-top: 10px;
}
.lvl-sentence {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  line-height: 2.2;
}
.lvl-sentence .finput {
  width: auto;
  display: inline-block;
  padding: 4px 8px;
}
.lvl-hint {
  color: var(--muted);
}
.lvl-adv {
  margin-top: 10px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}
.lvl-adv-toggle {
  margin-top: 10px;
}
.lvl-form-btns {
  margin-top: 12px;
}
```

- [ ] **Step 2: Verificação visual no browser (mock)**

Recarregar `http://localhost:8080/index.html?mock=1` → "Mais" → "Controle de nível". Confirmar:

- [ ] Cards com espaçamento/coluna (não coladas), chips coloridos (cinza + âmbar), botões alinhados à direita.
- [ ] Frase do form flui em linha, com os selects inline alinhados ao texto (quebra natural em telas estreitas).
- [ ] Bloco "Ajustes avançados" separado por linha superior.

- [ ] **Step 3: Commit**

```bash
git add data/irrigacao/style.css
git commit -m "style(irrigation): nível — CSS de cards + form em frase"
```

---

### Task 5: Verificação final, formatação e push

Fecha o trabalho: formata, roda todos os testes, e faz push com os dados de mock incluídos (requisito do usuário).

**Files:** nenhum novo; validação sobre os já modificados.

**Interfaces:** —

- [ ] **Step 1: Formatar**

Run: `trunk fmt` (se disponível). Se `trunk` não estiver instalado, pular sem alterar arquivos manualmente.
Expected: sem erros; possíveis reformatações em `data/irrigacao/*`.

- [ ] **Step 2: Rodar todos os testes JS**

Run:
```bash
cd data/irrigacao
node test_mock.js
node test_niveis.js
```
Expected: `OK: mock levels consistente` e `OK: helpers de nível`; ambos exit 0.

- [ ] **Step 3: Rodar suíte nativa**

Run: `./bin/run-tests.sh` (Windows: via Docker).
Expected: exit 0 (GREEN).

- [ ] **Step 4: Commit de formatação (se `trunk fmt` mudou algo)**

```bash
git add -A data/irrigacao
git commit -m "chore(irrigation): nível — trunk fmt"
```
(Pular se nada mudou.)

- [ ] **Step 5: NÃO commitar o toggle de teste do index.html**

O requisito do usuário é incluir os **dados** de mock (`mock.js`, feito na Task 1), não fazer produção carregar o mock. O `M data/irrigacao/index.html` do working tree é o `<script src="mock.js">` descomentado para teste local — deve permanecer **comentado** no repo (ver commit `fe85bf7aa`, "prod must not load mock"). Confirmar que `index.html` não está staged e restaurar o estado commitado:

```bash
git restore data/irrigacao/index.html   # descarta o toggle local; prod continua sem mock
git status --short data/irrigacao/index.html   # esperado: vazio (sem M)
```

- [ ] **Step 6: Push (inclui dados de mock)**

Confirmar que `data/irrigacao/mock.js` está entre os arquivos commitados (Task 1). Então:

```bash
git push origin sistema-irrigacao
```
Expected: push aceito; branch `sistema-irrigacao` atualizada com `app.js`, `style.css`, `mock.js`, `test_mock.js`, `test_niveis.js` — e **sem** alteração de `index.html`.

---

## Self-Review

**Spec coverage:**
- Carregamento paralelo `/levels`+`/stations`+`/zones`+`/sensors` → Task 3 Step 1. ✓
- Card por regra (nomes, polaridade, chips, editar/excluir, vazio) → Task 2 (`niveisCardHtml`) + Task 3. ✓
- Form em frase com dropdowns Zona/Estação/Sensor + polaridade + avançado recolhível → Task 2 (`niveisFormHtml`) + Task 3 (wiring). ✓
- Sensor recarrega ao trocar estação, só digitais → Task 3 Step 1 (`nv-node` change) + Task 2 (`sensorOptionsHtml` filtro `tipo===0`). ✓
- Correção semântica (mata dropdown ambíguo; frase sempre "→ reservatório baixo") → Task 2 (`polarityText`, `niveisFormHtml`). ✓
- CSS mínimo `.lvl-sentence` + `.lvl-adv` reusando vocab → Task 4. ✓
- Mock consistente cobrindo ambas polaridades → Task 1. ✓
- Requisito do usuário: commit/push inclui mock → Task 1 (commit do mock) + Task 5 Step 5. ✓
- Fora de escopo (status ao vivo, backend) → respeitado: nada em `src/`, sem novo endpoint. ✓

**Placeholder scan:** sem TBD/TODO; todo passo com código ou comando concreto. ✓

**Type consistency:** nomes usados na Task 3 (`niveisCardHtml`, `niveisFormHtml`, `sensorOptionsHtml`, `fmtDur`, `errText`, `wireNiveis`, `renderNiveis`) batem com as definições da Task 2 e com o bloco de Interfaces. Selects `#nv-zone/#nv-node/#nv-sidx/#nv-liga/#nv-id/#nv-minon/#nv-minoff/#nv-stale/#nv-msg` idênticos entre `niveisFormHtml` (Task 2) e o `save`/`wireNiveis` (Task 3). ✓
