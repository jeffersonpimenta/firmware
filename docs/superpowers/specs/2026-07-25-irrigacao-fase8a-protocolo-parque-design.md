# Irrigação — Fase 8a: Pré-requisitos de protocolo no parque (SERVICE_MAGIC · RESYNC_SEQ · PING_SURVEY)

**Design doc — 2026-07-25**
Spec de origem: `especificacao-irrigacao-mesh.md` §11.5, §11.9, §4.1–§4.2, §11.4. Roadmap: `2026-07-11-irrigacao-roadmap.md` (Fase 8). Primeiro recorte da Fase 8.

## 1. Contexto e recorte

A Fase 8 introduz o role `SERVICO` (§11): gateway itinerante multi-cliente que comanda o parque inteiro sem vínculo prévio com cada nó. §11.9 lista as mudanças que **estações e gateways já instalados precisam ter no binário antes de o device SERVICO existir** — sem elas, o nó de serviço é rejeitado pelo parque. A Fase 8a entrega exatamente essa fundação de protocolo, isolada e nativa-testável, para poder ir a campo antes de qualquer trabalho no device.

Decomposição da Fase 8 (brainstorming 2026-07-25):

- **8a (este doc)** — pré-requisitos de protocolo no parque.
- **8b** — device SERVICO núcleo: cofre LittleFS, re-tune de canal, *emissor* de varredura, r/w de config (2 rotas), import/export.
- **8c** — portal do device SERVICO (§11.8): abas Clientes/Rede/Log.
- **8d** — site survey §8.5: modo beacon PING_SURVEY em qualquer nó, registro no gateway, tabela no painel.

Escopo **IN** da 8a (§11.9):

1. **SERVICE_MAGIC** na validação de origem (§4.2, §11.5) — item 1 de §11.9.
2. **`RESYNC_SEQ`** handshake request→response (§4.1, §11.5) — parte do item 2 de §11.9.
3. **`PING_SURVEY` responder** — resposta à sonda com nodeinfo mínimo, em todos os papéis (§4.1, §11.4, §11.9 item 4).

Escopo **OUT** (adiado, com destino):

- `last_seq` **per-sender** — item 2 de §11.9 **já pronto** (`SeqTable` já indexa por remetente; `lastSeq()` já existe).
- **Regra do maior epoch** + adoção via `GET_CONFIG` no gateway — item 3 de §11.9 **já pronto** (`GatewayTables::adoptConfig`, `IrrigationModule.cpp:2353`).
- **Persistência do `SeqTable`** entre reboots — decisão explícita: **manter RAM-only** (ver §2, Q2). Não é item de §11.9.
- *Emissor* da sonda (varredura) → 8b. Beacon survey + log no gateway + tabela no painel → 8d.
- Role SERVICO / cofre / re-tune / portal → 8b/8c.

**Sem mudança na ABI de estação** (settings v5, 176 B, intacto). **Protocolo VERSION permanece 1** — todas as mudanças são aditivas.

## 2. Decisões desta fase

| # | Decisão | Escolha |
|---|---|---|
| Q1 | Split da Fase 8 | 8a/8b/8c/8d; começar por 8a (fundação de campo) |
| Q2 | Persistir `SeqTable` entre reboots | **Não — RAM-only.** Sob posse-da-PSK=autoridade (§11.1), replay-no-reboot não concede poder novo (quem replica já pode originar comando fresco). Zero desgaste de flash. 8a fica mínimo |
| Q3 | Codificação do SERVICE_MAGIC | **Bit em `Header.flags`** (`FLAG_FROM_SERVICE = 0x0001`), não valor mágico de 16 bits — deixa os outros 15 bits para flags futuras. Marca ≠ segredo (posse-da-PSK=autoridade) |
| Q4 | Anti-replay do `RESYNC_SEQ` REQUEST | **Isento do gate `SeqTable`** — a query é read-only e o remetente pode ter contador defasado (galinha-e-ovo). Replay da query é inócuo (não aciona nada) |
| Q5 | Conteúdo do PING_SURVEY REPLY | `role`, `configEpoch`, `vbatCentiV`, `fwVersion`, `latE7`, `lonE7`. **Nome omitido** (estação não guarda nome; prober resolve id→nome do próprio registro). SNR/RSSI vêm do rádio no receptor |
| Q6 | Bump de VERSION | **Não.** Aditivo: nós antigos ignoram o bit novo e não têm handler p/ os tipos novos (caem no default → STOP). É por isso que §11.9 exige estes binários no parque *antes* do SERVICO |

## 3. Compatibilidade e modelo de ameaça

**Aditividade.** `decodeHeader` já lê `flags` sem validar; `senderAuthorized` hoje ignora `flags`. Um controlador novo que setar `FLAG_FROM_SERVICE` contra firmware **antigo** é tratado como comando normal (aceito só se não-vinculado ou se `from==boundGateway`) — o bypass só funciona contra firmware **atualizado**, exatamente o requisito de §11.9. `MSG_PING_SURVEY`/`MSG_RESYNC_SEQ` já constam do enum mas não têm `case` no firmware antigo → caem no default e são ignorados sem crash.

**Segurança do SERVICE_MAGIC.** O bit não autentica nada — qualquer nó no canal pode setá-lo. É decisão de projeto da spec (§11.1): a posse da PSK do canal É a autorização; não há PKI nem escopo de permissão. O bit apenas sinaliza "trate como controlador de serviço, dispense o vínculo `gateway_id`". Consequência já registrada na spec: quem detém a PSK detém controle administrativo. Nenhuma regência nova aqui — 8a só implementa o marcador.

**Airtime.** Todas as respostas novas (RESYNC REPLY, PING_SURVEY REPLY) passam pelo `RateLimiter` existente e pela mesma guarda anti-amplificação usada no NACK de versão (só responde a pacote diretamente endereçado quando aplicável / dentro do rate-limit), para não virar vetor de flood.

## 4. Wire format (`IrrigationProtocol.h/.cpp`, aditivo)

### 4.1 Flag de cabeçalho
```
constexpr uint16_t FLAG_FROM_SERVICE = 0x0001;  // Header.flags: remetente é nó de serviço (§11.5)
```
`Header.flags` (u16) já existe e é hoje sempre 0. O encoder do controlador de serviço seta este bit; a estação o lê na validação de origem.

### 4.2 `MSG_RESYNC_SEQ` (tipo 11, já no enum)
```
struct ResyncSeq {
    uint8_t  kind;     // 0 = REQUEST (CTRL→EST), 1 = REPLY (EST→CTRL)
    uint32_t lastSeq;  // válido só no REPLY: last_seq que o receptor registrou p/ o remetente do REQUEST
};
size_t encodeResyncSeq(uint8_t *buf, size_t len, uint32_t seq, const ResyncSeq &m);
bool   decodeResyncSeq(const uint8_t *buf, size_t len, ResyncSeq &out);
```

### 4.3 `MSG_PING_SURVEY` (tipo 10, já no enum)
```
struct PingSurvey {
    uint8_t  kind;         // 0 = PROBE (sonda, req), 1 = REPLY
    // campos abaixo válidos só no REPLY:
    uint8_t  role;         // IrrigationRole
    uint32_t configEpoch;
    uint16_t vbatCentiV;
    uint16_t fwVersion;    // build id compacto do firmware (p/ planejamento de OTA §3.3)
    int32_t  latE7;        // 0 se sem coordenada
    int32_t  lonE7;
};
size_t encodePingSurvey(uint8_t *buf, size_t len, uint32_t seq, const PingSurvey &m);
bool   decodePingSurvey(const uint8_t *buf, size_t len, PingSurvey &out);
```
`id` do nó = `from` do pacote (não repetido no corpo). `nome` não vai no fio (§2, Q5). O PROBE tem corpo mínimo (`kind=0`); o REPLY carrega o nodeinfo. `fwVersion`: número compacto derivado da versão de firmware Meshtastic disponível em build (fonte exata definida no plano).

## 5. Comportamento

### 5.1 SERVICE_MAGIC (validação de origem)
`senderAuthorized` ganha o estado de flags do pacote em curso:
```
bool senderAuthorized(uint32_t from, uint16_t flags) const {
    if (flags & FLAG_FROM_SERVICE) return true;           // marca de serviço dispensa vínculo (§11.5)
    return settings.boundGateway == 0 || from == settings.boundGateway;
}
```
Chamadores em `handleReceived`/handlers de comando passam `h.flags`. Comando de serviço aceito é auditado com `AuditOrigin::SERVICO` (enum já existe). Os handlers de comando (`MSG_CMD_VALVULA`, `MSG_CMD_GPO`, `MSG_REMOTE_CMD`, `MSG_SET_CONFIG`, `MSG_GET_CONFIG`, `MSG_CMD_MAINT`, aprovação de pareamento) usam a nova assinatura.

### 5.2 RESYNC_SEQ
- **Estação (responder — o pré-requisito de §11.9):** ao receber `MSG_RESYNC_SEQ kind=REQUEST`, **não** passa pelo gate `seqTable.checkAndUpdate` (§2, Q4); responde `MSG_RESYNC_SEQ kind=REPLY` com `lastSeq = seqTable.lastSeq(mp.from)`, unicast ao remetente, sujeito ao `RateLimiter`. Validação de origem: aceita se `FLAG_FROM_SERVICE` ou remetente conhecido — mas como é read-only e não altera estado, pode responder amplamente (decisão no plano; default = mesma regra de `senderAuthorized`).
- **Controlador (consumidor):** ao receber o REPLY, retoma sua numeração de saída para aquela estação em `lastSeq + 1`. O consumo pleno vive no gateway e no device SERVICO (8b); em 8a implementa-se o lado **estação** (responder) e o mínimo do lado gateway para não quebrar o dispatch (tratar/ignorar o REPLY sem efeito colateral). *O REPLY não é gated por seq no requester (info idempotente).*

### 5.3 PING_SURVEY responder
Em **qualquer papel**, ao receber `MSG_PING_SURVEY kind=PROBE`: responde `kind=REPLY` unicast ao prober com o nodeinfo mínimo (§4.3), preenchido de `settings.role`, `settings.configEpoch`, leitura de Vbat corrente, `fwVersion`, `settings.latE7/lonE7`. **Isento de auth e de seq** (sonda é broadcast; a resposta só expõe nodeinfo), sujeito ao `RateLimiter` + guarda anti-amplificação. O *emissor* da sonda (varredura do SERVICO) **não** entra em 8a.

## 6. Arquitetura / arquivos tocados

| Arquivo | Mudança |
|---|---|
| `IrrigationProtocol.h` | `FLAG_FROM_SERVICE`; structs `ResyncSeq`/`PingSurvey`; assinaturas encode/decode |
| `IrrigationProtocol.cpp` | encode/decode dos 2 corpos (padrão writer/reader existente) |
| `IrrigationModule.h` | assinatura nova de `senderAuthorized`; declaração dos handlers `handleResyncSeq`/`handlePingSurvey` |
| `IrrigationModule.cpp` | `senderAuthorized(from,flags)` + chamadores; `case MSG_RESYNC_SEQ`/`MSG_PING_SURVEY` no dispatch; helpers de resposta |

Sem arquivos novos, sem módulo novo, sem ABI de settings, sem endpoint web (portal/painel do SERVICO é 8b/8c).

## 7. Testes (nativos)

- **`test_irrigation_protocol`** (codec puro): round-trip encode→decode de `ResyncSeq` (REQUEST e REPLY) e `PingSurvey` (PROBE e REPLY); persistência do bit `FLAG_FROM_SERVICE` no header; rejeição de buffer curto.
- **Suite de handlers do módulo** (a que já exercita `handleReceived`): 
  - SERVICE_MAGIC — estação **vinculada** aceita comando com `FLAG_FROM_SERVICE` vindo de nó ≠ gateway; **sem** o bit → NACK `REASON_UNAUTHORIZED`. Auditoria origem `SERVICO`.
  - RESYNC — REQUEST **não** é descartado por anti-replay mesmo com seq baixo/repetido; REPLY carrega `lastSeq` correto p/ o remetente.
  - PING_SURVEY — PROBE gera REPLY com `role/configEpoch/vbat/coords` corretos; resposta respeita rate-limit.

Suite nativa completa deve seguir **56/56 GREEN** (sem suite nova; casos adicionados aos suites existentes). Confirmação via Docker (memória `windows-native-test-docker`).

## 8. Critérios de aceite

1. Estação atualizada aceita comando marcado `FLAG_FROM_SERVICE` de remetente não-vinculado e o audita como `SERVICO`; rejeita o mesmo comando sem a marca.
2. Estação responde `RESYNC_SEQ` REQUEST com seu `last_seq` para o remetente, sem descartar a query por anti-replay.
3. Qualquer papel responde `PING_SURVEY` PROBE com nodeinfo mínimo, rate-limited.
4. Protocolo VERSION inalterado; ABI de settings inalterada; nós antigos não quebram ao receber os tipos/flag novos.
5. Suíte nativa 56/56 GREEN.
