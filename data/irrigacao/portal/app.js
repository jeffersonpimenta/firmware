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

refresh();
setInterval(refresh, 3000);
