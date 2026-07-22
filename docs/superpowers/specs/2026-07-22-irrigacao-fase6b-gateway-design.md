# Fase 6b — Intertravamentos, fila, log dimensionado e UI do painel (gateway) — Design

> Spec: `docs/Spec & template/especificacao-irrigacao-mesh.md` (§8.9–§8.12).
> Roadmap: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (Fase 6, lado gateway).
> Antecede: Fase 6a (estação) — `docs/superpowers/specs/2026-07-20-irrigacao-fase6a-estacao-design.md`.
> Branch: `sistema-irrigacao`. Data: 2026-07-22.

## Objetivo

Fechar a Fase 6 pelo lado do gateway: **motor de intertravamentos global** (avaliado no gateway) com **réplica local na estação** (bloqueia sem rádio), **fila de simultaneidade** no cronograma, **log de auditoria dimensionado para meses** em flash com export CSV/JSON, **janela de manutenção do tamper** pelo painel, **nomes de sensor**, e a **UI do painel** para sensores/GPO/intertravamentos/log.

Decisão de escopo (usuário, 2026-07-22): **6b inteira num único design + plano** com marcos (como a 6a). Sem subdividir.

## Decisões de arquitetura (usuário, 2026-07-22)

1. **Réplica local** guardada como regras no config da estação, **Settings ABI v5**, empurradas pelo mecanismo de push/epoch existente (§5.4, `StationEntry.blob`). A estação avalia sozinha e bloqueia mesmo sem rádio.
2. **Simultaneidade/fila**: **gate de admissão + FIFO**, mantendo o cronograma sequencial (1 programa/1 zona por vez). Contagem global de saídas abertas no gateway; toda abertura passa pelo gate. Sem programas paralelos (isso é Fase 7).
3. **Log do gateway**: ring circular em flash **~16k registros / ~256KB** (LittleFS). ~5 meses a 100 eventos/dia.
4. **Nomes de sensor**: guardados **no lado do gateway** (não empurrados à estação), p/ não inchar o blob de config.
5. **Backup**: só **export do log CSV/JSON** nesta fase. Backup completo de config (§7, com PSK cifrada) fica como feature futura separada.

## Contexto do código existente (o que a 6b toca)

- `ProgramScheduler` (Fase 4): 1 programa ativo, 1 zona por vez, passos sequenciais. `tick()` devolve uma `SchedAction` (OPEN/CLOSE) por chamada. A fila/gate se acopla ao redor dele, sem reescrevê-lo.
- `StationTelemetryCache`/`StationTelemetry`: só RAM, último heartbeat por estação. **Ainda não guarda leituras de sensor** — a 6b estende para armazenar o bloco de sensores + tamper do heartbeat.
- `ZoneTable`/`StationRegistry` (`GatewayTables`): zonas (id, node, tipo, index, maxMin…) e estações (blob de config desejada v4). Padrão de tabela: `upsert/removeById/byId/serialize/deserialize` com MAGIC+CRC.
- `IrrigationWebApi` (Fase 5a): `JsonWriter`/`JsonReader` sem alocação, builders (`buildOverview/buildStations/buildZones/buildPrograms`) e parsers (`parseZoneUpsert`, `parseCommand`…), tudo host-testado. A UI 6b segue este padrão.
- `AuditLog` (Fase 6a): ring **RAM** 16B/registro, storage do chamador. Reusável na estação (cap 100). **Não** serve ao gateway: 16k×16B = 256KB em RAM é inviável no ESP32 → gateway precisa de ring em **flash**.
- `AuditRecord` (16B): `tsSecs`,`origin`,`action`,`target`,`result`,`node`,`seq`. Carrega `node` (ator remoto) — suficiente para o log do gateway. **Reusado** pelo ring de flash.
- Heartbeat (`IrrigationProtocol`): já tem bloco de sensores trailing (`sensorCount`+`SensorReading[]`) e `decodeHeartbeat` já o decodifica; `EV_TAMPER=6`. **Sem mudança de fio** — o gateway apenas passa a ler `Heartbeat.sensors`.
- Enums de auditoria: `AuditOrigin::INTERTRAVAMENTO`=6 e `AuditAction::TAMPER`=10 já existem. **Nenhum enum novo de auditoria necessário.**

## Camadas (padrão 5a/6a)

### Camada A — componentes puros (host-testados)

**1. `InterlockTable`** — CRUD persistido das regras globais. `MAGIC`+CRC, `MAX=16`.

```
struct InterlockRule {
    uint8_t  id = 0;            // 0 = slot vazio
    uint8_t  tipo = 0;          // 0=SENSOR, 1=SIMULTANEIDADE
    // --- SENSOR ---
    uint32_t node = 0;          // estação dona do sensor
    uint8_t  sensorIdx = 0;     // 0..3
    uint8_t  condicao = 0;      // 0=ATIVO,1=INATIVO (digital); 2=MENOR_QUE,3=MAIOR_QUE (analógico)
    int32_t  valorCenti = 0;    // limiar em centi-unidades de engenharia
    uint16_t histereseCenti = 0;
    uint8_t  acao = 0;          // 0=BLOQUEAR_ABERTURA, 1=FECHAR_E_BLOQUEAR
    uint8_t  zoneIds[8] = {0};  // zonas afetadas (0 = fim da lista)
    bool     todas = false;     // "*" = todas as zonas
    char     mensagem[24] = {0};
    // --- SIMULTANEIDADE ---
    uint8_t  maxAbertas = 0;    // usado quando tipo=SIMULTANEIDADE
};
```

**2. `InterlockEngine`** — avaliador puro; guarda estado **latched por regra** (histerese). Entrada: um leitor de snapshot de sensor `(node,idx) → {present, active, valueCenti}` + conjunto de zonas abertas. Saída, por zona: `bloqueada` (novo ciclo proibido) e `deveFechar` (`fechar_e_bloquear` ativo agora), mais o id da regra que disparou (para auditoria e painel). Expõe o `maxAbertas` efetivo (menor cap entre regras de simultaneidade). Determinístico, sem I/O.

- Histerese: regra analógica dispara quando o valor cruza o limiar; só desarma quando o valor retorna além de `limiar ± histereseCenti`. Estado latched persiste entre ticks (RAM; reconstrói a partir das leituras após reboot).

**3. `OpenGate`** — admissão de concorrência global (puro). Conjunto de zonas abertas + cap corrente + fila FIFO. `request(zoneId) → ADMIT|HOLD`, `release(zoneId)`, `nextAdmittable()` (desenfileira quando abre capacidade). No glue combina com o veredito do `InterlockEngine`: zona bloqueada por regra **ou** sem capacidade → HOLD/fila; capacidade livre + não bloqueada → ADMIT.

**4. `FlashAuditRing`** — log circular em flash (gateway, escala de meses). Opera sobre uma interface abstrata `ByteStore` (`read(off,buf,n)`/`write(off,buf,n)`/`size()`), com um fake em memória nos testes e um arquivo LittleFS no glue. Layout fixo ~256KB: cabeçalho (`magic`, `head`, `count`, `wrap`, `crc`) + N×16B (`AuditRecord` reusado). `append` (flush debounced), iteração **mais-recente-primeiro**, e formatação de export CSV/JSON por streaming (sem materializar tudo em RAM). Pequeno buffer de escrita em RAM.

**5. `SensorNameTable`** — nomes lado gateway, `(node, sensorIdx) → char[16]`. Persistido, `MAGIC`+CRC. Esparsa; MAX razoável (ex.: 32 entradas).

**6. `IrrigationSettings` v5** — apêndice ao config da estação: `LocalInterlock localInterlocks[4]`.

```
struct LocalInterlock {
    uint8_t  sensorIdx = 0;    // 0..3 (sensor local)
    uint8_t  condicao = 0;     // mesmos códigos do InterlockRule
    int32_t  valorCenti = 0;
    uint16_t histereseCenti = 0;
    uint8_t  acao = 0;         // BLOQUEAR_ABERTURA | FECHAR_E_BLOQUEAR
    uint8_t  saidasMask = 0;   // bits = índices de válvula/GPO locais a fechar+bloquear
    // == 0 (sensorIdx sem regra) → slot inativo
};
```

Migração v4→v5 (padrão da 6a: campos v4 preservados como prefixo, offsets inalterados; `offsetof` static_asserts fixam o layout). `StationEntry.blob` cresce de novo → arquivo de registro antigo é **rejeitado por tamanho** (fallback vazio, sem corrupção), como na 6a.

**7. `evalCondition()`** — função pura compartilhada (condição + histerese + latch) reusada pelo `InterlockEngine` (gateway) e pela réplica local (estação). Evita duplicar a lógica de histerese nos dois lados. Assinatura ~ `bool evalCondition(condicao, active, valueCenti, thresholdCenti, histCenti, bool &latched)`.

### Camada B — cola (só ESP32/gateway; `IrrigationGateway`/`IrrigationModule`)

- **Heartbeat → cache**: decodificar `Heartbeat.sensors` e o bit de tamper para dentro de `StationTelemetry`/`StationTelemetryCache` (novos campos: `SensorReading sensors[]`, `sensorCount`, `tamper`). É a fonte do snapshot do `InterlockEngine`.
- **`runOnce` do gateway**: a cada tick, avaliar `InterlockEngine` sobre o cache; para cada zona com `deveFechar`, emitir fechamento por rádio (comando CLOSE) + auditar `origem=INTERTRAVAMENTO` com a regra e a leitura; alimentar `OpenGate` com o estado de abertura corrente; e **passar toda abertura do cronograma pelo gate** (admissão + FIFO) — passo bloqueado fica pendente e retoma quando libera. Todo bloqueio/desbloqueio vai ao log.
- **Push v5**: para cada estação, montar o subconjunto de regras de intertravamento que referenciam sensores **daquela** estação como `LocalInterlock[]` no blob v5 desejado e empurrar pelo mecanismo de epoch/push existente.
- **CMD MAINT_WINDOW(durationMin)**: nova ação de comando (append-only no enum de ações de comando; **sem bump de `IrrigationProto::VERSION`**). Painel → gateway → estação abre a janela de manutenção do tamper por N minutos. Complementa a janela automática (portal aberto) da 6a.
- **Persistência do log**: cada ação/gatilho/alerta do gateway → `FlashAuditRing.append`. Arquivo LittleFS `/prefs/irrigation_audit.dat` (staging + rename como allowlist, flush debounce, padrão da 6a).
- **Réplica local (estação)**: em `IrrigationModule::runOnce`, avaliar `localInterlocks` sobre as leituras do `SensorSampler` (via `evalCondition`); ao disparar, fechar+bloquear as saídas de `saidasMask` via `ValveController`/`GpoController` e auditar `origem=INTERTRAVAMENTO`. Funciona com rádio caído — o bloqueio não depende de pacote.

### Camada C — frontend (endpoints CI-gated + estático)

- Painel (`data/irrigacao/index.html`, `app.js`, `style.css`): seção de sensores por estação (nome + valor ao vivo, do cache), controles de GPO, editor CRUD de intertravamentos, visualizador do log (filtros por origem/ação/estação + links de export CSV/JSON), botão de janela de manutenção do tamper, edição de nome de sensor.
- Builders/parsers host-testados (`IrrigationWebApi`): `buildInterlocks`, `parseInterlockUpsert`, `parseInterlockDelete`, `buildSensorsGateway`, `buildAudit` + formatadores CSV/JSON, `parseMaintWindow`, `parseSensorName`.
- Rotas em `IrrigationWebEndpoints.cpp` sob `#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER` (só CI; nunca compila no build nativo — revisar à mão), padrão 5a/6a.

## Fluxo de dados

```
Estação HB (sensores+tamper) ──rádio──▶ Gateway decode ─▶ StationTelemetryCache
                                                              │
                                       InterlockEngine ◀──────┘  (+ zonas abertas)
                                          │  veredito por zona (bloqueada/deveFechar) + cap
                        ┌─────────────────┼─────────────────────────────┐
                 deveFechar          bloqueada/cap                    log
                 CLOSE p/ rádio      OpenGate (admissão+FIFO)    FlashAuditRing (flash)
                 + auditoria              │                            │
                                    ProgramScheduler.tick ─ gate ─▶ abre ou enfileira
Gateway push v5 (LocalInterlock subset) ──rádio──▶ Estação (réplica local, fecha+bloqueia sem rádio)
```

## Mudanças de protocolo / ABI

- **Settings ABI v5** (blob cresce; migração v4→v5; static_asserts de offset).
- **Nova ação de comando MAINT_WINDOW** — append-only, **sem** bump de `VERSION`.
- **Heartbeat**: bloco de sensores já existe (6a) — gateway passa a decodificar/usar. Sem mudança de fio.
- **Enums de auditoria**: `INTERTRAVAMENTO` e `TAMPER` já existem. Sem novos.

## Testes (suíte nativa)

Novas suítes: `test_interlock_table`, `test_interlock_engine`, `test_open_gate`, `test_flash_audit_ring`, `test_sensor_name_table`. Estender: `test_irrigation_config` (v5 + migração), `test_irrigation_protocol` (MAINT + decode de sensores no gateway), `test_program_scheduler` (integração de admissão/fila), `test_web_api`/`test_portal_api` (novos builders/parsers). Bumpar `test/native-suite-count` no mesmo commit que adiciona cada suíte.

Casos-chave: histerese latch (transiente na banda não dispara/desarma), `fechar_e_bloquear` vs `bloquear_abertura`, `todas`/`*`, FIFO libera na ordem quando cap abre, ring de flash dá a volta preservando os mais recentes + export CSV/JSON corretos, migração v4→v5 preserva campos, réplica local fecha+bloqueia sem rádio.

## Marcos

1. **Núcleo de segurança (puro)**: `IrrigationSettings` v5 + `evalCondition` + réplica local (estação); `InterlockTable`; `InterlockEngine`; `OpenGate`. Suítes.
2. **Log do gateway (puro)**: `FlashAuditRing` + `SensorNameTable` + extensão do `StationTelemetryCache` (decode de sensores). Suítes.
3. **Cola do gateway**: tick do engine no `runOnce`, admissão/fila no caminho do cronograma, push v5, CMD MAINT_WINDOW, persistência do log em flash.
4. **Frontend**: builders/parsers + endpoints + UI do painel (sensores/GPO/intertravamentos/log/tamper/nomes).

## Fora de escopo (YAGNI / fases futuras)

- Backup completo de config + export cifrado com PSK (§7) → futuro.
- Grupos hidráulicos: máquina de estados bomba/válvula guiada por ACK, matriz de falhas (§8.13) → **Fase 7**.
- Cronograma multi-zona paralelo → não (só gate de admissão agora).

## Restrições globais (valem sempre)

Teto absoluto 120 min compilado nas válvulas (`ValveController::MAX_OPEN_SECONDS`); GPO biestável é a exceção prevista (§8.11). Camada A pura: nunca `Arduino.h`/WiFi/HTTP/`FSCom`; persistência só na camada B. Payload rádio ≤ `IrrigationProto::MAX_PAYLOAD` (200B). Sem bump de `IrrigationProto::VERSION`. Cola HTTP sob `#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER`. Comentários em português; identificadores no estilo dos vizinhos. `trunk fmt` roda no CI (não instalado localmente). Suíte nativa nesta máquina precisa de Docker (ver memória `native-tests-need-docker-on-windows`).

## Defaults ajustáveis

`InterlockTable::MAX=16`; `LocalInterlock[4]` por estação; `mensagem[24]`; `zoneIds[8]` por regra; `SensorNameTable` ~32 entradas; ring de flash ~256KB/~16k registros.
