# Janela de acesso (Portal AP + BLE) no boot + botão — economia de energia nos nós de campo

Data: 2026-08-11 · Fase: economia de energia · Papéis afetados: `ESTACAO`, `REPETIDOR`

## Problema

Nos nós de campo (estação e repetidor) o Portal AP e o Bluetooth ficam
disponíveis o tempo todo, consumindo energia sem necessidade — a maioria do
tempo ninguém está conectado. Queremos que Portal AP **e** BLE fiquem ativos
só durante uma janela compartilhada, aberta no boot e reaberta ao apertar o
botão. Fora da janela, ambos desligados de verdade.

Gateway e device Serviço mantêm o comportamento atual (sem mudança).

## Restrição de plataforma (fixa o desenho)

ESP32 **não** religa BLE limpo em runtime (`src/platform/esp32/main-esp32.cpp:117`:
"no way to recover from bluetooth shutdown without reboot"). Consequências:

- Desligar BLE de verdade = `config.bluetooth.enabled = false` (só em RAM) →
  `nimbleBluetooth->deinit()` → `esp32ReleaseBluetoothMemoryIfUnused()`
  (`esp_bt_mem_release(BTDM)`, libera RAM, irreversível até reboot).
- `shouldReleaseBluetoothMemory()` retorna `!config.bluetooth.enabled`
  (`main-esp32.cpp:79`) — por isso zeramos a config em RAM **antes** de liberar.
- Depois do release, `setBluetoothEnable(true)` do PowerFSM vira no-op
  (checa `config.bluetooth.enabled` e `bluetoothMemoryReleased`). O teardown é
  *sticky* até reboot — exatamente o desejado.
- A config só muda em RAM; o flash mantém `bluetooth.enabled = true`, então o
  próximo boot sobe BLE de novo e reabre a janela.
- Portal AP não tem essa trava: `portalApLoop()` já sobe/desce o AP conforme
  `PortalSession::apShouldBeUp()`.

Decisão do usuário: **reboot ao apertar o botão é aceitável** (Abordagem B).
Trade-off aceito: reabrir a janela após o release custa um reboot (~1,5 s de
downtime) em troca de BLE totalmente desligado (RAM liberada) fora da janela.

## Escopo

- **Vale para:** `role ∈ {ESTACAO, REPETIDOR}` **e** `provisioned == true`
  (membro `IrrigationModule::provisioned`, setado no boot = `!safeMode`).
  Nó de fábrica / não provisionado mantém BLE + portal vivos para o wizard e o
  pareamento (§6) — **sem** teardown.
- **Não toca:** `GATEWAY`, `SERVICO`; WiFi STA client (NTP/clima); nada
  persistido em flash.

## Componentes

### 1. `PortalSession` (reuso, puro — `PortalSession.h/.cpp`)

Já modela a janela: `requestOpen(now)`, `noteClient(now, anyClient)`,
`tick(now)`, `apShouldBeUp()`, timeout `PORTAL_TIMEOUT_MS` (10 min por
inatividade). **Sem mudança de interface.** A única diferença de uso: quem
chama `noteClient` passa a considerar cliente BLE conectado como atividade
(ver componente 3).

### 2. Política pura de acesso (novo — testável)

Função/estrutura pura, sem Arduino/WiFi/BLE, para concentrar a decisão e
permitir teste nativo. Sugestão de assinatura (nome final a critério da
implementação):

```cpp
namespace AccessWindowPolicy {

// Papel elegível ao regime de janela (estação/repetidor provisionados).
bool eligible(IrrigationRole role, bool provisioned);

// Transição OPEN->CLOSED numa etapa: precisa derrubar o BLE agora?
bool shouldTearDownBle(bool eligible, bool windowOpenNow, bool windowWasOpen, bool bleAlreadyReleased);

enum class ButtonAction : uint8_t { NONE, REOPEN_LIVE, REBOOT_TO_REOPEN };

// SHORT press num nó elegível:
//  - BLE ainda vivo  -> REOPEN_LIVE (requestOpen, sem reboot)
//  - BLE já liberado -> REBOOT_TO_REOPEN (boot novo = janela nova)
ButtonAction buttonShortAction(bool eligible, bool bleReleased);

} // namespace AccessWindowPolicy
```

Regras:
- `shouldTearDownBle` = `eligible && windowWasOpen && !windowOpenNow && !bleAlreadyReleased`.
- `buttonShortAction`: `!eligible → NONE`; `eligible && !bleReleased → REOPEN_LIVE`;
  `eligible && bleReleased → REBOOT_TO_REOPEN`.

### 3. Estado no `IrrigationModule`

- `bool bleReleasedThisBoot = false;`
- `bool apWasUp = false;` — nível anterior de `portal.apShouldBeUp()` p/ detectar borda.
- `uint32_t reopenRebootAtMs = 0;` — 0 = sem reboot agendado (reusa o padrão
  `rebootAtMsec` existente se preferir; manter consistência).

### 4. Glue ESP32 de teardown (`#ifdef ARCH_ESP32`)

Sequência única, chamada só na borda OPEN→CLOSED de nó elegível:

```cpp
config.bluetooth.enabled = false;            // RAM only — flash intacto
if (nimbleBluetooth) nimbleBluetooth->deinit();
esp32ReleaseBluetoothMemoryIfUnused();       // agora shouldRelease()==true
bleReleasedThisBoot = true;
```

`esp32ReleaseBluetoothMemoryIfUnused()` já é declarada no glue ESP32; expor via
header acessível ao módulo se ainda não estiver (ver `target_specific.h`).

## Fluxo

1. **`setup()`** — se `eligible(role, provisioned)`: `portal.requestOpen(millis())`.
   Abre a janela no boot. BLE sobe normalmente (flash tem `bluetooth.enabled=true`);
   o AP sobe pelo `portalApLoop()` porque `apShouldBeUp()` fica true.

2. **`portalApLoop()` (PortalAp.cpp)** — `noteClient(now, anyClient)` onde
   `anyClient = (sApUp && WiFi.softAPgetStationNum() > 0) || (nimbleBluetooth && nimbleBluetooth->isConnected())`.
   Assim a janela renova enquanto houver cliente no AP **ou** no BLE.

3. **`runOnce()`** — depois do `portalApLoop()`, detectar borda:
   `bool up = portal.apShouldBeUp();` se `shouldTearDownBle(eligible, up, apWasUp, bleReleasedThisBoot)`
   → executar o teardown ESP32 (componente 4). `apWasUp = up;`. O AP já foi
   derrubado pelo próprio `portalApLoop()` quando `apShouldBeUp()` virou false.

4. **`onButtonEvent(SHORT)`** — para nó elegível (estação pareada **ou**
   repetidor provisionado): `buttonShortAction(eligible, bleReleasedThisBoot)`:
   - `REOPEN_LIVE` → `portal.requestOpen(millis())` (portal + BLE seguem vivos).
   - `REBOOT_TO_REOPEN` → LED de confirmação + `rebootAtMsec = millis() + 1500`.

   Adicionar branch para `REPETIDOR` no `onButtonEvent` (hoje cai no early-return
   `if (role != ESTACAO) return;`). Estação de fábrica (`boundGateway==0`)
   continua abrindo a janela de pareamento como hoje — **não** entra nesse regime.

## Janela

Reusa `PORTAL_TIMEOUT_MS` (10 min por inatividade, renovado por cliente AP ou
BLE). Ajustável em fase futura; não parametrizar agora (YAGNI).

## Testes

- **Nativo (`./bin/run-tests.sh`)**: novo `test/test_access_window_policy/` cobrindo
  `eligible`, `shouldTearDownBle` (todas as combinações de borda/flag) e
  `buttonShortAction`. Segue o padrão `void setup()/loop()` do repo
  (portduino é dono do `main`).
- **Glue ESP32** (deinit/release/reboot, `noteClient` com BLE): atrás de
  `#ifdef ARCH_ESP32`, sem cobertura nativa; validar no harness de hardware (MCP)
  numa placa `custom_irrigation`.

## Fora de escopo / não-objetivos

- Comportamento de `GATEWAY` e `SERVICO`.
- WiFi STA client (NTP/clima) — inalterado.
- Parametrização da duração da janela.
- Persistir a desativação de BLE em flash (mantém RAM-only por design).

## Riscos

- **Reboot no botão após release**: perde uptime e re-inicializa subsistemas.
  Aceito pelo usuário. Mitigado pelo delay curto (1,5 s) e LED de confirmação.
- **Release irreversível no boot**: se a janela fechar cedo demais e o usuário
  ainda precisar do BLE, precisa apertar o botão (→ reboot). Comportamento
  esperado, alinhado a "conexão feita só no boot e ao apertar a tecla".
