const ROLES = ["Estação", "Gateway", "Repetidor", "Serviço"];
const ORIGENS = ["sistema","cronograma","painel","portal","botao","entrada","intertravamento","failsafe","servico"];
const ACOES = ["abrir","fechar","pulso","gpo_on","gpo_off","parear","factory_reset","config_epoch","safe_in","safe_out","tamper","reboot","hiberna_in","hiberna_out","rejeitado"];
const RESULTADOS = ["ok","nack","timeout"];

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
