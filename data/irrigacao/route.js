// Roteador unico de entrada do portal de irrigacao.
// Fonte unica de verdade da navegacao: consulta o estado do device e redireciona
// para a tela certa (wizard / painel gateway / portal do no). Carregado ANTES do
// app.js de cada pagina.
// Ver docs/superpowers/specs/2026-09-27-irrigacao-roteamento-portal-cache-design.md
(function () {
  var GATEWAY = 1; // IrrigationRole::GATEWAY
  var PANEL = "/irrigacao/index.html";
  var PORTAL = "/irrigacao/portal/index.html";

  fetch("/api/portal/node")
    .then(function (r) {
      if (!r.ok) throw new Error("http " + r.status);
      return r.json();
    })
    .then(function (s) {
      var target;
      if (s.provisioned === false) target = PORTAL; // wizard de 1o boot
      else if (s.role === GATEWAY) target = PANEL; // gateway -> painel
      else target = PORTAL; // estacao/repetidor/servico -> portal
      var onPortal = location.pathname.indexOf("/irrigacao/portal/") === 0;
      var wantPortal = target === PORTAL;
      if (wantPortal !== onPortal) location.replace(target);
    })
    .catch(function () {
      // offline/preview: nao redireciona, deixa a pagina renderizar
    });
})();
