# Portal de campo — port visual (Estação / Serviço / Repetidor) + aba Enlace — Plano de Implementação

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Aplicar a linguagem visual do painel do gateway ao portal de campo que cada dispositivo serve por AP Wi-Fi, reestruturando as três experiências por papel (Estação, Serviço, Repetidor) conforme o mockup `Irrigacao Portal.dc.html`, e adicionar a aba "Enlace" do repetidor (SNR/RSSI + histórico + vizinhos) com o backend correspondente.

**Architecture:** Frontend estático (`data/irrigacao/portal/`) consome endpoints `/api/portal/*`. Fase A reescreve `index.html` + `app.js` (funções de render por papel) reusando componentes do gateway (`.card`, `.chip`, `.btn`, `.tabs/.tab`) mais classes novas escopadas em `.field-portal .fp-*`, e adiciona `uptimeS` ao node-state — tudo verificável offline via `mock.js`. Fase B adiciona a função pura `buildLink` (host-tested), a captura de SNR/RSSI do gateway no rx do módulo, coleta de vizinhos via NodeDB, e o endpoint `GET /api/portal/link` (ESP32, verificado só na CI).

**Tech Stack:** C++ (ESP32 + testes nativos host via `bin/run-tests.sh`), HTML/CSS/JS vanilla, JsonWriter/JsonReader do `IrrigationWebApi.h`.

**Fonte visual de verdade:** `C:\Users\jmelo\Downloads\Irrigacao Portal.dc.html` (telas 1a wizard, 1b Estação, 1c Serviço, 1d Repetidor). Valores de cor/tamanho já estão traduzidos para as classes CSS abaixo; consultar o mockup para dúvidas de layout.

**Estado atual relevante:**
- `data/irrigacao/portal/index.html` — já migrado para header `.top` + `.tabs`/`.tab` no rodapé + `.panel` (feito em sessão anterior).
- `data/irrigacao/portal/app.js` — render funcs atuais emitem markup simples (`<p>` lines, `<table>` de log). É o que vamos reescrever.
- `data/irrigacao/style.css` — bloco `.field-portal` (linhas ~981–1030) já enxuto; vamos ADICIONAR as classes `.fp-*`.
- `src/modules/irrigation/PortalApi.{h,cpp}` — builders puros (`buildNodeState`, etc.), host-tested em `src/modules/irrigation/test/test_portal_api.cpp` (ou equivalente — confirmar caminho no Task A1).
- `src/modules/irrigation/IrrigationModule.cpp:2656` — `portalFillNodeState` preenche o `NodeStateCtx`.
- `src/modules/irrigation/IrrigationPortalEndpoints.cpp` — registra `/api/portal/*`.

**Papéis (IrrigationRole):** ESTACAO=0, GATEWAY=1, REPETIDOR=2, SERVICO=3.

**Convenção de tabs por papel (rodapé):**
- Estação: `Este nó` / `Rede` / `Cobertura`
- Serviço: `Clientes` / `Rede (cli.)` / `Log` / `Cobertura`
- Repetidor: `Este nó` / `Enlace` / `Cobertura`
- Gateway servido pelo portal: mantém wizard só; papel gateway usa o painel rico (`data/irrigacao/`), não o portal — não alterar.

---

## FASE A — Port visual (frontend) + uptime

### Task A0: Localizar o teste do PortalApi e confirmar comando

**Files:**
- Inspect: `src/modules/irrigation/test/` (ou `test/` raiz) — achar o arquivo que testa `buildNodeState`.

- [ ] **Step 1: Achar o teste**

Run: `grep -rl "buildNodeState" --include=*.cpp .`
Expected: caminho do teste (ex.: `src/modules/irrigation/test/test_portal_api.cpp`). Anotar como `<TEST_PORTAL_API>`.

- [ ] **Step 2: Confirmar runner**

Run (Windows/Docker — ver memória [[native-tests-need-docker-on-windows]]): `./bin/run-tests.sh` sob o container `Dockerfile.test`.
Expected: suite verde (baseline). Anotar contagem em `test/native-suite-count`.

---

### Task A1: Adicionar `uptimeS` ao NodeStateCtx (TDD)

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h:12-27` (struct `NodeStateCtx`)
- Modify: `src/modules/irrigation/PortalApi.cpp` (função `buildNodeState`)
- Test: `<TEST_PORTAL_API>`

- [ ] **Step 1: Escrever teste que falha**

Adicionar ao `<TEST_PORTAL_API>`:

```cpp
TEST_CASE("buildNodeState inclui uptimeS")
{
    IrrigationWeb::NodeStateCtx ctx;
    ctx.role = 2;             // repetidor
    ctx.uptimeS = 1234567;
    char buf[512];
    size_t n = IrrigationWeb::buildNodeState(ctx, buf, sizeof(buf));
    REQUIRE(n > 0);
    std::string s(buf, n);
    REQUIRE(s.find("\"uptimeS\":1234567") != std::string::npos);
}
```

- [ ] **Step 2: Rodar — deve falhar (campo inexistente)**

Run: `./bin/run-tests.sh` (filtrar test_portal_api se suportado)
Expected: FAIL — `uptimeS` não é membro de `NodeStateCtx`.

- [ ] **Step 3: Adicionar o campo + serialização**

Em `PortalApi.h`, dentro de `NodeStateCtx`, após `uint32_t apSecondsLeft = 0;`:

```cpp
    uint32_t uptimeS = 0; // segundos desde o boot (millis()/1000)
```

Em `PortalApi.cpp`, dentro de `buildNodeState`, junto aos outros `w.key(...); w.num/uint(...)` (seguir o padrão exato já usado no arquivo p/ os inteiros existentes, ex. `apSecondsLeft`):

```cpp
    w.key("uptimeS");
    w.uint(ctx.uptimeS);
```

(Usar o mesmo método de escrita de inteiro que `apSecondsLeft` usa no arquivo — se for `w.num`, usar `w.num`.)

- [ ] **Step 4: Rodar — deve passar**

Run: `./bin/run-tests.sh`
Expected: PASS.

- [ ] **Step 5: Preencher no módulo**

Em `src/modules/irrigation/IrrigationModule.cpp`, função `portalFillNodeState` (~2671), após `out.apSecondsLeft = portal.secondsLeft(millis());`:

```cpp
    out.uptimeS = millis() / 1000;
```

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp src/modules/irrigation/IrrigationModule.cpp <TEST_PORTAL_API>
git commit -m "feat(irrigation): portal — uptimeS no node-state"
```

---

### Task A2: Classes CSS `.fp-*` (componentes do portal)

**Files:**
- Modify: `data/irrigacao/style.css` — ADICIONAR ao final do bloco `.field-portal` (após a regra `.field-portal input[type="file"]`).

- [ ] **Step 1: Adicionar as classes**

```css
/* ===== Componentes do portal de campo (mockup Irrigacao Portal) ===== */
.field-portal .fp-hd { display: flex; align-items: baseline; justify-content: space-between; gap: 8px; }
.field-portal .fp-hd .ttl { font-size: 16px; font-weight: 700; color: var(--text); }
.field-portal .fp-lbl { font-size: 11px; font-weight: 700; color: var(--muted); text-transform: uppercase; letter-spacing: 0.04em; margin-bottom: 6px; }
.field-portal .fp-note { background: oklch(0.97 0.003 100); border-radius: 12px; padding: 12px 14px; font-size: 12px; color: var(--muted); line-height: 1.4; }

.field-portal .fp-grid { display: flex; gap: 8px; }
.field-portal .fp-stat { flex: 1; background: oklch(0.97 0.003 100); border-radius: 12px; padding: 10px 12px; min-width: 0; }
.field-portal .fp-stat .lbl { font-size: 10px; color: var(--muted); text-transform: uppercase; letter-spacing: 0.04em; }
.field-portal .fp-stat .val { font-size: 16px; font-weight: 700; color: var(--text); margin-top: 2px; }
.field-portal .fp-stat .val.sm { font-size: 13px; font-weight: 600; margin-top: 4px; }

.field-portal .fp-pills { display: flex; flex-wrap: wrap; gap: 6px; }
.field-portal .fp-pill { font-size: 12.5px; font-weight: 600; padding: 6px 12px; border-radius: 999px; border: 1.5px solid var(--border); background: transparent; color: var(--muted); cursor: pointer; font-family: inherit; }
.field-portal .fp-pill.on { background: oklch(0.47 0.1 150 / 0.12); color: var(--green); border-color: transparent; }
.field-portal .fp-pill.sel { border-color: var(--green); color: var(--green); background: oklch(0.47 0.1 150 / 0.08); }

.field-portal .fp-seg { display: flex; gap: 6px; }
.field-portal .fp-seg button { flex: 1; font-family: inherit; border: 1.5px solid var(--border); padding: 8px; border-radius: 8px; background: transparent; color: var(--muted); font-size: 12.5px; font-weight: 700; cursor: pointer; }
.field-portal .fp-seg button.sel { border-color: var(--green); color: var(--green); background: oklch(0.47 0.1 150 / 0.1); }

.field-portal .fp-log { background: oklch(0.97 0.003 100); border-radius: 12px; overflow: hidden; }
.field-portal .fp-log .row { display: flex; gap: 10px; align-items: baseline; padding: 9px 12px; border-bottom: 1px solid var(--border); }
.field-portal .fp-log .row:last-child { border-bottom: none; }
.field-portal .fp-log .ts { width: 74px; flex-shrink: 0; font-size: 10.5px; color: var(--muted); }
.field-portal .fp-log .ac { flex: 1; min-width: 0; font-size: 12.5px; color: var(--text); }

.field-portal .fp-kv { display: flex; justify-content: space-between; gap: 8px; font-size: 13px; }
.field-portal .fp-kv .k { color: var(--muted); }
.field-portal .fp-kv .v { font-weight: 600; color: var(--text); }

.field-portal .fp-confirm { background: oklch(0.55 0.16 30 / 0.08); border: 1px solid oklch(0.55 0.16 30 / 0.3); border-radius: 10px; padding: 10px 12px; margin-top: 10px; }
.field-portal .fp-confirm .msg { font-size: 12px; color: var(--text); }
.field-portal .fp-confirm .btns { display: flex; gap: 8px; margin-top: 8px; }

.field-portal .fp-node { border-radius: 12px; background: oklch(0.97 0.003 100); padding: 11px 13px; }
.field-portal .fp-node .hd { display: flex; justify-content: space-between; gap: 8px; }
.field-portal .fp-node .id { font-family: monospace; font-size: 12.5px; font-weight: 600; color: var(--text); }
.field-portal .fp-node .meta { font-size: 11.5px; color: var(--muted); margin-top: 4px; }
.field-portal .fp-node .acts { display: flex; gap: 6px; flex-wrap: wrap; margin-top: 8px; }

.field-portal .fp-spark { display: flex; gap: 3px; align-items: flex-end; height: 36px; margin-top: 12px; }
.field-portal .fp-spark > i { flex: 1; border-radius: 2px; background: oklch(0.47 0.1 150 / 0.5); }

.field-portal .fp-active { display: flex; align-items: center; gap: 8px; justify-content: center; padding: 10px; border-radius: 10px; background: oklch(0.47 0.1 150 / 0.1); color: var(--green); font-size: 13px; font-weight: 600; }
.field-portal .fp-active .dot { width: 8px; height: 8px; border-radius: 999px; background: var(--green); }

.field-portal .fp-pulseopen { text-align: center; padding: 10px; border-radius: 10px; background: oklch(0.47 0.1 150 / 0.12); color: var(--green); font-size: 13px; font-weight: 600; }

.field-portal .card .stack { display: flex; flex-direction: column; gap: 10px; }
```

- [ ] **Step 2: Commit**

```bash
git add data/irrigacao/style.css
git commit -m "style(irrigation): portal — componentes .fp-* do mockup"
```

---

### Task A3: Header dinâmico + tabs por papel no `index.html`

**Files:**
- Modify: `data/irrigacao/portal/index.html`

Objetivo: header mostra nome do nó + `<papel> · canal privado` + chip; adicionar a aba `Enlace` (repetidor) ao rodapé; dar ids ao header.

- [ ] **Step 1: Header com ids**

Trocar o bloco `<header class="top">…</header>` por:

```html
  <header class="top">
    <div>
      <div class="farm" id="hdrName">Portal do Nó</div>
      <div class="sub" id="hdrSub">canal privado</div>
    </div>
    <span id="apLeft" class="chip gray hidden"></span>
  </header>
```

- [ ] **Step 2: Aba Enlace (repetidor) no rodapé**

No `<nav class="tabs">`, adicionar após a aba `survey` a aba de enlace (escondida por padrão; revelada por papel no app.js):

```html
    <button data-tab="enlace" class="tab rep-only hidden">Enlace</button>
```

- [ ] **Step 3: Seções de papel repetidor no `<main>`**

Antes de `</main>`, adicionar as duas seções do repetidor (Este nó já existe como `#tab-node`; reusaremos `#tab-node` para estação E repetidor via render — mas o repetidor tem card próprio "Este nó" + nota + coords, e a aba Enlace é nova). Adicionar só a aba Enlace:

```html
    <section id="tab-enlace" class="panel hidden">
      <div class="card">
        <h2>Qualidade do enlace</h2>
        <div class="fp-grid">
          <div class="fp-stat"><div class="lbl">SNR</div><div class="val" id="enlSnr">—</div></div>
          <div class="fp-stat"><div class="lbl">RSSI</div><div class="val" id="enlRssi">—</div></div>
        </div>
        <div class="fp-spark" id="enlSpark"></div>
        <div class="muted" style="font-size:11px;margin-top:6px;">SNR nas últimas leituras · atualiza a cada heartbeat</div>
      </div>
      <div class="card">
        <h2>Vizinhos na malha</h2>
        <div id="enlNeighbors" class="stack"><span class="muted">—</span></div>
      </div>
    </section>
```

- [ ] **Step 4: Preview offline**

Abrir `data/irrigacao/portal/index.html` no browser (mock.js carrega). Verificar que a estrutura carrega sem erro de console. Enlace ainda vazio (mock na Task A7).

- [ ] **Step 5: Commit**

```bash
git add data/irrigacao/portal/index.html
git commit -m "feat(irrigation): portal — header dinâmico + aba Enlace (repetidor)"
```

---

### Task A4: `app.js` — header + gate de papel + render "Este nó" (Estação/Repetidor)

**Files:**
- Modify: `data/irrigacao/portal/app.js`

- [ ] **Step 1: Helpers de header e papel**

No topo (após `const RESULTADOS = …`), adicionar:

```js
const ROLE_ESTACAO = 0, ROLE_GATEWAY = 1, ROLE_REPETIDOR = 2, ROLE_SERVICO = 3;
function fmtV(centi) { return (centi / 100).toFixed(2).replace('.', ',') + ' V'; }
function fmtUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600);
  return d > 0 ? `${d} d ${h} h` : `${h} h ${Math.floor((s % 3600) / 60)} m`;
}
function setHeader(s) {
  document.getElementById("hdrName").textContent = s.name || "(sem nome)";
  document.getElementById("hdrSub").textContent = (ROLES[s.role] || "Nó") + " · canal privado";
}
```

- [ ] **Step 2: Reescrever `renderNode` (card "Este nó" para Estação e Repetidor)**

Substituir a função `renderNode` inteira por:

```js
function renderNode(s) {
  setHeader(s);
  const el = document.getElementById("nodeState");
  const sync = s.boundGateway ? `Epoch ${s.configEpoch} · sincronizado` : "Não pareado";
  const gw = s.boundGateway ? "0x" + (s.boundGateway >>> 0).toString(16) : "—";
  const tamper = s.flags & 1 ? '<div class="tamper"><b>⚠ VIOLAÇÃO (tamper)</b></div>' : '';
  if (s.role === ROLE_REPETIDOR) {
    const solar = s.vpanelCentiV ? `<div class="fp-stat"><div class="lbl">Painel solar</div><div class="val">${fmtV(s.vpanelCentiV)}</div></div>` : '';
    el.innerHTML = `
      <div class="fp-hd"><span class="ttl">Este nó</span><span class="chip gray">Repetidor</span></div>
      <div class="fp-grid" style="margin-top:12px;">
        <div class="fp-stat"><div class="lbl">Bateria</div><div class="val">${fmtV(s.vbatCentiV)}</div></div>
        ${solar || `<div class="fp-stat"><div class="lbl">Gateway</div><div class="val sm">${gw}</div></div>`}
      </div>
      <div class="fp-grid" style="margin-top:8px;">
        ${solar ? `<div class="fp-stat"><div class="lbl">Gateway</div><div class="val sm">${gw}</div></div>` : ''}
        <div class="fp-stat"><div class="lbl">Uptime</div><div class="val sm">${fmtUptime(s.uptimeS || 0)}</div></div>
      </div>
      <div class="muted" style="font-size:12px;margin-top:12px;">${sync}</div>
      ${tamper}`;
    return;
  }
  // Estação
  const valves = [];
  for (let i = 0; i < s.numValves; i++) {
    const on = (s.valveStates >> i) & 1;
    valves.push(`<span class="fp-pill ${on ? "on" : ""}">Válvula ${i + 1} · ${on ? "Aberta" : "Fechada"}</span>`);
  }
  el.innerHTML = `
    <div class="fp-hd"><span class="ttl">Este nó</span><span class="chip green">${ROLES[s.role] || s.role}</span></div>
    <div class="fp-grid" style="margin-top:12px;">
      <div class="fp-stat"><div class="lbl">Bateria</div><div class="val">${fmtV(s.vbatCentiV)}</div></div>
      <div class="fp-stat"><div class="lbl">Gateway</div><div class="val sm">${gw}</div></div>
    </div>
    <div class="muted" style="font-size:12px;margin-top:10px;">${sync}${s.safeMode ? " · <b>modo seguro</b>" : ""}</div>
    <div style="margin-top:6px;"><div class="fp-lbl">Válvulas</div><div class="fp-pills">${valves.join("") || "<span class='muted'>—</span>"}</div></div>
    ${tamper}`;
}
```

- [ ] **Step 3: Gate de papel — mostrar/esconder cards e tabs**

Substituir a função `refresh` para aplicar visibilidade por papel. Trocar o corpo de `refresh` por:

```js
async function refresh() {
  const { ok, body } = await j("/api/portal/node");
  if (ok) {
    if (body.provisioned === false) { showWizard(); return; }
    lastRole = body.role;
    applyRoleLayout(body.role);
    renderNode(body);
    renderGpos(body);
    updateApChip(body);
    if (body.role === ROLE_SERVICO && !svcInit) initService();
  }
  if (lastRole !== ROLE_REPETIDOR) await refreshSensors();
}

function updateApChip(s) {
  const ap = document.getElementById("apLeft");
  if (s.apSecondsLeft) {
    ap.textContent = `AP ${Math.floor(s.apSecondsLeft / 60)}m${s.apSecondsLeft % 60}s`;
    ap.classList.remove("hidden");
  } else ap.classList.add("hidden");
}

function applyRoleLayout(role) {
  const rep = role === ROLE_REPETIDOR;
  // cards da aba "Este nó" que só a estação tem
  ["sensorsCard", "gposCard", "pulseForm"].forEach((id) => {
    const e = document.getElementById(id); if (e) e.classList.toggle("hidden-role", rep);
  });
  document.getElementById("logCard").classList.toggle("hidden-role", rep);
  // tabs: Rede é só estação; Enlace é só repetidor; Cobertura sempre
  document.querySelector('[data-tab="net"]').classList.toggle("hidden", rep);
  document.querySelectorAll(".rep-only").forEach((b) => b.classList.toggle("hidden", !rep));
}
```

Nota: `renderGpos`/`refreshSensors` já escondem seus cards quando vazios; `hidden-role` é um segundo gate. Adicionar em `style.css` (bloco field-portal): `.field-portal .hidden-role { display: none !important; }`. **Adicionar essa regra agora** (Edit no style.css).

- [ ] **Step 4: Mover a chamada do chip do `renderNode` antigo**

A antiga `renderNode` setava `apLeft`; agora quem seta é `updateApChip`. Garantir que não há referência duplicada.

- [ ] **Step 5: Preview**

Abrir `portal/index.html?role=0` (estação) e `?role=2` (repetidor). Conferir: estação mostra stats+válvulas+sensores+log+coords+pulso; repetidor mostra stats(bat/gw/uptime)+coords, esconde sensores/gpo/pulso/log/rede, revela aba Enlace.

- [ ] **Step 6: Commit**

```bash
git add data/irrigacao/portal/app.js data/irrigacao/style.css
git commit -m "feat(irrigation): portal — render Este nó por papel (estação/repetidor)"
```

---

### Task A5: `app.js` — GPO com confirm inline, Sensores, Auditoria em linhas, Pulso com pills, Rede segmentada

**Files:**
- Modify: `data/irrigacao/portal/app.js`
- Modify: `data/irrigacao/portal/index.html` (pulso/rede viram pills/segmentado)

- [ ] **Step 1: `renderGpos` com confirm inline (troca o `confirm()`)**

Substituir `renderGpos` + `sendGpo` por:

```js
let gpoPending = null; // {gpo}
function renderGpos(s) {
  const card = document.getElementById("gposCard");
  const list = document.getElementById("gposList");
  if (!s.numGpos) { card.classList.add("hidden"); return; }
  card.classList.remove("hidden");
  let html = "";
  for (let i = 0; i < s.numGpos; i++) {
    const on = (s.gpoStates >> i) & 1;
    html += `<div class="fp-kv" style="align-items:center;">
      <span class="k">GPO ${i} · <b style="color:var(--text)">${on ? "Ligado" : "Desligado"}</b></span>
      <button data-gpo="${i}" data-act="${on ? 0 : 1}">${on ? "Desligar" : "Ligar"}</button></div>`;
    if (gpoPending === i) {
      html += `<div class="fp-confirm"><div class="msg">GPO biestável — não desliga sozinho. Confirma o acionamento?</div>
        <div class="btns"><button class="btn ghost sm" data-cancelgpo="1">Cancelar</button>
        <button class="btn danger sm" data-confirmgpo="${i}">Confirmar</button></div></div>`;
    }
  }
  list.innerHTML = html;
  list.querySelectorAll("button[data-gpo]").forEach((b) => b.addEventListener("click", () => onGpoClick(+b.dataset.gpo, +b.dataset.act)));
  const c = list.querySelector("[data-cancelgpo]"); if (c) c.onclick = () => { gpoPending = null; refresh(); };
  const k = list.querySelector("[data-confirmgpo]"); if (k) k.onclick = () => doGpo(+k.dataset.confirmgpo, 1, true);
}
function onGpoClick(gpo, action) {
  if (action === 1) { gpoPending = gpo; refresh(); return; } // ligar biestável → confirma inline
  doGpo(gpo, 0, false);
}
async function doGpo(gpo, action, confirmFlag) {
  gpoPending = null;
  const { ok, body } = await j("/api/portal/gpo", { method: "POST", body: JSON.stringify({ gpo, action, durationS: 0, confirm: confirmFlag }) });
  document.getElementById("gpoMsg").textContent = ok ? "OK" : (body.errors || ["erro"]).join("; ");
  refresh();
}
```

- [ ] **Step 2: `refreshSensors` em linhas kv**

Substituir o corpo de render de `refreshSensors` (mantém fetch) por:

```js
async function refreshSensors() {
  const { ok, body } = await j("/api/portal/sensors");
  const el = document.getElementById("sensorsList");
  if (!ok || !body.sensors || !body.sensors.length) { el.textContent = "nenhum sensor configurado"; return; }
  el.innerHTML = body.sensors.map((s) => {
    const val = s.tipo === 1 ? `${(s.valor / 100).toFixed(2).replace('.', ',')} ${s.unidade}` : (s.valor ? "ativo" : "inativo");
    return `<div class="fp-kv"><span class="k">Sensor ${s.id}</span><span class="v">${val}</span></div>`;
  }).join("");
}
```

- [ ] **Step 3: `refreshLog` em `.fp-log` rows**

Substituir o corpo de `refreshLog` por:

```js
async function refreshLog() {
  const { ok, body } = await j("/api/portal/log");
  const el = document.getElementById("logList");
  if (!ok || !body.log || !body.log.length) { el.textContent = "log vazio"; return; }
  el.innerHTML = `<div class="fp-log">` + body.log.map((r) => {
    const ac = `${ACOES[r.acao] || r.acao} · ${ORIGENS[r.origem] || r.origem}${r.alvo ? " (" + r.alvo + ")" : ""}`;
    return `<div class="row"><span class="ts">${r.ts}</span><span class="ac">${ac} <small class="muted">${RESULTADOS[r.res] || r.res}</small></span></div>`;
  }).join("") + `</div>`;
}
```

- [ ] **Step 4: Pulso e Rede — markup pills/segmentado no index.html**

Trocar o `#pulseForm` (dentro de `#tab-node`) por:

```html
      <div id="pulseForm" class="card stack">
        <h2>Teste de pulso</h2>
        <div><div class="fp-lbl">Válvula</div><div class="fp-pills" id="pulsePills"></div></div>
        <label>Duração (s) <input type="number" id="pulseDur" min="1" max="7200" value="10" /></label>
        <div id="pulseState"></div>
        <button type="button" id="pulseGo" class="btn solid sm">Abrir</button>
        <span id="pulseMsg" class="muted"></span>
      </div>
```

Trocar o `#netForm` (dentro de `#tab-net`) por:

```html
      <div id="netForm" class="card stack">
        <h2>Comandar zona (via gateway)</h2>
        <div><div class="fp-lbl">Zona</div><div class="fp-pills" id="netPills"></div></div>
        <div class="fp-seg" id="netSeg">
          <button type="button" data-act="open" class="sel">Abrir</button>
          <button type="button" data-act="close">Fechar</button>
        </div>
        <label>Duração (s) <input type="number" id="netDur" min="1" max="7200" value="300" /></label>
        <button type="button" id="netGo" class="btn solid sm">Enviar</button>
        <span id="netMsg" class="muted"></span>
      </div>
```

- [ ] **Step 5: `app.js` — wiring de pulso pills + estado**

Substituir o antigo listener `pulseForm.submit` por:

```js
let pulseValve = 0, pulseOpen = false;
function renderPulsePills(n) {
  const wrap = document.getElementById("pulsePills");
  let h = "";
  for (let i = 0; i < (n || 0); i++) h += `<button type="button" class="fp-pill ${i === pulseValve ? "sel" : ""}" data-pv="${i}">${i}</button>`;
  wrap.innerHTML = h || "<span class='muted'>—</span>";
  wrap.querySelectorAll("[data-pv]").forEach((b) => b.onclick = () => { pulseValve = +b.dataset.pv; renderPulsePills(n); });
  const st = document.getElementById("pulseState");
  st.innerHTML = pulseOpen ? `<div class="fp-pulseopen">Válvula aberta</div>` : "";
  document.getElementById("pulseGo").textContent = pulseOpen ? "Fechar agora" : "Abrir";
}
document.getElementById("pulseGo").addEventListener("click", async () => {
  const durationS = +document.getElementById("pulseDur").value;
  if (pulseOpen) { pulseOpen = false; document.getElementById("pulseMsg").textContent = "Fechado"; renderPulsePills(document.querySelectorAll("#pulsePills [data-pv]").length); return; }
  const { ok, body } = await j("/api/portal/node/pulse", { method: "POST", body: JSON.stringify({ valveId: pulseValve, durationS }) });
  document.getElementById("pulseMsg").textContent = ok ? "OK" : (body.errors || ["erro"]).join("; ");
  pulseOpen = ok; renderPulsePills(document.querySelectorAll("#pulsePills [data-pv]").length);
  refresh();
});
```

Chamar `renderPulsePills(body.numValves)` dentro de `renderNode` (estação) no fim do ramo estação: adicionar antes do `return`/fecho: `renderPulsePills(s.numValves);`

- [ ] **Step 6: `app.js` — wiring de rede pills + segmentado**

Substituir `loadRoster` + listener `netForm.submit` por:

```js
let netZone = 0, netAction = "open";
async function loadRoster() {
  const { ok, body } = await j("/api/portal/net/roster");
  const wrap = document.getElementById("netPills");
  let items = (ok && Array.isArray(body) && body.length) ? body : Array.from({ length: 24 }, (_, i) => ({ id: i + 1, name: "Zona " + (i + 1) }));
  if (!netZone && items.length) netZone = items[0].id;
  wrap.innerHTML = items.map((z) => `<button type="button" class="fp-pill ${z.id === netZone ? "sel" : ""}" data-nz="${z.id}">${z.name}</button>`).join("");
  wrap.querySelectorAll("[data-nz]").forEach((b) => b.onclick = () => { netZone = +b.dataset.nz; loadRoster(); });
}
document.querySelectorAll("#netSeg button").forEach((b) => b.addEventListener("click", () => {
  netAction = b.dataset.act;
  document.querySelectorAll("#netSeg button").forEach((x) => x.classList.toggle("sel", x === b));
}));
document.getElementById("netGo").addEventListener("click", async () => {
  const durationS = +document.getElementById("netDur").value;
  const payload = netAction === "open" ? { kind: "open", zoneId: netZone, durationS } : { kind: "close", zoneId: netZone };
  const { ok, body } = await j("/api/portal/net/command", { method: "POST", body: JSON.stringify(payload) });
  document.getElementById("netMsg").textContent = ok ? "Enviado" : (body.errors || ["erro"]).join("; ");
});
```

- [ ] **Step 7: Preview (estação)**

`portal/index.html?role=0`: GPO confirm inline, sensores kv, log em linhas, pulso com pills + estado aberto, rede com pills+segmentado. Sem erros no console.

- [ ] **Step 8: Commit**

```bash
git add data/irrigacao/portal/index.html data/irrigacao/portal/app.js
git commit -m "feat(irrigation): portal — GPO confirm inline, sensores/log/pulso/rede no visual do mockup"
```

---

### Task A6: `app.js` — Serviço (clientes/scan/config) no visual do mockup

**Files:**
- Modify: `data/irrigacao/portal/app.js` (funções `loadClients`, `selectClient`, `pollScanSvc`, `fillEditor`, `loadSvcLog`)
- Modify: `data/irrigacao/portal/index.html` (header serviço já usa `.top`; ajustar chip de cliente ativo)

- [ ] **Step 1: `loadClients` com cards + confirm inline**

Substituir `loadClients` + `selectClient` por:

```js
let svcConfirmClient = null;
async function loadClients() {
  const { ok, body } = await j("/api/portal/service/clients");
  const el = document.getElementById("clientsList");
  if (!ok || !body.clients) { el.textContent = "—"; return; }
  el.innerHTML = `<div class="stack">` + (body.clients.map((c) => {
    const badge = c.active ? `<span style="font-size:11px;font-weight:700;color:var(--green);">▶ ativo</span>`
      : `<button data-sel="${c.id}">Selecionar</button>`;
    const confirm = svcConfirmClient === c.id ? `<div class="fp-confirm"><div class="msg">Re-tunar no canal deste cliente? O dispositivo reinicia (~3 s).</div>
      <div class="btns"><button class="btn ghost sm" data-cxl="1">Cancelar</button><button class="btn danger sm" data-cok="${c.id}">Confirmar</button></div></div>` : "";
    return `<div class="fp-node"><div class="hd"><div><div style="font-weight:600;color:var(--text);font-size:14px;">${c.nome || c.id}</div>
      <div class="meta">canal ${c.canal} · ${c.estacoes} nós</div></div>${badge}</div>${confirm}</div>`;
  }).join("") || "nenhum cliente no cofre") + `</div>`;
  el.querySelectorAll("button[data-sel]").forEach((b) => b.onclick = () => { svcConfirmClient = b.dataset.sel; loadClients(); });
  const cxl = el.querySelector("[data-cxl]"); if (cxl) cxl.onclick = () => { svcConfirmClient = null; loadClients(); };
  const cok = el.querySelector("[data-cok]"); if (cok) cok.onclick = () => doSelectClient(cok.dataset.cok);
}
async function doSelectClient(id) {
  svcConfirmClient = null;
  await j("/api/portal/service/select", { method: "POST", body: JSON.stringify({ id }) });
  document.getElementById("clientsMsg").textContent = "Re-tunando… reconecte ao portal após o reboot.";
}
```

- [ ] **Step 2: `pollScanSvc` com node cards + pills de ação**

Substituir `pollScanSvc` por:

```js
async function pollScanSvc() {
  const { ok, body } = await j("/api/portal/service/scan");
  const el = document.getElementById("svcNodes");
  if (!ok || !body.nodes || !body.nodes.length) { el.textContent = "nenhum respondente ainda"; return; }
  el.innerHTML = `<div class="stack">` + body.nodes.map((n) => {
    const hx = nodeHex(n.node);
    return `<div class="fp-node"><div class="hd"><span class="id">${hx}</span><span style="font-size:11px;font-weight:600;color:var(--green);">${ROLES[n.role] || n.role}</span></div>
      <div class="meta">epoch ${n.epoch} · ${(n.vbat / 100).toFixed(1).replace('.', ',')} V · fw 0x${(n.fw || 0).toString(16)} · snr ${(n.snr / 4).toFixed(0)}</div>
      <div class="acts">
        <button class="fp-pill" data-rd="${hx}">Ler cfg</button>
        <button class="fp-pill" data-pulse="${hx}">Pulso</button>
        <button class="fp-pill" data-zone="${hx}">Zona</button>
        <button class="fp-pill" style="color:var(--red);border-color:oklch(0.55 0.16 30 / 0.4);" data-rs="${hx}">RESYNC</button>
      </div></div>`;
  }).join("") + `</div>`;
  el.querySelectorAll("button[data-rd]").forEach((b) => b.onclick = () => readConfig(b.dataset.rd));
  el.querySelectorAll("button[data-pulse]").forEach((b) => b.onclick = () => nodeAction(b.dataset.pulse, "pulse"));
  el.querySelectorAll("button[data-zone]").forEach((b) => b.onclick = () => nodeAction(b.dataset.zone, "zone"));
  el.querySelectorAll("button[data-rs]").forEach((b) => b.onclick = () => nodeAction(b.dataset.rs, "resync"));
}
```

- [ ] **Step 3: `loadSvcLog` em `.fp-log`**

Substituir `loadSvcLog` por:

```js
async function loadSvcLog() {
  const { ok, body } = await j("/api/portal/service/log");
  const el = document.getElementById("svcLogList");
  if (!ok || !body.log || !body.log.length) { el.textContent = "log vazio"; return; }
  el.innerHTML = `<div class="fp-log">` + body.log.map((r) => {
    const ac = `${r.ev}${r.node ? " · 0x" + (r.node >>> 0).toString(16) : ""}`;
    return `<div class="row"><span class="ts">up=${r.up}</span><span class="ac">${ac}</span></div>`;
  }).join("") + `</div>`;
}
```

- [ ] **Step 4: `fillEditor` — segmentado de rota + `.fp-lbl` seções**

No `fillEditor`, trocar o `<label>Rota <select…>` e o botão por segmentado. Substituir a linha do `html += ... route ...` e a do botão por:

```js
  html += `<div><div class="fp-lbl">Rota de gravação</div><div class="fp-seg" id="cfgSeg">
    <button type="button" data-r="direct" class="sel">Direta (epoch+1)</button>
    <button type="button" data-r="gateway">Via gateway</button></div></div>`;
  html += `<button type="button" id="cfgSave" class="btn solid sm">Gravar config</button>`;
```

Depois de `box.classList.remove("hidden");`, adicionar wiring do segmentado (a var `cfgRoute` guarda a escolha):

```js
  window.__cfgRoute = "direct";
  box.querySelectorAll("#cfgSeg button").forEach((b) => b.onclick = () => {
    window.__cfgRoute = b.dataset.r;
    box.querySelectorAll("#cfgSeg button").forEach((x) => x.classList.toggle("sel", x === b));
  });
```

Em `writeConfig`, trocar `const route = g("route").value;` por `const route = window.__cfgRoute || "direct";`.

- [ ] **Step 5: Chip de cliente ativo no header (serviço)**

Em `initService`, após revelar as abas, setar o header:

```js
  document.getElementById("hdrName").textContent = "Dispositivo de Serviço";
  document.getElementById("hdrSub").textContent = "Cofre multi-cliente";
```

- [ ] **Step 6: Preview (serviço)**

`portal/index.html?role=3`: abas Clientes/Rede(cli.)/Log/Cobertura; clientes em cards com confirm inline; scan em node cards com pills; editor com segmentado. (mock de serviço já existe em mock.js — ver Task A7 se algum endpoint faltar.)

- [ ] **Step 7: Commit**

```bash
git add data/irrigacao/portal/app.js data/irrigacao/portal/index.html
git commit -m "feat(irrigation): portal — Serviço (clientes/scan/config) no visual do mockup"
```

---

### Task A7: mock.js — dados para preview offline (uptime, enlace, papéis)

**Files:**
- Modify: `data/irrigacao/mock.js`

- [ ] **Step 1: Campos novos no `portalNode`**

No objeto `STATE.portalNode`, garantir os campos consumidos: `role`, `name`, `boundGateway`, `configEpoch`, `safeMode`, `numValves`, `numGpos`, `valveStates`, `gpoStates`, `vbatCentiV`, `vpanelCentiV`, `flags`, `apSecondsLeft`, e adicionar:

```js
    uptimeS: 1234567,
    vpanelCentiV: 1350,
```

- [ ] **Step 2: Rota `/link` mock (enlace do repetidor)**

No handler de `fetch` override, no ramo `if (clean.includes('/api/portal'))`, adicionar antes do fallback:

```js
    if (ppath.startsWith('/link')) return mockResponse({
      snrQuarterDb: 33, rssiDbm: -72,
      history: [40, 55, 50, 65, 70, 60, 75, 80, 72, 78, 85, 82],
      neighbors: [
        { node: 0xa1b2c3d4, snrQuarterDb: 33, hops: 1, name: 'GW' },
        { node: 0x12345678, snrQuarterDb: 20, hops: 2, name: 'EST-2' },
      ],
    });
```

- [ ] **Step 3: Preview de todos os papéis**

Abrir `?role=0`, `?role=2`, `?role=3`, e `?wizard=1`. Conferir cada tela contra o mockup (1b/1d/1c/1a). Ajustar espaçamentos se destoar.

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/mock.js
git commit -m "test(irrigation): mock — uptime/vpanel + rota /link p/ preview do portal"
```

---

### Task A8: Enlace — render no `app.js` (consome `/api/portal/link`)

**Files:**
- Modify: `data/irrigacao/portal/app.js`

- [ ] **Step 1: Função `refreshEnlace`**

Adicionar:

```js
async function refreshEnlace() {
  if (lastRole !== ROLE_REPETIDOR) return;
  const { ok, body } = await j("/api/portal/link");
  if (!ok) return;
  document.getElementById("enlSnr").textContent = (body.snrQuarterDb / 4).toFixed(2).replace('.', ',') + " dB";
  document.getElementById("enlRssi").textContent = body.rssiDbm + " dBm";
  const hist = body.history || [];
  const max = Math.max(1, ...hist);
  document.getElementById("enlSpark").innerHTML = hist.map((v) => `<i style="height:${Math.round((v / max) * 100)}%"></i>`).join("");
  const ns = body.neighbors || [];
  document.getElementById("enlNeighbors").innerHTML = ns.length ? ns.map((n) => {
    const snr = (n.snrQuarterDb / 4).toFixed(1).replace('.', ',');
    const q = n.snrQuarterDb >= 24 ? "var(--green)" : n.snrQuarterDb >= 8 ? "var(--amber)" : "var(--red)";
    return `<div class="fp-node"><div class="hd"><span class="id">${nodeHex(n.node)}</span><span style="font-size:11px;font-weight:700;color:${q};">${snr} dB</span></div>
      <div class="meta">${n.name || "—"} · ${n.hops} ${n.hops === 1 ? "salto" : "saltos"}</div></div>`;
  }).join("") : "<span class='muted'>nenhum vizinho ouvido</span>";
}
```

- [ ] **Step 2: Chamar no ciclo + ao trocar de aba**

Adicionar `refreshEnlace();` ao fim de `refresh()` (dentro do `if (ok)`), e no handler de clique de tab, quando `b.dataset.tab === "enlace"` chamar `refreshEnlace()`.

- [ ] **Step 3: Preview (repetidor)**

`portal/index.html?role=2` → aba Enlace: SNR/RSSI, sparkline, vizinhos. Conferir contra 1d.

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/portal/app.js
git commit -m "feat(irrigation): portal — render da aba Enlace (repetidor)"
```

---

### Task A9: Ajuste do teste `test_niveis`/`test_mock` se tocarem no portal

**Files:**
- Inspect: `data/irrigacao/test_mock.js`, `data/irrigacao/test_niveis.js`

- [ ] **Step 1: Rodar os smoke tests JS**

Run: `node data/irrigacao/test_mock.js; node data/irrigacao/test_niveis.js`
Expected: saída limpa (exit 0). Se algum assert referenciar estrutura antiga do portal, atualizar para os novos campos/ids.

- [ ] **Step 2: Commit (se houve ajuste)**

```bash
git add data/irrigacao/test_mock.js data/irrigacao/test_niveis.js
git commit -m "test(irrigation): mock — alinhar smoke tests ao portal reestruturado"
```

---

## FASE B — Backend da aba Enlace (firmware, verificado só na CI)

### Task B1: `buildLink` — função pura + teste host

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h` (novo struct + assinatura)
- Modify: `src/modules/irrigation/PortalApi.cpp` (impl)
- Test: `<TEST_PORTAL_API>`

- [ ] **Step 1: Teste que falha**

```cpp
TEST_CASE("buildLink serializa própria métrica + vizinhos")
{
    IrrigationWeb::LinkCtx ctx;
    ctx.snrQuarterDb = 33;
    ctx.rssiDbm = -72;
    ctx.histCount = 3;
    ctx.hist[0] = 40; ctx.hist[1] = 60; ctx.hist[2] = 80;
    ctx.neighborCount = 1;
    ctx.neighbors[0] = { 0xA1B2C3D4u, 33, 1, "GW" };
    char buf[1024];
    size_t n = IrrigationWeb::buildLink(ctx, buf, sizeof(buf));
    REQUIRE(n > 0);
    std::string s(buf, n);
    REQUIRE(s.find("\"snrQuarterDb\":33") != std::string::npos);
    REQUIRE(s.find("\"rssiDbm\":-72") != std::string::npos);
    REQUIRE(s.find("\"history\":[40,60,80]") != std::string::npos);
    REQUIRE(s.find("\"neighbors\":[") != std::string::npos);
    REQUIRE(s.find("2712847316") != std::string::npos); // 0xA1B2C3D4 decimal
    REQUIRE(s.find("\"hops\":1") != std::string::npos);
    REQUIRE(s.find("\"name\":\"GW\"") != std::string::npos);
}
```

- [ ] **Step 2: Rodar — falha (símbolos inexistentes)**

Run: `./bin/run-tests.sh`
Expected: FAIL — `LinkCtx`/`buildLink` não existem.

- [ ] **Step 3: Declarar struct + assinatura em `PortalApi.h`**

Adicionar dentro do namespace, após `buildCoords`/`parseCoords`:

```cpp
// --- Aba "Enlace" (repetidor, §7.2) ---
struct LinkNeighbor {
    uint32_t node = 0;
    int8_t snrQuarterDb = 0;
    uint8_t hops = 0;
    char name[16] = {0};
};
struct LinkCtx {
    int8_t snrQuarterDb = 0;  // enlace ao gateway (último rx)
    int16_t rssiDbm = 0;
    uint8_t histCount = 0;
    uint8_t hist[12] = {0};   // SNR/quarter-dB nas últimas leituras (0..255 já normalizado p/ barra)
    uint8_t neighborCount = 0;
    LinkNeighbor neighbors[8];
};
size_t buildLink(const LinkCtx &ctx, char *buf, size_t cap);
```

- [ ] **Step 4: Implementar em `PortalApi.cpp`**

Seguir o padrão de `buildNodeState` (mesmos métodos do `JsonWriter`; ajustar nomes conforme o arquivo — `w.num`/`w.int`/`w.uint`/`w.str`/`w.beginArray`/`w.endArray`):

```cpp
size_t buildLink(const LinkCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.key("snrQuarterDb"); w.num(ctx.snrQuarterDb);
    w.key("rssiDbm");      w.num(ctx.rssiDbm);
    w.key("history"); w.beginArray();
    for (uint8_t i = 0; i < ctx.histCount; i++) w.num(ctx.hist[i]);
    w.endArray();
    w.key("neighbors"); w.beginArray();
    for (uint8_t i = 0; i < ctx.neighborCount; i++) {
        const LinkNeighbor &n = ctx.neighbors[i];
        w.beginObject();
        w.key("node"); w.uint(n.node);
        w.key("snrQuarterDb"); w.num(n.snrQuarterDb);
        w.key("hops"); w.num(n.hops);
        w.key("name"); w.str(n.name);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}
```

(Conferir a assinatura real de `JsonWriter` no `IrrigationWebApi.h` — usar `w.done()`/`w.pos` conforme o padrão dos outros builders.)

- [ ] **Step 5: Rodar — passa**

Run: `./bin/run-tests.sh`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp <TEST_PORTAL_API>
git commit -m "feat(irrigation): portal — buildLink (enlace do repetidor) + teste"
```

---

### Task B2: Captura de SNR/RSSI do gateway + ring de histórico no módulo

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (campos)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (captura no rx; helper `portalFillLink`)

- [ ] **Step 1: Campos no header**

Em `IrrigationModule.h`, na seção privada:

```cpp
    // Enlace (repetidor): última métrica de rx do gateway + ring de histórico p/ o portal.
    int8_t linkSnrQ = 0;       // rx_snr*4 do último pacote vindo do boundGateway
    int16_t linkRssi = 0;
    uint8_t linkHist[12] = {0};
    uint8_t linkHistCount = 0;
    uint8_t linkHistHead = 0;
    void noteGatewayLink(int8_t snrQ, int16_t rssi);
    void portalFillLink(IrrigationWeb::LinkCtx &out) const;
```

- [ ] **Step 2: Captura no rx**

Em `handleReceived`/`handleMessage` (o ponto onde `mp` é processado — mesmo `switch` do `MSG_*`), no início, antes do switch, adicionar:

```cpp
    if (settings.boundGateway && mp.from == settings.boundGateway)
        noteGatewayLink((int8_t)(mp.rx_snr * 4), (int16_t)mp.rx_rssi);
```

- [ ] **Step 3: Implementar `noteGatewayLink` (ring)**

```cpp
void IrrigationModule::noteGatewayLink(int8_t snrQ, int16_t rssi)
{
    linkSnrQ = snrQ;
    linkRssi = rssi;
    // normaliza SNR (−40..+40 quarter-dB ~ −10..+10 dB) p/ 0..100 (altura de barra)
    int v = snrQ + 40; if (v < 0) v = 0; if (v > 80) v = 80;
    linkHist[linkHistHead] = (uint8_t)(v * 100 / 80);
    linkHistHead = (linkHistHead + 1) % 12;
    if (linkHistCount < 12) linkHistCount++;
}
```

- [ ] **Step 4: Implementar `portalFillLink`**

```cpp
void IrrigationModule::portalFillLink(IrrigationWeb::LinkCtx &out) const
{
    out.snrQuarterDb = linkSnrQ;
    out.rssiDbm = linkRssi;
    out.histCount = linkHistCount;
    for (uint8_t i = 0; i < linkHistCount; i++)
        out.hist[i] = linkHist[(linkHistHead + 12 - linkHistCount + i) % 12];
    // vizinhos: NodeDB (até 8, por SNR desc não é necessário — ordem do DB)
    out.neighborCount = 0;
    size_t total = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < total && out.neighborCount < 8; i++) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || n->num == nodeDB->getNodeNum()) continue; // pula a si mesmo
        IrrigationWeb::LinkNeighbor &ln = out.neighbors[out.neighborCount++];
        ln.node = n->num;
        ln.snrQuarterDb = (int8_t)(n->snr * 4);
        ln.hops = n->hops_away;
        const char *nm = (n->has_user && n->user.short_name[0]) ? n->user.short_name : "";
        strncpy(ln.name, nm, sizeof(ln.name) - 1);
        ln.name[sizeof(ln.name) - 1] = 0;
    }
}
```

(Confirmar campos reais de `meshtastic_NodeInfoLite`: `num`, `snr`, `hops_away`, `has_user`, `user.short_name`. Ver `src/mesh/NodeDB.h` + `getMeshNodeByIndex`.)

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): estação — captura SNR/RSSI do gateway + ring p/ enlace"
```

---

### Task B3: Endpoint `GET /api/portal/link`

**Files:**
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp`
- Modify: `src/modules/irrigation/IrrigationModule.h` (método público `portalBuildLink` se necessário para o endpoint acessar `nodeDB`/campos privados)

- [ ] **Step 1: Handler**

Seguir o padrão de `hNode` (linha ~55). Adicionar:

```cpp
static void hLink(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) { res->setStatusCode(503); return; }
    IrrigationWeb::LinkCtx c;
    irrigationModule->portalFillLink(c); // tornar público OU expor um wrapper
    char buf[1024];
    size_t n = IrrigationWeb::buildLink(c, buf, sizeof(buf));
    res->setHeader("Content-Type", "application/json");
    res->write((uint8_t *)buf, n);
}
```

- [ ] **Step 2: Registrar**

Junto às outras `registerNode` (~287):

```cpp
    server->registerNode(new ResourceNode("/api/portal/link", "GET", &hLink));
```

- [ ] **Step 3: Tornar `portalFillLink` acessível**

Se `hLink` não é membro, mover `portalFillLink` para a seção `public:` de `IrrigationModule.h` (já declarado no Task B2 — garantir `public`).

- [ ] **Step 4: Build ESP32 (CI)**

Local não compila ESP32 (ver [[native-tests-need-docker-on-windows]]). Push e conferir o job `tbeam`/gateway na CI + `trunk fmt`.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationPortalEndpoints.cpp src/modules/irrigation/IrrigationModule.h
git commit -m "feat(irrigation): portal — endpoint GET /api/portal/link (enlace do repetidor)"
```

---

### Task B4: Bump da contagem da suite + verificação final

**Files:**
- Modify: `test/native-suite-count` (se o repo mantém esse arquivo — confirmar no Task A0)

- [ ] **Step 1: Atualizar contagem**

Incrementar conforme os casos adicionados (uptime + buildLink = +2 se cada TEST_CASE conta; conferir a convenção do arquivo).

- [ ] **Step 2: Suite verde**

Run: `./bin/run-tests.sh`
Expected: verde, contagem bate.

- [ ] **Step 3: Commit**

```bash
git add test/native-suite-count
git commit -m "test(irrigation): bump native-suite-count (uptime + buildLink)"
```

---

## Self-review / notas de execução

- **Cobertura do spec:** wizard (já existe, inalterado) · Estação nó/rede/cobertura (A4/A5) · Serviço clientes/rede/log/config/cobertura (A6) · Repetidor nó/enlace/cobertura (A4/A8/B) · uptime (A1) · enlace backend (B1–B3). ✔
- **Dados mock-only não portados como real:** rótulos humanos de auditoria e nomes de sensores permanecem como hoje (número). Follow-up separado se quiser nomes (precisa ABI/endpoint de nomes de sensor — ver [[irrigation-phase6b-status]]).
- **Ordem de tabs (serviço):** `initService` esconde as abas não-serviço; garantir que a nova aba `Enlace` (`.rep-only`) também fique escondida no serviço (o gate `applyRoleLayout` só roda p/ role != serviço; em `initService` esconder `.rep-only` explicitamente).
- **RSSI por vizinho:** NodeDB não guarda RSSI por nó de forma confiável (só SNR + hops). O card de vizinho mostra SNR + saltos; o RSSI aparece só na métrica do próprio enlace ao gateway. (Diverge do mockup, que mostra RSSI por vizinho — aceitável, documentado.)
- **Verificação frontend:** cada task A tem preview via `mock.js` (`?role=0/2/3`, `?wizard=1`). Nenhuma depende do firmware para o preview (rota `/link` mockada em A7).
- **CI caveat:** Fase B compila só ESP32 (CI). Rodar `trunk fmt` na CI. Ver [[native-tests-need-docker-on-windows]].
