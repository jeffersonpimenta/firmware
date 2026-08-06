# Irrigação — Conectividade WiFi do Gateway (STA + LAN + mDNS)

**Branch:** `sistema-irrigacao`
**Data:** 2026-08-06
**Fase:** 8a (conectividade)
**Depende de:** fases 5a/5b (painel gateway + portal cativo). Ver `[[irrigation-phase5a-status]]`, `[[irrigation-phase5b-status]]`.

## 1. Objetivo e escopo

Permitir que o **gateway** (T-Beam) se conecte a um **roteador WiFi existente** em modo estação (STA),
de modo que:

1. A UI (painel/portal) fique acessível pela **rede local** via `http://irrigacao.local` (mDNS),
   sem o operador precisar descobrir o IP atribuído por DHCP.
2. As credenciais do roteador sejam inseridas **pelo próprio portal** (card "Rede Wi-Fi"),
   sem app externo.

### Decisões travadas (brainstorm)

- **Meio:** WiFi STA para roteador existente (não Ethernet, não celular).
- **STA sempre ativo quando configurado.** O rádio STA fica conectado continuamente ao roteador.
- **AP (portal cativo) só sob demanda** — mantém a lógica atual: sobe ao segurar o botão, cai no timeout.
- **AP e STA coexistem** via `WIFI_AP_STA` quando o AP precisa subir; STA nunca é derrubado pelo portal.
- **mDNS:** `irrigacao.local`.
- **Provisionamento:** form no portal, **UI copiada exatamente** do card "Rede Wi-Fi" do mockup
  `Irrigacao Mobile.dc.html` (aba Mais).

### Fora de escopo (fases futuras — NÃO tocar agora)

- **Relógio / sincronização de hora / NTP.** Vem em etapa separada. O loop NTP da base
  (`WiFiAPClient.cpp:270`) continua existindo intacto: **não modificar, não expor, não configurar.**
  Ele passa a rodar automaticamente quando o STA conecta — isso é comportamento herdado da base,
  não faz parte deste ciclo e não deve ser mexido.
- IP estático / DHCP configurável (mDNS já resolve descoberta na prática).
- Servidor NTP editável.
- Página de diagnóstico de rede.

## 2. Estado atual (base já resolve quase tudo)

O firmware base do Meshtastic **já implementa** a stack de rede necessária:

- `initWifi()` (`src/main.cpp:1125`) roda no ESP32. Se `config.network.wifi_enabled && wifi_ssid[0]`,
  entra em `WIFI_STA`, conecta ao roteador, sobe o web server e registra mDNS.
- `MDNS.begin("Meshtastic")` + `MDNS.addService("meshtastic","tcp",...)`
  (`src/mesh/wifi/WiFiAPClient.cpp:87,157`).
- Reconexão automática e persistência das credenciais em `config.network` (config oficial Meshtastic,
  sobrevive a reboot).

O que **falta** e é o trabalho desta fase:

- **Conflito de modo.** `PortalAp.cpp:23` faz `WiFi.mode(WIFI_AP)` e `softAPdisconnect(true)` no teardown —
  isso **derruba o STA**. Precisa passar a cooperar (`WIFI_AP_STA` / voltar a `WIFI_STA`).
- **Hostname mDNS** `Meshtastic` → `irrigacao`.
- **UI + endpoints** de provisionamento no portal (o card do mockup).

## 3. Arquitetura (3 camadas — padrão das fases 5a/5b)

### (A) Núcleo puro, host-tested — `PortalWifiApi`

Novo par `PortalWifiApi.{h,cpp}` (ou funções em `PortalApi` seguindo o padrão existente), sem
dependência de hardware, testável no build nativo. Reusa `JsonWriter`/`JsonReader`/`ParseResult`.

Contratos:

```cpp
namespace IrrigationWeb {

// GET /api/portal/wifi
struct WifiStatusCtx {
    bool enabled = false;        // config.network.wifi_enabled
    bool staUp = false;          // WiFi.isConnected()
    char connectedSsid[33] = {0};// "" se não conectado
    char ip[16] = {0};           // "" se sem IP
};
size_t buildWifiStatus(const WifiStatusCtx &ctx, char *buf, size_t cap);

// GET /api/portal/wifi/scan  (resultado)
struct WifiScanItem {
    char ssid[33] = {0};
    int16_t rssi = 0;
    bool secure = true;
};
struct WifiScanCtx {
    bool scanning = false;       // true → lista ignorada; frontend mostra spinner
    uint8_t count = 0;
    WifiScanItem items[16];      // cap dimensionado para caber no buffer (ver §6)
};
size_t buildWifiScan(const WifiScanCtx &ctx, char *buf, size_t cap);

// POST /api/portal/wifi/connect  (corpo)
struct WifiConnectReq {
    char ssid[33] = {0};
    char psk[64] = {0};          // vazio = rede aberta
};
ParseResult parseWifiConnect(const char *json, size_t len, WifiConnectReq &out);
// Validação: ssid não-vazio; se psk não-vazio, len >= 8 (WPA2).

// GET /api/portal/wifi/connect  (progresso)
enum class WifiConnectState : uint8_t { Idle, Connecting, Success, Error };
struct WifiConnectCtx {
    WifiConnectState state = WifiConnectState::Idle;
    char ssid[33] = {0};
    char error[48] = {0};        // "Senha incorreta." etc, quando state==Error
};
size_t buildWifiConnect(const WifiConnectCtx &ctx, char *buf, size_t cap);

// POST /api/portal/wifi/toggle  (corpo)
struct WifiToggleReq { bool enabled = false; };
ParseResult parseWifiToggle(const char *json, size_t len, WifiToggleReq &out);

} // namespace IrrigationWeb
```

`forget` não precisa de corpo (POST vazio).

### (B) Glue ESP32-only

- **`IrrigationPortalEndpoints.cpp`** — novos handlers registrados em
  `registerIrrigationPortalHandlers`:
  - `GET  /api/portal/wifi`          → status
  - `POST /api/portal/wifi/scan`     → dispara `WiFi.scanNetworks(true /*async*/)`
  - `GET  /api/portal/wifi/scan`     → `{scanning:true}` ou lista (lê `WiFi.scanComplete()`)
  - `POST /api/portal/wifi/connect`  → grava `config.network`, salva, dispara begin/reconnect
  - `GET  /api/portal/wifi/connect`  → progresso (mapeia `WiFi.status()`)
  - `POST /api/portal/wifi/forget`   → limpa credenciais
  - `POST /api/portal/wifi/toggle`   → liga/desliga `wifi_enabled`

  A leitura de `WiFi.*` e a escrita de `config.network` ficam em métodos do módulo
  (`irrigationModule->portalWifiStatus(...)`, `portalWifiConnect(...)`, etc.), mantendo o handler
  fino — igual aos handlers existentes (`hNode`, `hCoordsSet`).

- **`IrrigationModule`** — métodos novos que encapsulam o acesso a `WiFi`/`config.network`:
  - `void portalWifiStatus(WifiStatusCtx&)`
  - `void portalWifiStartScan()` / `void portalWifiScanResult(WifiScanCtx&)`
  - `bool portalWifiConnect(const WifiConnectReq&)` — grava config, persiste, sinaliza reconexão
  - `void portalWifiConnectProgress(WifiConnectCtx&)`
  - `void portalWifiForget()`
  - `void portalWifiToggle(bool)`

  Esses métodos são `#if defined(ARCH_ESP32)` (acessam `WiFi`); no native ficam stub/ausentes,
  como o restante do glue ESP32 do módulo.

- **`PortalAp.cpp`** — arbitragem de modo (§4).

### (C) Frontend estático — `data/irrigacao/portal/`

Porta o **visual exato** do card "Rede Wi-Fi" do mockup (`Irrigacao Mobile.dc.html`, aba Mais):
card na lista "Mais" com `wifiSummaryLabel` (Desativado / Conectado a X / Não conectado) e a tela
Rede Wi-Fi com os estados: `list` (toggle + lista de redes com sinal/cadeado + "Atualizar"),
`password` (input senha + Conectar/Cancelar), `connecting` (spinner), `success`, `error`
(mensagem + Tentar novamente/Cancelar), `off`. "Esquecer rede" ao tocar a rede conectada.

O mockup usa um framework state-driven (`sc-if`/`sc-for`); portar para o padrão vanilla do portal
existente (mesmo esquema de fetch dos outros cards), preservando **markup/estilos/textos** idênticos.

## 4. Arbitragem AP/STA (o coração)

`PortalAp.cpp` deixa de forçar `WIFI_AP` e passa a cooperar com o STA da base:

```
bringUp():
    if (STA configurado)  WiFi.mode(WIFI_AP_STA);   // adiciona AP sem derrubar STA
    else                  WiFi.mode(WIFI_AP);        // comportamento atual (sem roteador)
    WiFi.softAP(ssid, PIN);
    sDns.start(...);

tearDown():
    sDns.stop();
    WiFi.softAPdisconnect(true);
    if (STA configurado)  WiFi.mode(WIFI_STA);       // NÃO WIFI_OFF — mantém STA vivo
    // senão: deixa como está (base decide)
```

"STA configurado" = `config.network.wifi_enabled && config.network.wifi_ssid[0]`.

O STA em si continua governado 100% pela base (`initWifi`/reconnect via `config.network`). O portal
**nunca** liga/desliga o STA diretamente — só grava a config e deixa a base reconectar. Isso evita
duas entidades disputando `WiFi.mode()`.

**mDNS:** trocar `MDNS.begin("Meshtastic")` → hostname derivado de irrigação (`"irrigacao"`), em
`WiFiAPClient.cpp:87,157`. Avaliar se deve ser condicional ao build de irrigação (guard de macro) para
não afetar outros alvos; decisão de implementação — registrar serviço http do painel.

## 5. Persistência e reconexão

`portalWifiConnect`:
1. Copia `ssid`/`psk` para `config.network.wifi_ssid`/`wifi_psk`, seta `wifi_enabled=true`.
2. Persiste a config pelo caminho padrão (`nodeDB->saveToDisk` / mecanismo de save de config já usado).
3. Sinaliza reconexão da base (mesmo mecanismo que o admin usa ao mudar rede — `needReconnect`/
   reinit conforme a base expõe). **Reusar**, não reimplementar.

`portalWifiForget`: zera `wifi_ssid`/`wifi_psk`, `wifi_enabled=false`, persiste, deixa o STA cair.

Credenciais são a config oficial do Meshtastic → sobrevivem a reboot sem store próprio novo.

## 6. Segurança

- Endpoints atrás do portal cativo (AP WPA2 + PIN), igual fase 5b.
- **Ponto crítico:** com STA sempre-on, os `/api/portal/wifi/*` também ficam alcançáveis pela LAN.
  Isso permitiria trocar credencial WiFi remotamente por qualquer um na LAN. **Exigir a mesma
  sessão/PIN do portal** nesses endpoints (a mesma proteção que os demais `/api/portal/*` já aplicam;
  se hoje a proteção é só "estar no AP", os endpoints de escrita de rede — `connect`/`forget`/`toggle`
  — devem exigir prova de sessão do portal, não apenas alcançabilidade IP). Ver como os endpoints de
  escrita atuais (`gpo`, `coords`, `net/command`) tratam auth e seguir o mesmo contrato ou reforçar.
- **Buffers:** dimensionar os buffers de resposta para o pior caso e checar `w.done()==0`
  (padrão do módulo). Scan com muitas redes pode estourar 512 B → limitar `count` (ex.: 16) e/ou
  usar buffer de heap como `hLog` faz.
- Nunca ecoar `wifi_psk` de volta em `buildWifiStatus`/scan.

## 7. Testes

- **Nativo (host):** `test_portal_wifi_api` — `buildWifiStatus`, `buildWifiScan`, `buildWifiConnect`,
  `parseWifiConnect` (validação de SSID vazio, senha < 8, rede aberta), `parseWifiToggle`, mapeamento
  de estados de connect. Suíte **56 → 57**.
- **ESP32 (glue + scan/connect real):** CI-gated — nunca compilado localmente
  (ver `[[native-tests-need-docker-on-windows]]`). Verificar o job ESP32 tbeam/gateway após o push.
- `trunk fmt` roda em CI (não instalado localmente).

## 8. Follow-ups conhecidos (registrar, não implementar)

- Relógio/hora/NTP (fase seguinte, explicitamente adiado).
- PIN do portal configurável (já era follow-up da 5b).
- IP estático / servidor NTP editável / diagnóstico de rede (fora de escopo).
- Reconciliar o hostname mDNS com outros alvos se o guard de macro ficar amplo demais.

## 9. Mapa mockup → estados (referência de implementação)

| Mockup (`Irrigacao Mobile.dc.html`) | Endpoint / estado |
|---|---|
| Card "Rede Wi-Fi" + `wifiSummaryLabel` | `GET /api/portal/wifi` |
| Toggle liga/desliga | `POST /api/portal/wifi/toggle` |
| "Atualizar" / lista de redes | `POST` + `GET /api/portal/wifi/scan` |
| Selecionar rede segura → tela senha | frontend (sem backend) |
| Botão "Conectar" | `POST /api/portal/wifi/connect` |
| Spinner "Conectando a X…" | `GET /api/portal/wifi/connect` → `connecting` |
| "Conectado a X" | `GET .../connect` → `success` |
| "Senha incorreta…" + Tentar novamente | `GET .../connect` → `error` |
| Tocar rede conectada → esquecer | `POST /api/portal/wifi/forget` |
