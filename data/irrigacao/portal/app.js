const ROLES = ["Estação", "Gateway", "Repetidor", "Serviço"];
const ORIGENS = ["sistema","cronograma","painel","portal","botao","entrada","intertravamento","failsafe","servico"];
const ACOES = ["abrir","fechar","pulso","gpo_on","gpo_off","parear","factory_reset","config_epoch","safe_in","safe_out","tamper","reboot","hiberna_in","hiberna_out","rejeitado"];
const RESULTADOS = ["ok","nack","timeout"];
let svcInit = false; // Fase 8c: abas SERVICO ativadas 1× quando role==Serviço

async function j(url, opts) {
  const r = await fetch(url, opts);
  const t = await r.text();
  let body = {};
  try { body = t ? JSON.parse(t) : {}; } catch (e) {}
  return { ok: r.ok, body };
}

function renderNode(s) {
  const el = document.getElementById("nodeState");
  const valves = [];
  for (let i = 0; i < s.numValves; i++) valves.push((s.valveStates >> i) & 1 ? "▉" : "▁");
  el.innerHTML = `
    <h2>${s.name || "(sem nome)"} <small class="muted">${ROLES[s.role] || s.role}</small></h2>
    <p>Bateria: <b>${(s.vbatCentiV / 100).toFixed(2)} V</b></p>
    <p>Válvulas: <span class="mono">${valves.join(" ") || "—"}</span></p>
    <p>Gateway vinculado: ${s.boundGateway ? "0x" + s.boundGateway.toString(16) : "não pareado"}</p>
    <p>Epoch: ${s.configEpoch} ${s.safeMode ? "· <b>modo seguro</b>" : ""}</p>
    ${s.flags & 1 ? '<p class="tamper"><b>⚠ VIOLAÇÃO (tamper)</b></p>' : ''}`;
  document.getElementById("apLeft").textContent =
    s.apSecondsLeft ? `AP: ${Math.floor(s.apSecondsLeft / 60)}m${s.apSecondsLeft % 60}s` : "";
}

function renderGpos(s) {
  const card = document.getElementById("gposCard");
  const list = document.getElementById("gposList");
  if (!s.numGpos) { card.classList.add("hidden"); return; }
  card.classList.remove("hidden");
  let html = "";
  for (let i = 0; i < s.numGpos; i++) {
    const on = (s.gpoStates >> i) & 1;
    html += `<p>GPO ${i}: <b>${on ? "ligado" : "desligado"}</b>
      <button type="button" data-gpo="${i}" data-act="${on ? 0 : 1}">${on ? "Desligar" : "Ligar"}</button></p>`;
  }
  list.innerHTML = html;
  list.querySelectorAll("button").forEach((b) => b.addEventListener("click", () => sendGpo(+b.dataset.gpo, +b.dataset.act)));
}

async function sendGpo(gpo, action) {
  // Biestável (durationS 0) ao ligar exige confirmação extra (§8.11).
  let confirmFlag = false;
  if (action === 1) {
    if (!confirm(`Ligar GPO ${gpo} de forma biestável (permanece até desligar)?`)) return;
    confirmFlag = true;
  }
  const { ok, body } = await j("/api/portal/gpo", {
    method: "POST",
    body: JSON.stringify({ gpo, action, durationS: 0, confirm: confirmFlag }),
  });
  document.getElementById("gpoMsg").textContent = ok ? "OK" : (body.errors || ["erro"]).join("; ");
  refresh();
}

async function refreshSensors() {
  const { ok, body } = await j("/api/portal/sensors");
  const el = document.getElementById("sensorsList");
  if (!ok || !body.sensors || !body.sensors.length) { el.textContent = "nenhum sensor configurado"; return; }
  el.innerHTML = body.sensors.map((s) => {
    const val = s.tipo === 1 ? `${(s.valor / 100).toFixed(2)} ${s.unidade}` : (s.valor ? "ativo" : "inativo");
    return `<p>Sensor ${s.id}: <b>${val}</b></p>`;
  }).join("");
}

async function refreshLog() {
  const { ok, body } = await j("/api/portal/log");
  const el = document.getElementById("logList");
  if (!ok || !body.log || !body.log.length) { el.textContent = "log vazio"; return; }
  el.innerHTML = "<table><tr><th>ts</th><th>origem</th><th>ação</th><th>alvo</th><th>res</th></tr>" +
    body.log.map((r) =>
      `<tr><td>${r.ts}</td><td>${ORIGENS[r.origem] || r.origem}</td><td>${ACOES[r.acao] || r.acao}</td><td>${r.alvo}</td><td>${RESULTADOS[r.res] || r.res}</td></tr>`
    ).join("") + "</table>";
}
document.getElementById("logRefresh").addEventListener("click", refreshLog);

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
    renderNode(body);
    renderGpos(body);
    if (body.role === 3 && !svcInit) initService(); // §11.8: device SERVICO
  }
  await refreshSensors();
}

document.querySelectorAll("nav.tabs button").forEach((b) =>
  b.addEventListener("click", () => {
    document.querySelectorAll("nav.tabs button").forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    document.querySelectorAll(".tab").forEach((t) => t.classList.add("hidden"));
    document.getElementById("tab-" + b.dataset.tab).classList.remove("hidden");
  })
);

document.getElementById("pulseForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const valveId = +document.getElementById("pulseValve").value;
  const durationS = +document.getElementById("pulseDur").value;
  const { ok, body } = await j("/api/portal/node/pulse", {
    method: "POST",
    body: JSON.stringify({ valveId, durationS }),
  });
  document.getElementById("pulseMsg").textContent = ok ? "OK" : (body.errors || ["erro"]).join("; ");
  refresh();
});

async function loadRoster() {
  const { ok, body } = await j("/api/portal/net/roster");
  const sel = document.getElementById("netZone");
  sel.innerHTML = "";
  if (ok && Array.isArray(body) && body.length) {
    body.forEach((z) => {
      const o = document.createElement("option");
      o.value = z.id;
      o.textContent = `${z.id} — ${z.name}`;
      sel.appendChild(o);
    });
  } else {
    // Estação sem roster: permite digitar o número da zona (1..255).
    for (let i = 1; i <= 24; i++) {
      const o = document.createElement("option");
      o.value = i;
      o.textContent = "Zona " + i;
      sel.appendChild(o);
    }
  }
}

document.getElementById("netForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const zoneId = +document.getElementById("netZone").value;
  const kind = document.getElementById("netAction").value;
  const durationS = +document.getElementById("netDur").value;
  const payload = kind === "open" ? { kind, zoneId, durationS } : { kind, zoneId };
  const { ok, body } = await j("/api/portal/net/command", {
    method: "POST",
    body: JSON.stringify(payload),
  });
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
  document.querySelectorAll('nav.tabs button:not(.svc-only)').forEach((b) => b.classList.add("hidden"));
  document.querySelector('[data-tab="svcclients"]').click();
  loadClients();
  loadSvcLog();
}

async function loadClients() {
  const { ok, body } = await j("/api/portal/service/clients");
  const el = document.getElementById("clientsList");
  if (!ok || !body.clients) { el.textContent = "—"; return; }
  el.innerHTML = body.clients.map((c) =>
    `<p>${c.active ? "▶ " : ""}<b>${c.nome || c.id}</b> <small class="muted">${c.canal} · ${c.estacoes} nós</small>
     ${c.active ? "<em>(ativo)</em>" : `<button data-sel="${c.id}">Selecionar</button>`}</p>`
  ).join("") || "nenhum cliente no cofre";
  el.querySelectorAll("button[data-sel]").forEach((b) => (b.onclick = () => selectClient(b.dataset.sel)));
}

async function selectClient(id) {
  if (!confirm("Re-tunar no canal deste cliente? O device REINICIA (~3 s).")) return;
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
  el.innerHTML = "<table><tr><th>nó</th><th>papel</th><th>epoch</th><th>bat</th><th>fw</th><th>snr</th><th></th></tr>" +
    body.nodes.map((n) => {
      const hx = nodeHex(n.node);
      return `<tr><td class="mono">${hx}</td><td>${ROLES[n.role] || n.role}</td><td>${n.epoch}</td>
        <td>${(n.vbat / 100).toFixed(1)}V</td><td>0x${(n.fw || 0).toString(16)}</td><td>${(n.snr / 4).toFixed(0)}</td>
        <td><button data-rd="${hx}">Ler cfg</button> <button data-pulse="${hx}">Pulso</button>
            <button data-zone="${hx}">Zona</button> <button data-rs="${hx}">RESYNC</button></td></tr>`;
    }).join("") + "</table>";
  el.querySelectorAll("button[data-rd]").forEach((b) => (b.onclick = () => readConfig(b.dataset.rd)));
  el.querySelectorAll("button[data-pulse]").forEach((b) => (b.onclick = () => nodeAction(b.dataset.pulse, "pulse")));
  el.querySelectorAll("button[data-zone]").forEach((b) => (b.onclick = () => nodeAction(b.dataset.zone, "zone")));
  el.querySelectorAll("button[data-rs]").forEach((b) => (b.onclick = () => nodeAction(b.dataset.rs, "resync")));
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
  html += `<label>Rota <select id="cfg_route"><option value="direct">Direta (epoch+1)</option><option value="gateway">Via gateway</option></select></label>`;
  html += `<button type="button" id="cfgSave">Gravar config</button>`;
  const box = document.getElementById("cfgEditor");
  box.innerHTML = html;
  box.classList.remove("hidden");
  document.getElementById("cfgSave").onclick = writeConfig;
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
  const route = g("route").value;
  const { ok, body } = await j("/api/portal/service/node/config/write",
    { method: "POST", body: JSON.stringify({ node: cfgNode, route, config }) });
  document.getElementById("cfgMsg").textContent = ok ? "Config gravada" : (body.errors || ["erro"]).join("; ");
}

async function loadSvcLog() {
  const { ok, body } = await j("/api/portal/service/log");
  const el = document.getElementById("svcLogList");
  if (!ok || !body.log || !body.log.length) { el.textContent = "log vazio"; return; }
  el.innerHTML = body.log.map((r) =>
    `<p class="mono">up=${r.up}${r.ts ? " ts=" + r.ts : ""} <b>${r.ev}</b>${r.node ? " 0x" + (r.node >>> 0).toString(16) : ""}</p>`
  ).join("");
}

document.getElementById("svcScanBtn").addEventListener("click", startScanSvc);
document.getElementById("svcLogRefresh").addEventListener("click", loadSvcLog);
document.getElementById("exportBtn").addEventListener("click", exportVault);
document.getElementById("importBtn").addEventListener("click", () => importVault(false));
document.getElementById("importReplaceBtn").addEventListener("click", () => importVault(true));
