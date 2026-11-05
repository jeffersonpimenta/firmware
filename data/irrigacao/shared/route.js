// Roteador único de entrada do portal de irrigação.
// decideTarget é puro e testável (test_route.js). O IIFE consulta o estado do
// device e redireciona uma vez para a SPA certa, PRESERVANDO a query string
// (mock/wizard/role sobrevivem). Carregado só pelo bootstrap /irrigacao/index.html.
// Ver docs/superpowers/specs/2026-11-05-irrigacao-ui-routing-simplificacao-design.md
(function () {
  var GATEWAY = 1; // IrrigationRole::GATEWAY
  var GW = "/irrigacao/gateway/index.html";
  var ND = "/irrigacao/node/index.html";

  // Decide o alvo a partir do estado + localização atual.
  // Retorna URL (com query) para redirecionar, ou null se já está no lugar certo.
  function decideTarget(s) {
    var want;
    if (s.provisioned === false) want = ND;       // wizard de 1º boot
    else if (s.role === GATEWAY) want = GW;        // gateway -> painel
    else want = ND;                                 // estação/repetidor/serviço/desconhecido
    var base = want.replace(/\/index\.html$/, "");
    var here = (s.pathname || "").replace(/\/index\.html$/, "");
    if (here === base) return null;                // já no destino
    return want + (s.search || "");                // preserva query
  }

  // Export para Node (teste) e browser.
  if (typeof module !== "undefined" && module.exports) module.exports.decideTarget = decideTarget;
  if (typeof window !== "undefined") window.decideTarget = decideTarget;

  // Só executa o fetch/redirect num browser real (não sob Node/teste).
  if (typeof window === "undefined" || typeof fetch !== "function" || !window.location) return;

  fetch("/api/portal/node")
    .then(function (r) { if (!r.ok) throw new Error("http " + r.status); return r.json(); })
    .then(function (s) {
      var target = decideTarget({
        provisioned: s.provisioned,
        role: s.role,
        pathname: location.pathname,
        search: location.search,
      });
      if (target) location.replace(target);
    })
    .catch(function () { /* offline/preview: não redireciona */ });
})();
