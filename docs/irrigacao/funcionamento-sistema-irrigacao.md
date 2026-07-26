# Sistema de Irrigação LoRa Mesh — Documento de Funcionamento

> Documento explicativo e detalhado de como o sistema de irrigação funciona:
> arquitetura, papéis, protocolo, persistência, segurança, cada subsistema e o
> estado de implementação. Escrito sobre o firmware Meshtastic (fork
> `jeffersonpimenta/firmware`, branch `sistema-irrigacao`).
>
> **Fontes canônicas:** especificação `myfork/especificacao-irrigacao-mesh.md`
> (v0.1); código em `src/modules/irrigation/`; frontends em `data/irrigacao/`;
> planos/specs por fase em `docs/superpowers/{specs,plans}/`.
>
> Este documento descreve *como o software funciona hoje*. Quando a spec pede
> algo que ainda é follow-on, isso está marcado explicitamente na seção 16.

---

## Índice

1. [Visão geral e princípios de projeto](#1-visão-geral-e-princípios-de-projeto)
2. [Glossário rápido](#2-glossário-rápido)
3. [Arquitetura de firmware](#3-arquitetura-de-firmware)
4. [Protocolo de aplicação](#4-protocolo-de-aplicação)
5. [Configuração e persistência](#5-configuração-e-persistência)
6. [Segurança e pareamento](#6-segurança-e-pareamento)
7. [A estação (role ESTACAO)](#7-a-estação-role-estacao)
8. [O gateway (role GATEWAY)](#8-o-gateway-role-gateway)
9. [Grupos hidráulicos (bomba / válvula mestre)](#9-grupos-hidráulicos-bomba--válvula-mestre)
10. [Equipamento de serviço (role SERVICO)](#10-equipamento-de-serviço-role-servico)
11. [Pesquisa de cobertura (site survey)](#11-pesquisa-de-cobertura-site-survey)
12. [Interfaces web (painel e portal)](#12-interfaces-web-painel-e-portal)
13. [Modo híbrido / espelhamento (24VAC)](#13-modo-híbrido--espelhamento-24vac)
14. [Parâmetros padrão consolidados](#14-parâmetros-padrão-consolidados)
15. [Estado de implementação por fase](#15-estado-de-implementação-por-fase)
16. [Limitações conhecidas e follow-ons](#16-limitações-conhecidas-e-follow-ons)
17. [Build, testes e verificação](#17-build-testes-e-verificação)
18. [Mapa de arquivos do módulo](#18-mapa-de-arquivos-do-módulo)

---

## 1. Visão geral e princípios de projeto

O sistema comanda **válvulas de irrigação** por uma rede **mesh LoRa** (Meshtastic),
para fazendas grandes, sem depender de internet e com enlaces de vários km.

Três tipos de nó físico:

- **Gateway-controlador** — nó central com alimentação de fonte. Roda o
  cronograma, envia comandos, cobra ACK, monitora a rede, guarda a auditoria
  central e serve o painel web. Opcionalmente espelha as saídas 24VAC de um
  controlador comercial existente (migração).
- **Estações de válvula** — nós remotos alimentados por painel solar. Acionam
  solenoides *latching*, leem sensores locais e executam lógica fail-safe
  autônoma, mesmo sem rádio.
- **Repetidores** (opcionais) — apenas retransmitem o canal da fazenda.

Há ainda um quarto papel, itinerante e sem I/O de aplicação, o **nó de serviço**
(§10), usado para manutenção multi-cliente em campo.

### Quatro princípios que governam todas as decisões

1. **Fail-safe primeiro.** Nenhuma falha de comunicação pode deixar uma válvula
   aberta indefinidamente. O pior cenário aceitável é *"não irrigou"* — nunca
   *"inundou"*. Toda abertura carrega uma duração máxima e a estação fecha
   sozinha ao expirar o timer **local**, independentemente do rádio. Há um teto
   absoluto **compilado no firmware de 120 min** que nenhuma config ou comando
   ultrapassa (`IrrigationProto::MAX_OPEN_SECONDS = 120*60`).
2. **Autonomia local.** A inteligência de segurança mora na estação. O rádio é
   meio de comando e sincronização, nunca dependência.
3. **Convergência automática.** Configurações dessincronizadas se corrigem
   sozinhas por um mecanismo de *epoch* (§5). Presença física só é exigida no
   pareamento.
4. **Offline por padrão.** 100% funcional sem internet. O painel e o portal são
   servidos pelo próprio ESP32.

---

## 2. Glossário rápido

| Termo | Significado |
|---|---|
| **Zona** | Unidade lógica que o cronograma e as entradas físicas enxergam. Resolve para (nó, tipo, índice de saída). É o que o usuário programa. |
| **Estação** | Perfil lógico de um nó de campo no gateway. O ID Meshtastic do nó é um *atributo substituível* do perfil (troca de placa = novo nó + mesmo perfil). |
| **Válvula** | Saída de solenoide latching (ponte H). Abre com pulso em A, fecha com pulso em B. |
| **GPO** | *General Purpose Output* — saída digital genérica (relé/MOSFET): portão, bomba, luz, sirene. Herda todo o arcabouço das válvulas. |
| **Epoch** | Contador monotônico de versão de config por estação. Base da convergência (§5.4). |
| **PSK** | Chave AES-256 do canal privado da fazenda. Sua posse é a autoridade de administração. |
| **Vínculo** | Associação estação ↔ gateway gravada no pareamento; a estação só aceita comandos do seu gateway (ou do nó de serviço). |
| **Grupo hidráulico** | Bomba/válvula mestre + zonas + regras de sequenciamento coordenado (§9). |
| **Role** | Papel do nó (`ESTACAO`, `GATEWAY`, `REPETIDOR`, `SERVICO`), persistido em settings, definido no provisionamento. |
| **Núcleo comum** | Pilha idêntica em qualquer nó (protocolo, cripto, persistência, portal, LED, botão). |
| **Banca 2+ nós** | Teste de bancada com dois ou mais rádios reais, exigido antes de campo para tudo que não é testável no host. |

---

## 3. Arquitetura de firmware

### 3.1 Binário único com papel (role)

Existe **um único binário**. O firmware é um *núcleo comum a todos os papéis* (a
"pilha cliente", idêntica em qualquer nó) mais módulos ativados pelo campo
`role`, que cobre exclusivamente o que depende do hardware da placa.

**Núcleo comum (ativo em todos os papéis):** protocolo de aplicação, cripto,
anti-replay e versionamento; persistência com epoch e escrita atômica;
pareamento/vínculo/allowlist/reset de fábrica; captive portal de campo (inclusive
a aba de Rede, para comandar o sistema a partir de qualquer nó); modo pesquisa de
cobertura; heartbeat/telemetria, mini-log, LED de status e botão multifunção.

**Módulos ativados pelo `role`** (enum `IrrigationRole`, em `IrrigationSettings.h`):

| Role | Valor | Papel |
|---|---|---|
| `ESTACAO` | 0 | Ponte H, sensores, tamper: aciona válvulas/GPOs com timer fail-safe local, lê sensores e aplica intertravamentos locais. |
| `GATEWAY` | 1 | Alimentação de fonte, RTC (opcional) e entradas 24VAC: motor de cronograma, coordenação da rede, painel web, auditoria central. |
| `REPETIDOR` | 2 | Sem I/O de aplicação; só retransmite o canal. Não entra na allowlist de comando. |
| `SERVICO` | 3 | Sem I/O de aplicação, bateria própria: gateway itinerante de manutenção multi-cliente (§10). |

Trocar o papel **não exige recompilar** — apenas reprovisionar (respeitando o pin
map da placa).

### 3.2 Integração com o Meshtastic

- Comunicação de aplicação por **portnum privado** (`PRIVATE_APP`, faixa 256–511).
  O `IrrigationModule` é um `SinglePortModule` (um único portnum).
- **Canal privado com PSK AES-256 própria por fazenda**, gerada aleatoriamente no
  primeiro boot do gateway (nunca chave padrão). Pacotes de terceiros não
  decriptam.
- Payloads binários compactos (≤ ~200 bytes úteis por pacote) — `MAX_PAYLOAD =
  200` — respeitando o airtime do LoRa.

### 3.3 O `IrrigationModule`

Classe central: `IrrigationModule : public SinglePortModule, private
concurrency::OSThread` (`src/modules/irrigation/IrrigationModule.h`). Três pontos
de entrada:

- **`wantPacket(p)`** — filtro: aceita só pacotes do nosso portnum.
- **`handleReceived(mp)`** — decodifica o header (`IrrigationProto::decodeHeader`)
  e faz *dispatch* por `header.type` para os handlers (`handleCmdValvula`,
  `handleSetConfig`, `handlePairAnnounce`, `handleGwHeartbeat`,
  `handlePingSurvey`, `handleResyncSeq`, …). Handlers de comando passam pelo
  `senderAuthorized()` antes de agir; handlers de pareamento e de sonda são
  isentos por projeto (a autorização deles é física / read-only).
- **`runOnce()`** — laço periódico (`OSThread`): amostra sensores, tica timers de
  válvula/GPO, roda o `gwTick()` quando é gateway (1×/s), emite heartbeat, tica o
  beacon de survey, atualiza LED e persiste o que ficou *dirty*.

O objeto é global (`extern IrrigationModule *irrigationModule`). A UI física (LED
+ botão) roda numa thread separada de 25 ms (`IrrigationUiThread`) que chama
`onButtonEvent()` e `ledOnNow()`.

### 3.4 Particionamento de flash (preparado para OTA)

A OTA em si é futura (§9 da spec, Fase 9), mas o particionamento já é adotado
porque mudá-lo depois exigiria reflash físico de todo o parque:

| Partição | Finalidade |
|---|---|
| `nvs` | Configurações, credenciais, contadores, sequências |
| `otadata` | Seleção da partição de app ativa |
| `app0` / `app1` | Firmware ativo + slot de atualização (duplas, mesmo tamanho) |
| `littlefs` | Arquivos estáticos do painel/portal (HTML/CSS/JS) e dados grandes (auditoria, cofre) |
| `coredump` | Diagnóstico pós-pânico |

Regra de **rollback automático**: imagem nova entra como *pendente* e só é
confirmada após boot saudável (init completa + primeira transmissão de heartbeat
na estação, ou painel no ar no gateway). **UI e firmware são desacoplados**:
atualizar só os arquivos de `littlefs` não toca a aplicação.

---

## 4. Protocolo de aplicação

Arquivo: `src/modules/irrigation/IrrigationProtocol.h` / `.cpp`
(namespace `IrrigationProto`).

### 4.1 Cabeçalho

Todo pacote começa com um header de **8 bytes** (`HEADER_LEN = 8`):

```c
struct Header {
    uint8_t  version;  // VERSION = 1
    uint8_t  type;     // MsgType
    uint16_t flags;    // bit0 = FLAG_FROM_SERVICE
    uint32_t seq;      // sequência anti-replay do remetente
};
```

- `version` fixo em **1**. Mismatch → rejeição segura.
- `flags` carrega `FLAG_FROM_SERVICE = 0x0001` quando o remetente é o nó de
  serviço. `setServiceFlag(buf, len)` carimba esse bit no offset 2 de um pacote já
  codificado.
- `seq` é o número de sequência monotônico **por remetente** (anti-replay).

### 4.2 Tipos de mensagem

`enum MsgType` (13 tipos):

| # | Tipo | Direção | Conteúdo principal |
|---|---|---|---|
| 1 | `CMD_VALVULA` | CTRL → EST | id da válvula, ação (0 fechar / 1 abrir), **duração máx (s)** |
| 2 | `CMD_GPO` | CTRL → EST | id do GPO, ação, duração (0 = biestável) |
| 3 | `ACK` | EST → CTRL | seq confirmado, status/motivo, estados, Vbat, epoch |
| 4 | `HEARTBEAT` | EST → GW | estados, Vbat, Vpainel, RSSI/SNR, reboots+causa, flags, epoch, sensores |
| 5 | `SET_CONFIG` | CTRL → EST | bloco de config **fragmentável**, epoch alvo, CRC32, totalLen |
| 6 | `GET_CONFIG` | CTRL → EST | solicita config vigente (resposta com epoch + CRC) |
| 7 | `EVENTO` | EST → GW | alerta assíncrono (código + arg) |
| 8 | `PAIR_ANNOUNCE` | EST → CTRL | anúncio de pareamento (nome, versão) |
| 9 | `PAIR_GRANT` | CTRL → EST | concessão: PSK(32), nome do canal, gatewayId |
| 10 | `PING_SURVEY` | qualquer | sonda/beacon (kind: 0 sonda, 1 reply, **2 beacon de cobertura**) |
| 11 | `RESYNC_SEQ` | CTRL → EST | consulta `last_seq` (kind: 0 request, 1 reply) |
| 12 | `REMOTE_CMD` | portal → GW | comando de zona vindo da aba Rede do portal de campo |
| 13 | `CMD_MAINT` | painel → GW → EST | janela de manutenção do tamper (min; 0 = fechar) |

`CTRL` = o nó controlador da transação: o gateway em operação normal, ou o nó
`SERVICO` quando presente.

Cada tipo tem `encode*`/`decode*` que devolvem/consomem bytes totais (header +
corpo). Decodifica-se sempre `decodeHeader` primeiro, depois o corpo conforme o
`type`.

### 4.3 Regras obrigatórias

- **Fail-safe embutido no comando.** Toda abertura carrega a duração máxima. A
  estação fecha ao expirar o timer local. Valor efetivo =
  `min(comando, config_local, teto_firmware_120min)`.
- **Anti-replay.** `seq` monotônico por remetente, mantido no receptor em
  `SeqTable` (`last_seq[remetente]`). Pacote com seq já visto/antigo é descartado
  e registrado. **A `SeqTable` é RAM-only** (não persiste): a decisão de projeto é
  que *posse-da-PSK = autoridade*, então replay-no-reboot não concede poder novo.
- **ACK de aplicação.** O gateway reenvia comandos não confirmados até N
  tentativas (`CommandTracker`, padrão 3 / 8 s) e gera alerta em caso de falha.
- **Renovação.** Um comando de abrir recebido com a saída **já ativa** não é erro:
  reinicia o timer fail-safe local com a nova duração. É o que permite ao gateway
  estender com segurança um acionamento em curso (usado pelos grupos hidráulicos).
- **Rate limiting no receptor.** Máx. N comandos/min (`RateLimiter`, padrão 10).
- **Validação de origem.** Predicado puro, sem estado:

  ```c
  inline bool senderAuthorizedBy(uint16_t flags, uint32_t from, uint32_t boundGateway) {
      if (flags & FLAG_FROM_SERVICE) return true;        // marca de serviço dispensa vínculo (§11.5)
      return boundGateway == 0 || from == boundGateway;  // posse-da-PSK / vínculo (§4.2)
  }
  ```

  Como o pacote só chega decifrado a quem possui a PSK, **a posse da PSK constitui
  a autoridade de administração**. O gateway só processa mensagens de nós
  registrados ou do nó de serviço.
- **Bateria mínima.** Abaixo do limiar, a estação **recusa abrir** (NACK com
  motivo `REASON_BATTERY_LOW`) mas **sempre aceita fechar**.

### 4.4 NACK e motivos

`ACK` carrega `status` (`ACK_OK` / `ACK_NACK`) e um `reason`:

`REASON_BATTERY_LOW`, `REASON_RATE_LIMIT`, `REASON_INVALID_ID`,
`REASON_UNAUTHORIZED`, `REASON_BAD_VERSION`, `REASON_BAD_PAYLOAD`,
`REASON_SAFE_MODE`, `REASON_BAD_CRC`, `REASON_CONFIG_TOO_BIG`,
`REASON_FRAG_INVALID`, `REASON_COMMIT_FAIL`.

### 4.5 Comportamento sob negação de serviço (flood/jamming)

- Comando de **abrir** não entregue → ciclo perdido, alerta de falha de ACK.
- Comando de **fechar** não entregue → irrelevante: o timer local fecha a válvula.
- Ausência de heartbeats → alerta de estação muda (§8.3).
- SNR/RSSI e utilização de canal expostos no painel para diagnóstico de
  interferência.

---

## 5. Configuração e persistência

### 5.1 Três camadas de configuração

1. **Perfil de hardware (pin map), por nó** — funções lógicas (slots) mapeadas a
   pinos; `-1` = ausente. Ponte H (arrays A/B pareados), GPO, botão, LED, ADC de
   bateria/painel, tamper, sensores. Validações da UI: conflito de pino; GPIOs
   34–39 do ESP32 são só entrada (proibidos em saídas); pinos de boot/strapping
   com aviso.
2. **Tabela de zonas (roteamento lógico), no gateway** — a zona resolve para
   (nó, tipo válvula/gpo, índice). O campo opcional `fonte` habilita o modo
   híbrido (disparo por entrada 24VAC lida por optoacoplador).
3. **Registro de estações (identidade lógica)** — perfis lógicos onde o ID
   Meshtastic é um atributo substituível. Inclui **coordenadas** manuais
   (`lat/lon/descricao`) para navegação por GPS até o equipamento.

### 5.2 O blob de settings (ABI v5, 176 bytes)

A config persistida de um nó é uma struct de layout fixo, `IrrigationSettings`
(`IrrigationSettings.h`). É a mesma coisa que trafega no `SET_CONFIG` pelo rádio e
o que grava em flash — por isso é **ABI-locked**: qualquer mudança de layout exige
*bump* de `version` e dos `static_assert`.

```
Layout (176 bytes, v5):
  0  magic "IRR1"(4) | 4  version(2)=5 | 6  role(1) | 7  numValves(1)
  8  boundGateway(4) | 12 hbMinutes(2) | 14 vbatMinAbrirCentiV(2) | 16 maxOpenConfigS(2)
 18  cmdRatePerMin(1) | 20 pulseMs(2)
 22  pinsHbridgeA[8] | 30 pinsHbridgeB[8]
 40  configEpoch(4) | 44 pinsDigitalIn[4] | 48 digitalInActiveLow(1) | 49 pinBtn | 50 pinLed
 52  pinsGpo[2] | 54 pinTamper | 55 hwFlags | 56 latE7(4) | 60 lonE7(4)
 64  sensores[4]×16(64)                                   ← calibração 2 pontos, tipo, debounce
128  localInterlocks[4]×12(48)                            ← réplica local de intertravamento
Total = 176
```

A struct evoluiu por versões (v1=40 B, v3=52 B, v4=128 B, v5=176 B).
`migrateIrrigationSettings(raw, n, out)` promove blobs antigos para a struct atual
— nós antigos ignoram campos novos, novos leem antigos com defaults. Isso é o que
permite que **a Fase 8 seja aditiva** sobre parques já instalados.

### 5.3 Sincronização por epoch (§5.4)

1. Toda edição de config de estação no gateway incrementa `config_epoch`.
2. A estação persiste config + epoch e reporta o epoch em **todo heartbeat e ACK**.
3. Gateway vê `epoch_estacao < desejado` → reenvia a config (com ACK) até
   convergir.
4. **Regra do maior epoch (edição fora de banda):** se o gateway vê
   `epoch_estacao > epoch_local`, **não sobrescreve** — a estação foi configurada
   por um nó de serviço. Ele faz `GET_CONFIG`, adota a config recebida, alinha seu
   contador e registra em auditoria com origem `servico`
   (`GatewayTables::adoptConfig`).

O painel mostra o estado por estação: `sincronizada` / `pendente` /
`inalcançável`.

### 5.4 Escrita atômica e modo seguro

- **Escrita atômica em dois passos:** config nova em chave de *staging* →
  validação por CRC32 → troca da chave ativa → limpeza. Queda de energia no meio
  nunca corrompe a config ativa.
- A estação **só envia o ACK de config após o commit** (ACK = gravado, não só
  recebido).
- NVS vazia/corrompida → boot em **modo seguro**: válvulas fechadas, GPOs
  inativos, só aceita pareamento; flag no anúncio e no LED.
- `SET_CONFIG` é fragmentável (`FragmentReassembler` remonta os fragmentos;
  `FRAG_DATA_MAX = 160` por fragmento) e validado por CRC32 do blob completo antes
  do commit.

### 5.5 Backup exportável

O painel exporta um **JSON único** (estações, zonas, programas, intertravamentos,
grupos, log recente, PSK) — `gwBuildBackup()` → `GET /api/irrigation/export`.
Importável num gateway substituto; as estações reconciliam por epoch. Esse mesmo
formato de backup, sob um envelope multi-cliente, é o perfil que o nó de serviço
guarda no cofre (§10).

---

## 6. Segurança e pareamento

### 6.1 Três camadas independentes

1. **PSK AES-256 por fazenda** (canal privado): isola fazendas vizinhas por
   criptografia.
2. **Vínculo nó ↔ gateway** (`Allowlist` no gateway; `boundGateway` na estação):
   a estação descarta comandos de qualquer outro remetente. Permite até duas
   fazendas do mesmo dono compartilharem PSK sem comando cruzado.
3. **Sequência anti-replay** por par (`SeqTable`).

### 6.2 Fluxo de adoção (posse física obrigatória)

Arquivos: `Pairing.h/.cpp` (`StationPairing`, `GatewayPairing`), handlers
`handlePairAnnounce` / `handlePairGrant`, `commitPairing()`.

1. Nó novo liga em modo fábrica (sem chave; não aceita comandos).
2. **Botão físico** no nó abre janela de pareamento de 2 min → emite
   `PAIR_ANNOUNCE`.
3. Usuário aprova no painel do gateway; o gateway responde com `PAIR_GRANT`
   (PSK da fazenda + gatewayId) **pelo próprio rádio LoRa**, num canal de
   pareamento com chave conhecida. A exposição momentânea é risco tolerado: a
   janela dura 2 min, só existe após o botão físico e só é atendida mediante
   aprovação explícita.
4. Nó grava credenciais (commit atômico), reinicia no canal da fazenda; o gateway
   o adiciona à allowlist e associa a um perfil de estação.

Os handlers de pareamento **não** passam por `senderAuthorized` — a janela física
+ botão é a autorização (spec §6).

### 6.3 Reset de fábrica e exportação da chave

- **Reset de fábrica** (botão 10 s, `factoryReset()`): apaga credenciais e
  configs e **força o fechamento das válvulas no pin map antigo** antes de zerar (o
  bug clássico de "reset abre válvula em latching" é evitado por projeto).
- **Exportar a chave da fazenda** (`logFarmKey()`): pressão longa no botão do
  gateway despeja a PSK primária em base64 + nome do canal no console serial, para
  cadastro no cofre do nó de serviço (§11.2). No painel, a mesma função é exposta
  com PIN.
- Portal Wi-Fi sempre com WPA2 + PIN; AP desliga após inatividade.

---

## 7. A estação (role ESTACAO)

A estação é onde mora a segurança. Ela funciona **sozinha** mesmo sem rádio.

### 7.1 Acionamento de válvulas — `ValveController` + `GpioValveDriver`

- Solenoides **latching**: `GpioValveDriver::pulse(index, open)` dá um pulso de
  `pulseMs` (padrão 60 ms) no pino A (abrir) ou B (fechar) da ponte H.
- `ValveController` mantém, por válvula, o estado desejado e o **timer fail-safe
  local**. Ao expirar, fecha sozinho — sem depender de nenhum pacote.
- **No boot**, `forceCloseAll()` emite um pulso de fechamento incondicional em
  todas as válvulas. Válvula latching pode estar fisicamente aberta após uma
  queda; o boot não sabe o estado real, então fecha por garantia. (Esse ponto foi
  um *critical* corrigido na Fase 1.)
- Teto de abertura efetivo = `min(comando, maxOpenConfigS, 120min)`.

### 7.2 GPOs — `GpoController` + `GpioGpoDriver`

Saídas de nível (relé/MOSFET). Herdam duração máxima opcional (`0` = biestável,
permanece até comando contrário — exige confirmação extra na UI), ACK, auditoria e
acionamento por painel/portal/cronograma.

### 7.3 Sensores — `SensorSampler`

Declarados no pin map (até 4 slots, `SensorSlot` de 16 B cada):

- **Digital:** nível lógico com debounce (padrão 200 ms) e polaridade
  configuráveis (boia, pressostato, sensor de chuva).
- **Analógico:** ADC com calibração linear de dois pontos
  (`adc_min/max → eng_min/max` + unidade) e período de amostragem (padrão 30 s).

Valores vão no heartbeat (bloco de sensores trailing) e aparecem no painel e no
portal.

### 7.4 Intertravamentos locais (réplica) — última linha sem rádio

Quando uma regra de intertravamento envolve um sensor **da própria estação**, ela
é **replicada localmente** (campo `localInterlocks[4]` do settings, 12 B cada). A
estação bloqueia/fecha **mesmo sem rádio**:

- `bloquear_abertura` — impede novos ciclos.
- `fechar_e_bloquear` — interrompe ciclos em curso.

Condições: `ATIVO`, `INATIVO`, `MENOR_QUE`, `MAIOR_QUE` (com histerese para
analógico). O gateway remonta e empurra essas regras via
`gwRebuildLocalInterlocks()`.

### 7.5 Botão multifunção e LED — `ButtonGesture` + `LedPattern`

Gestos (`ButtonGestureDetector`):

| Gesto | Ação |
|---|---|
| Curta (pareado) | Sobe o captive portal (10 min) |
| Dupla | Acionamento manual local: abre válvula 0 pela duração padrão; nova dupla fecha |
| Longa (3 s) | Teste de pulso (abre 10 s, fecha automático) |
| 10 s | Reset de fábrica (confirmação por LED) |
| Nó de fábrica | Qualquer pressão abre a janela de pareamento |

LED de status (`LedPatternController`, tabela única no firmware):

| Padrão | Significado |
|---|---|
| 1 piscada / 5 s | Normal, pareado, sincronizado |
| 2 piscadas / 5 s | Sem contato com o gateway |
| 3 piscadas / 5 s | Config pendente ou modo seguro |
| Piscada rápida contínua | Janela de pareamento / portal ativo |
| Aceso fixo | Válvula ou GPO aberto |
| SOS | Bateria crítica / hibernação |

### 7.6 Heartbeat, eventos e tamper

- **Heartbeat** (`sendHeartbeat`, padrão 10 min): estados, Vbat, Vpainel,
  RSSI/SNR, contador de reboots + causa, flags (tamper/safe/hibernação), epoch e
  bloco de sensores.
- **EVENTO** assíncrono (`sendEvento`): manual open/close, teste de pulso,
  pareado, reset de fábrica, tamper.
- **Bateria em níveis** com histerese: aviso (12,2 V) → destaque; crítico
  (11,8 V) → recusa abrir; hibernação (11,5 V) → fecha tudo, heartbeat 1/h, sai ao
  recarregar acima da histerese.
- **Contador de reboots + causa** (power-on, brownout, watchdog, pânico, manual);
  taxa anômala (> N em 24 h) gera alerta.
- **Tamper** (`tickTamper`): entrada de switch de tampa; abertura fora de janela
  de manutenção gera `EV_TAMPER` de alta prioridade com nome + coordenadas. A
  janela de manutenção é aberta pelo painel (`CMD_MAINT`) ou automaticamente
  enquanto o portal daquele nó está aberto.

### 7.7 Mini-log local — `AuditLog`

Ring de 100 registros em RAM (`AUDIT_CAP = 100`), consultável pelo portal de campo
— útil quando a estação esteve fora de alcance. Persistido periodicamente.

---

## 8. O gateway (role GATEWAY)

O gateway coordena a rede. O estado agregado vive em `IrrigationGateway gateway`,
só significativo quando `role == GATEWAY`. As tabelas ficam em `GatewayTables`.

### 8.1 Tabelas — `GatewayTables`

- **`ZoneTable`** — zonas (roteamento lógico → nó/tipo/índice).
- **`StationRegistry`** — perfis de estação (identidade, params, epoch,
  coordenadas), com iterador `nodeAt`.
- **`ProgramTable`** — cronograma semanal.

Persistidas em flash; CRUD exposto por `gwApplyZoneUpsert/Delete`,
`gwApplyProgramUpsert/Toggle/Delete`.

### 8.2 Cronograma — `ProgramScheduler`

Cronograma semanal (dias, horários, sequência de zonas e durações). O cronograma
**só enxerga zonas**.

> **Restrição de hardware (usuário, 2026-07-12):** o gateway pode **não ter RTC**.
> O scheduler idle graciosamente sem hora de parede; o sistema segue 100%
> operacional via comandos manuais, portal e **modo espelho** (que usa só
> `millis()`). `computeLocalSecs()` é a fonte única de hora local: devolve `false`
> quando não há RTC.

### 8.3 Confiabilidade de comando — `CommandTracker`

Reenvia comandos não confirmados (padrão 3 tentativas / 8 s) e gera alerta de
falha de ACK. `gwSendValveCmd(...)` retorna o `txSeq` usado.

### 8.4 Monitoramento e alertas — `StationMonitor` + `AlertCenter`

- **Estação muda:** ausência de heartbeat por `silencio_alerta_min` (padrão 35 min
  = 3,5 × HB) → alerta com nome + **coordenadas**.
- **Bateria** com histerese (0,2 V) nos limiares.
- **Reboot anômalo:** taxa alta em 24 h → alerta.
- **Degradação de enlace:** tendência de queda de SNR (média móvel) gera alerta
  preventivo antes da perda total.
- `StationTelemetryCache` guarda a última telemetria por estação para o painel.

### 8.5 Admissão e intertravamentos globais — `InterlockEngine` + `OpenGate`

- **`InterlockEngine`** avalia as regras com visão global (veredito por zona +
  histerese), incluindo `simultaneidade` (máx. de saídas abertas).
- **`OpenGate`** é o gate de admissão: antes de abrir, checa bloqueios; zonas
  bloqueadas por simultaneidade são **enfileiradas** (FIFO, execução sequencial)
  em vez de sobrepor. Todo bloqueio/desbloqueio vai à auditoria com a regra e a
  leitura que disparou.
- `InterlockTable` persiste as regras; `SensorNameTable` guarda nomes de sensores
  (gateway-side, não empurrado às estações).

### 8.6 Auditoria persistente — `FlashAuditRing`

Buffer circular em **flash** (`/prefs/irrigation_audit.dat`, ~256 KB / 16384
slots, meses de operação). Toda ação é registrada com timestamp, origem
(`cronograma`, `painel`, `portal_campo:<no>`, `botao_fisico`,
`entrada_fisica:<gpio>`, `intertravamento`, `failsafe_timer`, `sistema`,
`servico`, `grupo_hidraulico`), ator, ação, alvo, resultado e seq. Exportável em
CSV/JSON pelo painel; incluído no backup. É **somente-append** na perspectiva da
UI.

### 8.7 Reconciliação de epoch

`gwReconcileEpoch(node, remoteEpoch)` implementa a §5.3: reenvia config a quem
está atrás; adota via `GET_CONFIG` quem está à frente (edição por nó de serviço).
Cooldowns são *node-keyed* (não por posição na allowlist).

---

## 9. Grupos hidráulicos (bomba / válvula mestre)

Atende setups em que um motor/bomba só opera bem com um número mínimo e máximo de
válvulas abertas, exigindo sequenciamento coordenado. Introduz um **invariante de
segurança distribuído: bomba ligada ⇒ pelo menos `min_abertas_com_bomba` válvulas
confirmadamente abertas.**

Arquivos: `HydraulicGroupTable` (config + persistência própria em LittleFS),
`HydraulicGroupEngine` (a máquina de estados).

### 9.1 Configuração

A bomba é uma **zona `gpo` comum** — pode estar em qualquer estação da mesh. O
grupo agrupa bomba, zonas e parâmetros: `min_abertas_com_bomba`, `max_abertas`,
`transicao` (`abrir_antes_de_fechar` padrão / `fechar_antes_de_abrir`),
`sobreposicao_s`, `partida_apos_abrir_s`, `parar_antes_de_fechar_s`,
`funcionamento_min_min`, `max_partidas_hora`.

### 9.2 Execução — máquina guiada por ACK

O cronograma continua enxergando zonas. Quando um programa toca zonas de um grupo,
o gateway **serializa a execução** respeitando `max_abertas` e injeta a
orquestração da bomba como uma máquina de estados onde **nenhum passo ocorre sem a
confirmação (ACK) do anterior**:

```
abrir V1 → [ACK] → aguardar partida_apos_abrir_s → ligar bomba → [ACK]
  → irrigar V1 …
  → abrir V2 → [ACK] → aguardar sobreposicao_s → fechar V1 → [ACK]
  → irrigar V2 … (repete)
  → fim da última zona: desligar bomba → [ACK]
  → aguardar parar_antes_de_fechar_s → fechar última válvula
```

A bomba nunca parte contra tudo fechado nem para com fluxo bloqueado. O comando de
ligar a bomba embarca como duração máxima o **teto do ciclo completo** (fail-safe
local também na bomba); o gateway a **renova** a cada transição de zona.

`funcionamento_min_min` e `max_partidas_hora` protegem o motor de ciclagem curta
(pedidos próximos mantêm a bomba ligada na "ponte" em vez de religar).

### 9.3 Matriz de falhas

| Falha | Reação |
|---|---|
| Abrir a próxima válvula falha (sem ACK após retries) | Não fechar a corrente: renová-la, alertar, retentar. Se não der para renovar antes do timer expirar → **desligar a bomba primeiro**, depois deixar a válvula fechar. |
| Fechar a anterior falha | Não viola o invariante (válvula a mais = pressão menor). Alerta; violação persistente de `max_abertas` → encerramento ordenado. |
| Gateway indisponível no meio | Timers locais fecham válvulas e bomba. |
| Estação reinicia com válvula aberta | Boot fecha tudo; o heartbeat revela e o gateway desliga a bomba ou renova. |

### 9.4 Roteamento único e proteção local

- **`routeZoneToGroup(zoneId, open, durationS)`** é o ponto **único** de decisão:
  zona de grupo → motor (`setDesired`); zona livre → caminho normal. Isso corrige
  o bug real de "open-direto-sem-bomba" nos caminhos manual/portal (antes só o
  scheduler era *group-aware*). Zona de grupo é isenta do cap global de
  simultaneidade — o grupo se autolimita por `max_abertas` (Opção A).
- **Proteção local (sem rádio):** recomenda-se um sensor de pressão **na mesma
  estação que aciona a bomba**, com intertravamentos locais replicados: pressão
  alta → `fechar_e_bloquear` (deadhead); pressão baixa sustentada →
  `fechar_e_bloquear` (operação a seco). A integridade do motor nunca depende de um
  pacote chegar.

### 9.5 Unificação

A regra `simultaneidade` (§8.5) é o caso particular de um grupo **sem bomba** (só
`max_abertas`) — tratada pelo mesmo mecanismo.

---

## 10. Equipamento de serviço (role SERVICO)

Nó portátil (ESP32 + rádio, bateria própria, sem I/O de aplicação) que atua como
**gateway itinerante multi-cliente**: guarda credenciais de vários clientes,
sintoniza o canal de um deles e lê/escreve config dos nós ao alcance — sem
internet e sem contato físico. Um único device atende todo o parque.

> **Modelo de autoridade:** a posse da PSK do canal é a autorização. Sem PKI,
> assinatura ou consentimento do cliente — decisão de projeto em favor da
> simplicidade. Consequência registrada: quem detém o device detém controle
> administrativo sobre todas as fazendas cujos perfis ele contém.

### 10.1 Cofre de clientes — `ServiceVault` + `LittleFsProfileStore`

Reside em **LittleFS** (não NVS). Estrutura:

```
/clientes/index.json    → [{ id, nome, canal, gateway, arquivo }]
/clientes/<id>.json     → perfil completo (backup §5.5 do cliente)
/clientes/<id>.seq      → contadores de sequência (arquivo separado)
/log/servico.jsonl      → log local append-only
```

Regras: **parsing em streaming** (um cliente por vez); os `.seq` vivem em arquivo
separado (gravados a cada comando, evitam reescrever o perfil e desgastar flash);
toda gravação usa **staging + rename atômico**. O `IProfileStore` é a interface
injetável (há `RamProfileStore` para teste e `LittleFsProfileStore` para produção,
que é no-op sem FSCom).

### 10.2 Troca de canal em runtime — `ServiceController::applyRetune`

No Meshtastic o **slot de frequência deriva do nome do canal**. Selecionar um
cliente exige re-tunar o rádio (nome + PSK + `modem_preset`), não só trocar a
chave. Como o firmware não aplica isso sem reinício, o comportamento é **persistir
o cliente ativo e reiniciar** (poucos segundos). `applyRetune` espelha o
`commitPairing`: aplica canal + `config.lora.modem_preset` + `reloadConfig` +
reboot em 3 s. Após o re-tune, aguarda o *settle time* antes de transmitir.

### 10.3 Varredura de rede — `svcEmitProbe`

Não há escuta simultânea de múltiplas frequências, então a varredura **itera os
clientes**: aplicar canal → emitir `PING_SURVEY` (kind=0, sonda, com
`FLAG_FROM_SERVICE`) → escutar pela janela (§14) → registrar quem respondeu (id,
nome, role, epoch, Vbat, versão, SNR/RSSI). A sonda é necessária porque o
heartbeat (10 min) é longo demais para escuta passiva. **Todos os papéis respondem
à sonda** com nodeinfo mínimo (`handlePingSurvey`).

### 10.4 Sequências — `RESYNC_SEQ`

Quando o device não conhece seu `seq` para um par (device novo, perfil importado,
`.seq` perdido), ele consulta o `last_seq` que o receptor registrou
(`handleResyncSeq`, **isento do gate anti-replay** — é query read-only, evita o
problema do ovo-e-galinha) e retoma de `last_seq + 1`. Sem isso o device ficaria
permanentemente rejeitado por anti-replay.

### 10.5 Leitura e escrita de configuração — duas rotas

1. **Via gateway (padrão):** o device envia `SET_CONFIG` **ao gateway** do
   cliente, que incrementa o epoch e propaga normalmente. Fonte de verdade única.
   → **Follow-on** (ver §16): o wire ainda não tem "configurar estação X via
   gateway".
2. **Direta à estação (gateway inacessível — implementada):** o device escreve na
   estação com `epoch = atual + 1` e marca a alteração como pendente no perfil. Ao
   reencontrar o gateway, a **regra do maior epoch** puxa a config da estação e
   alinha o contador, registrando origem `servico`.

Leitura (`GET_CONFIG`) é sempre direta e não altera epoch. O device remonta o
`SET_CONFIG` de resposta via `reasm` e o **cacheia sem aplicar em si mesmo**
(`handleSvcSetConfigReply`) — ao contrário da estação, que aplicaria. Pode
exportar a config lida no formato de backup (clonar estação a ser substituída).
`svcPortalSeedConfig` faz *read-before-write* para não zerar `boundGateway`/epoch
ao editar.

### 10.6 Import / export — `ServiceBackup`

Formato: o JSON de backup (§5.5) sob um **envelope multi-cliente**, incluindo `seq`
e `snapshot_epoch`. `ServiceBackup` faz base64 + um scanner JSON estrutural
aninhado (porque o `JsonReader` do web é *flat-only*): `parseEnvelope`, `validate`,
`extractLight`, `buildClientBackup`, `planMerge`. Semântica: **merge por `id` de
cliente** (sobrescreve homônimos, preserva os demais), com `--replace` explícito. O
arquivo é validado integralmente em área temporária antes de qualquer
substituição.

Vias: **portal Wi-Fi** (`GET/POST` — caminho principal); **botão SERVICO longo**
despeja o envelope no serial (bancada, `svcExportToConsole`). Cartão SD e o parser
serial completo são follow-on.

### 10.7 Portal do device (§11.8) — `ServicePortalApi` + `ServicePortalEndpoints`

No role `SERVICO`, o portal comum ganha três abas (frontend em
`data/irrigacao/portal/`, reveladas quando `role == 3`):

- **Clientes:** lista e seleção do cliente ativo (dispara re-tune + reinício),
  varredura, import/export.
- **Rede do cliente ativo:** nós detectados com estado; por nó — ler config,
  **editor de campos completo** (settings v5 inteiro, round-trip 176 B
  byte-idêntico via `buildStationConfig`/`parseStationConfig`), escrever config
  (rota direta), teste de pulso, acionar zona, `RESYNC_SEQ`.
- **Log:** `servico.jsonl` local. Sem RTC, os registros carimbam `uptime` e
  adotam o timestamp do gateway quando disponível.

Os endpoints `/api/portal/service/*` são **CI-only** (excluídos do build nativo).

### 10.8 Impacto no parque instalado (§11.9) — garantido pela Fase 8a

Para o nó de serviço ser aceito por estações/gateways **já instalados**, quatro
itens tinham de estar nos binários **antes** da distribuição (todos prontos):

1. Aceitação de comando por `FLAG_FROM_SERVICE` na validação de origem.
2. `last_seq` por remetente + resposta a `RESYNC_SEQ`.
3. Regra do maior epoch com adoção via `GET_CONFIG` no gateway.
4. Resposta à sonda `PING_SURVEY` com nodeinfo mínimo, em todos os papéis.

Por isso a Fase 8 é **aditiva** (protocolo VERSION=1 e ABI v5 intactos): nós
antigos ignoram a flag/tipos novos.

---

## 11. Pesquisa de cobertura (site survey)

Permite mapear qualidade de sinal antes de instalar estações/repetidores.
Arquivos: `SurveyBeacon.h` (state machine), `SurveyLog.h` (ring RAM), glue no
módulo.

- Qualquer nó pode entrar em **modo beacon**: emite `PING_SURVEY` **kind=2**
  (BEACON) a cada N s com potência normal. O valor `kind=2` é aditivo — os nós
  **não respondem** a ele (sem tempestade de airtime); só o gateway loga
  passivamente.
- **`SurveyBeacon`** controla intervalo (padrão 5 s) e auto-expiração (padrão
  5 min); `tick()` no `runOnce` dispara `emitSurveyBeacon()`.
- O **gateway** registra cada beacon recebido em **`SurveyLog`** (ring RAM,
  capacidade 32, transiente) com SNR (`rx_snr*4`), RSSI e, se informada, a
  coordenada do ponto (digitada no portal, opcional). O branch kind==2 em
  `handlePingSurvey` é isento de auth/seq (como o `handleResyncSeq`).
- **UI:** aba "Cobertura" no painel do gateway (tabela do log ao vivo, poll 3 s,
  botão Limpar) e no portal compartilhado (controle iniciar/parar). Endpoints
  `GET /api/irrigation/survey` + `POST .../survey/clear` (painel) e
  `POST /api/portal[/service]/survey/{start,stop}` (portais) — todos CI-only.

---

## 12. Interfaces web (painel e portal)

### 12.1 Arquitetura em camadas

Existe uma separação deliberada, ditada pela testabilidade no host:

- **`IrrigationWebApi` (puro, native-tested):** um JSON writer/reader **feito à
  mão** (o repo não tem ArduinoJson; `jsoncpp` não é native-safe). Toda a lógica de
  montar/parsear payloads (`build*`/`parse*`) vive aqui e é coberta pela suíte
  `test_irrigation_webapi`. O mesmo vale para `ServicePortalApi`.
- **Endpoints (CI-only):** `IrrigationWebEndpoints.cpp`,
  `IrrigationPortalEndpoints.cpp`, `ServicePortalEndpoints.cpp` — a cola HTTP que
  registra rotas no `ContentHandler` do servidor HTTPS do Meshtastic
  (`esp32_https_server`). Ficam sob `#if !MESHTASTIC_EXCLUDE_WEBSERVER` e são
  **excluídos do build nativo** (`variants/native/portduino.ini`), então **nunca
  compilam no host** — só no CI/ESP32.
- **Frontend estático (LittleFS):** `data/irrigacao/` (painel) e
  `data/irrigacao/portal/` (portal compartilhado). Páginas ricas no navegador do
  cliente; falam com o firmware por *polling* de endpoints JSON.

Os endpoints do gateway são *gated* em `IrrigationRole::GATEWAY`; os do portal
SERVICO em `role == SERVICO`.

### 12.2 Painel web do gateway (`data/irrigacao/`, §7.1)

Abas: **Estações** (registro, pareamento pendente, estado, coordenadas, pin map em
"avançado", troca de nó, teste de pulso), **Zonas** (roteamento, `max_min`/
`padrao_min`, tipo, fonte física), **Programas** (cronograma), **Intertravamentos**,
**Sensores**, **GPO**, **Grupos** (CRUD + status ao vivo + abrir/fechar manual),
**Tamper**, **Log** (auditoria com filtros, export CSV/JSON), **Cobertura**
(survey). Toda mudança remota só é dada como aplicada após ACK com epoch
confirmado; a UI mostra o estado da convergência. Header tem botão de download do
backup.

### 12.3 Captive portal de campo (`data/irrigacao/portal/`, §7.2 — todos os papéis)

Botão físico sobe um AP Wi-Fi (`Irrigacao-<nome>`); DNS cativo abre a página.
Abas: **Este nó** (estado vivo, config local, teste de pulso, coordenadas),
**Rede** (envia comandos pelo rádio ao gateway e às estações — `REMOTE_CMD` —,
operando o sistema inteiro a partir de qualquer nó; autenticado pela PSK/vínculo, a
posse física do botão é a autorização), **Cobertura** (survey). No role SERVICO, as
três abas extras da §10.7 são reveladas. `PortalSession`/`PortalAp`/`PortalApi`
gerenciam o ciclo de vida do AP; desliga após inatividade. mDNS
(`http://<nome>.local`) como alternativa.

---

## 13. Modo híbrido / espelhamento (24VAC)

Recurso de **migração**: o sistema pode operar **só espelhando** as entradas
físicas 24VAC de um controlador comercial existente, sem cronograma próprio.
Arquivo: `MirrorMode.h/.cpp`.

- O gateway lê entradas 24VAC por optoacoplador (campo `fonte` na zona, §5.2) e
  replica o estado em saídas de qualquer estação — o **mapeamento
  entrada→(nó,saída)** é configurável na UI, assim como a **polaridade** ativo-alto
  / ativo-baixo por entrada.
- **Quando o espelho está ativo, TODA configuração e TODOS os intertravamentos são
  bypassados** — a escrava replica cegamente o estado da entrada do mestre
  (requisito reforçado pelo usuário, 2026-07-12).
- Usa **apenas `millis()`** (sem RTC), então mantém o sistema operacional mesmo em
  gateway sem relógio.
- **Exceção de segurança:** o teto fail-safe compilado de 120 min permanece mesmo
  em espelhamento (salvo override explícito do usuário). A renovação periódica do
  comando cobre acionamentos longos.

---

## 14. Parâmetros padrão consolidados

| Parâmetro | Padrão | Onde |
|---|---|---|
| Teto absoluto de abertura | **120 min (compilado)** | Firmware da estação |
| Heartbeat | 10 min | Config da estação |
| Alerta de silêncio | 35 min (3,5 × HB) | Gateway |
| Retentativas de comando | 3 (/ 8 s) | Gateway |
| Pulso da ponte H | 60 ms | Pin map |
| Bateria: aviso / bloqueio / hibernação | 12,2 / 11,8 / 11,5 V | Config da estação |
| Rate limit de comandos | 10/min | Firmware da estação |
| Janela de pareamento | 2 min | Firmware |
| Timeout do portal Wi-Fi | 10 min sem cliente | Firmware |
| Reset de fábrica | botão 10 s | Firmware |
| Sobreposição na transição (grupo) | 10 s | Config do grupo |
| Partida da bomba após abrir | 5 s | Config do grupo |
| Parada da bomba antes de fechar | 8 s | Config do grupo |
| Funcionamento mínimo da bomba | 5 min | Config do grupo |
| Máx. de partidas da bomba | 6/h | Config do grupo |
| Escuta da sonda de varredura | 4 s por cliente | Firmware SERVICO |
| Intervalo do beacon de survey | 5 s | Firmware |
| Timeout do beacon de survey | 5 min | Firmware |
| Settle time após re-tune | 500 ms | Firmware SERVICO |

---

## 15. Estado de implementação por fase

O trabalho foi dividido em 9 fases; cada uma entrega software funcional e testável.
**Fases 1–8 concluídas; Fase 9 é futura.**

| Fase | Escopo | Estado |
|---|---|---|
| 1 | Núcleo do protocolo + estação fail-safe (mensagens binárias, anti-replay, rate limit, `ValveController`, ACK, heartbeat, role) | ✅ concluída |
| 2 | Persistência atômica, epoch, `SET/GET_CONFIG` fragmentável, modo seguro | ✅ concluída |
| 3 | Pareamento, vínculo, allowlist, reset de fábrica, botão, LED | ✅ concluída |
| 4 | Gateway: registro de estações, zonas, cronograma, retries, alertas, `MirrorMode` | ✅ concluída |
| 5 | Painel web (5a) + captive portal de campo (5b) | ✅ concluída (aba Instalador §8.4 cortada) |
| 6 | Sensores, intertravamentos (global + réplica local), GPO, tamper, auditoria — estação (6a) + gateway (6b) | ✅ concluída |
| 7 | Grupos hidráulicos: motor guiado por ACK (7a) + painel/roteamento (7b) | ✅ concluída |
| 8 | Role SERVICO: protocolo no parque (8a), cofre/núcleo (8b), portal (8c), site survey (8d) | ✅ concluída |
| 9 | Particionamento de flash para OTA nas variants alvo | ⏳ futura |

Estado de teste registrado após a Fase 8d: **suíte nativa completa 916/916 GREEN,
62 suites** (Docker, exit 0). Ver §17.

Detalhes fase a fase estão em `docs/superpowers/{specs,plans}/*irrigacao*`.

---

## 16. Limitações conhecidas e follow-ons

Itens que a spec prevê mas que hoje são **follow-on** (marcados no código e nos
planos):

- **Config VIA_GATEWAY para estação X:** a rota de escrita do nó de serviço é
  **DIRECT** (epoch+1, adotado pela regra do maior epoch). Falta o comando
  gateway-side "configurar estação X" para a rota via gateway.
- **Aprovar pareamento pelo device de serviço:** falta o intake device-side de
  `PAIR_ANNOUNCE`.
- **Cifra da PSK no backup:** hoje o backup é *plaintext everywhere* (a cifra §5.5
  por senha do usuário foi adiada).
- **Cartão SD** (import/export via `import.json` na raiz) e **parser serial
  completo** de IMPORT — hoje só portal + botão→console.
- **`gwTimeAdopted`** é stub (retorna 0): o carimbo temporal adotado do gateway
  (via HB/ACK) para o log do nó de serviço ainda não está fiado.
- **`sensorNames` no backup** sai como `[]` (sem builder dedicado).
- **OTA** (transferência/aplicação de imagens, via Wi-Fi e via mesh) — Fase 9.

Fora de escopo desta versão (roadmap §9 da spec): ajuste sazonal, pausa por chuva,
sensor de fluxo/detecção de vazamento, relatórios de consumo, mapa da mesh, gateway
de backup, ponte MQTT/internet, umidade de solo no cronograma, fertirrigação,
cronograma local de contingência na estação.

> **⚠️ Banca 2+ nós exigida antes de campo.** Tudo que é comportamento de **rádio
> real** — pareamento (canal de grant), re-tune/varredura do nó de serviço,
> GET/SET_CONFIG entre binários, SNR/RSSI, beacon/log de survey — **não é testável
> no host**. A suíte nativa cobre a lógica pura; o comportamento entre rádios só se
> valida em bancada com dois ou mais nós.

---

## 17. Build, testes e verificação

### 17.1 Suíte nativa (host)

O grosso da lógica (codecs, tabelas, engines, camada web pura) roda como **testes
C++ nativos no host**, sem hardware:

```
./bin/run-tests.sh      # exit 0 GREEN · 1 RED · 2 AMBER · 3 FILTERED
```

No **Windows**, a suíte roda via **Docker Desktop** com `MSYS_NO_PATHCONV=1` (ver
memória `windows-native-test-docker`). Último estado registrado: **62 suites /
916 casos GREEN**.

Suites de irrigação incluem: `test_irrigation_protocol`, `test_irrigation_replay`,
`test_irrigation_webapi`, `test_service_backup`, `test_service_vault`,
`test_service_controller`, `test_service_portal`, `test_hydraulic_group_engine`,
`test_survey_beacon`, `test_survey_log`, entre outras. O contador
`test/native-suite-count` guarda o total esperado (62).

### 17.2 O que só compila no CI/ESP32

Endpoints HTTP e frontend são **CI-only** — excluídos do build nativo em
`variants/native/portduino.ini` (via `build_src_filter`). O build de hardware
(ex.: `tbeam`) é confirmado pelo CI do fork; nunca foi rodado localmente.

Formatação: `trunk fmt` (o `trunk` não roda no host Windows; o código segue o
`clang-format` do repo e o CI valida).

### 17.3 Harness de hardware

Testes com rádio real usam o MCP `meshtastic/meshtastic-mcp` com
`MESHTASTIC_FIRMWARE_ROOT` apontando para este checkout (ver
`.github/copilot-instructions.md`).

---

## 18. Mapa de arquivos do módulo

Tudo em `src/modules/irrigation/` (~86 arquivos). Agrupado por função:

**Núcleo / protocolo**
- `IrrigationModule.{h,cpp}` — módulo central (dispatch, runOnce, glue de tudo).
- `IrrigationProtocol.{h,cpp}` — wire protocol (header, 13 tipos, encode/decode, CRC32).
- `IrrigationSettings.{h,cpp}` — blob ABI v5 (176 B), migração, load/save.
- `SeqTable.{h,cpp}` — anti-replay (`last_seq` por remetente, RAM-only).
- `RateLimiter.{h,cpp}` — N comandos/min.
- `FragmentReassembler.{h,cpp}` — remonta `SET_CONFIG` fragmentado.

**Estação (I/O local)**
- `ValveController.{h,cpp}` + driver de ponte H — válvulas latching + timer fail-safe.
- `GpoController.{h,cpp}` — saídas de nível.
- `SensorSampler.{h,cpp}` — sensores digital/analógico.
- `ButtonGesture.{h,cpp}` — gestos do botão.
- `LedPattern.{h,cpp}` — padrões de LED.
- `AuditLog.{h,cpp}` — mini-log local (ring 100).

**Pareamento / segurança**
- `Pairing.{h,cpp}` — `StationPairing` / `GatewayPairing`.
- `Allowlist.{h,cpp}` — estações adotadas no gateway.

**Gateway (coordenação)**
- `GatewayTables.{h,cpp}` — ZoneTable / StationRegistry / ProgramTable.
- `IrrigationGateway.{h,cpp}` — agregado de estado do gateway.
- `ProgramScheduler.{h,cpp}` — cronograma semanal.
- `CommandTracker.{h,cpp}` — retries de ACK.
- `StationMonitor.{h,cpp}` + `StationTelemetryCache.{h,cpp}` — muda/bateria/reboots/telemetria.
- `InterlockEngine.{h,cpp}` + `InterlockTable.{h,cpp}` — intertravamentos globais.
- `OpenGate.{h,cpp}` — admissão + fila FIFO.
- `SensorNameTable.{h,cpp}` — nomes de sensores (gateway-side).
- `FlashAuditRing.{h,cpp}` + `LittleFsByteStore.{h,cpp}` — auditoria persistente ~256 KB.
- `MirrorMode.{h,cpp}` — espelho 24VAC.

**Grupos hidráulicos**
- `HydraulicGroupEngine.{h,cpp}` — máquina guiada por ACK + matriz de falhas.
- `HydraulicGroupTable.{h,cpp}` — config + persistência.

**Nó de serviço (SERVICO)**
- `ServiceBackup.{h,cpp}` — codec de backup §5.5 + base64 + scanner JSON aninhado.
- `ServiceVault.{h,cpp}` — cofre multi-cliente.
- `IProfileStore.h` + `LittleFsProfileStore.{h,cpp}` — armazenamento injetável.
- `ServiceController.{h,cpp}` — re-tune / rota de config / resync / scan.
- `ServicePortalApi.{h,cpp}` + `ServicePortalEndpoints.{h,cpp}` (CI-only) — portal §11.8.
- `IServiceLogReader.h` — leitor do `servico.jsonl`.

**Survey**
- `SurveyBeacon.h` — state machine do beacon.
- `SurveyLog.h` — ring RAM de pontos (SNR/RSSI/coord).

**Web (painel + portal)**
- `IrrigationWebApi.{h,cpp}` — camada pura (build/parse JSON), native-tested.
- `IrrigationWebEndpoints.{h,cpp}` (CI-only) — painel do gateway.
- `PortalAp.{h,cpp}` / `PortalApi.{h,cpp}` / `PortalSession.{h,cpp}` — captive portal.
- `IrrigationPortalEndpoints.{h,cpp}` (CI-only) — portal de campo.

**Frontends** (fora do módulo): `data/irrigacao/` (painel) e
`data/irrigacao/portal/` (portal compartilhado).

---

*Documento gerado a partir da especificação `myfork/especificacao-irrigacao-mesh.md`
(v0.1) e do código-fonte em `src/modules/irrigation/`. Para o detalhamento de
decisões por fase, ver `docs/superpowers/{specs,plans}/*irrigacao*`.*
