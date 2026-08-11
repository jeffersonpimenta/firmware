# Tela unificada "Enlace / Cobertura" — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fundir as telas "Malha / Enlace" e "Cobertura" do painel web do gateway numa única tela com toggle segmentado, sem perder funcionalidade.

**Architecture:** SPA em `data/irrigacao/app.js`. Extrair a lógica de render das duas telas em helpers puros (`malhaCardsHtml`, `coberturaTableHtml`, `radioSegHtml`), testados via o harness node existente (stub de DOM + `eval` do app + export por `Object.assign(global.L, …)`). Depois trocar as duas telas por um único `renderRadio()` que lê estado de módulo (`signalSeg`), busca só o dataset da aba ativa e liga os eventos. Atualizar menu e mapas de roteamento; remover código morto.

**Tech Stack:** JavaScript vanilla (browser SPA), testes ad-hoc em Node.js (`node test_*.js`).

## Global Constraints

- Único arquivo de produção tocado: `data/irrigacao/app.js`. **Sem CSS novo** — reusa `.segrow` + `.seg wide` já existentes.
- Toda saída de dados do usuário/nó passa por `esc()` (constraint XSS do repo).
- Helpers de render são **puros**: recebem dados, retornam string HTML, não tocam o DOM nem fazem fetch.
- Rótulos exatos: menu = `Enlace / Cobertura`; subtítulo = `SNR/RSSI dos nós e cobertura de sinal`; abas = `Enlace` | `Cobertura`.
- Empty states preservados verbatim: `Nenhuma estação conhecida.` (enlace) e `Sem beacons recebidos.` (cobertura).
- Stub de teste **deve** definir `global.window = { location: { search: '' } }` — o `app.js` lê `window.location.search` no topo (o `test_niveis.js` atual quebra por não fazer isso; não é regressão desta mudança).

---

### Task 1: Helpers puros de render + testes

Adiciona três funções puras ao `app.js` (copiando a lógica de render atual) e um arquivo de teste que as valida. Não altera roteamento nem remove nada ainda — as telas antigas continuam funcionando em paralelo.

**Files:**
- Modify: `data/irrigacao/app.js` (adicionar 3 funções perto das telas atuais, ex. logo acima de `renderMalha` na linha ~3645)
- Create: `data/irrigacao/test_radio.js`

**Interfaces:**
- Consumes (já existentes em `app.js`, disponíveis no escopo do eval): `num(v)`, `esc(s)`, `nodeHex(n)`, `fmtVolts(centiV)`, `fmtSince(idadeS)`.
- Produces (usados pela Task 2 e pelos testes):
  - `malhaCardsHtml(list) -> string` — cards por estação conhecida; `list` = array de `{node, name, snrQuarterDb, rssiDbm, vbatCentiV}`.
  - `coberturaTableHtml(rows) -> string` — tabela de survey + botão `#cov-clear`; `rows` = array de `{no, role, coord, lat, lon, snr, rssi, idadeS}`.
  - `radioSegHtml(seg) -> string` — toggle segmentado; `seg` = `'enlace' | 'cobertura'`; botões com `data-radioseg`.

- [ ] **Step 1: Escrever o teste que falha**

Criar `data/irrigacao/test_radio.js`:

```js
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
```

- [ ] **Step 2: Rodar o teste e confirmar que falha**

Run: `cd data/irrigacao && node test_radio.js`
Expected: FAIL — throw `ReferenceError: malhaCardsHtml is not defined` na linha do `Object.assign` (helpers ainda não existem).

- [ ] **Step 3: Implementar os três helpers**

Em `data/irrigacao/app.js`, inserir logo acima de `// ===== Malha / Enlace …` (linha ~3645):

```js
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
```

- [ ] **Step 4: Rodar o teste e confirmar que passa**

Run: `cd data/irrigacao && node test_radio.js`
Expected: `OK: helpers de rádio (enlace/cobertura)` e exit 0.

- [ ] **Step 5: Commit**

```bash
git add data/irrigacao/app.js data/irrigacao/test_radio.js
git commit -m "feat(irrigation): helpers puros de render Enlace/Cobertura + testes"
```

---

### Task 2: Tela `renderRadio`, menu e roteamento; remove telas antigas

Liga os helpers numa única tela com estado de módulo, atualiza o menu "Mais" e os três mapas de roteamento, e remove `renderMalha`/`renderCobertura` (agora código morto).

**Files:**
- Modify: `data/irrigacao/app.js`

**Interfaces:**
- Consumes (Task 1): `malhaCardsHtml(list)`, `coberturaTableHtml(rows)`, `radioSegHtml(seg)`. E já existentes: `getJson(path)`, `postJson(path, body)`, `view`.
- Produces: `renderRadio() -> Promise<void>` (registrada em `RENDER.radio`); estado de módulo `signalSeg`.

- [ ] **Step 1: Adicionar estado de módulo + `renderRadio()`**

Em `data/irrigacao/app.js`, logo abaixo dos helpers da Task 1 (antes de onde estava `renderMalha`):

```js
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
```

- [ ] **Step 2: Remover as funções antigas `renderMalha` e `renderCobertura`**

Apagar o bloco inteiro de `renderCobertura` (de `// ===== Cobertura (site survey §8.5) =====` até o fim da função, ~linhas 2166–2195) e o bloco inteiro de `renderMalha` (de `// ===== Malha / Enlace (SNR/RSSI/bateria por nó) =====` até o fim da função, ~linhas 3645–3672). A lógica delas agora vive nos helpers da Task 1.

- [ ] **Step 3: Atualizar o menu `renderMais`**

Em `renderMais`, substituir as duas linhas:

```js
    ['cobertura', 'Cobertura', 'Pesquisa de sinal (site survey)'],
    ['malha', 'Malha / Enlace', 'Qualidade de rádio (SNR/RSSI) de cada nó'],
```

por uma só:

```js
    ['radio', 'Enlace / Cobertura', 'SNR/RSSI dos nós e cobertura de sinal'],
```

- [ ] **Step 4: Atualizar os três mapas de roteamento**

No mapa `RENDER`, remover:
```js
  cobertura: renderCobertura,
  malha: renderMalha,
```
e adicionar:
```js
  radio: renderRadio,
```

No mapa `SECTION_LABELS`, remover `cobertura: 'Cobertura',` e `malha: 'Malha',` e adicionar `radio: 'Enlace / Cobertura',`.

No objeto `POLLED`, remover `cobertura: 1,` e `malha: 1,` e adicionar `radio: 1,`.

- [ ] **Step 5: Verificar que os testes ainda passam e o app ainda faz eval**

Run: `cd data/irrigacao && node test_radio.js`
Expected: `OK: helpers de rádio (enlace/cobertura)` (helpers intactos).

- [ ] **Step 6: Verificar que nenhuma referência às chaves/funções antigas restou**

Run (do repo root): `git grep -nE "renderMalha|renderCobertura|['\"](malha|cobertura)['\"]|(malha|cobertura):" -- data/irrigacao/app.js`
Expected: **sem saída** (nenhuma linha). Se aparecer algo, é referência órfã — remover antes de commitar.

- [ ] **Step 7: Commit**

```bash
git add data/irrigacao/app.js
git commit -m "refactor(irrigation): unifica Malha e Cobertura na tela Enlace/Cobertura"
```

---

## Verificação manual (opcional, após Task 2)

Preview offline com o mock: abrir `data/irrigacao/index.html` no navegador (o `mock.js` já está incluído). Ir em **Mais → Enlace / Cobertura**, alternar o toggle **Enlace | Cobertura**, confirmar que os cards de estação e a tabela de survey (com **Limpar**) aparecem e que o toggle persiste durante o auto-refresh de 3 s.

## Notas

- Débito pré-existente (fora do escopo): `test_niveis.js` quebra contra o `app.js` atual porque seu stub não define `window.location.search`. Este plano não conserta isso; se for corrigir, aplicar o mesmo stub `global.window = { location: { search: '' } }` lá.
