# Simplificação de routing e estrutura da UI de irrigação — Plano de Implementação

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Entrada única por papel (bootstrap) e árvore de ficheiros intuitiva para a UI de irrigação, com comportamento idêntico em todas as plataformas.

**Architecture:** `/irrigacao/index.html` vira um bootstrap mínimo que carrega só `shared/route.js`; `route.js` consulta `GET /api/portal/node` e redireciona uma vez para `gateway/` ou `node/`, preservando a query string. Ficheiros agrupados por papel (`gateway/ node/ shared/ dev/`). Fase 2 opcional renomeia os prefixos de API para casar os papéis.

**Tech Stack:** HTML/CSS/JS vanilla (sem build, sem módulos), servidos do LittleFS; firmware C++ (ESP32) via `ContentHandler.cpp`; staging do FS em `bin/platformio-custom.py` (SCons/PlatformIO); testes JS como scripts Node standalone.

## Global Constraints

- JS vanilla, sem dependências, sem módulos; globais em `window` (padrão de `api.js`). — copiado de `api.js`
- Referências a assets **absolutas** a partir da raiz web (`/irrigacao/...`). O gateway sai da raiz; relativos quebrariam.
- Alvos de redirect terminam sempre em `index.html` explícito (ESP8266WebServer não resolve index-de-diretório).
- `?mock=1` é só-dev: o snippet de injeção só dispara com `?mock` na URL; a pasta `dev/` é excluída da imagem LittleFS do device.
- APIs intactas na Fase 1 (`/api/portal`, `/api/irrigation`). Rename só na Fase 2.
- Verificação por-task via `python -m http.server` servindo `data/` (rápido). O rig ESP8266 (`tools/ui-rig-esp8266/`) é ferramenta local **não versionada**; serve para medir velocidade/feel no fim, não é entregável.
- Specs/comentários em pt-BR, seguindo o estilo do repo.

---

## FASE 1 — núcleo (routing + estrutura)

### Task 1: `route.js` — `decideTarget` puro, query-preserving, com unit test

Isola a decisão de routing numa função pura e corrige o bug de perda de query. Ainda não move ficheiros (o teste é puro e independe da árvore).

**Files:**
- Modify: `data/irrigacao/route.js` (reescrita completa)
- Create: `data/irrigacao/test_route.js` (script Node; move para `dev/` na Task 2)

**Interfaces:**
- Produces: `decideTarget({ provisioned, role, pathname, search }) -> string | null`
  - retorna URL de destino (incluindo `search`) quando deve redirecionar; `null` quando já está no destino certo ou não deve mexer.
  - Exposto como `window.decideTarget` no browser e `module.exports.decideTarget` sob Node.

- [ ] **Step 1: Escrever o teste que falha**

Create `data/irrigacao/test_route.js`:

```js
// test_route.js — unit da decisão de routing (decideTarget). Roda sob Node:
//   node test_route.js       (a partir de data/irrigacao; na Task 2 passa a dev/, ver readFileSync)
const fs = require('fs');
const vm = require('vm');

const code = fs.readFileSync('route.js', 'utf-8');
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
```

- [ ] **Step 2: Rodar o teste e confirmar que falha**

Run: `cd data/irrigacao && node test_route.js`
Expected: FAIL — `decideTarget` é `undefined` (route.js ainda não exporta), erro `TypeError: decideTarget is not a function`.

- [ ] **Step 3: Reescrever `route.js`**

Replace o conteúdo de `data/irrigacao/route.js` por:

```js
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
```

- [ ] **Step 4: Rodar o teste e confirmar que passa**

Run: `cd data/irrigacao && node test_route.js`
Expected: PASS — `route.js: 7 pass, 0 fail`.

- [ ] **Step 5: Commit**

```bash
git add data/irrigacao/route.js data/irrigacao/test_route.js
git commit -m "refactor(irrigation): route.js com decideTarget puro + preserva query string"
```

---

### Task 2: Restruturar a árvore + bootstrap + staging

Move os ficheiros para a árvore agrupada por papel, cria o bootstrap, torna as refs absolutas, remove `route.js` das SPAs e exclui `dev/` do LittleFS.

**Files:**
- Move: `data/irrigacao/app.js` → `data/irrigacao/gateway/app.js`
- Move: `data/irrigacao/index.html` → `data/irrigacao/gateway/index.html`
- Move: `data/irrigacao/portal/index.html` → `data/irrigacao/node/index.html`
- Move: `data/irrigacao/portal/app.js` → `data/irrigacao/node/app.js`
- Move: `data/irrigacao/style.css` → `data/irrigacao/shared/style.css`
- Move: `data/irrigacao/api.js` → `data/irrigacao/shared/api.js`
- Move: `data/irrigacao/route.js` → `data/irrigacao/shared/route.js`
- Move: `data/irrigacao/mock.js` → `data/irrigacao/dev/mock.js`
- Move: `data/irrigacao/test_mock.js` → `data/irrigacao/dev/test_mock.js`
- Move: `data/irrigacao/test_niveis.js` → `data/irrigacao/dev/test_niveis.js`
- Move: `data/irrigacao/test_radio.js` → `data/irrigacao/dev/test_radio.js`
- Move: `data/irrigacao/test_route.js` → `data/irrigacao/dev/test_route.js`
- Create: `data/irrigacao/index.html` (bootstrap novo)
- Modify: `bin/platformio-custom.py:389-395` (staging exclui `dev/`)

**Interfaces:**
- Consumes: `shared/route.js` (Task 1) carregado pelo bootstrap.
- Produces: árvore web `/irrigacao/{gateway,node,shared,dev}/...` e `/irrigacao/index.html` (bootstrap).

- [ ] **Step 1: Mover ficheiros com `git mv`**

```bash
cd data/irrigacao
mkdir gateway node shared dev
git mv app.js gateway/app.js
git mv index.html gateway/index.html
git mv portal/index.html node/index.html
git mv portal/app.js node/app.js
git mv style.css shared/style.css
git mv api.js shared/api.js
git mv route.js shared/route.js
git mv mock.js dev/mock.js
git mv test_mock.js dev/test_mock.js
git mv test_niveis.js dev/test_niveis.js
git mv test_radio.js dev/test_radio.js
git mv test_route.js dev/test_route.js
rmdir portal
```

- [ ] **Step 2: `gateway/index.html` — refs absolutas, sem route.js**

Em `data/irrigacao/gateway/index.html` substituir o bloco `<link>` do head e o bloco de `<script>` do fim.

Trocar `<link rel="stylesheet" href="style.css" />` por:
```html
    <link rel="stylesheet" href="/irrigacao/shared/style.css" />
```

Trocar o bloco final (os 4 `<script>`) por (note: **sem** `route.js`; mock aponta para `dev/`):
```html
    <script>if(new URLSearchParams(location.search).has('mock')){document.write('<script src="/irrigacao/dev/mock.js"><\/script>');}</script>
    <script src="/irrigacao/shared/api.js"></script>
    <script src="/irrigacao/gateway/app.js"></script>
```

- [ ] **Step 3: `node/index.html` — refs absolutas, sem route.js**

Em `data/irrigacao/node/index.html`:

Trocar `<link rel="stylesheet" href="/irrigacao/style.css" />` por:
```html
  <link rel="stylesheet" href="/irrigacao/shared/style.css" />
```

Trocar o bloco final de `<script>` por (sem `route.js`; mock e app em caminhos novos):
```html
  <script>if(new URLSearchParams(location.search).has('mock')){document.write('<script src="/irrigacao/dev/mock.js"><\/script>');}</script>
  <script src="/irrigacao/shared/api.js"></script>
  <script src="/irrigacao/node/app.js"></script>
```

- [ ] **Step 4: Criar o bootstrap `data/irrigacao/index.html`**

```html
<!DOCTYPE html>
<html lang="pt-BR">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Irrigação</title>
    <link rel="stylesheet" href="/irrigacao/shared/style.css" />
  </head>
  <body>
    <!-- Bootstrap: consulta o papel do device e redireciona para a SPA certa.
         Nenhuma SPA pesada carrega aqui. route.js preserva a query string. -->
    <main class="view"><p class="muted" style="padding:1rem">A carregar…</p></main>
    <script>if(new URLSearchParams(location.search).has('mock')){document.write('<script src="/irrigacao/dev/mock.js"><\/script>');}</script>
    <script src="/irrigacao/shared/route.js"></script>
  </body>
</html>
```

- [ ] **Step 5: Atualizar os `readFileSync` dos testes movidos**

Os testes agora correm a partir de `data/irrigacao/dev/`. Ajustar os caminhos relativos:

Em `data/irrigacao/dev/test_mock.js` linha 2 — `mock.js` fica no mesmo dir (`dev/`), então mantém:
```js
let mockCode = fs.readFileSync('mock.js', 'utf-8');
```
(sem mudança; confirmar que roda de `dev/`.)

Em `data/irrigacao/dev/test_niveis.js` linha 2 e `data/irrigacao/dev/test_radio.js` linha 2 — `app.js` passou a `gateway/app.js`:
```js
const appCode = fs.readFileSync('../gateway/app.js', 'utf-8');
```

Em `data/irrigacao/dev/test_route.js` — `route.js` passou a `shared/route.js`:
```js
const code = fs.readFileSync('../shared/route.js', 'utf-8');
```

- [ ] **Step 6: Staging do LittleFS exclui a pasta `dev/`**

Em `bin/platformio-custom.py`, substituir o bloco `_DEV_ONLY` + `_stage_fs` (linhas ~389-395) por:

```python
    # Staging do FS sem artefatos de dev — exclui a pasta dev/ inteira
    # (mock.js + testes) para não levar peso morto ao LittleFS do device.
    _DEV_DIRS = {"dev"}

    def _stage_fs(source, target, env):
        if os.path.isdir(_fs_stage):
            shutil.rmtree(_fs_stage)
        shutil.copytree(_fs_src, _fs_stage,
                        ignore=lambda d, names: [n for n in names if n in _DEV_DIRS])
```

- [ ] **Step 7: Verificar — testes JS passam**

Run:
```bash
cd data/irrigacao/dev && node test_route.js && node test_mock.js && node test_niveis.js && node test_radio.js
```
Expected: cada um imprime `... pass, 0 fail` / sai 0 (sem `ENOENT`).

- [ ] **Step 8: Verificar — serving estático e routing (sem hardware)**

Run (a partir de `data/`):
```bash
cd data && python -m http.server 8137 --bind 127.0.0.1 &
sleep 1
curl -s -o /dev/null -w "%{http_code} boot\n"    http://127.0.0.1:8137/irrigacao/index.html
curl -s -o /dev/null -w "%{http_code} gw\n"       http://127.0.0.1:8137/irrigacao/gateway/index.html
curl -s -o /dev/null -w "%{http_code} node\n"     http://127.0.0.1:8137/irrigacao/node/index.html
curl -s -o /dev/null -w "%{http_code} css\n"      http://127.0.0.1:8137/irrigacao/shared/style.css
curl -s -o /dev/null -w "%{http_code} gwjs\n"     http://127.0.0.1:8137/irrigacao/gateway/app.js
curl -s -o /dev/null -w "%{http_code} mock\n"     http://127.0.0.1:8137/irrigacao/dev/mock.js
```
Expected: todos `200`.

Browser (manual): `http://127.0.0.1:8137/irrigacao/index.html?mock=1` redireciona para `/irrigacao/node/index.html?mock=1` (role=0 no mock) **com `?mock=1` preservado** e com dados do mock a aparecer; `?mock=1&role=1` redireciona para `/irrigacao/gateway/index.html?mock=1&role=1`; `?mock=1&wizard=1` mostra o wizard.

- [ ] **Step 9: Verificar — imagem LittleFS do firmware exclui dev/**

A exclusão de `dev/` é do build do **firmware** (via `platformio-custom.py`), não do rig
(o rig usa `data_dir` cru e serve `dev/` de propósito, para o mock).
Run: `C:/Users/Jefferson/.platformio/penv/Scripts/pio.exe run -e custom_irrigation -t buildfs`
Expected: o staging em `.pio/build/custom_irrigation/fs_stage/irrigacao/` **não** contém a pasta `dev/`.

- [ ] **Step 10: Commit**

```bash
git add -A data/irrigacao bin/platformio-custom.py
git commit -m "refactor(irrigation): árvore agrupada por papel + bootstrap de entrada

gateway/ node/ shared/ dev/; /irrigacao/index.html vira bootstrap que
carrega shared/route.js; refs absolutas; staging LittleFS exclui dev/."
```

---

### Task 3: Firmware — alvo do portal cativo + trailing-slash

Alinha o `ContentHandler` ao bootstrap: o redirect cativo passa a apontar para `/irrigacao/index.html`, e caminhos de diretório sob `irrigacao` servem `index.html`.

**Files:**
- Modify: `src/mesh/http/ContentHandler.cpp` (bloco ~499-521)

**Interfaces:**
- Consumes: bootstrap `/irrigacao/index.html` (Task 2).

- [ ] **Step 1: Trailing-slash → index.html (sob irrigacao)**

Em `src/mesh/http/ContentHandler.cpp`, logo após o bloco que seta `Cache-Control` para assets de irrigacao (a seguir à linha `res->setHeader("Cache-Control", "no-cache");`), inserir:

```cpp
        // Caminho de diretório sob irrigacao (ex.: "irrigacao/" ou "irrigacao/gateway/")
        // resolve para o index.html daquela pasta — ESP8266/ESP32 não fazem index-de-diretório.
        if (parameter1.rfind("irrigacao", 0) == 0 && !parameter1.empty() && parameter1.back() == '/')
            parameter1 += "index.html";
```

- [ ] **Step 2: Alvo do redirect cativo → bootstrap**

No mesmo ficheiro, no bloco `webServerPortalMode`, trocar as duas construções de `loc`:

De:
```cpp
                std::string loc = "http://" + std::string(WiFi.softAPIP().toString().c_str()) + "/irrigacao/portal/index.html";
```
Para:
```cpp
                std::string loc = "http://" + std::string(WiFi.softAPIP().toString().c_str()) + "/irrigacao/index.html";
```

E de:
```cpp
                std::string loc = "http://192.168.4.1/irrigacao/portal/index.html";
```
Para:
```cpp
                std::string loc = "http://192.168.4.1/irrigacao/index.html";
```

- [ ] **Step 3: Verificar — compila**

Run (env ESP32 de irrigação ativo = `custom_irrigation`):
```bash
C:/Users/Jefferson/.platformio/penv/Scripts/pio.exe run -e custom_irrigation
```
Expected: `[SUCCESS]`. Grep de sanidade: `grep -rn "portal/index.html" src/mesh/http/ContentHandler.cpp` → **zero** resultados.
(Nota Windows: se o link falhar com erro de SCons/penv, ver memória `platformio-scons-penv-broken` — alinhar tool-scons e usar 1 só PIO core.)

- [ ] **Step 4: Commit**

```bash
git add src/mesh/http/ContentHandler.cpp
git commit -m "fix(irrigation): redirect cativo aponta ao bootstrap + trailing-slash serve index.html"
```

---

### Task 4: Manter o rig ESP8266 fora dos commits

**Files:**
- Modify/Create: `.gitignore` (raiz do repo)

- [ ] **Step 1: Ignorar o rig**

Acrescentar ao `.gitignore` da raiz:
```
# Bancada local de teste de UI (ESP8266) — ferramenta de dev, não versionada
/tools/ui-rig-esp8266/
```

- [ ] **Step 2: Verificar**

Run: `git status --porcelain tools/ui-rig-esp8266`
Expected: **sem saída** (ignorado).

- [ ] **Step 3: Commit**

```bash
git add .gitignore
git commit -m "chore: ignora bancada local de UI esp8266 (não versionada)"
```

---

## FASE 2 (opcional) — renomear APIs para casar os papéis

Só depois da Fase 1 gravada e verificada. Um commit por prefixo (bisectável).
Renomeação puramente de prefixo de string; sem contrato externo.

### Task 5: `/api/irrigation` → `/api/gateway`

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp` (89 refs), `src/modules/irrigation/IrrigationWebEndpoints.h` (1), `src/modules/irrigation/IrrigationModule.h` (1)
- Modify: `data/irrigacao/gateway/app.js` (`var API`), e refs literais (3)
- Modify: `data/irrigacao/dev/mock.js` (regex de intercept do gateway)
- Modify: testes que referenciam o path (`test/test_irrigation_webapi/`)

**Interfaces:**
- Produces: superfície HTTP do gateway em `/api/gateway/*`.

- [ ] **Step 1: Trocar o prefixo no C++**

Substituir todas as ocorrências de `"/api/irrigation` por `"/api/gateway` nos ficheiros C++:
```bash
grep -rl "/api/irrigation" src/ | xargs sed -i 's#/api/irrigation#/api/gateway#g'
```

- [ ] **Step 2: Trocar no front-end gateway**

Em `data/irrigacao/gateway/app.js` linha 6:
```js
var API = '/api/gateway'; // var (não const) para ficar visível como window.API ao api.js
```
E atualizar as 3 refs literais restantes (`grep -n "/api/irrigation" data/irrigacao/gateway/app.js`) para `/api/gateway`.

- [ ] **Step 3: Atualizar o mock**

Em `data/irrigacao/dev/mock.js`, a linha que normaliza o path do gateway:
```js
const path = url.replace(/.*\/api\/gateway/, '');
```
(era `\/api\/irrigation`).

- [ ] **Step 4: Atualizar os testes**

Em `test/test_irrigation_webapi/` trocar referências `/api/irrigation` → `/api/gateway` (`grep -rl "/api/irrigation" test/`).

- [ ] **Step 5: Verificar — zero refs antigas + suites**

```bash
grep -rn "/api/irrigation" src/ data/ test/ | grep -v "/.pio/" | grep -v "/.rollback/"   # => vazio
cd data/irrigacao/dev && node test_mock.js   # => pass
./bin/run-tests.sh                            # => GREEN (ou filtrar test_irrigation_webapi)
```
Expected: grep vazio; testes verdes.

- [ ] **Step 6: Commit**

```bash
git add -A src data/irrigacao test
git commit -m "refactor(irrigation): renomeia /api/irrigation -> /api/gateway"
```

---

### Task 6: `/api/portal` → `/api/node` (inclui `/service`)

**Files:**
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp` (30), `ServicePortalEndpoints.cpp` (13), `ServicePortalEndpoints.h` (1), `IrrigationPortalEndpoints.h` (1), `IrrigationModule.h` (1)
- Modify: `src/mesh/http/ContentHandler.cpp` (4)
- Modify: `data/irrigacao/node/app.js` (29), `data/irrigacao/gateway/app.js` (16), `data/irrigacao/shared/route.js` (1, o `GET /api/portal/node`), `data/irrigacao/dev/mock.js` (3)
- Modify: testes (`test/test_portal_api/`, `test/test_service_portal/`)

**Interfaces:**
- Consumes: nada da Task 5 (prefixos independentes).
- Produces: superfície do portal em `/api/node/*` e serviço em `/api/node/service/*`.

- [ ] **Step 1: Trocar o prefixo no C++**

```bash
grep -rl "/api/portal" src/ | xargs sed -i 's#/api/portal#/api/node#g'
```
(`/api/portal/service` vira `/api/node/service` automaticamente — é o mesmo prefixo.)

- [ ] **Step 2: Trocar no front-end**

```bash
sed -i 's#/api/portal#/api/node#g' data/irrigacao/node/app.js data/irrigacao/gateway/app.js data/irrigacao/shared/route.js
```
Confirmar em `route.js` que ficou `fetch("/api/node/node")` — o endpoint de estado do nó é `/node` sob o prefixo, logo o path final é `/api/node/node` (correto; é o rename do prefixo, o handler C++ também mudou).

- [ ] **Step 3: Atualizar o mock**

Em `data/irrigacao/dev/mock.js`, as 3 refs: o teste `clean.includes('/api/node')` e o `clean.replace(/.*\/api\/node/, '')` (eram `/api/portal`).

- [ ] **Step 4: Atualizar os testes**

`grep -rl "/api/portal" test/ | xargs sed -i 's#/api/portal#/api/node#g'`

- [ ] **Step 5: Verificar — zero refs antigas + suites + routing**

```bash
grep -rn "/api/portal" src/ data/ test/ | grep -v "/.pio/" | grep -v "/.rollback/"   # => vazio
cd data/irrigacao/dev && node test_route.js && node test_mock.js   # => pass
./bin/run-tests.sh   # => GREEN
```
Browser (rig/http.server): bootstrap ainda roteia por papel (route.js agora chama `/api/node/node`, mock intercepta).
Expected: grep vazio; testes verdes; routing OK.

- [ ] **Step 6: Commit**

```bash
git add -A src data/irrigacao test
git commit -m "refactor(irrigation): renomeia /api/portal -> /api/node (inclui /service)"
```

---

## Notas de verificação final (manual, não-commit)

- Reflashar o rig ESP8266 (`pio run -e amica -t upload` + `-t uploadfs` em `tools/ui-rig-esp8266`) e medir velocidade/feel: `http://192.168.4.1/` → bootstrap → SPA certa; mock vivo; wizard; troca de papel.
- No device ESP32 real: confirmar que `dev/` não está na imagem (`?mock=1` inerte) e que `/` cativo cai no bootstrap e roteia pelo papel gravado no provisionamento.
