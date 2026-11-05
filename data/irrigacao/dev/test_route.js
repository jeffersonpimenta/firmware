// test_route.js — unit da decisão de routing (decideTarget). Roda sob Node:
//   node test_route.js       (a partir de data/irrigacao; na Task 2 passa a dev/, ver readFileSync)
const fs = require('fs');
const vm = require('vm');

const code = fs.readFileSync('../shared/route.js', 'utf-8');
const sandbox = { module: { exports: {} }, window: {}, location: {}, fetch: () => {} };
vm.createContext(sandbox);
vm.runInContext(code, sandbox);
const decideTarget = sandbox.module.exports.decideTarget;

let pass = 0, fail = 0;
function eq(got, want, name) {
  if (got === want) { pass++; }
  else { fail++; console.error(`FAIL ${name}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); }
}

const GW = '/irrigacao/gateway/index.html';
const ND = '/irrigacao/node/index.html';

// não-provisionado -> node (wizard), a partir do bootstrap
eq(decideTarget({ provisioned: false, role: 1, pathname: '/irrigacao/index.html', search: '' }), ND, 'wizard');
// gateway -> painel gateway
eq(decideTarget({ provisioned: true, role: 1, pathname: '/irrigacao/index.html', search: '' }), GW, 'gateway');
// estação (role 0) -> node
eq(decideTarget({ provisioned: true, role: 0, pathname: '/irrigacao/index.html', search: '' }), ND, 'estacao');
// papel desconhecido -> node (degradação segura)
eq(decideTarget({ provisioned: true, role: 9, pathname: '/irrigacao/index.html', search: '' }), ND, 'desconhecido');
// preserva query string no alvo
eq(decideTarget({ provisioned: true, role: 1, pathname: '/irrigacao/index.html', search: '?mock=1&role=1' }), GW + '?mock=1&role=1', 'preserva-query');
// já no destino certo (gateway no painel) -> null (não redireciona)
eq(decideTarget({ provisioned: true, role: 1, pathname: '/irrigacao/gateway/index.html', search: '' }), null, 'ja-no-gateway');
// já no destino certo (estação no node) -> null
eq(decideTarget({ provisioned: true, role: 0, pathname: '/irrigacao/node/index.html', search: '' }), null, 'ja-no-node');

console.log(`route.js: ${pass} pass, ${fail} fail`);
process.exit(fail ? 1 : 0);
