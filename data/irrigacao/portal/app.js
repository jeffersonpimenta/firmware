const ROLES = ["Estação", "Gateway", "Repetidor", "Serviço"];

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
    <p>Epoch: ${s.configEpoch} ${s.safeMode ? "· <b>modo seguro</b>" : ""}</p>`;
  document.getElementById("apLeft").textContent =
    s.apSecondsLeft ? `AP: ${Math.floor(s.apSecondsLeft / 60)}m${s.apSecondsLeft % 60}s` : "";
}

async function refresh() {
  const { ok, body } = await j("/api/portal/node");
  if (ok) renderNode(body);
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
refresh();
setInterval(refresh, 3000);
