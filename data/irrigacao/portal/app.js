const ROLES = ["Estação", "Gateway", "Repetidor", "Serviço"];
const ORIGENS = ["sistema","cronograma","painel","portal","botao","entrada","intertravamento","failsafe","servico"];
const ACOES = ["abrir","fechar","pulso","gpo_on","gpo_off","parear","factory_reset","config_epoch","safe_in","safe_out","tamper","reboot","hiberna_in","hiberna_out","rejeitado"];
const RESULTADOS = ["ok","nack","timeout"];
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
let svcInit = false; // Fase 8c: abas SERVICO ativadas 1× quando role==Serviço
let lastRole = 0; // Fase 8d: última role vista, escolhe endpoint de survey

async function j(url, opts) {
  const r = await fetch(url, opts);
  const t = await r.text();
  let body = {};
  try { body = t ? JSON.parse(t) : {}; } catch (e) {}
  return { ok: r.ok, body };
}

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
  renderPulsePills(s.numValves);
}

let gpoPending = null;
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
  if (action === 1) { gpoPending = gpo; refresh(); return; }
  doGpo(gpo, 0, false);
}
async function doGpo(gpo, action, confirmFlag) {
  gpoPending = null;
  const { ok, body } = await j("/api/portal/gpo", { method: "POST", body: JSON.stringify({ gpo, action, durationS: 0, confirm: confirmFlag }) });
  document.getElementById("gpoMsg").textContent = ok ? "OK" : (body.errors || ["erro"]).join("; ");
  refresh();
}

async function refreshSensors() {
  const { ok, body } = await j("/api/portal/sensors");
  const el = document.getElementById("sensorsList");
  if (!ok || !body.sensors || !body.sensors.length) { el.textContent = "nenhum sensor configurado"; return; }
  el.innerHTML = body.sensors.map((s) => {
    const val = s.tipo === 1 ? `${(s.valor / 100).toFixed(2).replace('.', ',')} ${s.unidade}` : (s.valor ? "ativo" : "inativo");
    return `<div class="fp-kv"><span class="k">Sensor ${s.id}</span><span class="v">${val}</span></div>`;
  }).join("");
}

async function refreshLog() {
  const { ok, body } = await j("/api/portal/log");
  const el = document.getElementById("logList");
  if (!ok || !body.log || !body.log.length) { el.textContent = "log vazio"; return; }
  el.innerHTML = `<div class="fp-log">` + body.log.map((r) => {
    const ac = `${ACOES[r.acao] || r.acao} · ${ORIGENS[r.origem] || r.origem}${r.alvo ? " (" + r.alvo + ")" : ""}`;
    return `<div class="row"><span class="ts">${r.ts}</span><span class="ac">${ac} <small class="muted">${RESULTADOS[r.res] || r.res}</small></span></div>`;
  }).join("") + `</div>`;
}
document.getElementById("logRefresh").addEventListener("click", refreshLog);

async function refreshEnlace() {
  if (lastRole === ROLE_SERVICO || lastRole === ROLE_GATEWAY) return;
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

async function loadCoords() {
  const { ok, body } = await j("/api/portal/coords");
  if (ok) {
    document.getElementById("coordLat").value = (body.latE7 || 0) / 1e7;
    document.getElementById("coordLon").value = (body.lonE7 || 0) / 1e7;
  }
}
document.getElementById("coordsForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const latE7 = Math.round(+document.getElementById("coordLat").value * 1e7);
  const lonE7 = Math.round(+document.getElementById("coordLon").value * 1e7);
  const { ok, body } = await j("/api/portal/coords", { method: "POST", body: JSON.stringify({ latE7, lonE7 }) });
  document.getElementById("coordMsg").textContent = ok ? "Salvo" : (body.errors || ["erro"]).join("; ");
});

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
    refreshEnlace();
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
  if (role === ROLE_SERVICO) return; // serviço usa initService p/ o próprio layout
  const rep = role === ROLE_REPETIDOR;
  ["sensorsCard", "gposCard", "pulseForm", "logCard"].forEach((id) => {
    const e = document.getElementById(id); if (e) e.classList.toggle("hidden-role", rep);
  });
  document.querySelector('[data-tab="net"]').classList.toggle("hidden", rep);
  document.querySelectorAll(".node-mais").forEach((b) => b.classList.remove("hidden"));
}

// ── Wizard de 1º boot (§6) — escolha de papel, some após provisionar ─────────
let wizardShown = false;
function showWizard() {
  if (wizardShown) return;
  wizardShown = true;
  document.querySelector("header").classList.add("hidden");
  document.querySelector("nav.tabs").classList.add("hidden");
  document.querySelectorAll("section.panel").forEach((t) => t.classList.add("hidden"));
  document.getElementById("wizard").classList.remove("hidden");
  const roles = document.querySelectorAll(".wz-role");
  const farmWrap = document.getElementById("wz-farm-wrap");
  let selRole = 1; // default: Gateway
  const syncFarm = () => { farmWrap.style.display = selRole === 1 ? "block" : "none"; };
  roles.forEach((b) => (b.onclick = () => {
    roles.forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    selRole = +b.dataset.role;
    syncFarm();
  }));
  syncFarm();
  document.getElementById("wz-go").onclick = async () => {
    const payload = { role: selRole };
    const farm = document.getElementById("wz-farm").value.trim();
    if (selRole === 1 && farm) payload.farmName = farm;
    document.getElementById("wz-msg").textContent = "Gravando…";
    const { ok, body } = await j("/api/portal/provision", { method: "POST", body: JSON.stringify(payload) });
    document.getElementById("wz-msg").textContent = ok
      ? "✓ Configurado! O dispositivo reinicia em ~3 s. Reconecte ao portal depois."
      : "Erro: " + ((body.errors || ["falha"]).join("; "));
  };
}

document.querySelectorAll("nav.tabs button").forEach((b) =>
  b.addEventListener("click", () => {
    document.querySelectorAll("nav.tabs button").forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    document.querySelectorAll(".panel").forEach((t) => t.classList.add("hidden"));
    document.getElementById("tab-" + b.dataset.tab).classList.remove("hidden");
    if (b.dataset.tab === "mais") { refreshEnlace(); loadCoords(); wifiCloseScreen(); }
  })
);

let pulseValve = 0, pulseOpen = false;
function renderPulsePills(n) {
  const wrap = document.getElementById("pulsePills");
  if (!wrap) return;
  let h = "";
  for (let i = 0; i < (n || 0); i++) h += `<button type="button" class="fp-pill ${i === pulseValve ? "sel" : ""}" data-pv="${i}">${i}</button>`;
  wrap.innerHTML = h || "<span class='muted'>—</span>";
  wrap.querySelectorAll("[data-pv]").forEach((b) => b.onclick = () => { pulseValve = +b.dataset.pv; renderPulsePills(n); });
  const st = document.getElementById("pulseState");
  if (st) st.innerHTML = pulseOpen ? `<div class="fp-pulseopen">Válvula aberta</div>` : "";
  const go = document.getElementById("pulseGo");
  if (go) go.textContent = pulseOpen ? "Fechar agora" : "Abrir";
}
document.getElementById("pulseGo").addEventListener("click", async () => {
  const durationS = +document.getElementById("pulseDur").value;
  const n = document.querySelectorAll("#pulsePills [data-pv]").length;
  if (pulseOpen) { pulseOpen = false; document.getElementById("pulseMsg").textContent = "Fechado"; renderPulsePills(n); return; }
  const { ok, body } = await j("/api/portal/node/pulse", { method: "POST", body: JSON.stringify({ valveId: pulseValve, durationS }) });
  document.getElementById("pulseMsg").textContent = ok ? "OK" : (body.errors || ["erro"]).join("; ");
  pulseOpen = ok; renderPulsePills(n);
  refresh();
});

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

loadRoster();
loadCoords();
refresh();
refreshLog();
setInterval(refresh, 3000);

// ── Fase 8c — portal do device SERVICO (§11.8) ──────────────────────────────
function initService() {
  svcInit = true;
  document.querySelectorAll(".svc-only").forEach((b) => b.classList.remove("hidden"));
  // Esconde as abas de nó/rede-local; o device SERVICO usa Clientes/Rede(cliente)/Log.
  document.querySelectorAll('nav.tabs button:not(.svc-only):not(.survey-tab)').forEach((b) => b.classList.add("hidden"));
  document.querySelector('[data-tab="svcclients"]').click();
  document.getElementById("hdrName").textContent = "Dispositivo de Serviço";
  document.getElementById("hdrSub").textContent = "Cofre multi-cliente";
  document.querySelectorAll(".node-mais").forEach((b) => b.classList.add("hidden"));
  loadClients();
  loadSvcLog();
}

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

function exportVault() { location.href = "/api/portal/service/export"; }

async function importVault(replace) {
  const f = document.getElementById("importFile").files[0];
  if (!f) { document.getElementById("clientsMsg").textContent = "escolha um arquivo"; return; }
  const url = "/api/portal/service/import" + (replace ? "/replace" : "");
  const { ok, body } = await j(url, { method: "POST", body: await f.text() });
  document.getElementById("clientsMsg").textContent = ok ? "Importado" : (body.errors || ["erro"]).join("; ");
  loadClients();
}

async function startScanSvc() {
  await j("/api/portal/service/scan", { method: "POST" });
  document.getElementById("svcnetMsg").textContent = "Varredura disparada…";
  setTimeout(pollScanSvc, 1500);
}

function nodeHex(n) { return "!" + (n >>> 0).toString(16).padStart(8, "0"); }

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

async function nodeAction(node, action) {
  let extra = {};
  if (action === "pulse") {
    extra = { valveId: +prompt("Válvula (0-7)", "0"), durationS: +prompt("Duração (s)", "10") };
  } else if (action === "zone") {
    const zoneId = +prompt("Zona (1-255)", "1");
    const open = confirm("Abrir? (Cancelar = fechar)");
    extra = { zoneId, open: open ? 1 : 0, durationS: open ? +prompt("Duração (s)", "300") : 0 };
  }
  const { ok, body } = await j("/api/portal/service/node/action",
    { method: "POST", body: JSON.stringify({ node, action, ...extra }) });
  document.getElementById("svcnetMsg").textContent = ok ? "OK" : (body.errors || ["erro"]).join("; ");
}

// Editor de config — campos escalares/pinos como inputs; sensores/intertravamentos como JSON.
const CFG_SCALARS = ["numValves", "hbMinutes", "vbatMinAbrirCentiV", "maxOpenConfigS", "cmdRatePerMin", "pulseMs",
  "digitalInActiveLow", "pinBtn", "pinLed", "pinTamper", "hwFlags", "latE7", "lonE7"];
const CFG_PINS = ["pinsHbridgeA", "pinsHbridgeB", "pinsDigitalIn", "pinsGpo"];
let cfgNode = null;

async function readConfig(node) {
  cfgNode = node;
  document.getElementById("cfgMsg").textContent = "Lendo… (aguardando reply do nó)";
  for (let i = 0; i < 20; i++) {
    const { ok, body } = await j("/api/portal/service/node/config/read", { method: "POST", body: JSON.stringify({ node }) });
    if (ok && !body.pending) { fillEditor(body); document.getElementById("cfgMsg").textContent = "Config lida de " + node; return; }
    await new Promise((r) => setTimeout(r, 700));
  }
  document.getElementById("cfgMsg").textContent = "Timeout lendo config (nó fora de alcance?)";
}

function fillEditor(c) {
  let html = `<p class="muted">Geridos (read-only): role=${c.role} · epoch=${c.configEpoch} · gw=0x${(c.boundGateway >>> 0).toString(16)}</p>`;
  CFG_SCALARS.forEach((k) => (html += `<label>${k} <input id="cfg_${k}" type="number" value="${c[k]}"></label>`));
  CFG_PINS.forEach((k) => (html += `<label>${k} (csv) <input id="cfg_${k}" value="${(c[k] || []).join(",")}"></label>`));
  html += `<label>sensores (JSON) <textarea id="cfg_sensores" rows="4">${JSON.stringify(c.sensores || [])}</textarea></label>`;
  html += `<label>localInterlocks (JSON) <textarea id="cfg_localInterlocks" rows="4">${JSON.stringify(c.localInterlocks || [])}</textarea></label>`;
  html += `<div><div class="fp-lbl">Rota de gravação</div><div class="fp-seg" id="cfgSeg">
    <button type="button" data-r="direct" class="sel">Direta (epoch+1)</button>
    <button type="button" data-r="gateway">Via gateway</button></div></div>`;
  html += `<button type="button" id="cfgSave" class="btn solid sm">Gravar config</button>`;
  const box = document.getElementById("cfgEditor");
  box.innerHTML = html;
  box.classList.remove("hidden");
  document.getElementById("cfgSave").onclick = writeConfig;
  window.__cfgRoute = "direct";
  box.querySelectorAll("#cfgSeg button").forEach((b) => b.onclick = () => {
    window.__cfgRoute = b.dataset.r;
    box.querySelectorAll("#cfgSeg button").forEach((x) => x.classList.toggle("sel", x === b));
  });
}

async function writeConfig() {
  const g = (id) => document.getElementById("cfg_" + id);
  const config = {};
  CFG_SCALARS.forEach((k) => (config[k] = +g(k).value));
  CFG_PINS.forEach((k) => (config[k] = g(k).value.split(",").map((x) => +x.trim())));
  try {
    config.sensores = JSON.parse(g("sensores").value);
    config.localInterlocks = JSON.parse(g("localInterlocks").value);
  } catch (e) {
    document.getElementById("cfgMsg").textContent = "JSON inválido em sensores/localInterlocks";
    return;
  }
  const route = window.__cfgRoute || "direct";
  const { ok, body } = await j("/api/portal/service/node/config/write",
    { method: "POST", body: JSON.stringify({ node: cfgNode, route, config }) });
  document.getElementById("cfgMsg").textContent = ok ? "Config gravada" : (body.errors || ["erro"]).join("; ");
}

async function loadSvcLog() {
  const { ok, body } = await j("/api/portal/service/log");
  const el = document.getElementById("svcLogList");
  if (!ok || !body.log || !body.log.length) { el.textContent = "log vazio"; return; }
  el.innerHTML = `<div class="fp-log">` + body.log.map((r) => {
    const ac = `${r.ev}${r.node ? " · 0x" + (r.node >>> 0).toString(16) : ""}`;
    return `<div class="row"><span class="ts">up=${r.up}</span><span class="ac">${ac}</span></div>`;
  }).join("") + `</div>`;
}

document.getElementById("svcScanBtn").addEventListener("click", startScanSvc);
document.getElementById("svcLogRefresh").addEventListener("click", loadSvcLog);
document.getElementById("exportBtn").addEventListener("click", exportVault);
document.getElementById("importBtn").addEventListener("click", () => importVault(false));
document.getElementById("importReplaceBtn").addEventListener("click", () => importVault(true));

// ── Fase 8d — modo cobertura (site survey §8.5) ─────────────────────────────
const svBase = () => (lastRole === 3 ? "/api/portal/service/survey" : "/api/portal/survey");
document.getElementById("surveyForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const intervalS = +document.getElementById("svInterval").value;
  const timeoutS = Math.round(+document.getElementById("svTimeout").value * 60);
  const lat = +document.getElementById("svLat").value, lon = +document.getElementById("svLon").value;
  const payload = { intervalS, timeoutS };
  if (lat || lon) { payload.lat = Math.round(lat * 1e7); payload.lon = Math.round(lon * 1e7); }
  const { ok, body } = await j(svBase() + "/start", { method: "POST", body: JSON.stringify(payload) });
  document.getElementById("svMsg").textContent = ok ? "Beacon iniciado" : (body.errors || ["erro"]).join("; ");
});
document.getElementById("svStop").addEventListener("click", async () => {
  const { ok } = await j(svBase() + "/stop", { method: "POST" });
  document.getElementById("svMsg").textContent = ok ? "Beacon parado" : "erro";
});

// ── Rede Wi-Fi (portal de campo) ────────────────────────────────────────────
// Estado local da tela Wi-Fi
let wifiEnabled = false;       // reflete o toggle atual
let wifiSelectedNet = null;    // {ssid, rssi, secure} da rede escolhida
let wifiScanTimer = null;      // timer de polling do scan
let wifiConnectTimer = null;   // timer de polling do connect

// Helpers de visibilidade
function wifiShow(id) { const e = document.getElementById(id); if (e) e.classList.remove("hidden"); }
function wifiHide(id) { const e = document.getElementById(id); if (e) e.classList.add("hidden"); }

// Atualiza a aparência do toggle de acordo com wifiEnabled
function wifiRenderToggle() {
  const track = document.getElementById("wifiToggle");
  const knob  = document.getElementById("wifiToggleKnob");
  if (!track || !knob) return;
  track.style.background = wifiEnabled ? "var(--green)" : "oklch(0.85 0.006 100)";
  knob.style.left = wifiEnabled ? "18px" : "2px";
}

// Cancela todos os timers pendentes de Wi-Fi
function wifiClearTimers() {
  if (wifiScanTimer)    { clearTimeout(wifiScanTimer);    wifiScanTimer    = null; }
  if (wifiConnectTimer) { clearTimeout(wifiConnectTimer); wifiConnectTimer = null; }
}

// Mostra a sub-tela Wi-Fi e esconde a lista principal do "Mais"
function wifiOpenScreen() {
  wifiHide("mais-list");
  wifiShow("wifi-screen");
}

// Volta à lista principal do "Mais" e cancela timers
function wifiCloseScreen() {
  wifiClearTimers();
  wifiHide("wifi-screen");
  wifiShow("mais-list");
  // Garante que sub-vistas fiquem limpas para a próxima abertura
  wifiShowSubView("list");
}

// Alterna entre as sub-vistas dentro da tela Wi-Fi
// vista: "list" | "password" | "connecting" | "success" | "error"
function wifiShowSubView(vista) {
  ["wifiListView", "wifiPasswordView", "wifiConnectingView", "wifiSuccessView", "wifiErrorView"].forEach((id) => {
    document.getElementById(id).classList.add("hidden");
  });
  const map = {
    list:       "wifiListView",
    password:   "wifiPasswordView",
    connecting: "wifiConnectingView",
    success:    "wifiSuccessView",
    error:      "wifiErrorView",
  };
  if (map[vista]) document.getElementById(map[vista]).classList.remove("hidden");
}

// Atualiza o card de resumo na lista de "Mais"
function wifiUpdateSummary(body) {
  const el = document.getElementById("wifiSummaryLabel");
  if (!el) return;
  if (!body.enabled) { el.textContent = "Desativado"; return; }
  el.textContent = body.staUp && body.connectedSsid ? "Conectado a " + body.connectedSsid : "Não conectado";
}

// Renderiza a lista de redes depois de um scan concluído
function wifiRenderNetworks(networks, connectedSsid) {
  const list = document.getElementById("wifiNetworksList");
  const hint = document.getElementById("wifiForgetHint");
  wifiHide("wifiScanning");
  if (!networks || networks.length === 0) {
    wifiHide("wifiNetworksList");
    if (hint) hint.classList.add("hidden");
    wifiShow("wifiEmpty");
    return;
  }
  wifiHide("wifiEmpty");
  wifiShow("wifiNetworksList");
  const hasConnected = networks.some((n) => n.ssid === connectedSsid);
  if (hint) hint.classList.toggle("hidden", !hasConnected);

  list.innerHTML = networks.map((net) => {
    const connected = net.ssid === connectedSsid;
    const border = connected ? "oklch(0.47 0.1 150 / 0.35)" : "var(--border)";
    // Sinal em 3 barras (rssi: -100 fraco → -50 forte)
    const sig = net.rssi >= -65 ? 3 : net.rssi >= -80 ? 2 : 1;
    const lit = "var(--green)", dim = "oklch(0.85 0.006 100)";
    const bars = `<div style="display:flex;align-items:flex-end;gap:2px;height:14px;flex-shrink:0;">
      <div style="width:3px;height:5px;border-radius:1px;background:${sig >= 1 ? lit : dim};"></div>
      <div style="width:3px;height:9px;border-radius:1px;background:${sig >= 2 ? lit : dim};"></div>
      <div style="width:3px;height:14px;border-radius:1px;background:${sig >= 3 ? lit : dim};"></div>
    </div>`;
    const connBadge = connected
      ? `<span style="font-size:10.5px;font-weight:700;padding:3px 8px;border-radius:999px;background:oklch(0.47 0.1 150 / 0.12);color:var(--green);white-space:nowrap;flex-shrink:0;">Conectado</span>`
      : "";
    return `<div data-ssid="${net.ssid.replace(/"/g, "&quot;")}" data-secure="${net.secure ? 1 : 0}" data-connected="${connected ? 1 : 0}"
      style="cursor:pointer;background:var(--card);border:1px solid ${border};box-shadow:var(--shadow-card);border-radius:14px;padding:12px 14px;display:flex;align-items:center;justify-content:space-between;gap:10px;">
      <div style="display:flex;align-items:center;gap:10px;min-width:0;">
        ${bars}
        <div style="font-size:14px;font-weight:600;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;">${net.ssid}</div>
      </div>
      ${connBadge}
    </div>`;
  }).join("");

  list.querySelectorAll("[data-ssid]").forEach((el) => {
    el.addEventListener("click", () => {
      if (+el.dataset.connected) { wifiForget(); return; }
      const ssid   = el.dataset.ssid;
      const secure = +el.dataset.secure;
      wifiSelectedNet = networks.find((n) => n.ssid === ssid) || { ssid, rssi: 0, secure: !!secure };
      if (!secure) {
        wifiDoConnect("");
      } else {
        document.getElementById("wifiPasswordSsid").textContent = ssid;
        document.getElementById("wifiPasswordInput").value = "";
        wifiShowSubView("password");
      }
    });
  });
}

// Inicia um scan: POST /scan depois POLL GET /scan a cada 1500ms
async function wifiStartScan() {
  wifiClearTimers();
  wifiHide("wifiEmpty");
  wifiHide("wifiNetworksList");
  const hint = document.getElementById("wifiForgetHint");
  if (hint) hint.classList.add("hidden");
  wifiShow("wifiScanning");
  await j("/api/portal/wifi/scan", { method: "POST" });
  wifiPollScan();
}

async function wifiPollScan() {
  const { ok, body } = await j("/api/portal/wifi/scan");
  if (!ok) { wifiHide("wifiScanning"); return; }
  if (body.scanning) {
    wifiScanTimer = setTimeout(wifiPollScan, 1500);
    return;
  }
  // Scan concluído
  const { ok: stOk, body: stBody } = await j("/api/portal/wifi");
  const connectedSsid = stOk ? stBody.connectedSsid : null;
  wifiRenderNetworks(body.networks || [], connectedSsid);
}

// Envia POST /connect e faz polling de GET /connect
async function wifiDoConnect(psk) {
  const ssid = wifiSelectedNet ? wifiSelectedNet.ssid : "";
  wifiShowSubView("connecting");
  document.getElementById("wifiConnectingLabel").textContent = "Conectando a " + ssid + "…";
  wifiClearTimers();
  await j("/api/portal/wifi/connect", { method: "POST", body: JSON.stringify({ ssid, psk }) });
  wifiPollConnect(ssid);
}

async function wifiPollConnect(ssid) {
  const { ok, body } = await j("/api/portal/wifi/connect");
  if (!ok || body.state === "connecting" || body.state === "idle") {
    wifiConnectTimer = setTimeout(() => wifiPollConnect(ssid), 1500);
    return;
  }
  if (body.state === "success") {
    const label = document.getElementById("wifiSuccessLabel");
    if (label) label.textContent = "Conectado a " + (body.ssid || ssid);
    wifiShowSubView("success");
    // Atualiza card de resumo
    wifiUpdateSummary({ enabled: true, staUp: true, connectedSsid: body.ssid || ssid });
  } else {
    // error ou estado desconhecido
    const msg = document.getElementById("wifiErrorMsg");
    if (msg) msg.textContent = body.error || "Falha ao conectar. Verifique a senha e tente novamente.";
    wifiShowSubView("error");
  }
}

// Esquece a rede conectada
async function wifiForget() {
  await j("/api/portal/wifi/forget", { method: "POST" });
  wifiUpdateSummary({ enabled: true, staUp: false, connectedSsid: null });
  wifiStartScan();
}

// ---- Wiring de eventos ----

// Abre a tela Wi-Fi ao clicar no navcard
document.getElementById("wifiNavCard").addEventListener("click", async () => {
  wifiOpenScreen();
  wifiShowSubView("list");
  // Carrega estado inicial
  const { ok, body } = await j("/api/portal/wifi");
  if (ok) {
    wifiEnabled = !!body.enabled;
    wifiUpdateSummary(body);
  }
  wifiRenderToggle();
  if (wifiEnabled) {
    wifiShow("wifiOnState");
    wifiHide("wifiOffState");
    wifiStartScan();
  } else {
    wifiShow("wifiOffState");
    wifiHide("wifiOnState");
  }
});

// Botão "Mais" (voltar)
document.getElementById("wifiBack").addEventListener("click", () => wifiCloseScreen());

// Toggle de Wi-Fi
document.getElementById("wifiToggle").addEventListener("click", async () => {
  wifiClearTimers();
  wifiEnabled = !wifiEnabled;
  wifiRenderToggle();
  await j("/api/portal/wifi/toggle", { method: "POST", body: JSON.stringify({ enabled: wifiEnabled }) });
  if (wifiEnabled) {
    wifiShow("wifiOnState");
    wifiHide("wifiOffState");
    wifiHide("wifiEmpty");
    wifiHide("wifiNetworksList");
    wifiStartScan();
  } else {
    wifiHide("wifiOnState");
    wifiShow("wifiOffState");
  }
  wifiUpdateSummary({ enabled: wifiEnabled, staUp: false, connectedSsid: null });
});

// Botão "Atualizar"
document.getElementById("wifiRefreshBtn").addEventListener("click", () => wifiStartScan());

// Botão "Buscar novamente" (estado vazio)
document.getElementById("wifiRescanBtn").addEventListener("click", () => wifiStartScan());

// Vista de senha — botão voltar (‹ Redes Wi-Fi) e botão cancelar
function wifiCancelPassword() {
  wifiSelectedNet = null;
  document.getElementById("wifiPasswordInput").value = "";
  wifiShowSubView("list");
}
document.getElementById("wifiPasswordBack").addEventListener("click", wifiCancelPassword);
document.getElementById("wifiPasswordCancel").addEventListener("click", wifiCancelPassword);

// Vista de senha — botão conectar
document.getElementById("wifiPasswordConnect").addEventListener("click", () => {
  const psk = document.getElementById("wifiPasswordInput").value;
  wifiDoConnect(psk);
});

// Vista de sucesso — botão concluído
document.getElementById("wifiSuccessBtn").addEventListener("click", () => {
  wifiSelectedNet = null;
  wifiShowSubView("list");
  wifiStartScan();
});

// Vista de erro — cancelar
document.getElementById("wifiErrorCancel").addEventListener("click", () => {
  wifiSelectedNet = null;
  wifiShowSubView("list");
});

// Vista de erro — tentar novamente
document.getElementById("wifiErrorRetry").addEventListener("click", () => {
  wifiShowSubView("password");
  document.getElementById("wifiPasswordInput").value = "";
});

// Inicializa o summary label do card ao carregar a página
(async () => {
  const { ok, body } = await j("/api/portal/wifi");
  if (ok) wifiUpdateSummary(body);
})();
