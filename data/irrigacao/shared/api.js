// api.js — camada de dados compartilhada entre app.js (gateway) e portal/app.js.
// Vanilla JS, sem dependências, sem módulos. Expõe globais em window.
// Carregado antes de app.js / portal/app.js; mock.js (quando ativo) substitui
// window.fetch antes deste arquivo ser executado.
'use strict';

// Gateway helper — usa a constante API definida pelo app que carrega este arquivo.
// getJson: retorna o JSON parseado diretamente (compatível com app.js).
async function getJson(path) {
  const r = await fetch(API + path);
  if (!r.ok) throw new Error(r.status);
  return r.json();
}

// postJson: retorna { ok, body } (compatível com app.js).
async function postJson(path, body) {
  const r = await fetch(API + path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  const b = await r.json().catch(() => ({}));
  return { ok: r.ok, body: b };
}

// Portal helper — recebe URL completa + options (compatível com portal/app.js).
// Retorna { ok, body } para GET e POST.
async function j(url, opts) {
  const r = await fetch(url, opts);
  const t = await r.text();
  let body = {};
  try { body = t ? JSON.parse(t) : {}; } catch (e) {}
  return { ok: r.ok, body };
}

// Helper de fetch bruto — passa opts diretamente, retorna Response nativa.
// Usado por helpers internos que precisam de controle total (download/upload).
function rawFetch(url, opts) {
  return fetch(url, opts);
}
