# Irrigação — Fase 7a: Grupos hidráulicos (motor de orquestração)

**Design doc — 2026-07-24**
Spec de origem: `especificacao-irrigacao-mesh.md` §8.13. Roadmap: `2026-07-11-irrigacao-roadmap.md` (Fase 7).

## 1. Contexto e recorte

A Fase 7 (§8.13) atende setups em que uma bomba/válvula-mestre só opera com um número mínimo e máximo de válvulas abertas, exigindo sequenciamento coordenado. Introduz o invariante de segurança distribuído: **bomba ligada ⇒ pelo menos `min_abertas_com_bomba` válvulas confirmadamente abertas**.

Como a Fase 6, a Fase 7 é **dividida em 7a/7b**, cada metade entregando software testável sozinho:

- **7a (esta spec)** — motor de orquestração: `HydraulicGroupTable` (config + persistência), `HydraulicGroupEngine` (máquina de estados guiada por ACK), matriz de falhas, unificação com `simultaneidade` (§8.10), integração no `gwTick`, testes nativos.
- **7b (spec futura)** — endpoints web/portal + aba "Grupos" do painel; helper de config da proteção local de pressão; re-route opcional do cap global de simultaneidade para o mecanismo de grupo.

**Sem mudança na ABI de estação.** A bomba é uma zona `gpo` comum (§8.13, "A bomba é uma zona `gpo` comum — pode estar em qualquer estação da mesh"). O firmware da estação já sabe acionar GPO com timer fail-safe local (Fase 6a). Grupos são **orquestração 100% no gateway**: a blob de réplica de estação (v5, 176 B) permanece intacta.

## 2. Arquitetura escolhida

O `ProgramScheduler` atual é **guiado por tempo** (emite `OPEN`/`CLOSE` por zona conforme a duração do passo). §8.13 exige sequenciamento **guiado por ACK** (abrir V2 → ACK → sobreposição → fechar V1; atrasos de partida/parada da bomba). Um novo `HydraulicGroupEngine` fica **no caminho do comando**, entre o scheduler e o rádio:

```
ProgramScheduler (tempo)
   | OPEN/CLOSE(zona)
   v
HydraulicGroupEngine (guiado por ACK)
   | zona agrupada?  absorve → sequência bomba/válvula
   | zona livre?     passthrough (caminho atual intacto)
   v
gwSendValveCmd + CommandTracker (ACK)
```

Razões (vs. estender o scheduler ou um mini-scheduler paralelo): mantém o `ProgramScheduler` como fonte única de tempo/cronograma; isola a lógica de ACK numa unidade focada e testável nativa; segue o estilo dos módulos de propósito único do gateway (`OpenGate`, `InterlockEngine`, `MirrorMode`, colados em `IrrigationModule`).

### Modelo de conjunto-desejado

Em vez de reordenar um stream cru de comandos, o engine mantém, por grupo, o **conjunto-desejado** de zonas membro abertas (com duração) e o **conjunto-confirmado** (actual). A máquina de estados reconcilia actual → desejado via transições gated por ACK, injetando a orquestração da bomba. Isso é robusto: o scheduler apenas declara intenção ("quero V2 aberta por N s; quero V1 fechada"); o engine decide a ordem segura (abrir-antes-de-fechar com sobreposição, bomba on/off, atrasos).

Nota de sincronismo do scheduler: numa borda de passo, o drain de `gwTick` produz `CLOSE(V1)` seguido de `OPEN(V2)` **no mesmo tick** (via `pendingOpenNext`). O engine vê o par e reconcilia; não depende de reordenação entre ticks.

## 3. Data model

```cpp
struct HydraulicGroup {
    uint8_t  id = 0;             // 0 = slot vazio
    char     name[16] = {0};
    uint8_t  bombaZoneId = 0;    // zona gpo da bomba; 0 = grupo SEM bomba (== §8.10 simultaneidade)
    uint8_t  zoneIds[8] = {0};   // válvulas membro
    uint8_t  zoneCount = 0;
    uint8_t  minOpen = 1;        // min_abertas_com_bomba
    uint8_t  maxOpen = 1;        // max_abertas (0 = sem teto)
    uint8_t  transicao = 0;      // 0 = abrir_antes_de_fechar, 1 = fechar_antes_de_abrir
    uint16_t overlapS = 10;      // sobreposicao_s
    uint16_t startAfterOpenS = 5;   // partida_apos_abrir_s
    uint16_t stopBeforeCloseS = 8;  // parar_antes_de_fechar_s
    uint16_t minRunMin = 5;      // funcionamento_min_min
    uint8_t  maxStartsHour = 6;  // max_partidas_hora
};

class HydraulicGroupTable {
  public:
    static constexpr size_t MAX = 8;
    static constexpr uint32_t MAGIC = 0x49484731; // "IHG1"
    bool upsert(const HydraulicGroup &g);   // valida: zona ∈ ≤1 grupo; false = cheia/inválida
    bool removeById(uint8_t id);
    const HydraulicGroup *byId(uint8_t id) const;
    const HydraulicGroup *byZone(uint8_t zoneId) const;      // grupo que contém a zona-membro
    const HydraulicGroup *byPumpZone(uint8_t zoneId) const;  // grupo cuja bomba é essa zona
    const HydraulicGroup *groupAt(size_t index) const;
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);          // falha => tabela vazia
  private:
    HydraulicGroup groups[MAX];
};
```

Entry ≈ 39 B; 8 grupos → serialização folgada (< 512 B). Persistência: arquivo próprio `GW_GRUPOS_PATH` (staged write + rename atômico), espelhando `loadInterlocks/saveInterlocks` (Fase 6b). `loadGroups`/`saveGroups` em `IrrigationModule`.

**Validações de `upsert`**: `id != 0`; `zoneCount ∈ [1,8]`; nenhuma `zoneIds[]` já pertence a outro grupo; `bombaZoneId` não é membro do próprio grupo; `minOpen ≤ zoneCount`; `maxOpen == 0 || maxOpen ≥ minOpen`. (Validação de existência de zona/tipo-gpo da bomba fica no 7b/endpoint, quando o `ZoneTable` estiver acessível; o engine tolera zona ausente como no scheduler atual — loga e ignora.)

## 4. HydraulicGroupEngine

### 4.1 Interface

```cpp
struct GroupEmit {                 // comando concreto a enviar
    uint32_t node; uint8_t index; uint8_t tipo; // resolvidos pela zona
    uint8_t  zoneId; uint8_t action;            // 1 abrir, 0 fechar
    uint16_t durationS;
};

class HydraulicGroupEngine {
  public:
    void setDesired(uint8_t groupId, uint8_t zoneId, bool open, uint16_t durationS);
    // Chamar 1×/tick após o scheduler. Emite ≤N comandos prontos p/ envio.
    size_t tick(const HydraulicGroupTable &tbl, const ZoneTable &zones,
                uint32_t nowMs, GroupEmit *out, size_t cap);
    void noteSent(uint32_t node, uint8_t zoneId, uint8_t action, uint32_t seq);
    void onAck(uint32_t node, uint32_t ackedSeq);       // avança a máquina
    void onCmdFailed(uint32_t node, uint8_t zoneId, uint8_t action); // matriz de falhas
    // Observabilidade p/ auditoria/alertas (glue consome):
    bool takeAlert(GroupAlert &out);                    // fila curta de eventos p/ AlertCenter/AuditLog
};
```

O engine **não** chama `gwSendValveCmd` nem toca o rádio: emite `GroupEmit` que o glue envia, e recebe de volta `noteSent(seq)` e `onAck(seq)`. Correlaciona ACK por seus próprios seqs pendentes (não depende do `CommandTracker`, que já retirou o slot). Falha definitiva vem de `onCmdFailed`, chamado pelo glue no ramo FAILED de `tracker.poll`.

### 4.2 Máquina de estados (por grupo)

```
IDLE
  → (desejado não-vazio) OPENING     abre até minOpen válvulas; cada uma gated por ACK
  → PUMP_START                       espera partida_apos_abrir_s; liga bomba (dur = teto do ciclo); [ACK]
  → RUNNING                          reconcilia membros conforme desejado:
       transição (abrir_antes_de_fechar): abre nova [ACK] → espera sobreposicao_s → fecha antiga [ACK]
                                          (durante a sobreposição admite momentaneamente maxOpen+1)
       transição (fechar_antes_de_abrir): fecha antiga [ACK] → abre nova [ACK] (sem overlap)
       renovação da bomba a cada transição (§4.2 spec): re-envia bomba ON c/ nova duração
  → (desejado vazio) PUMP_STOP       desliga bomba [ACK]
  → DRAIN                            espera parar_antes_de_fechar_s
  → CLOSE_LAST                       fecha última válvula [ACK]
  → IDLE
```

- Duração embarcada na bomba = **teto do ciclo completo do grupo** (fail-safe local também na bomba, §8.13). Renovada a cada transição de zona.
- `minOpen > 1`: abre `minOpen` válvulas (as primeiras `minOpen` do desejado) antes de `PUMP_START`.
- **Edge `minOpen == maxOpen` com `fechar_antes_de_abrir`**: a transição não cabe sem violar `minOpen`. Nesse caso a bomba **para** entre zonas (`PUMP_STOP → DRAIN → troca → PUMP_START`), registrado em auditoria — comportamento explícito, não silencioso.

### 4.3 Bridging / anti-ciclagem do motor

- Partidas de bomba (transições OFF→ON) contadas numa **janela deslizante de 1 h** por grupo.
- Gap entre passos < `funcionamento_min_min` (medido desde a última partida): a bomba **permanece ligada na ponte** — nenhuma nova partida contada; ao chegar novo desejado, `RUNNING` retoma sem reiniciar a bomba.
- Partida fresca necessária mas `max_partidas_hora` estourado **ou** `minOpen` válvulas não confirmadas abertas: **defer** — válvulas do grupo ficam FECHADAS, passo adiado, alerta emitido. **Nunca abre válvula alimentada por bomba sem a bomba** (fail-safe first; pior caso = "não irrigou").

### 4.4 Matriz de falhas (§8.13)

O engine rastreia `localExpiresAtMs` por válvula aberta (= `durationS` do último open enviado; a estação fecha nesse instante pelo timer local).

| Falha | Reação do engine |
|---|---|
| Abrir a próxima falha (sem ACK após retries → `onCmdFailed`) | **Não fecha a corrente**: renova-a (re-envia open, reinicia o timer local) + alerta + retenta a próxima. Se não dá p/ renovar antes de `localExpiresAtMs` da corrente → **desliga a bomba primeiro**, depois deixa a corrente fechar. |
| Fechar a anterior falha | Não viola o invariante (válvula a mais = pressão menor). Alerta; violação persistente de `maxOpen` → **encerramento ordenado** do grupo (`PUMP_STOP → DRAIN → fecha tudo`). |
| Gateway indisponível no meio do ciclo | Timers locais fecham válvulas e bomba (duração embarcada). Engine não faz nada (está fora). No restart, reconcilia pelo actual-set do heartbeat. |
| Estação reinicia com válvula aberta | Boot em estado fechado (§5.5); o heartbeat revela a mudança; o engine desliga a bomba ou renova a sequência. O actual-set é atualizado pelo `StationTelemetryCache`. |

### 4.5 Reconciliação por telemetria

O engine deriva o **conjunto-confirmado** dos ACKs que recebe. Adicionalmente, quando um heartbeat revela que uma válvula do grupo mudou de estado fora da orquestração (reboot de estação, §5.5), o glue informa o engine via `onAck`-equivalente de observação (ou um `observeActual(node,zoneId,open)`), permitindo desligar a bomba se o invariante `minOpen` cair. Detalhe de fiação fica no plano; a spec fixa o comportamento: **queda do actual-set abaixo de `minOpen` com bomba ligada dispara parada ordenada da bomba**.

## 5. Integração no glue (`IrrigationModule::gwTick`)

1. Agregado `IrrigationGateway` ganha `HydraulicGroupTable groups` + `HydraulicGroupEngine groupEngine`.
2. No drain do scheduler: para cada `SchedAction`, `groups.byZone(a.zoneId)`:
   - **agrupada** → `groupEngine.setDesired(...)`; **não** envia direto (nem `openGate.request`).
   - **livre** → caminho atual intacto (interlock + openGate + `gwSendValveCmd`).
3. Após o drain, chamar `groupEngine.tick(...)`; para cada `GroupEmit`, enviar via `gwSendValveCmd` e `groupEngine.noteSent(node,zona,acao,txSeq)`.
   - `gwSendValveCmd` passa a expor o `txSeq` usado (retorno ou out-param) para o `noteSent`.
4. `handleGwAck` → `groupEngine.onAck(mp.from, ack.ackedSeq)` (após `tracker.onAck`).
5. Ramo FAILED de `tracker.poll` → `groupEngine.onCmdFailed(node,zona,acao)`.
6. Drenar `groupEngine.takeAlert()` → `AlertCenter` + `auditEvent` (origem nova `AuditOrigin::GRUPO_HIDRAULICO` ou reuso de `CRONOGRAMA`/`SISTEMA` — decisão no plano).
7. Persistência: `loadGroups`/`saveGroups` (`GW_GRUPOS_PATH`/`_TMP`) chamados no init e após mutações da tabela.

Interlocks continuam valendo para zonas agrupadas: antes de `setDesired`-abrir, o glue respeita `zoneVerdict.bloqueada`/`deveFechar` como hoje (uma zona bloqueada não entra no desejado; `fechar_e_bloquear` remove do desejado).

## 6. Unificação com simultaneidade (§8.10)

Um grupo com `bombaZoneId == 0` e apenas `maxOpen` definido é exatamente a regra `simultaneidade` do §8.10, para suas zonas membro. O engine trata os dois pelo mesmo mecanismo (a orquestração da bomba é simplesmente pulada quando não há bomba).

O cap **global** `IL_SIMULTANEIDADE` (via `OpenGate.setCap`, Fase 6b) permanece **intacto** para compatibilidade — ele age sobre todas as zonas, não por grupo. Migrá-lo para um grupo sem-bomba abrangente é **follow-up do 7b**, evitando desestabilizar o path de interlocks do 6b nesta metade.

## 7. Invariantes de segurança preservados

- Teto absoluto de 120 min compilado continua na estação (clampa qualquer duração).
- Bomba embarca duração = teto do ciclo do grupo; renovada a cada transição (§4.2 spec) — fail-safe local também na bomba.
- Engine nunca abre válvula alimentada por bomba sem a bomba ligada.
- Proteção local de pressão (§8.13, última linha) reusa os intertravamentos locais replicados da Fase 6a (`evalLocalInterlocks`): `pressão alta → fechar_e_bloquear` (deadhead) e `pressão baixa sustentada → fechar_e_bloquear` (operação a seco), na mesma estação da bomba. Nenhum código novo no 7a; helper de config no 7b.

## 8. Testes nativos (`./bin/run-tests.sh`)

Estilo determinístico (injeção de `nowMs` + `onAck`), espelhando `CommandTracker`/`MirrorMode`.

**`HydraulicGroupTable`**: round-trip serialize/deserialize; `byZone`/`byPumpZone`; rejeição de zona em 2 grupos; rejeição de `minOpen > zoneCount`; deserialize corrompido → tabela vazia.

**`HydraulicGroupEngine`** (sink mock de `GroupEmit` + ACK/tempo injetados):
1. Happy path `minOpen=1`: abre V1 → ACK → espera `startAfterOpenS` → bomba ON → ACK → transição V1→V2 (abrir-antes-de-fechar + overlap) → fim → bomba OFF → drain → fecha última.
2. `fechar_antes_de_abrir`: sem overlap; ordem fecha-antes-abre.
3. `minOpen > 1`: abre `minOpen` antes de ligar a bomba.
4. Abrir-próxima-falha: renova a corrente + alerta + retenta; sem renovação possível → bomba OFF primeiro.
5. Fechar-anterior-falha: alerta, sem violação; persistente → encerramento ordenado.
6. Bridging: gap < `minRunMin` mantém bomba ligada, sem nova partida contada.
7. `max_partidas_hora` estourado → defer + alerta; válvulas ficam fechadas.
8. Grupo sem bomba == cap de simultaneidade das zonas membro.
9. Estação reboot (actual-set cai < `minOpen`) → parada ordenada da bomba.
10. Renovação da bomba a cada transição (duração re-enviada).

## 9. Fora do escopo do 7a

- Endpoints web/portal + aba "Grupos" do painel (7b).
- Helper de config da proteção local de pressão (7b; a mecânica já existe na 6a).
- Re-route do cap global `IL_SIMULTANEIDADE` para o mecanismo de grupo (7b/follow-up).
- Backup/export incluindo grupos (segue quando o 7b expuser a tabela; o formato de backup já é extensível).

## 10. Riscos / decisões abertas p/ o plano

- **Fiação do `noteSent`/seq**: `gwSendValveCmd` hoje é `void` e usa `++txSeq` interno; o plano decide expor o seq (retorno) sem quebrar os ~8 call-sites existentes.
- **Observação de actual-set por heartbeat** (§4.5): a interface exata (`observeActual`) e onde chamá-la no `handleGwHeartbeat`.
- **Origem de auditoria**: nova `GRUPO_HIDRAULICO` vs. reuso — impacto no enum persistido do `AuditLog`/`FlashAuditRing` (checar se enum é serializado com valor fixo).
- **Buffer de `GroupEmit` por tick**: dimensionar `cap` (transições geram poucos comandos; teto seguro ex. 8).
