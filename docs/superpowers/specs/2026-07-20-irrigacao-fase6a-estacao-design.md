# Fase 6a — Sensores, GPO, tamper e mini-log na estação (§8.9–§8.12, lado estação) — Design

> Spec de referência: `docs/Spec & template/especificacao-irrigacao-mesh.md` §5.1, §8.9, §8.10, §8.11, §8.12.
> Antecessora: Fase 5b (captive portal) — `docs/superpowers/specs/2026-07-17-irrigacao-fase5b-portal-no-design.md`.
> Roadmap: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (Fase 6, dividida em 6a/6b em 2026-07-20).
> Branch: `sistema-irrigacao`.

## Objetivo

Dar à estação tudo o que a Fase 6b (motor de intertravamentos + log grande + UI do painel, lado gateway)
vai consumir: entradas de sensor declaradas no pin map com leitura calibrada reportada no heartbeat,
saída GPO fim-a-fim (comando → acionamento → estado no ACK/heartbeat), alerta de tamper, e um mini-log
de auditoria local consultável pelo portal de campo. De quebra fecha dois follow-ups conhecidos:
`gpoStates` sempre 0 (5a) e coordenadas locais aguardando ABI v4 (5b).

## Decisões de escopo (confirmadas com o usuário 2026-07-20)

1. **Fase 6 dividida em 6a (estação) / 6b (gateway)**. 6a entrega protocolo + estação; 6b (motor de
   intertravamentos global, réplica local de regras, log de auditoria dimensionado para meses no gateway,
   UI do painel) depende do que a 6a põe no ar.
2. **ABI v4 num bump só**: 4 slots de sensor + 2 pinos GPO + pino tamper + coordenadas locais (lat/lon).
   Um único bump resolve a fase inteira e o follow-up da 5b.
3. **Transporte = heartbeat + envio antecipado**. Bloco de sensores anexado ao heartbeat; mudança digital
   ou variação analógica fora da banda de histerese dispara heartbeat imediato (rate-limited). Sem
   mensagem nova; réplica local de intertravamento (6b) cobre a reação instantânea na própria estação.
4. **Janela de manutenção do tamper (nesta fase) = portal aberto**. Portal do nó ativo suprime o EVENTO
   (o estado continua indo no heartbeat). Janela ativável pelo painel fica para a 6b.

## Arquitetura — camadas da 5a/5b

### Camada A — C++ puro (testado no host)

#### `IrrigationSettings` v4 (`IrrigationSettings.h`/`.cpp`)

Blob ABI-locked cresce 52 → 128 bytes (SET_CONFIG já fragmenta). Novos campos (offsets exatos fixados
na implementação, com `static_assert` e comentário de layout como hoje):

- `int8_t pinsGpo[2]` — saídas de nível; `-1` ausente.
- `int8_t pinTamper` — entrada dedicada; `-1` desativa (§8.12).
- `uint8_t hwFlags` — bit0 = tamper ativo-baixo; demais reservados (zero).
- `int32_t latE7, lonE7` — coordenadas locais ×1e-7; `0/0` = não preenchidas (§8.8, follow-up 5b).
- `SensorSlot sensores[4]`, cada um (16 bytes):
  - `int8_t pino` (`-1` = slot vazio), `uint8_t tipo` (0 digital, 1 analógico),
  - `uint8_t flags` (bit0 ativo-baixo — digital), `uint8_t amostragemS` (analógico; 0 → default 30),
  - `uint16_t debounceMs` (digital; 0 → default 200),
  - `uint16_t adcMin, adcMax` — pontos de calibração (analógico),
  - `int16_t engMin, engMax` — valores de engenharia em **centi-unidades** (ex.: 1000 = 10,00 bar),
  - `uint8_t unidade` (enum: 0 raw, 1 bar, 2 %, 3 m, 4 °C), `uint8_t pad`.
- Nomes de sensor **não** entram no blob — ficam no gateway (6b), payload enxuto.
- `migrateIrrigationSettings`: v1/v2/v3 → v4 com novos campos em default (`-1`/zero). Bump do
  `static_assert` de tamanho (128) e da constante `version = 4`.

#### `SensorSampler` (novo — `SensorSampler.h`/`.cpp`)

Leitura + condicionamento dos 4 slots, puro via leitores injetados:

- `struct Reading { bool valid; bool digital; int16_t valueCenti; }` — digital usa 0/100 (centi de 0/1)
  para uniformizar o transporte.
- `configure(const IrrigationSettings&)`; `tick(nowMs, IAnalogReader&, IDigitalReader&)`.
- Digital: debounce por slot + polaridade. Analógico: amostragem no período do slot, interpolação linear
  `adcMin/adcMax → engMin/engMax` com clamp nas pontas.
- **Mudança significativa** → `bool earlyHeartbeatDue(nowMs)`: digital mudou de estado estável, ou
  analógico atravessou banda de histerese fixa de 2 % do span de engenharia desde o último valor
  reportado. Rate-limit compilado: mínimo 30 s entre heartbeats antecipados. `noteReported(nowMs)`
  zera a referência após o envio.

#### `GpoController` (novo — `GpoController.h`/`.cpp`)

Espelha o padrão do `ValveController`, mas para saída de **nível** (não pulso latching):

- Driver injetado `IGpoDriver { void set(uint8_t index, bool on); }`.
- `command(index, action, durationS, nowMs)`: `durationS == 0` = **biestável** (permanece até comando
  contrário — permitido pela §8.11; confirmação extra fica na UI); `> 0` = temporizado com timer
  fail-safe local que desliga sozinho (mesma disciplina da válvula). Teto compilado de 120 min **não**
  se aplica ao biestável (spec §8.11 explicita `0` = biestável).
- `tick(nowMs)` expira temporizados; `uint8_t states()` bitmap para ACK/heartbeat; `allOff()` para
  modo seguro (§5.5: GPOs inativos) e factory reset.

#### `AuditLog` (novo — `AuditLog.h`/`.cpp`, compartilhado com a 6b)

Ring de registros binários de 16 bytes, capacidade parametrizada (estação: 100; gateway 6b: milhares):

- `struct AuditRecord { uint32_t tsSecs; uint8_t origin; uint8_t action; uint8_t target; uint8_t result;
  uint32_t node; uint32_t seq; }`.
- Enums da §8.9: `origin` (cronograma, painel, portal_campo, botao_fisico, entrada_fisica,
  intertravamento, failsafe_timer, sistema, servico); `action` (abrir, fechar, pulso, gpo_on, gpo_off,
  parear, factory_reset, config_epoch, safe_mode_in/out, tamper, reboot, hibernacao_in/out);
  `result` (ok, nack+motivo, timeout).
- API pura: `append(const AuditRecord&)`, `size()`, `at(i)` (mais recente primeiro),
  `serialize(buf, cap)` / `deserialize(buf, n)` com magic+CRC (mesmo padrão do `ProgramScheduler`).
- Persistência (camada B): arquivo LittleFS único reescrito por `saveAuditLog()` com debounce de
  gravação (grava no máximo 1×/min e sempre em `factoryReset`/shutdown) — eventos de irrigação são
  raros; desgaste de flash irrelevante. Sobrevive a reboot (§8.9: útil quando a estação esteve fora
  de alcance).
- Somente-append na perspectiva da UI (sem endpoint de apagar).

#### Protocolo (`IrrigationProtocol.h`/`.cpp`) — sem bump de versão

- **Heartbeat + bloco de sensores (trailing, opcional)**: após os campos v1,
  `count(u8) + count × { id(u8), tipo(u8), valueCenti(i16) }` (máx. +17 bytes). `decodeHeartbeat`
  tolera bloco ausente (payload antigo ⇒ `sensorCount = 0`) e ignora bytes além do bloco declarado.
- **`EV_TAMPER = 6`** novo em `EventCode` (arg = 1 abriu / 0 fechou).
- `MSG_CMD_GPO` já tem codec — sem mudança de wire; ganha handler na estação (abaixo).

### Camada B — cola no `IrrigationModule` (+ arquivos ESP32-only)

- **`handleCmdGpo`** (novo, simétrico ao `handleCmdValvula`): auth (allowlist/gateway vinculado),
  anti-replay, rate-limit, `REASON_SAFE_MODE` em modo seguro, despacho ao `GpoController`, ACK com
  `gpoStates` real. Auditoria: origem derivada do remetente.
- **`gpoStates` real** em `sendAck`/`sendHeartbeat` e em `portalFillNodeState` (linha hoje hard-coded 0).
- **Sampler no `runOnce`**: `sampler.tick(...)` com leitores Arduino (`analogRead`/`digitalRead` numa
  cola fina); `earlyHeartbeatDue()` ⇒ `sendHeartbeat()` fora do ciclo.
- **Tamper**: entrada dedicada com debounce (reusa a lógica do sampler digital, slot interno);
  transição para ativo ⇒ `HB_FLAG_TAMPER` no heartbeat + `EV_TAMPER` imediato **se** portal fechado
  (`portal.apShouldBeUp() == false`); registro em auditoria sempre.
- **Auditoria plugada** nos pontos existentes: botão (manual open/close/pulse), `portalPulse`,
  `portalRunNetCommand` (origem portal_campo), `handleCmdValvula`/`handleCmdGpo` (origem sistema/painel
  via gateway), timer fail-safe do `ValveController` (fechamento autônomo), `commitPairing`,
  `factoryReset` (loga antes de apagar? — **decisão: o factory reset apaga também o log**, estação
  volta zerada; o gateway 6b guarda o histórico), entrada/saída de modo seguro, boot (reboot + causa).
- **Persistência**: `loadAuditLog()/saveAuditLog()` (FSCommon, mesmo padrão do allowlist);
  coordenadas gravadas via `saveIrrigationSettings` normal.
- **`IrrigationPortalEndpoints.cpp`** (guard `ARCH_ESP32 && !MESHTASTIC_EXCLUDE_WEBSERVER`, como 5b):
  - `GET /api/portal/sensors` — leituras correntes (id, tipo, valor, unidade).
  - `GET /api/portal/log` — mini-log (mais recente primeiro, limite 100).
  - `POST /api/portal/gpo` — `{ "gpo": n, "action": 0|1, "durationS": s }` local (biestável exige
    `confirm: true` no corpo).
  - `GET/POST /api/portal/coords` — lê/grava coordenadas locais (settings v4).
  - `PortalApi` (camada A) ganha os builders/parsers correspondentes, host-testados.

### Camada C — frontend estático (`data/irrigacao/portal/`)

Extensão da aba **Este nó**: cartões de sensores (valor + unidade, digital como ativo/inativo), GPOs
com botão liga/desliga (biestável abre diálogo de confirmação, §8.11), badge de tamper, seção
mini-log (tabela ts/origem/ação/resultado), formulário de coordenadas (lat/lon decimais). Sem
framework, mesmo estilo dos arquivos existentes.

## Fora do escopo (fica na 6b)

Motor de intertravamentos (global no gateway + réplica local na estação), log de auditoria do gateway
(meses, export CSV/JSON, backup), janela de manutenção ativável pelo painel, nomes de sensor,
UI do painel para sensores/GPO/log, enfileiramento por simultaneidade no cronograma.

## Testes (suíte nativa, 46 → 49)

- `test_sensor_sampler` — debounce, polaridade, calibração/clamp, período de amostragem, histerese,
  rate-limit do heartbeat antecipado.
- `test_gpo_controller` — biestável, temporizado + fail-safe, allOff, bitmap.
- `test_audit_log` — ring wrap, ordem, serialize/deserialize com CRC, capacidade parametrizada.
- Extensões: `test_irrigation_settings` (migração v3→v4, ABI 120B), `test_irrigation_protocol`
  (heartbeat com/sem bloco de sensores, compat retro, EV_TAMPER), `test_portal_api` (builders/parsers
  novos).
- Execução: subagent-driven TDD com revisão em dois estágios por task (como 5b). Build ESP32 validado
  só no CI (tbeam), como nas fases anteriores.
