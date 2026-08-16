# OTA local por contato (BLE-via-app) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** habilitar OTA local por contato (loader BLE stock do Meshtastic) no portal do nó, com gate de segurança que impede atualizar durante ciclo de irrigação ativo.

**Architecture:** reusa o loader OTA stock (`MeshtasticOTA` / partição `flashApp`). O portal **arma** o modo (não carrega bytes); a transferência é via app Meshtastic por BLE. Predicado de gate puro (native-tested); glue + endpoints + frontend são ESP32/CI-only, espelhando `AdminModule.cpp:365-407` e os handlers existentes de `IrrigationPortalEndpoints.cpp`.

**Tech Stack:** C++ (ESP32/Arduino), esp_ota_ops (`MeshtasticOTA`), httpsserver (webserver embarcado), HTML/JS estático em `data/irrigacao/portal/`, Unity (suite nativa em Docker).

**Spec:** `docs/superpowers/specs/2026-08-16-irrigacao-ota-ble-local-design.md`

## Global Constraints

- **Aditivo total:** protocolo VERSION=1 intacto; settings ABI **sem bump** (v8 208 B). Único delta: 1 valor **append-only** em `AuditAction` (`OTA_ARM`).
- **Sem tipo de wire novo** (OTA é local/BLE, não passa pelo protocolo LoRa).
- **Native-testável só a lógica pura** (predicado do gate). Endpoints/glue/frontend = webserver-guarded (`#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER`), **excluídos do nativo** — só compilam no build ESP32 (CI).
- **Suite nativa completa deve ficar GREEN** ao final (Docker; baseline atual + casos novos). Sem suite nova (casos entram em `test_portal_api`).
- Rodar Docker nativo: ver [[native-test-docker-cp-workaround]] / [[windows-native-test-docker]].
- `trunk fmt` não roda no host Windows; seguir clang-format do repo.
- Push ao fork `jeffersonpimenta/firmware` só no closeout, após OK do usuário.

## File Structure

| Arquivo | Papel | Compila no nativo? |
|---|---|---|
| `src/modules/irrigation/PortalApi.h` (+`.cpp`) | pure: `otaArmAllowed()`, `parseOtaArm()`, `buildOtaStatus()`, ctx | **sim** (native-tested) |
| `test/test_portal_api/test_portal_api.cpp` | casos Unity novos | sim |
| `src/modules/irrigation/AuditLog.h` | `AuditAction::OTA_ARM` append-only | sim |
| `src/modules/irrigation/IrrigationModule.h` (+`.cpp`) | glue: `otaCycleActive()`, `portalOtaArm()`, `portalFillOtaStatus()` | glue sob `#if ARCH_ESP32` (não no nativo) |
| `src/modules/irrigation/IrrigationPortalEndpoints.cpp` | `hOtaStatus`/`hOtaArm` + registro | **não** (webserver-guarded) |
| `data/irrigacao/portal/index.html` | nav + seção aba Firmware | n/a |
| `data/irrigacao/portal/app.js` | `refreshFirmware`/arm | n/a |
| `docs/irrigacao/ota-ble.md` | runbook provisionamento + campo | n/a |

---

### Task 1: Predicado de gate + parse + status (puro, native-tested)

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h`
- Modify: `src/modules/irrigation/PortalApi.cpp`
- Test: `test/test_portal_api/test_portal_api.cpp`

**Interfaces:**
- Produces:
  - `bool IrrigationWeb::otaArmAllowed(bool anyValveOpen, bool anyGpoOn, bool groupActive)` — `true` sse nenhum ativo.
  - `struct IrrigationWeb::OtaArmReq { bool hasHash; uint8_t hash[32]; }`
  - `IrrigationWeb::ParseResult IrrigationWeb::parseOtaArm(const char *json, size_t len, OtaArmReq &out)` — aceita opcional `"hash"` (64 hex chars → 32 bytes); ausente ⇒ `hasHash=false`, `hash` zerado; hex inválido/len≠64 ⇒ erro de parse.
  - `struct IrrigationWeb::OtaStatusCtx { const char *fwVersion; bool loaderPresent; bool loaderBle; bool cycleActive; }`
  - `size_t IrrigationWeb::buildOtaStatus(const OtaStatusCtx &c, char *buf, size_t cap)` — JSON `{"fwVersion":"...","loaderPresent":bool,"loaderBle":bool,"cycleActive":bool,"canArm":bool}` onde `canArm = loaderPresent && loaderBle && !cycleActive`.

- [ ] **Step 1: Escrever os testes que falham** (adicionar em `test/test_portal_api/test_portal_api.cpp`, antes do runner)

```cpp
static void test_otaArmAllowed_gate()
{
    TEST_ASSERT_TRUE(otaArmAllowed(false, false, false)); // livre
    TEST_ASSERT_FALSE(otaArmAllowed(true, false, false)); // valvula aberta
    TEST_ASSERT_FALSE(otaArmAllowed(false, true, false)); // gpo ligado
    TEST_ASSERT_FALSE(otaArmAllowed(false, false, true)); // grupo/scheduler ativo
}

static void test_parseOtaArm_optional_hash()
{
    OtaArmReq a = {};
    const char *no = "{}";
    ParseResult pr = parseOtaArm(no, strlen(no), a);
    TEST_ASSERT_TRUE(pr.ok);
    TEST_ASSERT_FALSE(a.hasHash);

    OtaArmReq b = {};
    const char *yes = "{\"hash\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}";
    ParseResult pr2 = parseOtaArm(yes, strlen(yes), b);
    TEST_ASSERT_TRUE(pr2.ok);
    TEST_ASSERT_TRUE(b.hasHash);
    TEST_ASSERT_EQUAL_UINT8(0x00, b.hash[0]);
    TEST_ASSERT_EQUAL_UINT8(0x11, b.hash[1]);
    TEST_ASSERT_EQUAL_UINT8(0xff, b.hash[31]);

    OtaArmReq c = {};
    const char *bad = "{\"hash\":\"xyz\"}";
    ParseResult pr3 = parseOtaArm(bad, strlen(bad), c);
    TEST_ASSERT_FALSE(pr3.ok);
}

static void test_buildOtaStatus_json()
{
    OtaStatusCtx c = {};
    c.fwVersion = "2.5.0.irrig";
    c.loaderPresent = true;
    c.loaderBle = true;
    c.cycleActive = false;
    char buf[256];
    size_t n = buildOtaStatus(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"fwVersion\":\"2.5.0.irrig\""));
    TEST_ASSERT_TRUE(contains(buf, "\"loaderPresent\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"loaderBle\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"cycleActive\":false"));
    TEST_ASSERT_TRUE(contains(buf, "\"canArm\":true"));

    OtaStatusCtx c2 = c;
    c2.cycleActive = true;
    char buf2[256];
    buildOtaStatus(c2, buf2, sizeof(buf2));
    TEST_ASSERT_TRUE(contains(buf2, "\"canArm\":false"));
}
```

Registrar no runner (dentro do `setup()`/`main` existente ao final do arquivo, junto dos outros `RUN_TEST`):

```cpp
    RUN_TEST(test_otaArmAllowed_gate);
    RUN_TEST(test_parseOtaArm_optional_hash);
    RUN_TEST(test_buildOtaStatus_json);
```

- [ ] **Step 2: Declarar as interfaces em `PortalApi.h`** (junto às outras structs/protótipos do namespace `IrrigationWeb`)

```cpp
// --- OTA local por contato (BLE-via-app) ---
bool otaArmAllowed(bool anyValveOpen, bool anyGpoOn, bool groupActive);

struct OtaArmReq {
    bool hasHash = false;
    uint8_t hash[32] = {0};
};
ParseResult parseOtaArm(const char *json, size_t len, OtaArmReq &out);

struct OtaStatusCtx {
    const char *fwVersion = "";
    bool loaderPresent = false;
    bool loaderBle = false;
    bool cycleActive = false;
};
size_t buildOtaStatus(const OtaStatusCtx &c, char *buf, size_t cap);
```

- [ ] **Step 3: Rodar os testes e ver falhar (link/símbolo ausente)**

Docker native (suite única, mais rápido). Comando de referência (ver memórias de Docker):
```bash
./bin/run-tests.sh test_portal_api
```
Esperado: FAIL (símbolos `otaArmAllowed`/`parseOtaArm`/`buildOtaStatus` não definidos).

- [ ] **Step 4: Implementar em `PortalApi.cpp`**

```cpp
bool otaArmAllowed(bool anyValveOpen, bool anyGpoOn, bool groupActive)
{
    return !(anyValveOpen || anyGpoOn || groupActive);
}

// hex 1 char -> 0..15, ou -1
static int hexNib(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

ParseResult parseOtaArm(const char *json, size_t len, OtaArmReq &out)
{
    out = OtaArmReq{};
    JsonReader r(json, len);
    char hex[65] = {0};
    if (r.getString("hash", hex, sizeof(hex))) {
        if (strlen(hex) != 64)
            return ParseResult::fail("hash deve ter 64 hex chars");
        for (int i = 0; i < 32; i++) {
            int hi = hexNib(hex[i * 2]);
            int lo = hexNib(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0)
                return ParseResult::fail("hash hex invalido");
            out.hash[i] = (uint8_t)((hi << 4) | lo);
        }
        out.hasHash = true;
    }
    return ParseResult::success();
}

size_t buildOtaStatus(const OtaStatusCtx &c, char *buf, size_t cap)
{
    bool canArm = c.loaderPresent && c.loaderBle && !c.cycleActive;
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyString("fwVersion", c.fwVersion);
    w.keyBool("loaderPresent", c.loaderPresent);
    w.keyBool("loaderBle", c.loaderBle);
    w.keyBool("cycleActive", c.cycleActive);
    w.keyBool("canArm", canArm);
    w.endObject();
    return w.finish();
}
```

> **Nota ao implementer:** confirme a API real de `JsonReader`/`JsonWriter`/`ParseResult` em `PortalApi.cpp` e nas structs vizinhas (ex. `getString`, `keyString`, `keyBool`, `beginObject`, `success()`/`fail()`) e ajuste as chamadas ao helper existente do arquivo — **não** invente um novo writer. Se o writer usado no arquivo for outro (ex. `snprintf` manual como em `buildNodeState`), replique esse mesmo estilo em vez do `JsonWriter` acima.

- [ ] **Step 5: Rodar os testes e ver passar**

```bash
./bin/run-tests.sh test_portal_api
```
Esperado: PASS (todos, incluindo os 3 novos).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp test/test_portal_api/test_portal_api.cpp
git commit -m "feat(irrigation): predicado puro do gate de OTA + parse/status (native-tested)"
```

---

### Task 2: `AuditAction::OTA_ARM` (append-only)

**Files:**
- Modify: `src/modules/irrigation/AuditLog.h:17-24`

**Interfaces:**
- Produces: `AuditAction::OTA_ARM` (próximo valor após `CMD_SUPRIMIDO`).

- [ ] **Step 1: Adicionar o valor no fim do enum** (append-only — nunca reordenar)

```cpp
enum class AuditAction : uint8_t {
    ABRIR = 0, FECHAR, PULSO, GPO_ON, GPO_OFF, PAREAR, FACTORY_RESET,
    CONFIG_EPOCH, SAFE_MODE_IN, SAFE_MODE_OUT, TAMPER, REBOOT,
    HIBERNA_IN, HIBERNA_OUT,
    CMD_REJEITADO,
    ESPELHO,
    CMD_SUPRIMIDO,
    OTA_ARM,       // entrou em modo atualização (OTA/BLE) pelo portal — para o app, força-fecha antes
};
```

- [ ] **Step 2: Se houver tabela de rótulos de auditoria, adicionar o rótulo**

Procurar um `switch`/tabela que mapeia `AuditAction` → string (ex. em `AuditLog.cpp` ou no builder do log do portal). Se existir, acrescentar o caso `OTA_ARM` → `"ota_arm"`. Se não existir tabela, pular.

Run (localizar):
```bash
grep -rn "CMD_SUPRIMIDO\|ESPELHO" src/modules/irrigation/*.cpp
```

- [ ] **Step 3: Build nativo compila** (enum é usado no nativo)

```bash
./bin/run-tests.sh test_portal_api
```
Esperado: PASS (sem regressão de compilação).

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/AuditLog.h
git commit -m "feat(irrigation): AuditAction::OTA_ARM (append-only)"
```

---

### Task 3: Glue no módulo — query de ciclo + arm + status (ESP32/CI-only)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (declarações públicas)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (impl sob `#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER`)

**Interfaces:**
- Consumes: `IrrigationWeb::otaArmAllowed`, `OtaArmReq`, `OtaStatusCtx` (Task 1); `AuditAction::OTA_ARM` (Task 2); `MeshtasticOTA::{getAppPartition,getAppDesc,checkOTACapability,saveConfig,trySwitchToOTA}` (`src/platform/esp32/MeshtasticOTA.h`); `valves.isOpen(i)`, `gpos.isOn(i)`, `valves.forceCloseAll()`, `gpos.allOff()`; `gateway.groupEngine.openConfirmedCount()`, `gateway.scheduler.runningProgramId()`; `config.network`; `rebootAtMsec`.
- Produces:
  - `bool IrrigationModule::otaCycleActive()`
  - `bool IrrigationModule::portalOtaArm(const IrrigationWeb::OtaArmReq &r)`
  - `void IrrigationModule::portalFillOtaStatus(IrrigationWeb::OtaStatusCtx &c)`

- [ ] **Step 1: Declarar os 3 métodos no `IrrigationModule.h`** (junto aos outros `portal*` públicos)

```cpp
    // OTA local por contato (BLE) — §3.3/§9. ESP32/webserver-only.
    bool otaCycleActive();
    bool portalOtaArm(const IrrigationWeb::OtaArmReq &r);
    void portalFillOtaStatus(IrrigationWeb::OtaStatusCtx &c);
```

- [ ] **Step 2: Incluir o header do loader (guardado)** no topo de `IrrigationModule.cpp`, junto dos outros includes ESP32

```cpp
#if defined(ARCH_ESP32)
#include "platform/esp32/MeshtasticOTA.h"
#endif
```

- [ ] **Step 3: Implementar os 3 métodos** (num bloco `#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER`, junto de `portalProvision` por volta de `IrrigationModule.cpp:3172`)

```cpp
bool IrrigationModule::otaCycleActive()
{
    // Válvulas locais abertas?
    for (uint8_t i = 0; i < settings.numValves; i++)
        if (valves.isOpen(i))
            return true;
    // GPOs locais ligados?
    for (uint8_t i = 0; i < settings.numGpos; i++)
        if (gpos.isOn(i))
            return true;
    // Gateway: grupo hidráulico ou programa em execução?
    if (gwIsGateway()) {
        if (gateway.groupEngine.openConfirmedCount() > 0)
            return true;
        if (gateway.scheduler.runningProgramId() != 0)
            return true;
    }
    return false;
}

void IrrigationModule::portalFillOtaStatus(IrrigationWeb::OtaStatusCtx &c)
{
    c.fwVersion = optstr(APP_VERSION);
    const esp_partition_t *part = MeshtasticOTA::getAppPartition();
    c.loaderPresent = (part != nullptr);
    c.loaderBle = false;
    if (part) {
        static esp_app_desc_t desc;
        if (MeshtasticOTA::getAppDesc(part, &desc))
            c.loaderBle = MeshtasticOTA::checkOTACapability(&desc, METHOD_OTA_BLE);
    }
    c.cycleActive = otaCycleActive();
}

bool IrrigationModule::portalOtaArm(const IrrigationWeb::OtaArmReq &r)
{
    // Gate: nunca entrar em OTA com ciclo ativo (o loader não roda o fail-safe de 120 min).
    bool anyValve = false, anyGpo = false;
    for (uint8_t i = 0; i < settings.numValves; i++)
        anyValve = anyValve || valves.isOpen(i);
    for (uint8_t i = 0; i < settings.numGpos; i++)
        anyGpo = anyGpo || gpos.isOn(i);
    bool groupActive = gwIsGateway() && (gateway.groupEngine.openConfirmedCount() > 0 ||
                                         gateway.scheduler.runningProgramId() != 0);
    if (!IrrigationWeb::otaArmAllowed(anyValve, anyGpo, groupActive)) {
        LOG_WARN("Irrigation: OTA recusado — ciclo ativo");
        return false;
    }

    // Preflight do loader.
    const esp_partition_t *part = MeshtasticOTA::getAppPartition();
    if (!part)
        return false;
    static esp_app_desc_t desc;
    if (!MeshtasticOTA::getAppDesc(part, &desc) || !MeshtasticOTA::checkOTACapability(&desc, METHOD_OTA_BLE))
        return false;

    // Defense-in-depth: força-fecha tudo antes de perder o app.
    valves.forceCloseAll();
    gpos.allOff();

    MeshtasticOTA::saveConfig(&config.network, meshtastic_OTAMode_OTA_BLE, (uint8_t *)r.hash);
    if (!MeshtasticOTA::trySwitchToOTA()) {
        LOG_ERROR("Irrigation: trySwitchToOTA falhou");
        return false;
    }
    auditEvent(AuditOrigin::PAINEL, AuditAction::OTA_ARM, 0, AuditResult::OK);
    LOG_INFO("Irrigation: OTA armado (BLE), reboot no loader em 2 s");
    rebootAtMsec = millis() + 2000;
    return true;
}
```

> **Notas ao implementer:**
> - Confirme os nomes reais: `settings.numValves`/`numGpos` (contadores), `valves.isOpen(uint8_t)`, `gpos.isOn(uint8_t)`, `gateway.groupEngine.openConfirmedCount()`, `gateway.scheduler.runningProgramId()`, `auditEvent(...)`, `AuditOrigin::PAINEL`. Ajuste às assinaturas do repo (ver `GpoController.h`, `ValveController.h`, `HydraulicGroupEngine`, `ProgramScheduler`, uso de `auditEvent` em `portalProvision`).
> - `optstr(APP_VERSION)` vem de `configuration.h` (já incluído no build). Se não visível, incluir `configuration.h`.
> - `meshtastic_OTAMode_OTA_BLE` = 1 (enum `admin.pb.h`). `METHOD_OTA_BLE` vem de `MeshtasticOTA.h`.

- [ ] **Step 4: Build nativo (glue não entra, mas o arquivo deve compilar) + build ESP32**

O nativo **não** compila este bloco (guardado por `ARCH_ESP32`+webserver). Verificar que o nativo ainda linka:
```bash
./bin/run-tests.sh test_portal_api
```
Esperado: PASS. Build ESP32 do glue é verificado no CI (Task 6 lista o gate).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): glue OTA — gate de ciclo ativo + arm loader BLE + status"
```

---

### Task 4: Endpoints do portal (CI-only)

**Files:**
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp`

**Interfaces:**
- Consumes: `irrigationModule->portalFillOtaStatus(...)`, `irrigationModule->portalOtaArm(...)`, `IrrigationWeb::{OtaStatusCtx,OtaArmReq,buildOtaStatus,parseOtaArm}`.
- Produces: rotas HTTP `GET /api/portal/ota/status`, `POST /api/portal/ota`.

- [ ] **Step 1: Adicionar os dois handlers** (junto dos outros `static void h*`, seguindo o padrão de `hProvision`)

```cpp
static void hOtaStatus(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    OtaStatusCtx c = {};
    irrigationModule->portalFillOtaStatus(c);
    char buf[256];
    if (!buildOtaStatus(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hOtaArm(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    OtaArmReq r;
    ParseResult pr = parseOtaArm(body, nb, r);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalOtaArm(r)) {
        sendJson(res, "{\"errors\":[\"OTA recusado (ciclo ativo ou loader ausente/sem BLE)\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true,\"reboot\":true}");
}
```

- [ ] **Step 2: Registrar as rotas** em `registerIrrigationPortalHandlers` (junto dos outros `registerNode`)

```cpp
    server->registerNode(new ResourceNode("/api/portal/ota/status", "GET", &hOtaStatus));
    server->registerNode(new ResourceNode("/api/portal/ota", "POST", &hOtaArm));
```

- [ ] **Step 3: Verificar que o nativo ainda linka** (arquivo é excluído do nativo, mas garante que não quebrou includes)

```bash
./bin/run-tests.sh test_portal_api
```
Esperado: PASS. (compilação real do endpoint é no ESP32/CI — Task 6.)

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationPortalEndpoints.cpp
git commit -m "feat(irrigation): endpoints /api/portal/ota[/status] (CI-only)"
```

---

### Task 5: Aba "Firmware" no portal (frontend)

**Files:**
- Modify: `data/irrigacao/portal/index.html`
- Modify: `data/irrigacao/portal/app.js`

**Interfaces:**
- Consumes: `GET /api/portal/ota/status` → `{fwVersion,loaderPresent,loaderBle,cycleActive,canArm}`; `POST /api/portal/ota` → `{ok,reboot}` ou `{errors:[...]}`.

- [ ] **Step 1: HTML — botão de nav + seção da aba** (seguir a estrutura das abas existentes em `portal/index.html`; a aba fica visível em todos os papéis)

```html
<!-- em nav.tabs, junto dos outros botões -->
<button data-tab="firmware">Firmware</button>

<!-- seção da aba (irmã das outras tab-*) -->
<section id="tab-firmware" class="hidden">
  <h2>Atualização de firmware</h2>
  <p>Versão atual: <strong id="fw-version">—</strong></p>
  <p id="fw-loader" class="muted">Verificando loader…</p>
  <button id="fw-arm" disabled>Entrar em modo atualização (BLE)</button>
  <p class="muted">
    Após confirmar, o nó reinicia em modo atualização. Abra o app
    <strong>Meshtastic</strong> oficial, conecte por Bluetooth a este nó e envie
    o arquivo <code>.bin</code>. O nó fica ~5–15 min atualizando e volta sozinho.
    <strong>Feche todas as zonas antes de atualizar.</strong>
  </p>
</section>
```

- [ ] **Step 2: JS — refresh do status + arm** (em `portal/app.js`, seguindo o padrão de `render*`/`refresh*`)

```javascript
async function refreshFirmware() {
  try {
    const r = await fetch("/api/portal/ota/status");
    if (!r.ok) return;
    const s = await r.json();
    document.getElementById("fw-version").textContent = s.fwVersion || "—";
    const loader = document.getElementById("fw-loader");
    if (!s.loaderPresent) loader.textContent = "OTA indisponível: loader ausente.";
    else if (!s.loaderBle) loader.textContent = "OTA indisponível: loader sem suporte BLE.";
    else if (s.cycleActive) loader.textContent = "Feche as zonas ativas antes de atualizar.";
    else loader.textContent = "Pronto para atualizar via BLE.";
    document.getElementById("fw-arm").disabled = !s.canArm;
  } catch (e) {}
}

document.getElementById("fw-arm").addEventListener("click", async () => {
  if (!confirm("Entrar em modo atualização? O nó vai reiniciar e ficar alguns minutos fora do ar."))
    return;
  const btn = document.getElementById("fw-arm");
  btn.disabled = true;
  try {
    const r = await fetch("/api/portal/ota", { method: "POST", body: "{}" });
    if (r.ok) {
      document.getElementById("fw-loader").textContent =
        "Modo atualização armado. Use o app Meshtastic (BLE) para enviar o .bin.";
    } else {
      const j = await r.json().catch(() => ({}));
      document.getElementById("fw-loader").textContent =
        "Falha: " + ((j.errors && j.errors[0]) || "erro");
      btn.disabled = false;
    }
  } catch (e) {
    btn.disabled = false;
  }
});
```

Chamar `refreshFirmware()` quando a aba Firmware é aberta (no handler de clique das abas — seguir o padrão de `if (b.dataset.tab === "mais") {...}`):

```javascript
    if (b.dataset.tab === "firmware") refreshFirmware();
```

> **Nota ao implementer:** confira os nomes/IDs reais das abas e o mecanismo de show/hide em `portal/app.js` (`nav.tabs button`, `tab-<name>`, `hidden`). Se o portal esconde abas por papel (ex. `initService`), garantir que a aba `firmware` **não** é escondida (aparece em todos os papéis). Sem framework — só DOM puro, igual ao resto do arquivo.

- [ ] **Step 3: Sanidade JS**

```bash
node --check data/irrigacao/portal/app.js
```
Esperado: sem erro.

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/portal/index.html data/irrigacao/portal/app.js
git commit -m "feat(irrigation): aba Firmware no portal (status + armar OTA BLE)"
```

---

### Task 6: Runbook + verificação final

**Files:**
- Create: `docs/irrigacao/ota-ble.md`

- [ ] **Step 1: Escrever o runbook** `docs/irrigacao/ota-ble.md`

Conteúdo (seções):
- **Provisionamento (1×, USB):** gravar o loader OTA `mt-esp32-ota.bin` (variante *combined* ou *BLE*) no `flashApp`/ota_1 no flash de fábrica (ver `bin/device-install.sh`, offset `ota_1`). Sem loader → portal mostra "OTA indisponível: loader ausente".
- **BLE habilitado:** confirmar que a variante `custom_irrigation` não desabilita BLE (Heltec V2 tem BLE). Checar flags do build.
- **Procedimento de campo:**
  1. Fechar todas as zonas (o portal recusa OTA com ciclo ativo).
  2. Portal do nó → aba **Firmware** → conferir versão + "Pronto para atualizar".
  3. **Entrar em modo atualização (BLE)** → nó reinicia no loader.
  4. App **Meshtastic** oficial → conectar por BLE ao nó → enviar `.bin` (baixado antes, com internet).
  5. Aguardar (~5–15 min). Nó volta no firmware novo; conferir a versão na aba Firmware.
- **Rollback:** health-check automático **não** implementado nesta fase (follow-up §3.3). Recuperação de imagem ruim = reflash USB.
- **Segurança:** entrar em OTA para o app (LoRa/scheduler/fail-safe). O gate força-fecha e recusa com ciclo ativo; ainda assim, atualizar com o sistema ocioso.

- [ ] **Step 2: Suíte nativa completa GREEN (Docker)**

```bash
./bin/run-tests.sh
```
Esperado: exit 0, todas as suites GREEN (baseline atual + 3 casos novos em `test_portal_api`). Anotar a contagem final.

- [ ] **Step 3: Commit**

```bash
git add docs/irrigacao/ota-ble.md
git commit -m "docs(irrigation): runbook OTA local por contato (BLE)"
```

---

## Gate de merge / verificação externa (não é task)

- **Build ESP32 no CI EXIGIDO** — Tasks 3 e 4 (glue + endpoints) são webserver/ESP32-guarded e **não compilam no nativo**; só o CI (ou build local `custom_irrigation`) prova que compilam/linkam.
- **Banca 1 nó EXIGIDA** (radio/BLE não é native-testável):
  1. armar com zona aberta → **recusado**;
  2. armar livre → reboota no loader; app Meshtastic envia `.bin` por BLE → volta no fw novo;
  3. confirmar que válvulas ficaram fechadas durante o modo OTA;
  4. **verificar quem arma o `ota_hash`** (portal vs app) — se o app re-arma com o próprio hash, o `hash` do POST é redundante; ajustar `hOtaArm`/`portalOtaArm` se necessário.

## Self-review (coberto)

- Spec §4(A) gate → Tasks 1+3; §4(B) endpoint → Task 4; §4(C)/D aba → Task 5; §5 provisionamento → Task 6; §6 ABI (`OTA_ARM` append-only) → Task 2; §7 testes → Task 1 (puro) + gates externos. C (rollback), assinatura, mesh, WiFi = não-escopo (spec §8), sem task. Sem placeholders; tipos consistentes entre tasks (`OtaArmReq`/`OtaStatusCtx`/`otaArmAllowed`/`portalOtaArm`/`portalFillOtaStatus`).
