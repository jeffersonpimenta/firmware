# Irrigação — Priorização de airtime e back-pressure

- Data: 2026-08-15
- Branch: `sistema-irrigacao`
- Status: aprovado (brainstorming)

## Problema

O canal LoRa satura. Quatro sintomas confirmados pelo usuário:

1. **Flood no epoch/cena** — várias estações disparam heartbeat/evento ao mesmo tempo quando muda config ou dispara cena.
2. **Airtime crônico alto** — uso alto mesmo em regime; risco de duty-cycle.
3. **Telemetria Meshtastic nativa** compete por airtime duplicando dados que o heartbeat de irrigação já carrega.
4. **Comando lento/perdido** — comando de válvula atrasa ou falha com canal ocupado.

Restrição dura confirmada: **comando de válvula tem prioridade absoluta**. Sob back-pressure, heartbeat/telemetria/survey cedem airtime; comando nunca é barrado.

A solução precisa **se adaptar** ao número de estações e ao preset LoRa (não conhecidos a priori).

## Diagnóstico do código atual

- `src/modules/irrigation/IrrigationModule.cpp` nunca seta `p->priority`. Os ~25 emissores repetem `service->sendToMesh(p, RX_SRC_LOCAL, false)` — comando, heartbeat, ACK, survey saem todos em `DEFAULT(64)`. O router **já ordena a fila de TX por `priority`**; falta o módulo informar a prioridade.
- Heartbeat agendado por intervalo fixo `settings.hbMinutes` (default 10 min), **sem jitter** (loop linha ~1073) → estações caem em fase e floodam juntas após reboot em massa / mudança de epoch.
- Heartbeat dispara **sem consultar utilização do canal**.
- Telemetria nativa (`DeviceTelemetry`/`EnvironmentTelemetry`) roda por fora, redundante com o heartbeat.

O tipo IrrigationProto vive em `payload.bytes[1]` (`Header.type`). A API nativa `AirTime` (`src/airtime.h`) expõe `channelUtilizationPercent()`, `utilizationTXPercent()`, `isTxAllowedChannelUtil(polite)`, `isTxAllowedAirUtil()`. O harness `test/test_traffic_management/test_main.cpp` já injeta um `airTime` stub global — back-pressure fica testável em nativo.

## Design

### 1. Choke point único + ladder de prioridade

Introduzir um wrapper que substitui os ~25 `service->sendToMesh(p, RX_SRC_LOCAL, false)`:

```cpp
// Lê o tipo IrrigationProto de p->decoded.payload.bytes[1], aplica prioridade
// e o gate de airtime (Seção 2), então envia. Libera o pacote se barrado.
// Retorna true se enviado, false se barrado/liberado.
bool IrrigationModule::txPacket(meshtastic_MeshPacket *p);
```

Mapa tipo → prioridade (função pura `priorityForType(uint8_t type, ...)`, testável):

| Tipo | Priority | Gate |
|---|---|---|
| `CMD_VALVULA`, `CMD_GPO`, `REMOTE_CMD`, `REMOTE_TRIGGER` | `HIGH (100)` | não |
| `ACK` | `RESPONSE (80)` | não |
| `EVENTO` crítico (`EV_TAMPER`, safe-mode, bateria), `CMD_MAINT` | `ALERT (110)` | não |
| `SET_CONFIG`, `GET_CONFIG`, `PAIR_ANNOUNCE`, `PAIR_GRANT`, `RESYNC_SEQ`, `REMOTE_LED` | `DEFAULT (64)` | não |
| `HEARTBEAT` | `BACKGROUND (10)` | sim |
| `PING_SURVEY` (probe/beacon/reply) | `BACKGROUND (10)` | sim |

`EVENTO` não crítico (manual open/close, test pulse, paired) fica em `DEFAULT`. Só tamper/safe-mode/bateria escalam para `ALERT`.

O comando P2P-fallback (loop ~1121, com `FLAG_FROM_SERVICE`) passa a usar `txPacket` como os demais — herda `HIGH`.

### 2. Gate de airtime (back-pressure)

Dentro de `txPacket`, para tipos com gate=sim:

```cpp
if (gated && airTime && !airTime->isTxAllowedChannelUtil(/*polite=*/true)) {
    packetPool.release(p);
    return false;   // HB/survey cedem quando canal cheio
}
```

Comando/ACK/alarme nunca são barrados (impolite — sempre passam). É o mesmo critério `isTxAllowedChannelUtil(true)` usado pela telemetria nativa.

### 3. Heartbeat adaptativo: backoff + jitter

Substitui o schedule fixo da linha ~1073.

**Backoff multiplicativo por utilização** (função pura `hbBackoffFactor(float chUtilPercent)`):

| channelUtilizationPercent | fator |
|---|---|
| < 25% | 1x |
| 25–40% | 2x |
| 40–60% | 4x |
| > 60% | 8x |

`intervaloEfetivoMs = hbMinutes*60000 * fator`. Alivia sozinho quando o canal esvazia. `earlyHeartbeatDue` (mudança significativa de sensor) mantém o gate de airtime mas **ignora** o backoff — evento relevante ainda reporta.

**Jitter/spread determinístico** (função pura `hbJitterOffsetMs(uint32_t nodeNum, uint32_t epoch, uint32_t janelaMs)`):

```
offset = hash(nodeNum ^ epoch) % janelaMs
janelaMs = min(intervaloEfetivoMs / 4, 30000)
```

Aplicado ao agendamento do heartbeat; re-semeado quando `configEpoch` muda. `nodeNum` único descorrelaciona estações; `epoch` muda a fase quando a cena chega — resolve o flood de cena sem coordenação central.

### 4. Telemetria nativa off nos papéis de irrigação

Ao adotar papel ESTACAO/GATEWAY (`settings.role`), desabilitar telemetria device/environment nativa por default (intervalo/enabled zerados no config aplicado). Heartbeat de irrigação já carrega vbat/estados/sensores. Knob permanece exposto para reativação manual.

### 5. Knobs de config (backward-compat)

Novos campos anexados **no fim** do blob de settings (mesmo padrão dos offsets pós-blob já usados no StationRegistry):

- `hbBackoffEnable` (bool, default true)
- `hbJitterEnable` (bool, default true)

Firmware antigo ignora campos ausentes; sem campo obrigatório novo. Bump documentado de versão de blob se necessário.

## Isolamento / unidades testáveis

Lógica pura extraída para funções livres (sem estado, sem `airTime` real):

- `priorityForType(type, isCriticalEvent) -> Priority`
- `isGatedType(type) -> bool`
- `hbBackoffFactor(chUtilPercent) -> uint8_t`
- `hbJitterOffsetMs(nodeNum, epoch, janelaMs) -> uint32_t`

`txPacket` e o schedule de heartbeat consomem essas funções; teste de integração usa o `airTime` stub do `test_traffic_management`.

## Testes (nativos)

1. `priorityForType` retorna a prioridade correta por tipo, inclusive evento crítico vs não crítico.
2. `txPacket` seta `p->priority` conforme o tipo do payload.
3. Gate: com `channelUtil` alto, HB/survey são barrados e liberados; comando/ACK passam.
4. `hbBackoffFactor` retorna o multiplicador certo em cada faixa (bordas 25/40/60).
5. `hbJitterOffsetMs` é determinístico, muda com epoch, sempre dentro da janela.
6. Adoção de papel de irrigação desabilita telemetria nativa.

Critério: `./bin/run-tests.sh` GREEN.

## Fora de escopo (YAGNI)

- Redesenho para polling gateway→estação (quebra P2P-fallback; grande).
- Coalescência de múltiplos heartbeats num frame.
- Ajuste dinâmico de preset LoRa.
