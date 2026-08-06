# Irrigação Fase 8a — Conectividade WiFi do Gateway — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Gateway conecta a um roteador WiFi (STA sempre-on quando configurado), fica acessível na LAN via `http://irrigacao.local`, e é provisionado por um card "Rede Wi-Fi" no portal cativo — sem tocar em NTP/relógio.

**Architecture:** Reusa a stack de rede da base do Meshtastic (`initWifi`/reconnect/mDNS via `config.network`). O trabalho novo é: (A) núcleo puro host-tested `PortalWifiApi` em `PortalApi.{h,cpp}`; (B) glue ESP32-only (métodos WiFi no `IrrigationModule`, endpoints `/api/portal/wifi/*`, arbitragem AP/STA em `PortalAp.cpp`, hostname mDNS); (C) frontend do card "Rede Wi-Fi" portado do mockup. NTP fica fora — o loop da base roda sozinho, intocado.

**Tech Stack:** C++ (Arduino/ESP32), Unity (testes nativos), `esp32_https_server`, `WiFi.h`/`ESPmDNS`, JsonWriter/JsonReader caseiros do módulo.

**Spec:** `docs/superpowers/specs/2026-08-06-irrigacao-conectividade-wifi-gateway-design.md`

---

## Notas de execução

- **Testes nativos neste box (Windows) exigem Docker** — ver `[[native-tests-need-docker-on-windows]]`. Rodar:
  `./bin/test-native-docker.sh -f test_portal_api` (ou `pio test -e native -f test_portal_api` num host Linux).
- **Tasks 1–5 (núcleo puro)** são TDD real: teste falha → implementa → passa. Vão no suite existente
  `test/test_portal_api/test_main.cpp` (mesmo padrão de coords/provision/link). **Suite não muda de número** (56).
- **Tasks 6–11 (glue ESP32 + frontend)** NÃO compilam localmente (`ARCH_ESP32`, `esp32_https_server`).
  São validadas por revisão + o job ESP32 tbeam/gateway no CI. Sem passo de "rodar teste" local; o passo de
  verificação é inspeção contra o contrato + `git commit`.
- `trunk fmt` roda no CI (não instalado local). Não bloquear por isso.
- Todos os buffers de resposta checam `w.done()` (0 = truncou → 500), padrão do módulo.

---

## Task 1: `buildWifiStatus` (núcleo puro)

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h` (structs + assinatura)
- Modify: `src/modules/irrigation/PortalApi.cpp` (implementação)
- Test: `test/test_portal_api/test_main.cpp`

- [ ] **Step 1: Adicionar contratos ao header**

Em `PortalApi.h`, dentro de `namespace IrrigationWeb`, após o bloco "Aba Enlace":

```cpp
// --- Aba "Rede Wi-Fi" (§7 fase 8a) ---
struct WifiStatusCtx {
    bool enabled = false;         // config.network.wifi_enabled
    bool staUp = false;           // WiFi.isConnected()
    char connectedSsid[33] = {0}; // "" se não conectado
    char ip[16] = {0};            // "" se sem IP
};
size_t buildWifiStatus(const WifiStatusCtx &ctx, char *buf, size_t cap);
```

- [ ] **Step 2: Escrever o teste que falha**

Em `test/test_portal_api/test_main.cpp`, adicionar:

```cpp
static void test_buildWifiStatus_connected()
{
    WifiStatusCtx c = {};
    c.enabled = true;
    c.staUp = true;
    strcpy(c.connectedSsid, "Fazenda_Escritorio");
    strcpy(c.ip, "192.168.0.42");
    char buf[128];
    size_t n = buildWifiStatus(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"enabled\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"staUp\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"connectedSsid\":\"Fazenda_Escritorio\""));
    TEST_ASSERT_TRUE(contains(buf, "\"ip\":\"192.168.0.42\""));
}

static void test_buildWifiStatus_disconnected()
{
    WifiStatusCtx c = {};
    c.enabled = false;
    char buf[128];
    size_t n = buildWifiStatus(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"enabled\":false"));
    TEST_ASSERT_TRUE(contains(buf, "\"staUp\":false"));
    TEST_ASSERT_TRUE(contains(buf, "\"connectedSsid\":\"\""));
}
```

Registrar no `setup()`: `RUN_TEST(test_buildWifiStatus_connected); RUN_TEST(test_buildWifiStatus_disconnected);`

- [ ] **Step 3: Verificar que falha**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: FALHA de compilação/link — `buildWifiStatus` não definido.

- [ ] **Step 4: Implementar**

Em `PortalApi.cpp`:

```cpp
size_t buildWifiStatus(const WifiStatusCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyBool("enabled", ctx.enabled);
    w.keyBool("staUp", ctx.staUp);
    w.keyStr("connectedSsid", ctx.connectedSsid);
    w.keyStr("ip", ctx.ip);
    w.endObject();
    return w.done();
}
```

> Nota: `keyStr` existe? O idioma do módulo usa `w.keyStr(...)` em `buildNodeState`. Se `keyStr` não
> estiver no header, usar `w.key("connectedSsid"); w.str(ctx.connectedSsid);`.

- [ ] **Step 5: Verificar que passa**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: PASS (todos os testes do suite verdes).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp test/test_portal_api/test_main.cpp
git commit -m "feat(irrigation): portal — buildWifiStatus (status Rede Wi-Fi)"
```

---

## Task 2: `buildWifiScan` (núcleo puro)

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h`, `src/modules/irrigation/PortalApi.cpp`
- Test: `test/test_portal_api/test_main.cpp`

- [ ] **Step 1: Contratos no header**

```cpp
struct WifiScanItem {
    char ssid[33] = {0};
    int16_t rssi = 0;
    bool secure = true;
};
struct WifiScanCtx {
    bool scanning = false;   // true → frontend mostra spinner, ignora items
    uint8_t count = 0;       // <= 16
    WifiScanItem items[16];
};
size_t buildWifiScan(const WifiScanCtx &ctx, char *buf, size_t cap);
```

- [ ] **Step 2: Teste que falha**

```cpp
static void test_buildWifiScan_list()
{
    WifiScanCtx c = {};
    c.scanning = false;
    c.count = 2;
    strcpy(c.items[0].ssid, "Fazenda_Escritorio"); c.items[0].rssi = -48; c.items[0].secure = true;
    strcpy(c.items[1].ssid, "Aberta");             c.items[1].rssi = -70; c.items[1].secure = false;
    char buf[512];
    size_t n = buildWifiScan(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"scanning\":false"));
    TEST_ASSERT_TRUE(contains(buf, "\"ssid\":\"Fazenda_Escritorio\""));
    TEST_ASSERT_TRUE(contains(buf, "\"rssi\":-48"));
    TEST_ASSERT_TRUE(contains(buf, "\"secure\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"secure\":false"));
}

static void test_buildWifiScan_scanning()
{
    WifiScanCtx c = {};
    c.scanning = true;
    char buf[128];
    size_t n = buildWifiScan(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"scanning\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"networks\":[]"));
}
```

Registrar ambos no `setup()`.

- [ ] **Step 3: Verificar que falha**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: FALHA — `buildWifiScan` indefinido.

- [ ] **Step 4: Implementar**

```cpp
size_t buildWifiScan(const WifiScanCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyBool("scanning", ctx.scanning);
    w.key("networks");
    w.beginArray();
    if (!ctx.scanning) {
        uint8_t n = ctx.count > 16 ? 16 : ctx.count;
        for (uint8_t i = 0; i < n; i++) {
            w.beginObject();
            w.keyStr("ssid", ctx.items[i].ssid);
            w.keyNum("rssi", ctx.items[i].rssi);
            w.keyBool("secure", ctx.items[i].secure);
            w.endObject();
        }
    }
    w.endArray();
    w.endObject();
    return w.done();
}
```

> Se `JsonWriter` não expõe `beginObject` dentro de array de forma encadeada, seguir exatamente o
> padrão de `buildRoster`/`buildLink` em `PortalApi.cpp` (que já serializam arrays de objetos).

- [ ] **Step 5: Verificar que passa**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp test/test_portal_api/test_main.cpp
git commit -m "feat(irrigation): portal — buildWifiScan (lista de redes)"
```

---

## Task 3: `parseWifiConnect` + validação (núcleo puro)

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h`, `src/modules/irrigation/PortalApi.cpp`
- Test: `test/test_portal_api/test_main.cpp`

- [ ] **Step 1: Contratos no header**

```cpp
struct WifiConnectReq {
    char ssid[33] = {0};
    char psk[64] = {0};  // vazio = rede aberta
};
ParseResult parseWifiConnect(const char *json, size_t len, WifiConnectReq &out);
```

- [ ] **Step 2: Teste que falha**

```cpp
static void test_parseWifiConnect_valid()
{
    WifiConnectReq p = {};
    const char *j = "{\"ssid\":\"Fazenda\",\"psk\":\"segredo123\"}";
    ParseResult r = parseWifiConnect(j, strlen(j), p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("Fazenda", p.ssid);
    TEST_ASSERT_EQUAL_STRING("segredo123", p.psk);
}

static void test_parseWifiConnect_openNetwork()
{
    WifiConnectReq p = {};
    const char *j = "{\"ssid\":\"Aberta\",\"psk\":\"\"}";
    ParseResult r = parseWifiConnect(j, strlen(j), p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("", p.psk);
}

static void test_parseWifiConnect_rejectsEmptySsid()
{
    WifiConnectReq p = {};
    const char *j = "{\"ssid\":\"\",\"psk\":\"segredo123\"}";
    ParseResult r = parseWifiConnect(j, strlen(j), p);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_parseWifiConnect_rejectsShortPsk()
{
    WifiConnectReq p = {};
    const char *j = "{\"ssid\":\"Fazenda\",\"psk\":\"curta\"}"; // 5 < 8
    ParseResult r = parseWifiConnect(j, strlen(j), p);
    TEST_ASSERT_FALSE(r.ok);
}
```

Registrar os quatro no `setup()`.

- [ ] **Step 3: Verificar que falha**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: FALHA — `parseWifiConnect` indefinido.

- [ ] **Step 4: Implementar**

```cpp
ParseResult parseWifiConnect(const char *json, size_t len, WifiConnectReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    char ssid[33] = {0};
    char psk[64] = {0};
    if (!rd.getStr("ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        r.fail("ssid vazio");
        return r;
    }
    // psk é opcional (rede aberta = string vazia). Se presente e não-vazio, exige >= 8 (WPA2).
    rd.getStr("psk", psk, sizeof(psk));
    size_t plen = strlen(psk);
    if (plen > 0 && plen < 8) {
        r.fail("senha < 8 caracteres");
        return r;
    }
    memcpy(out.ssid, ssid, sizeof(out.ssid));
    memcpy(out.psk, psk, sizeof(out.psk));
    return r;
}
```

- [ ] **Step 5: Verificar que passa**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp test/test_portal_api/test_main.cpp
git commit -m "feat(irrigation): portal — parseWifiConnect (SSID/senha + validacao WPA2)"
```

---

## Task 4: `buildWifiConnect` (estados de progresso) (núcleo puro)

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h`, `src/modules/irrigation/PortalApi.cpp`
- Test: `test/test_portal_api/test_main.cpp`

- [ ] **Step 1: Contratos no header**

```cpp
enum class WifiConnectState : uint8_t { Idle = 0, Connecting = 1, Success = 2, Error = 3 };
struct WifiConnectCtx {
    WifiConnectState state = WifiConnectState::Idle;
    char ssid[33] = {0};
    char error[48] = {0}; // preenchido só quando state==Error
};
size_t buildWifiConnect(const WifiConnectCtx &ctx, char *buf, size_t cap);
```

- [ ] **Step 2: Teste que falha**

```cpp
static void test_buildWifiConnect_connecting()
{
    WifiConnectCtx c = {};
    c.state = WifiConnectState::Connecting;
    strcpy(c.ssid, "Fazenda");
    char buf[128];
    size_t n = buildWifiConnect(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"state\":\"connecting\""));
    TEST_ASSERT_TRUE(contains(buf, "\"ssid\":\"Fazenda\""));
}

static void test_buildWifiConnect_error()
{
    WifiConnectCtx c = {};
    c.state = WifiConnectState::Error;
    strcpy(c.ssid, "Fazenda");
    strcpy(c.error, "Senha incorreta.");
    char buf[128];
    size_t n = buildWifiConnect(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"state\":\"error\""));
    TEST_ASSERT_TRUE(contains(buf, "\"error\":\"Senha incorreta.\""));
}

static void test_buildWifiConnect_success()
{
    WifiConnectCtx c = {};
    c.state = WifiConnectState::Success;
    strcpy(c.ssid, "Fazenda");
    char buf[128];
    size_t n = buildWifiConnect(c, buf, sizeof(buf));
    TEST_ASSERT_TRUE(contains(buf, "\"state\":\"success\""));
}
```

Registrar os três no `setup()`.

- [ ] **Step 3: Verificar que falha**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: FALHA — `buildWifiConnect` indefinido.

- [ ] **Step 4: Implementar**

```cpp
size_t buildWifiConnect(const WifiConnectCtx &ctx, char *buf, size_t cap)
{
    const char *s = "idle";
    switch (ctx.state) {
    case WifiConnectState::Connecting: s = "connecting"; break;
    case WifiConnectState::Success:    s = "success";    break;
    case WifiConnectState::Error:      s = "error";      break;
    default:                           s = "idle";       break;
    }
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyStr("state", s);
    w.keyStr("ssid", ctx.ssid);
    w.keyStr("error", ctx.error);
    w.endObject();
    return w.done();
}
```

- [ ] **Step 5: Verificar que passa**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp test/test_portal_api/test_main.cpp
git commit -m "feat(irrigation): portal — buildWifiConnect (estados de progresso)"
```

---

## Task 5: `parseWifiToggle` (núcleo puro)

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h`, `src/modules/irrigation/PortalApi.cpp`
- Test: `test/test_portal_api/test_main.cpp`

- [ ] **Step 1: Contratos no header**

```cpp
struct WifiToggleReq { bool enabled = false; };
ParseResult parseWifiToggle(const char *json, size_t len, WifiToggleReq &out);
```

- [ ] **Step 2: Teste que falha**

```cpp
static void test_parseWifiToggle_true()
{
    WifiToggleReq t = {};
    const char *j = "{\"enabled\":true}";
    ParseResult r = parseWifiToggle(j, strlen(j), t);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(t.enabled);
}

static void test_parseWifiToggle_rejectsMissing()
{
    WifiToggleReq t = {};
    const char *j = "{}";
    ParseResult r = parseWifiToggle(j, strlen(j), t);
    TEST_ASSERT_FALSE(r.ok);
}
```

Registrar ambos no `setup()`.

- [ ] **Step 3: Verificar que falha**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: FALHA — `parseWifiToggle` indefinido.

- [ ] **Step 4: Implementar**

```cpp
ParseResult parseWifiToggle(const char *json, size_t len, WifiToggleReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    bool en = false;
    if (!rd.getBool("enabled", en)) {
        r.fail("campo enabled ausente");
        return r;
    }
    out.enabled = en;
    return r;
}
```

- [ ] **Step 5: Verificar que passa**

Run: `./bin/test-native-docker.sh -f test_portal_api`
Expected: PASS — fim do núcleo puro, todos os testes WiFi verdes.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp test/test_portal_api/test_main.cpp
git commit -m "feat(irrigation): portal — parseWifiToggle"
```

---

## Task 6: Métodos WiFi no `IrrigationModule` (glue ESP32)

> Sem teste local (acessa `WiFi`/`config.network`, só compila em ESP32). Validar por revisão + CI.

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (declarações)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (implementações, guardadas por `ARCH_ESP32`)

- [ ] **Step 1: Declarar no header**

Em `IrrigationModule.h`, junto dos outros `portal*` públicos:

```cpp
// Fase 8a — provisionamento WiFi STA (glue ESP32; stub no native)
void portalWifiStatus(IrrigationWeb::WifiStatusCtx &out);
void portalWifiStartScan();
void portalWifiScanResult(IrrigationWeb::WifiScanCtx &out);
bool portalWifiConnect(const IrrigationWeb::WifiConnectReq &req);
void portalWifiConnectProgress(IrrigationWeb::WifiConnectCtx &out);
void portalWifiForget();
void portalWifiToggle(bool enabled);
```

- [ ] **Step 2: Implementar (guardado por ARCH_ESP32)**

Em `IrrigationModule.cpp`. Padrão: dentro de `#if defined(ARCH_ESP32)` implementação real; `#else`
stubs vazios (para o link nativo, já que os endpoints são native-excluded mas o módulo compila em native).

```cpp
#if defined(ARCH_ESP32)
#include <WiFi.h>
#include "mesh/wifi/WiFiAPClient.h"

void IrrigationModule::portalWifiStatus(IrrigationWeb::WifiStatusCtx &out)
{
    out.enabled = config.network.wifi_enabled;
    out.staUp = WiFi.isConnected();
    if (out.staUp) {
        strncpy(out.connectedSsid, WiFi.SSID().c_str(), sizeof(out.connectedSsid) - 1);
        strncpy(out.ip, WiFi.localIP().toString().c_str(), sizeof(out.ip) - 1);
    } else if (config.network.wifi_ssid[0]) {
        strncpy(out.connectedSsid, "", sizeof(out.connectedSsid) - 1);
    }
}

void IrrigationModule::portalWifiStartScan()
{
    // Async: retorna imediato; resultado colhido depois via portalWifiScanResult.
    WiFi.scanDelete();
    WiFi.scanNetworks(true /*async*/, false /*show_hidden*/);
}

void IrrigationModule::portalWifiScanResult(IrrigationWeb::WifiScanCtx &out)
{
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING || n == WIFI_SCAN_FAILED && n < 0) {
        out.scanning = true; // ainda rodando (ou nunca iniciou)
        return;
    }
    out.scanning = false;
    uint8_t cnt = 0;
    for (int16_t i = 0; i < n && cnt < 16; i++) {
        strncpy(out.items[cnt].ssid, WiFi.SSID(i).c_str(), sizeof(out.items[cnt].ssid) - 1);
        out.items[cnt].rssi = WiFi.RSSI(i);
        out.items[cnt].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        cnt++;
    }
    out.count = cnt;
}

bool IrrigationModule::portalWifiConnect(const IrrigationWeb::WifiConnectReq &req)
{
    strncpy(config.network.wifi_ssid, req.ssid, sizeof(config.network.wifi_ssid) - 1);
    strncpy(config.network.wifi_psk, req.psk, sizeof(config.network.wifi_psk) - 1);
    config.network.wifi_enabled = true;
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    // Sinaliza a base para reconectar com as novas credenciais (mesmo caminho do AdminModule).
    needReconnect = true; // flag global de WiFiAPClient
    return true;
}

void IrrigationModule::portalWifiConnectProgress(IrrigationWeb::WifiConnectCtx &out)
{
    strncpy(out.ssid, config.network.wifi_ssid, sizeof(out.ssid) - 1);
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        out.state = IrrigationWeb::WifiConnectState::Success;
    } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
        out.state = IrrigationWeb::WifiConnectState::Error;
        strncpy(out.error, st == WL_NO_SSID_AVAIL ? "Rede nao encontrada." : "Senha incorreta.",
                sizeof(out.error) - 1);
    } else {
        out.state = IrrigationWeb::WifiConnectState::Connecting;
    }
}

void IrrigationModule::portalWifiForget()
{
    config.network.wifi_ssid[0] = '\0';
    config.network.wifi_psk[0] = '\0';
    config.network.wifi_enabled = false;
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    WiFi.disconnect(false, true); // derruba STA, mantém rádio p/ o AP do portal
}

void IrrigationModule::portalWifiToggle(bool enabled)
{
    config.network.wifi_enabled = enabled;
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    if (enabled) needReconnect = true;
    else WiFi.disconnect(false, true);
}
#else
void IrrigationModule::portalWifiStatus(IrrigationWeb::WifiStatusCtx &) {}
void IrrigationModule::portalWifiStartScan() {}
void IrrigationModule::portalWifiScanResult(IrrigationWeb::WifiScanCtx &) {}
bool IrrigationModule::portalWifiConnect(const IrrigationWeb::WifiConnectReq &) { return false; }
void IrrigationModule::portalWifiConnectProgress(IrrigationWeb::WifiConnectCtx &) {}
void IrrigationModule::portalWifiForget() {}
void IrrigationModule::portalWifiToggle(bool) {}
#endif
```

**Verificar contra a base antes de commitar:**
- Confirmar o nome real da flag de reconexão em `src/mesh/wifi/WiFiAPClient.h`/`.cpp`
  (`needReconnect` aparece em `WiFiAPClient.cpp:289`). Se for `static`, expor um setter
  (`void requestWifiReconnect();`) em `WiFiAPClient.h` e chamá-lo — NÃO reimplementar reconexão.
- Confirmar assinatura de `nodeDB->saveToDisk(...)` e o enum de segmento (`SEGMENT_CONFIG`).
- Confirmar `#include "configuration.h"` traz `config`.

- [ ] **Step 3: Verificar (revisão, sem build local)**

Ler o diff: guardas `ARCH_ESP32` corretas, stubs nativos presentes, nenhuma reimplementação de
reconexão, `psk` nunca logado. `git add -A && git status`.

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): gateway — metodos WiFi STA (status/scan/connect/forget/toggle)"
```

---

## Task 7: Endpoints `/api/portal/wifi/*` (glue ESP32)

**Files:**
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp`

- [ ] **Step 1: Adicionar handlers**

Antes de `registerIrrigationPortalHandlers`, seguindo o padrão de `hNode`/`hGpo`:

```cpp
static void hWifiStatus(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    WifiStatusCtx c = {};
    irrigationModule->portalWifiStatus(c);
    char buf[160];
    if (!buildWifiStatus(c, buf, sizeof(buf))) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

static void hWifiScanStart(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    irrigationModule->portalWifiStartScan();
    sendJson(res, "{\"ok\":true}");
}

static void hWifiScanResult(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    WifiScanCtx c = {};
    irrigationModule->portalWifiScanResult(c);
    char buf[1024]; // 16 redes * ~56 B
    if (!buildWifiScan(c, buf, sizeof(buf))) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

static void hWifiConnectStart(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) { res->setStatusCode(404); return; }
    char body[160];
    size_t nb = readBody(req, body, sizeof(body));
    WifiConnectReq p;
    ParseResult pr = parseWifiConnect(body, nb, p);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->portalWifiConnect(p)) {
        sendJson(res, "{\"errors\":[\"conexao rejeitada\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

static void hWifiConnectProgress(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    WifiConnectCtx c = {};
    irrigationModule->portalWifiConnectProgress(c);
    char buf[160];
    if (!buildWifiConnect(c, buf, sizeof(buf))) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

static void hWifiForget(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    irrigationModule->portalWifiForget();
    sendJson(res, "{\"ok\":true}");
}

static void hWifiToggle(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) { res->setStatusCode(404); return; }
    char body[64];
    size_t nb = readBody(req, body, sizeof(body));
    WifiToggleReq t;
    ParseResult pr = parseWifiToggle(body, nb, t);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    irrigationModule->portalWifiToggle(t.enabled);
    sendJson(res, "{\"ok\":true}");
}
```

- [ ] **Step 2: Registrar as rotas**

Dentro de `registerIrrigationPortalHandlers`, adicionar:

```cpp
    server->registerNode(new ResourceNode("/api/portal/wifi", "GET", &hWifiStatus));
    server->registerNode(new ResourceNode("/api/portal/wifi/scan", "POST", &hWifiScanStart));
    server->registerNode(new ResourceNode("/api/portal/wifi/scan", "GET", &hWifiScanResult));
    server->registerNode(new ResourceNode("/api/portal/wifi/connect", "POST", &hWifiConnectStart));
    server->registerNode(new ResourceNode("/api/portal/wifi/connect", "GET", &hWifiConnectProgress));
    server->registerNode(new ResourceNode("/api/portal/wifi/forget", "POST", &hWifiForget));
    server->registerNode(new ResourceNode("/api/portal/wifi/toggle", "POST", &hWifiToggle));
```

- [ ] **Step 3: Verificar (revisão)**

Rotas batem com o §9 do spec; buffers dimensionados; `parse*`/`build*` já existem (Tasks 1-5).

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationPortalEndpoints.cpp
git commit -m "feat(irrigation): portal — endpoints /api/portal/wifi/* (scan/connect/forget/toggle)"
```

---

## Task 8: Arbitragem AP/STA em `PortalAp.cpp` (glue ESP32)

**Files:**
- Modify: `src/modules/irrigation/PortalAp.cpp`

- [ ] **Step 1: `bringUp`/`tearDown` cooperam com o STA**

```cpp
#include "configuration.h" // config.network

static bool staConfigured()
{
    return config.network.wifi_enabled && config.network.wifi_ssid[0] != '\0';
}

static void bringUp()
{
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "Irrigacao-%s", owner.short_name);
    // AP+STA quando há roteador configurado: sobe o AP SEM derrubar o STA da base.
    WiFi.mode(staConfigured() ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(ssid, IRRIGATION_PORTAL_PIN);
    sDns.start(53, "*", WiFi.softAPIP());
    LOG_INFO("Irrigation portal: AP up (%s), mode=%s", ssid, staConfigured() ? "AP_STA" : "AP");
    sApUp = true;
}

static void tearDown()
{
    sDns.stop();
    WiFi.softAPdisconnect(true);
    // Não desligar o rádio se há STA: volta a STA puro para manter a LAN viva.
    if (staConfigured())
        WiFi.mode(WIFI_STA);
    LOG_INFO("Irrigation portal: AP down");
    sApUp = false;
}
```

- [ ] **Step 2: Verificar (revisão)**

`WiFi.mode(WIFI_AP)` não aparece mais incondicional; teardown nunca faz `WIFI_OFF`; STA nunca é
tocado pelo portal exceto pela troca de modo. O STA em si continua governado por `initWifi`/reconnect.

- [ ] **Step 3: Commit**

```bash
git add src/modules/irrigation/PortalAp.cpp
git commit -m "fix(irrigation): portal — AP coexiste com STA (WIFI_AP_STA), teardown mantem STA"
```

---

## Task 9: Hostname mDNS `irrigacao` (glue ESP32)

**Files:**
- Modify: `src/mesh/wifi/WiFiAPClient.cpp:87,157` (as duas chamadas `MDNS.begin`)

- [ ] **Step 1: Trocar o hostname**

Em ambas as ocorrências, trocar `MDNS.begin("Meshtastic")` por um hostname de irrigação. Para não
afetar outros alvos, usar guarda de macro do build de irrigação (confirmar a macro real usada pelo
env do gateway — ex.: `IRRIGATION` / flag do `platformio.ini`):

```cpp
#ifdef IRRIGATION_BUILD
        if (MDNS.begin("irrigacao")) {
#else
        if (MDNS.begin("Meshtastic")) {
#endif
```

> **Confirmar a macro** que distingue o firmware de irrigação antes de escrever. Se não houver macro
> dedicada, definir uma no env do gateway em `platformio.ini` (`-D IRRIGATION_BUILD`) e usá-la aqui.
> Manter o `addService("meshtastic", ...)` como está (o serviço só precisa existir; o hostname é o que
> vira `irrigacao.local`).

- [ ] **Step 2: Verificar (revisão)**

As duas ocorrências (linhas ~87 e ~157) foram trocadas de forma idêntica e guardadas pela macro;
build não-irrigação intocado.

- [ ] **Step 3: Commit**

```bash
git add src/mesh/wifi/WiFiAPClient.cpp platformio.ini
git commit -m "feat(irrigation): mDNS hostname irrigacao.local (guardado por build)"
```

---

## Task 10: Frontend — card "Rede Wi-Fi" (portado do mockup)

**Files:**
- Modify: `data/irrigacao/portal/index.html`
- Modify: `data/irrigacao/portal/app.js`
- Reference: `C:\Users\jmelo\Downloads\Irrigacao Mobile.dc.html` (card + tela Rede Wi-Fi, aba Mais)

- [ ] **Step 1: Portar o markup/estilos do mockup**

Copiar do mockup o **visual exato** (markup, estilos inline, textos) do:
- Card "Rede Wi-Fi" na lista "Mais" (com o subtítulo `wifiSummaryLabel`).
- A tela "Rede Wi-Fi" com os estados: `list` (toggle + "Atualizar" + lista de redes com sinal/cadeado),
  `password` (input senha + Conectar/Cancelar), `connecting` (spinner "Conectando a X…"),
  `success` ("Conectado a X" + Concluído), `error` (mensagem + Tentar novamente/Cancelar), `off`.

Adaptar do framework `sc-if`/`sc-for` do mockup para o padrão vanilla usado no portal atual
(o mesmo esquema de render/estado dos outros cards em `app.js`). **Preservar cores, textos e layout.**

- [ ] **Step 2: Ligar aos endpoints**

Em `app.js`, seguindo o padrão de fetch dos cards existentes:
- Abrir a tela → `GET /api/portal/wifi` (preenche `wifiSummaryLabel`, estado do toggle).
- Toggle → `POST /api/portal/wifi/toggle {enabled}`; se ligou, dispara scan.
- "Atualizar"/abrir lista → `POST /api/portal/wifi/scan`, depois **poll** `GET /api/portal/wifi/scan`
  a cada ~1,5 s até `scanning=false`, então renderiza `networks`.
- Selecionar rede segura → tela senha; rede aberta → conecta direto (psk vazio).
- "Conectar" → `POST /api/portal/wifi/connect {ssid,psk}`; entra em `connecting`; **poll**
  `GET /api/portal/wifi/connect` até `state` virar `success` ou `error`; renderiza a tela correspondente.
- Tocar a rede conectada → `POST /api/portal/wifi/forget`.

- [ ] **Step 3: Verificar (revisão + smoke manual opcional)**

Abrir `index.html` local e conferir que os estados renderizam como o mockup (sem backend, os fetch
falham — validar só o visual/estados). Contra o ESP32 real fica para o teste de hardware/CI.

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/portal/index.html data/irrigacao/portal/app.js
git commit -m "feat(irrigation): portal — card Rede Wi-Fi (UI do mockup + fluxo scan/connect)"
```

---

## Task 11: Endurecer auth dos endpoints de escrita de rede (segurança)

**Files:**
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp`
- Reference: como `hGpo`/`hCoordsSet`/`hNetCommand` tratam sessão/PIN hoje

- [ ] **Step 1: Levantar o contrato de auth atual**

Ler como os endpoints de escrita existentes (`/api/portal/gpo`, `/api/portal/coords POST`,
`/api/portal/net/command`) provam sessão do portal (PIN/token/estar-no-AP). Ver `PortalSession.cpp`
e se há checagem de sessão nos handlers.

- [ ] **Step 2: Aplicar a mesma prova aos endpoints de escrita de WiFi**

`connect`/`forget`/`toggle` são escrita de rede e ficam alcançáveis pela LAN com STA on. Aplicar
**a mesma verificação** que os outros endpoints de escrita usam (mesma função/guard). Se hoje a única
proteção é "estar no AP", adicionar a checagem de sessão do portal a estes três handlers (rejeitar com
403 quando não autenticado). `status`/`scan` (leitura, sem segredo) podem seguir o padrão dos GET.

> Não inventar esquema de auth novo — reusar exatamente o que o portal já aplica. Se os endpoints de
> escrita atuais NÃO têm proteção além do AP, registrar isso como achado e alinhar com o dono antes de
> abrir escrita de rede na LAN (pode-se, no mínimo, exigir que `connect/forget/toggle` só respondam
> quando o AP do portal está no ar — `PortalSession::apShouldBeUp()`).

- [ ] **Step 3: Verificar (revisão)**

Os três endpoints de escrita de WiFi têm a mesma barreira dos demais endpoints de escrita.

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationPortalEndpoints.cpp
git commit -m "fix(irrigation): portal — endpoints de escrita WiFi exigem sessao do portal"
```

---

## Task 12: Fechamento

- [ ] **Step 1: Rodar o suite nativo completo**

Run: `./bin/run-tests.sh` (ou `./bin/test-native-docker.sh`)
Expected: GREEN (exit 0). `test_portal_api` inclui os novos testes WiFi.

- [ ] **Step 2: Atualizar a memória de projeto**

Criar `memory/irrigation-phase8a-status.md` (tipo project) resumindo: escopo entregue (STA+LAN+mDNS,
card Rede Wi-Fi, arbitragem AP/STA), NTP adiado, macro de build do mDNS, caveats de CI ESP32, e o
achado de auth da Task 11. Adicionar linha em `MEMORY.md`. Linkar `[[irrigation-phase5b-status]]`.

- [ ] **Step 3: Verificação final e push**

Confirmar o job ESP32 tbeam/gateway no CI após push (compila `PortalAp.cpp`/`IrrigationPortalEndpoints.cpp`/
`IrrigationModule.cpp` — nunca compilados localmente, ver `[[native-tests-need-docker-on-windows]]`).
`trunk fmt` roda no CI.

---

## Self-Review (cobertura do spec)

- §1 escopo (STA+LAN+mDNS, NTP fora) → Tasks 6,8,9 + escopo respeitado (nenhuma task toca NTP). ✔
- §3A núcleo puro → Tasks 1-5. ✔
- §3B glue (módulo/endpoints/PortalAp/mDNS) → Tasks 6,7,8,9. ✔
- §3C frontend → Task 10. ✔
- §4 arbitragem AP/STA → Task 8. ✔
- §5 persistência/reconexão → Task 6 (saveToDisk + needReconnect). ✔
- §6 segurança → Task 11 + buffers dimensionados/`done()` nas Tasks 1-2,7 + psk nunca ecoado (Task 6/1). ✔
- §7 testes → Tasks 1-5 (nativo) + Task 12 (suite). ✔
- §9 mapa mockup→estados → Task 10. ✔

Sem placeholders de conteúdo (os "confirmar X" são checagens de integração deliberadas contra a base,
com o valor esperado nomeado). Tipos/assinaturas consistentes entre header (Tasks 1-5) e uso (Tasks 6-7).
