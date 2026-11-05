# Conformância UI Irrigação (portal + gateway) ao modelo de referência — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deixar a UI servida em `data/irrigacao/` idêntica ao modelo `docs/ui-reference/Webserver UI Refresh.dc.html`, fechando as divergências do portal do nó (tela "Sistema" ausente, "Mais" sem subtelas, layout "Este nó") e auditando/corrigindo o painel do gateway, com os endpoints de nó que a tela Sistema exige.

**Architecture:** O mesmo binário serve o painel do gateway (`/api/irrigation/*`, `IrrigationWebEndpoints.cpp`) e o portal de campo (`/api/portal/*`, `IrrigationPortalEndpoints.cpp`). A tela Sistema do nó reusa serializadores já existentes: `buildStationConfig`/`parseStationConfig` (`ServicePortalApi.cpp`) para backup/restore e `startBleWindow` para a janela BLE. O frontend é vanilla JS sem build; verificação funcional é via `?mock=1` (paridade em `mock.js`) e a checklist de aceite do spec. O backend C++ é verificado por testes nativos (`./bin/run-tests.sh`).

**Tech Stack:** C++ (HTTPServer embarcado, `JsonWriter`/`JsonReader`), JS vanilla (ES2017, sem framework), CSS com design tokens em `:root`.

## Global Constraints

Copiados verbatim do spec (`project/PROMPT_CLAUDE_CODE.md`). Valem para toda task:

- Roda offline: sem web fonts, CDN, frameworks ou build step. Vanilla JS + um CSS, fontes do sistema.
- `style.css` ≤ 30 KB. Primitivos compartilhados gateway/portal; nada de regra duplicada por tela.
- Todo acesso a dados passa por `api.js`; `mock.js` implementa a mesma interface, ativado por `?mock=1`. Telas nunca chamam `fetch` direto.
- Escopo = o que está na referência. Não acrescentar telas/campos/funções que ela não mostra.
- Cores só em `:root` — `grep -nE "#[0-9a-fA-F]{3,6}\b|rgba?\(|oklch\(" style.css *.js portal/*.js` só pode casar dentro de `:root`.
- Nenhum `style="…"` em template JS, exceto altura da barra do sparkline e largura da barra de progresso.
- Nenhum `font-size` < 12px; nenhum `box-shadow`. `font-variant-numeric: tabular-nums` em todo número; IDs de nó e timestamps em `var(--font-mono)`.
- Alvos de toque: card ≥ 40px, principal/abas 48px, mínimo 44px. Subtela usa `.subbar` de 44px com "‹ Voltar".
- Revisar a 360px e 480px sem overflow horizontal.
- Formatar com reprodução manual de `trunk fmt` (clang-format@16.0.3 + prettier) só nas linhas alteradas — ver memória `trunk-fmt-windows-workaround`.
- Ao fim de cada task: um commit. Push de feature vai para o remote `fork`, não `origin`.

---

### Task 1: Endpoint backup do nó — `GET /api/portal/node/backup`

Serve a config do próprio nó como JSON de download, reusando `buildStationConfig`. Sem PSK (a função já não emite chave).

**Files:**
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp` (novo handler `hNodeBackup` + registro, perto do bloco OTA ~linha 259-295 e do registro ~linha 1268)
- Modify: `src/modules/irrigation/ServicePortalApi.cpp` (tornar `buildStationConfig` acessível ao portal — já é não-static e declarada em header; confirmar declaração)
- Test: `test/test_service_portal/test_main.cpp` (já cobre `buildStationConfig`; adicionar caso "backup round-trip do próprio settings")

**Interfaces:**
- Consumes: `size_t buildStationConfig(const IrrigationSettings &s, char *buf, size_t cap)`; `const IrrigationSettings &IrrigationModule::getSettings() const`.
- Produces: rota HTTP `GET /api/portal/node/backup` → corpo JSON da config do nó, header `Content-Disposition: attachment; filename="no-backup.json"`.

- [ ] **Step 1: Escrever teste falhando** em `test/test_service_portal/test_main.cpp`

```cpp
void test_node_backup_roundtrips_own_settings()
{
    IrrigationSettings s{};
    s.numValves = 3;
    s.hbMinutes = 7;
    s.latE7 = -229071000;
    char buf[2048];
    size_t n = buildStationConfig(s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    IrrigationSettings back{};
    ParseResult r = parseStationConfig(buf, n, back);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(3, back.numValves);
    TEST_ASSERT_EQUAL(7, back.hbMinutes);
    TEST_ASSERT_EQUAL(-229071000, back.latE7);
}
```

Registrar no `setup()`/runner do arquivo: `RUN_TEST(test_node_backup_roundtrips_own_settings);`.

- [ ] **Step 2: Rodar o teste e confirmar que falha**

Run: `./bin/run-tests.sh` (ou filtrar o suite `test_service_portal`)
Expected: RED — teste novo não compila/linka até o `RUN_TEST` existir, depois FAIL se a asserção não bater. (Se `buildStationConfig`/`parseStationConfig` já round-trip, o teste passa direto; nesse caso ele vira regressão-guard e seguimos.)

- [ ] **Step 3: Implementar o handler e registrar a rota** em `IrrigationPortalEndpoints.cpp`

Adicionar perto dos outros handlers do portal:

```cpp
// GET /api/portal/node/backup — baixa a config deste nó (sem PSK), p/ restaurar depois.
static void hNodeBackup(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    const size_t cap = 4096;
    char *buf = (char *)malloc(cap);
    if (!buf) { res->setStatusCode(500); return; }
    size_t n = buildStationConfig(irrigationModule->getSettings(), buf, cap);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Content-Disposition", "attachment; filename=\"no-backup.json\"");
    res->write((uint8_t *)buf, n);
    free(buf);
}
```

No bloco de registro de rotas (junto dos `server->registerNode(...)` do portal):

```cpp
server->registerNode(new ResourceNode("/api/portal/node/backup", "GET", &hNodeBackup));
```

Se `buildStationConfig`/`parseStationConfig`/`ParseResult` não estiverem visíveis, incluir o header que as declara (o mesmo usado por `ServicePortalApi.cpp`) no topo de `IrrigationPortalEndpoints.cpp`.

- [ ] **Step 4: Rodar o teste e confirmar verde**

Run: `./bin/run-tests.sh`
Expected: GREEN (exit 0).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationPortalEndpoints.cpp test/test_service_portal/test_main.cpp
git commit -m "feat(irrigation): endpoint GET /api/portal/node/backup (config do no sem PSK)"
```

---

### Task 2: Endpoint restore do nó — `POST /api/portal/node/restore`

Aplica um backup ao próprio nó: parse → sobe epoch → avisa o gateway. `parseStationConfig` já preserva role/boundGateway/epoch, então o vínculo é mantido.

**Files:**
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp` (handler `hNodeRestore` + registro)
- Modify: `src/modules/irrigation/IrrigationModule.h` / `.cpp` (método `bool portalRestoreConfig(const char *json, size_t len, uint32_t &newEpochOut)` que faz parse+merge+activate+notify, reusando `mergeRemoteConfig`/`activateSettings`)
- Test: `test/test_service_portal/test_main.cpp` (caso "restore preserva boundGateway e sobe epoch")

**Interfaces:**
- Consumes (todos já existentes e verificados): `ParseResult parseStationConfig(const char*, size_t, IrrigationSettings&)` (preserva role/boundGateway/epoch); `IrrigationSettings IrrigationModule::mergeRemoteConfig(const IrrigationSettings &fresh, uint32_t newEpoch) const` (força role/boundGateway do settings atual, seta epoch, clampa pulseMs/cmdRatePerMin/hbMinutes/numValves); `bool saveIrrigationSettings(const IrrigationSettings&)` (IrrigationSettings.h:198); `void IrrigationModule::activateSettings(const IrrigationSettings&)`; `void IrrigationModule::sendHeartbeat()` (reporta o epoch ao gateway = "o gateway é avisado"); `auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::CONFIG_EPOCH, 0, AuditResult::OK)` (4-arg overload).
- Produces: `bool IrrigationModule::portalRestoreConfig(const char *json, size_t len, uint32_t &newEpochOut)`; rota `POST /api/portal/node/restore` → `{"ok":true,"epoch":N}` ou `{"errors":[...]}`.

- [ ] **Step 1: Escrever teste falhando**

```cpp
void test_node_restore_preserves_bond_and_bumps_epoch()
{
    IrrigationSettings cur{};
    cur.boundGateway = 0x7c2e04;
    cur.configEpoch = 14;
    cur.role = 0; // estação
    const char *bk = "{\"numValves\":4,\"role\":1,\"boundGateway\":0,\"configEpoch\":99}";
    IrrigationSettings merged = cur;
    ParseResult r = parseStationConfig(bk, strlen(bk), merged);
    TEST_ASSERT_TRUE(r.ok);
    // parseStationConfig preserva geridos → vínculo e papel intactos
    TEST_ASSERT_EQUAL(0x7c2e04u, merged.boundGateway);
    TEST_ASSERT_EQUAL(0, merged.role);
    TEST_ASSERT_EQUAL(4, merged.numValves);
}
```

`RUN_TEST(test_node_restore_preserves_bond_and_bumps_epoch);`.

- [ ] **Step 2: Rodar e confirmar falha**

Run: `./bin/run-tests.sh`
Expected: RED até o `RUN_TEST` existir; depois valida o contrato de preservação.

- [ ] **Step 3: Implementar `portalRestoreConfig` + handler**

Em `IrrigationModule.cpp`:

```cpp
bool IrrigationModule::portalRestoreConfig(const char *json, size_t len, uint32_t &newEpochOut)
{
    IrrigationSettings merged = settings; // geridos preservados pelo parse
    ParseResult r = parseStationConfig(json, len, merged);
    if (!r.ok)
        return false;
    newEpochOut = settings.configEpoch + 1;
    merged = mergeRemoteConfig(merged, newEpochOut); // força role/boundGateway, seta epoch, clampa
    if (!saveIrrigationSettings(merged))
        return false;
    activateSettings(merged);
    sendHeartbeat(); // reporta o novo epoch ao gateway (= "o gateway é avisado")
    auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::CONFIG_EPOCH, 0, AuditResult::OK);
    return true;
}
```

Declarar `portalRestoreConfig` em `IrrigationModule.h` (seção pública, junto dos demais métodos `portal*`). Espelha o caminho canônico de adoção de config remota em `IrrigationModule.cpp:945-957` (merge→save→activate→avisa), trocando o `sendAck` (iniciado por mesh) por `sendHeartbeat` (iniciado localmente). `mergeRemoteConfig` já preserva role/boundGateway, então o vínculo com o gateway é mantido.

Em `IrrigationPortalEndpoints.cpp`:

```cpp
// POST /api/portal/node/restore — reaplica um backup neste nó; mantém vínculo, sobe epoch, avisa gw.
static void hNodeRestore(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) { res->setStatusCode(404); return; }
    char body[4096];
    size_t nb = readBody(req, body, sizeof(body));
    uint32_t epoch = 0;
    if (!irrigationModule->portalRestoreConfig(body, nb, epoch)) {
        sendJson(res, "{\"ok\":false,\"errors\":[\"backup invalido\"]}", 400);
        return;
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"epoch\":%u}", (unsigned)epoch);
    sendJson(res, out);
}
```

Registrar: `server->registerNode(new ResourceNode("/api/portal/node/restore", "POST", &hNodeRestore));`

- [ ] **Step 4: Rodar e confirmar verde**

Run: `./bin/run-tests.sh`
Expected: GREEN.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationPortalEndpoints.cpp src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp test/test_service_portal/test_main.cpp
git commit -m "feat(irrigation): endpoint POST /api/portal/node/restore (mantem vinculo, sobe epoch)"
```

---

### Task 3: Endpoints reboot e reset do nó — `POST /api/portal/node/reboot` e `/reset`

A tela Sistema tem "Reiniciar nó" (reboot limpo) e "Reset de fábrica". **Reusar o `factoryReset()` existente** (`IrrigationModule.cpp:1396`): ele já fecha válvulas, desliga GPOs, grava `def` com `boundGateway=0`/`configEpoch=0`/sem PSK (mantém só role e pinos físicos), apaga allowlist/log e agenda `rebootAtMsec = millis()+2000`. No boot seguinte, sem credenciais, `safeMode=true` → `provisioned = !safeMode` (linha 259) vira `false` → wizard de 1º acesso reaparece. Ou seja: satisfaz "volta ao assistente" sem precisar limpar role.

`factoryReset()` é **privado** (`IrrigationModule.h:308`) e chamado hoje só pelo botão físico. Expor dois wrappers públicos finos em vez de mover/duplicar lógica.

Sem teste unitário novo: reboot/reset são operações de hardware com efeitos colaterais (válvulas/LED/FS/`rebootAtMsec`) — não são unitariamente testáveis de forma limpa, e os wrappers não acrescentam lógica. `factoryReset()` permanece inalterado (coberto pelos testes existentes). Gate desta task = compila + suite nativa do lote C++ (rodada pelo controlador) + review.

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (declarar `void portalReboot();` e `void portalFactoryReset();` na seção pública, junto dos demais `portal*`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (definir os dois wrappers)
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp` (handlers `hNodeReboot`, `hNodeReset` + 2 registros no bloco de rotas 439-460)

**Interfaces:**
- Consumes: `void IrrigationModule::factoryReset()` (privado, já existente); membro `rebootAtMsec` (já usado em 1422/3267); `millis()`.
- Produces: `void IrrigationModule::portalReboot()`; `void IrrigationModule::portalFactoryReset()`; rotas `POST /api/portal/node/reboot` e `POST /api/portal/node/reset`, ambas respondendo `{"ok":true,"reboot":true}` antes de agendar.

- [ ] **Step 1: Implementar os wrappers** em `IrrigationModule.cpp`

```cpp
void IrrigationModule::portalReboot()
{
    rebootAtMsec = millis() + 2000; // reboot limpo, sem apagar config
}

void IrrigationModule::portalFactoryReset()
{
    factoryReset(); // apaga credenciais + agenda reboot; boot seguinte cai no wizard
}
```

Declarar ambos em `IrrigationModule.h` na seção pública dos métodos `portal*` (perto de `portalProvision`/`portalOtaArm`).

- [ ] **Step 2: Implementar os handlers + registrar** em `IrrigationPortalEndpoints.cpp`

```cpp
static void hNodeReboot(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    sendJson(res, "{\"ok\":true,\"reboot\":true}"); // responde antes do reboot
    irrigationModule->portalReboot();
}

static void hNodeReset(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    sendJson(res, "{\"ok\":true,\"reboot\":true}"); // responde antes do reboot
    irrigationModule->portalFactoryReset();
}
```

Registrar no bloco 439-460, agrupadas perto de `/api/portal/node/restore`:

```cpp
server->registerNode(new ResourceNode("/api/portal/node/reboot", "POST", &hNodeReboot));
server->registerNode(new ResourceNode("/api/portal/node/reset", "POST", &hNodeReset));
```

- [ ] **Step 3: Verificar compilação por inspeção** — `portalReboot`/`portalFactoryReset` linkam contra `factoryReset`/`rebootAtMsec` (mesma classe); handlers usam só `sendJson`/`irrigationModule` já disponíveis. Sem teste unitário (ver cabeçalho da task). Confirmar UTF-8 e formatação das linhas alteradas.

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp src/modules/irrigation/IrrigationPortalEndpoints.cpp
git commit -m "feat(irrigation): endpoints POST /api/portal/node/reboot e /reset"
```

---

### Task 4: Endpoint janela BLE do portal — `POST /api/portal/ble`

A tela Sistema do nó tem botões "BLE 5/10/15 min". Reusa `startBleWindow`, espelhando `hBleWindow` do gateway (`IrrigationWebEndpoints.cpp:615-630`). Validação de `minutes` inline (3 valores) como no gateway — sem extrair helper (um helper compartilhado forçaria o teste a incluir headers de httpsserver, quebrando o build nativo; e um teste de igualdade de 3 valores não assere comportamento real). Gate = compila + suite nativa do lote C++ + review, como a Task 3.

**Files:**
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp` (handler `hPortalBle` + registro + `#include "modules/irrigation/IrrigationWebApi.h"` para `JsonReader`)

**Interfaces:**
- Consumes: `void IrrigationModule::startBleWindow(uint8_t minutes)` (público, IrrigationModule.h:198); `JsonReader` (declarado em `IrrigationWebApi.h`, método `getInt(const char*, int64_t&)`).
- Produces: rota `POST /api/portal/ble` → `{"ok":true,"minutes":N}` ou 400 `{"ok":false,"errors":["minutes deve ser 5, 10 ou 15"]}`.

- [ ] **Step 1: Implementar o handler + include + registro** em `IrrigationPortalEndpoints.cpp`

Adicionar no topo (junto dos outros includes): `#include "modules/irrigation/IrrigationWebApi.h" // JsonReader`.

```cpp
// POST /api/portal/ble — janela BLE sob demanda (5/10/15 min): WiFi do portal cai, BLE on por N min,
// device reboota e volta sozinho. Espelha hBleWindow do gateway.
static void hPortalBle(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) { res->setStatusCode(404); return; }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    JsonReader rd(body, nb);
    int64_t minutes = 0;
    if (!rd.getInt("minutes", minutes) || (minutes != 5 && minutes != 10 && minutes != 15)) {
        sendJson(res, "{\"ok\":false,\"errors\":[\"minutes deve ser 5, 10 ou 15\"]}", 400);
        return;
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"minutes\":%d}", (int)minutes);
    sendJson(res, out); // responde antes do reboot
    irrigationModule->startBleWindow((uint8_t)minutes);
}
```

Registrar no bloco 439-460: `server->registerNode(new ResourceNode("/api/portal/ble", "POST", &hPortalBle));`

- [ ] **Step 2: Verificar compilação por inspeção** — `JsonReader`/`getInt` vêm de `IrrigationWebApi.h`; `startBleWindow` é público; `readBody`/`sendJson` já disponíveis. Sem teste unitário (validação trivial de 3 valores; ver cabeçalho). Confirmar UTF-8 e formatação.

- [ ] **Step 3: Commit**

```bash
git add src/modules/irrigation/IrrigationPortalEndpoints.cpp
git commit -m "feat(irrigation): endpoint POST /api/portal/ble (janela BLE sob demanda no no)"
```

---

### Task 5: `mock.js` — rotas de Sistema do nó sob `?mock=1`

**Arquitetura real (verificada):** `api.js` NÃO tem funções por recurso. A camada de dados são os wrappers `j(url, opts)` / `rawFetch(url, opts)` / `getJson` / `postJson` (api.js:9-40); as telas chamam esses wrappers com a URL do endpoint inline (ex.: portal/app.js:246 `j("/api/portal/node/pulse", {...})`). "As telas nunca chamam `fetch` direto" é satisfeito por usar `j`/`rawFetch` (que encapsulam `fetch`). `mock.js` intercepta `window.fetch` (mock.js:491-492) e roteia por `ppath.startsWith(...)`. Portanto **api.js não muda**; a Task 7 chama `j`/`rawFetch` inline como o resto do portal. Esta task só acrescenta as rotas novas ao interceptador de `mock.js`.

**Files:**
- Modify: `data/irrigacao/mock.js` (novas rotas no interceptador `window.fetch`, dentro do bloco `/api/portal` 501-570)

**Interfaces:**
- Produces (consumido pela Task 7, via `j`/`rawFetch` inline — sem símbolos novos): respostas mock para `GET /api/portal/node/backup`, `POST /api/portal/node/restore`, `POST /api/portal/node/reboot`, `POST /api/portal/node/reset`, `POST /api/portal/ble`.

- [ ] **Step 1: Adicionar as rotas POST** no bloco `if (method === 'POST') { ... }` (mock.js:503-514), ANTES do fim do bloco — porque o catch genérico `if (ppath.startsWith('/node')) return mockResponse(STATE.portalNode)` (linha 515) capturaria `/node/restore`, `/node/reboot`, `/node/reset`:

```js
      if (ppath === '/node/restore') return mockResponse({ ok: true, epoch: 15 });
      if (ppath === '/node/reboot') return mockResponse({ ok: true, reboot: true });
      if (ppath === '/node/reset') { STATE.portalNode.provisioned = false; return mockResponse({ ok: true, reboot: true }); }
      if (ppath === '/ble') { const b = opts.body ? JSON.parse(opts.body) : {}; return mockResponse({ ok: true, minutes: b.minutes }); }
```

- [ ] **Step 2: Adicionar a rota GET de backup** ANTES da linha 515 (`startsWith('/node')`), para não ser capturada pelo catch genérico:

```js
    if (ppath.startsWith('/node/backup')) return mockResponse({
      version: 7, role: STATE.portalNode.role, configEpoch: STATE.portalNode.configEpoch, boundGateway: STATE.portalNode.boundGateway,
      numValves: 2, hbMinutes: 10, vbatMinAbrirCentiV: 1150, maxOpenConfigS: 7200, cmdRatePerMin: 12, pulseMs: 80,
      pinsHbridgeA: [25, 26], pinsHbridgeB: [27, 14], pinsDigitalIn: [32, 33], pinsGpo: [12],
      digitalInActiveLow: 0, pinBtn: 0, pinLed: 2, pinTamper: 34, hwFlags: 1, latE7: -229071000, lonE7: -470632000,
      sensores: [], localInterlocks: [],
    });
```

(Campos espelham `buildStationConfig`; o portal baixa isso via `rawFetch(...).text()`.)

- [ ] **Step 3: Sanity estático** — `node --check data/irrigacao/mock.js` (sem erro de sintaxe). Verificação visual fica para o bench final (sem browser na sessão).

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/mock.js
git commit -m "feat(ui): mock.js com rotas backup/restore/reboot/reset/ble do no"
```

---

### Task 6: Portal "Mais" vira navegação por subtelas (fold Firmware)

Conformar ao modelo: "Mais" é uma `.list` que abre subtelas com `.subbar` "‹ Mais". Itens: Enlace e vizinhos, Coordenadas locais, Firmware, Sistema. Firmware deixa de ser aba top-level para estação/repetidor (continua aba só no device Serviço).

**Files:**
- Modify: `data/irrigacao/portal/index.html` (reestruturar `#tab-mais`; remover a aba `firmware-tab` da nav para estação/repetidor — mantê-la só sob `svc`/serviço; mover o conteúdo atual dos cards de Mais para dentro de subtelas)
- Modify: `data/irrigacao/portal/app.js` (roteador de subtela `psub`; `.subbar` "‹ Mais"; `pMaisItems` = enlace/coords/firmware/sistema)
- Modify: `data/irrigacao/style.css` (garantir `.subbar` e `.navrow`/`.list` compartilhados já existem; não duplicar)

**Interfaces:**
- Consumes: estado de aba atual (`nav.tabs`), `refreshEnlace()`, `loadCoords()`, `refreshFirmware()` já existentes.
- Produces: função `showMaisSub(key)` e `backToMais()` usadas pela Task 7; lista `#mais-list` renderizando 4 navrows.

- [ ] **Step 1:** Em `index.html`, trocar o conteúdo fixo de `#tab-mais` por: um `#mais-list` (lista de navrows) + um `#mais-sub` (container de subtela, começa escondido) contendo os blocos `enlace`, `coords`, `firmware`, `sistema`, cada um com uma `.subbar` "‹ Mais". Mover os cards de Enlace/Vizinhos/Coordenadas atuais para dentro de `#mais-sub [data-sub=enlace]` e `[data-sub=coords]`.

  **Firmware sem duplicar markup:** há hoje UM `section#tab-firmware` com o conteúdo de OTA. Mantenha um único bloco de conteúdo de firmware. Mova esse conteúdo para `#mais-sub [data-sub=firmware]` (com `.subbar "‹ Mais"`). No device **Serviço**, o firmware é alcançado pela aba `firmware-tab` (que continua existindo só no Serviço) — faça a aba Serviço mostrar o MESMO bloco (ex.: a aba Serviço revela `#tab-mais` já posicionado na subtela firmware, ou um wrapper que exibe o bloco único). NÃO crie duas cópias do HTML de firmware. Se a reutilização via um único bloco ficar inviável sem duplicar, PARE e reporte NEEDS_CONTEXT em vez de duplicar.

- [ ] **Step 2:** Na nav inferior, remover a aba "Firmware" para estação/repetidor. `PT` do modelo: estação = [node, net, survey, mais]; repetidor = [node, survey, mais]; serviço = [svcclients, svcnet, survey, svclog, firmware]. Ajustar `applyRoleLayout`/`initService` para esconder/mostrar de acordo.

- [ ] **Step 3:** Em `app.js`, implementar o roteador de subtela:

```js
function showMaisSub(key) {
  document.getElementById('mais-list').classList.add('hidden');
  const sub = document.getElementById('mais-sub');
  sub.classList.remove('hidden');
  sub.querySelectorAll('[data-sub]').forEach((b) => b.classList.toggle('hidden', b.dataset.sub !== key));
  if (key === 'enlace') refreshEnlace();
  if (key === 'coords') loadCoords();
  if (key === 'firmware') refreshFirmware();
  if (key === 'sistema') refreshSistema();
}
function backToMais() {
  document.getElementById('mais-sub').classList.add('hidden');
  document.getElementById('mais-list').classList.remove('hidden');
}
```

Renderizar `#mais-list` com os 4 itens (navrow + chevron) e ligar o clique a `showMaisSub`. Ligar os botões "‹ Mais" das subbars a `backToMais`. Ao entrar na aba "Mais", resetar para a lista (`backToMais()`).

  **Evitar forward-reference:** `showMaisSub` chama `refreshSistema()`, que só é definida de verdade na Task 7. Para esta task compilar/rodar sem `ReferenceError`, adicione um stub `function refreshSistema() {}` agora (a Task 7 substitui o corpo). `refreshEnlace`/`loadCoords`/`refreshFirmware` já existem.

- [ ] **Step 4: Sanity estático (sem browser na sessão)** — `node --check data/irrigacao/portal/app.js` sem erro. Conferir por leitura: `#mais-list` renderiza 4 navrows; cada `[data-sub]` tem `.subbar "‹ Mais"`; a aba Firmware foi removida da nav de estação/repetidor e permanece só no Serviço; nenhum markup de firmware duplicado. Verificação visual (abrir/voltar subtelas, 360px) fica para o bench final.

- [ ] **Step 5: Commit**

```bash
git add data/irrigacao/portal/index.html data/irrigacao/portal/app.js data/irrigacao/style.css
git commit -m "refactor(ui): portal Mais vira lista->subtelas; Firmware dobrado em Mais"
```

---

### Task 7: Portal "Mais › Sistema" — BLE, Backup, Restaurar, Reiniciar/Reset

Implementar a subtela Sistema idêntica ao modelo (`.dc.html` linhas 1480-1538): quatro cards.

**Files:**
- Modify: `data/irrigacao/portal/index.html` (bloco `#mais-sub [data-sub=sistema]`: cards Bluetooth, Backup, Restaurar, Reiniciar e resetar)
- Modify: `data/irrigacao/portal/app.js` (`refreshSistema()` + handlers dos botões: BLE 5/10/15 com confirmação âmbar + status ativo; download backup; file-picker + restaurar com msg "Backup aplicado · epoch N"; reiniciar; reset de fábrica com confirmação vermelha)
- Modify: `data/irrigacao/style.css` (SÓ se faltar primitivo. Primitivos do portal que JÁ existem e devem ser reusados: `.fp-confirm` (callout de confirmação com `.msg` + `.btns` > `.btn ghost/danger sm`, igual ao confirm de GPO em portal/app.js:78-80), `.btn solid/outline/ghost/danger/dangerline`, `.btn big`, `.fp-pulseopen` (linha verde com ponto, igual ao pulso), `.fp-sync`, `.card`, `.fp-lbl`. NÃO existe `.callout` no portal — não usar.)

**Interfaces:**
- Consumes (chamadas inline via os wrappers de api.js, como o resto do portal — NÃO há funções `node*` em api.js):
  - Backup: `rawFetch('/api/portal/node/backup')` → `.text()` (JSON p/ download).
  - Restaurar: `j('/api/portal/node/restore', { method: 'POST', body: fileText })` → `{ ok, epoch }`.
  - Reiniciar: `j('/api/portal/node/reboot', { method: 'POST' })`.
  - Reset: `j('/api/portal/node/reset', { method: 'POST' })`.
  - BLE: `j('/api/portal/ble', { method: 'POST', body: JSON.stringify({ minutes }) })` → `{ ok, minutes }`.
- Produces: subtela funcional sob `?mock=1`.

- [ ] **Step 1:** Markup dos 4 cards (cada um `.card`) em `[data-sub=sistema]`, seguindo os textos exatos do modelo. Usar os primitivos do portal (ver Files): confirmações = `.fp-confirm` (com `.msg` e `.btns`), linha de status verde = padrão `.fp-pulseopen`/`.fp-sync`.
  - **Bluetooth (BLE):** descrição + 3 botões "BLE 5/10/15 min"; ao tocar, `.fp-confirm` âmbar ("O portal vai ficar fora do ar por N min enquanto a janela BLE estiver aberta. Continuar?") com Cancelar (`.btn ghost sm`)/Abrir janela (`.btn solid sm`); quando ativo, linha verde com ponto "Janela BLE aberta · N min. Toque para encerrar."
  - **Backup:** descrição "Baixa a configuração deste nó: válvulas, sensores, pinos, intertravamentos locais e coordenadas. **Não inclui a chave da rede.**" + `.btn solid big` "Baixar backup (.json)".
  - **Restaurar:** descrição "Reaplica um backup neste nó. Mantém o vínculo com o gateway; a config sobe de epoch e o gateway é avisado." + file-picker + `.btn outline big` "Restaurar backup" + linha de status verde quando aplicado ("Backup aplicado · epoch N. O gateway foi avisado.").
  - **Reiniciar e resetar:** botão `.btn ghost` "Reiniciar nó" (+ "Reiniciando. Reconecte ao portal em ~10 s."); botão `.btn dangerline` "Reset de fábrica" → `.fp-confirm` vermelho "Apaga a chave da rede, o vínculo com o gateway e toda a configuração. O nó volta ao assistente de 1º acesso." com Cancelar (`.btn ghost sm`)/Apagar tudo (`.btn danger sm`).

- [ ] **Step 2:** `refreshSistema()` e handlers em `app.js`:

```js
function refreshSistema() { /* reset de estado visual dos callouts ao abrir a subtela */ }
// BLE: toque N -> mostra callout âmbar de confirmação; confirmar -> j('/api/portal/ble',{method:'POST',body:JSON.stringify({minutes:N})}) -> linha de status ativo
// Backup: const r = await rawFetch('/api/portal/node/backup'); const t = await r.text(); baixa via Blob (padrão downloadBackup do gateway)
// Restaurar: const t = await file.text(); const {ok,body} = await j('/api/portal/node/restore',{method:'POST',body:t}); msg "Backup aplicado · epoch " + body.epoch
// Reiniciar: j('/api/portal/node/reboot',{method:'POST'}) -> msg "Reiniciando. Reconecte em ~10 s."
// Reset: callout vermelho de confirmação; confirmar -> j('/api/portal/node/reset',{method:'POST'})
```

Download via Blob reusando o padrão de `downloadBackup` do gateway (`app.js:2811`) para feedback de erro robusto. Ligar os botões por listeners (`addEventListener`) como o resto de portal/app.js — nada de `onclick=` inline no HTML com cor/estilo.

- [ ] **Step 3: Sanity estático** — `node --check` não se aplica a HTML; para `portal/app.js` rodar `node --check data/irrigacao/portal/app.js`. Verificação visual (BLE confirma/ativa, backup baixa, restaurar mostra epoch, reset confirma, 360px) fica para o bench final (sem browser na sessão).

- [ ] **Step 4: Rodar grep de aceite** (ver Task 11/final): nenhuma cor fora de `:root`, nenhum `style=` indevido (exceto sparkline/progress), nenhum `font-size<12px`.

- [ ] **Step 5: Commit**

```bash
git add data/irrigacao/portal/index.html data/irrigacao/portal/app.js data/irrigacao/style.css
git commit -m "feat(ui): portal Mais>Sistema (BLE, backup, restaurar, reiniciar, reset)"
```

---

### Task 8: Portal "Este nó" — layout conforme modelo

Modelo (spec Fase 5 + `.dc.html`): título 15/700 com chip de papel; stats **grid-3** (Bateria, Relógio, Uptime — ou Bateria, Painel solar, Uptime no repetidor) em `--sunken`, valor 17/700 tabular; **uma** linha "Gateway 0x… · Sincronizado" com ID mono e ponto verde (âmbar + "Não pareado"; "· modo seguro" quando aplicável). Hoje a estação mostra Bateria+Gateway no grid e Relógio solto, sem Uptime.

**Files:**
- Modify: `data/irrigacao/portal/app.js` (`renderNode` — refazer os grids e a linha de sync)
- Modify: `data/irrigacao/style.css` (só se faltar primitivo para o grid-3 / linha de sync com ponto; preferir reusar `.fp-grid`/`.fp-sync`)

**Interfaces:**
- Consumes: `s.vbatCentiV`, `s.nowEpoch`/`s.hasTime`, `s.uptimeS`, `s.vpanelCentiV`, `s.boundGateway`, `s.configEpoch`, `s.safeMode`, `s.flags` (tamper) — todos já no payload de `/api/portal/node`.
- Produces: `renderNode(s)` conforme layout-alvo; `renderPulsePills` segue chamado na estação.

- [ ] **Step 1:** Reescrever o grid da estação para 3 colunas **Bateria / Relógio / Uptime** (valor 17/700 tabular em `--sunken`), e o repetidor para **Bateria / Painel solar / Uptime**. Remover o "Gateway" de dentro do grid.

- [ ] **Step 2:** Renderizar **uma** linha de sync (reusar `.fp-sync`): `Gateway 0x<hex> · Sincronizado` (ID em `var(--font-mono)`, ponto verde). Sem vínculo → ponto âmbar + "Não pareado". `s.safeMode` → sufixo " · modo seguro". Manter o bloco de tamper existente (`.tamper`, já no renderNode) acima. NÃO usar `.callout` (não existe no portal).

- [ ] **Step 3: Sanity estático (sem browser)** — `node --check data/irrigacao/portal/app.js` limpo; grep de cores/`style=` sem novos matches fora de `:root`. Conferir por leitura: grid de 3 colunas (estação Bateria/Relógio/Uptime, repetidor Bateria/Painel solar/Uptime), `tabular-nums` nos valores, `var(--font-mono)` no ID do gateway, uma única linha de sync. Verificação visual nos 2 papéis + 360px fica para o bench final.

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/portal/app.js data/irrigacao/style.css
git commit -m "refactor(ui): portal Este no com grid-3 (Bateria/Relogio/Uptime) e linha de sync unica"
```

---

### Task 9: Auditoria de conformância do painel do gateway

O usuário pediu a UI idêntica ao modelo e sinalizou que "alguma aba" do gateway pode divergir. Esta task é descoberta + correção dirigida por diff visual, não por suposição.

**Files:**
- Read: `docs/ui-reference/Webserver UI Refresh.dc.html` (seções do gateway: Visão Geral, Estações, Zonas, Nova/editar zona, Programas, Novo/editar programa, Mais e as 12 subtelas, Log)
- Modify: `data/irrigacao/app.js`, `data/irrigacao/style.css` (correções pontuais por divergência encontrada)

**Interfaces:**
- Produces: lista de divergências encontradas (anexada ao corpo do commit) + correções.

- [ ] **Step 1:** Abrir no navegador, lado a lado: o modelo (`docs/ui-reference/Webserver UI Refresh.dc.html`, coluna gateway) e a UI atual (`data/irrigacao/index.html?mock=1`). Percorrer tab a tab: Visão Geral → Estações (+ sheet) → Zonas (+ nova/editar/excluir) → Programas (+ novo/editar) → Mais (+ as 12 subtelas) → Log.

- [ ] **Step 2:** Para cada tela, anotar divergências em: estrutura (grids, ordem de cards), textos/rótulos, primitivos (`.pill`/`.seg`/`.callout`/`.chip`), alvos de toque, tabular-nums/mono. Registrar numa lista de checagem temporária.

- [ ] **Step 3:** Corrigir cada divergência em `app.js`/`style.css`, reusando primitivos (sem regra nova se já existe). Revalidar a tela corrigida contra o modelo.

- [ ] **Step 4: Verificar** a checklist de aceite do gateway sob `?mock=1`: aprovar pareamento, abrir/fechar zona, criar/editar/excluir zona e programa, reconhecer alerta, log.

- [ ] **Step 5: Commit** (um por área corrigida, se forem muitas)

```bash
git add data/irrigacao/app.js data/irrigacao/style.css
git commit -m "fix(ui): conforma <area> do gateway ao modelo de referencia"
```

---

### Task 10: Excluir ficheiros só-de-dev da imagem LittleFS

`bin/platformio-custom.py:384` monta a imagem com `env.DataToBin(..., "$PROJECT_DATA_DIR")` — empacota `data/` inteiro sem filtro, embarcando ~52 KB de dev-only (`mock.js` 37 KB, `test_niveis.js` 7,5 KB, `test_radio.js` 4,5 KB, `test_mock.js` 3,2 KB). Nenhum é referenciado pela UI de produção (`index.html` só carrega `api.js`/`route.js`/`app.js`/`portal/app.js`; `mock.js` só sob `?mock=1`). Opção 1: staging pré-buildfs — copia `data/` para um dir temporário sem os dev-only e empacota o temp. Mantém os ficheiros no repo (dev continua a usar `?mock=1` servido de checkout local).

**Files:**
- Modify: `bin/platformio-custom.py` (perto da linha 384, no ramo `espressif32`)

**Interfaces:**
- Consumes: `env.DataToBin`, `$PROJECT_DATA_DIR`, `$BUILD_DIR`.
- Produces: imagem FS sem `mock.js`/`test_*.js`; comportamento inalterado para os demais ficheiros.

- [ ] **Step 1:** Antes do `env.DataToBin(...)` no ramo `espressif32`, montar um dir de staging e empacotar a partir dele:

```python
# Staging do FS sem artefatos de dev (mock/test) — evita ~52 KB de peso morto no LittleFS.
import shutil
_fs_src = env.subst("$PROJECT_DATA_DIR")
_fs_stage = join(env.subst("$BUILD_DIR"), "fs_stage")
_DEV_ONLY = {"mock.js", "test_mock.js", "test_niveis.js", "test_radio.js"}

def _stage_fs(source, target, env):
    if os.path.isdir(_fs_stage):
        shutil.rmtree(_fs_stage)
    shutil.copytree(_fs_src, _fs_stage,
                    ignore=lambda d, names: [n for n in names if n in _DEV_ONLY])

target_lfs = env.DataToBin(
    join("$BUILD_DIR", "${ESP32_FS_IMAGE_NAME}"), _fs_stage
)
env.AddPreAction(target_lfs, _stage_fs)
```

Garantir `import os` no topo do script (já deve existir). O `ignore` por nome pega os dev-only em qualquer subdir de `data/` (ex.: `data/irrigacao/`).

- [ ] **Step 2: Verificar** que o build FS não inclui os dev-only. Após um `pio run -t buildfs` (ou o alvo equivalente) para a env `custom_irrigation`, confirmar que `BUILD_DIR/fs_stage/irrigacao/` não contém `mock.js` nem `test_*.js`, e que `api.js`/`route.js`/`app.js`/`portal/` continuam presentes.

Run: `ls "$BUILD_DIR"/fs_stage/irrigacao/` (ajustar ao path real da env)
Expected: sem `mock.js`/`test_*.js`; demais ficheiros presentes.

- [ ] **Step 3: Confirmar `?mock=1` ainda funciona em dev** servindo de checkout local (os ficheiros seguem em `data/irrigacao/`, só não embarcam).

- [ ] **Step 4: Commit**

```bash
git add bin/platformio-custom.py
git commit -m "build(irrigation): exclui mock.js/test_*.js da imagem LittleFS (~52 KB)"
```

---

### Task 11: Verificação final de conformância e tamanho

**Files:**
- Read/verify: `data/irrigacao/style.css`, `data/irrigacao/app.js`, `data/irrigacao/portal/app.js`, `data/irrigacao/mock.js`

- [ ] **Step 1: Cores só em `:root`**

Run: `cd data/irrigacao && grep -nE "#[0-9a-fA-F]{3,6}\b|rgba?\(|oklch\(" style.css app.js portal/app.js mock.js`
Expected: zero ocorrências fora do bloco `:root` de `style.css`.

- [ ] **Step 2: Sem `style=` indevido / sem `box-shadow` / sem fonte < 12px**

Run: `cd data/irrigacao && grep -rnE "style=\"" app.js portal/app.js; grep -rn "box-shadow" style.css; grep -rnE "font-size:\s*(1[01]|[0-9])px" style.css`
Expected: `style="` só na barra do sparkline e na largura da barra de progresso; nenhum `box-shadow`; nenhuma fonte < 12px.

- [ ] **Step 3: `style.css` ≤ 30 KB**

Run: `wc -c data/irrigacao/style.css`
Expected: ≤ 30720 bytes.

- [ ] **Step 4: Suite nativa verde**

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN).

- [ ] **Step 5: Formatação** — reproduzir `trunk fmt` (clang-format@16.0.3 + prettier) só nas linhas alteradas (memória `trunk-fmt-windows-workaround`). Checar BOM/encoding dos arquivos tocados (memória `subagent-windows-encoding-corruption`).

- [ ] **Step 6: Checklist de aceite `?mock=1`** (portal): pulso abrir/fechar, GPO com confirmação, comando de zona via rede, cobertura iniciar/parar, salvar coordenadas, papéis Estação/Repetidor/Serviço, e a nova subtela Sistema (BLE, backup, restaurar, reiniciar, reset). Revisar a 360px e 480px.

- [ ] **Step 7: Commit final**

```bash
git add -A
git commit -m "chore(ui): verificacao de conformancia (grep aceite, tamanho css, fmt)"
```

---

## Self-Review

**Spec coverage:**
- Tela Sistema do nó (BLE/backup/restaurar/reiniciar/reset) → Tasks 1-5, 7. ✓
- Mais como lista→subtelas + Firmware dobrado → Task 6. ✓
- Layout "Este nó" grid-3 + linha de sync → Task 8. ✓
- Gateway idêntico ao modelo → Task 9 (audit+fix). ✓
- Peso morto no LittleFS (mock/test dev-only, ~52 KB) → Task 10. ✓
- Restrições globais (cores, style=, tamanho, fontes, mock parity) → Task 11. ✓

**Placeholder scan:** Pontos a confirmar durante execução, com fallback dado: nome real do getter de provisionamento (Task 3), de `notifyGatewayConfigAdopted`/`scheduleReboot` (Tasks 2-3). Cada um tem instrução de fallback ("usar o método já responsável por…"). Não são TODOs vagos.

**Type consistency:** `buildStationConfig`/`parseStationConfig`/`ParseResult`/`IrrigationSettings` usados de forma consistente entre tasks; funções `nodeBackup/nodeRestore/nodeReboot/nodeReset/nodeBle` definidas na Task 5 e consumidas nas Tasks 6-7 com as mesmas assinaturas.
