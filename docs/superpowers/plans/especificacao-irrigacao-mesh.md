# Sistema de irrigação via LoRa mesh (Meshtastic)

**Documento de especificação — v0.1 (rascunho de trabalho)**
Escopo do projeto: **somente software** (firmware + interfaces). O hardware é assumido como dado.

---

## 1. Visão geral

Sistema de comando remoto de válvulas de irrigação sobre rede mesh LoRa (Meshtastic), composto por:

- **Gateway-controlador**: nó central com energia de fonte, que executa o cronograma de irrigação, envia comandos às estações, monitora a rede e serve o painel web. Opcionalmente opera em modo híbrido, replicando as saídas 24VAC de um controlador comercial existente (recurso de migração).
- **Estações de válvula**: nós remotos alimentados por energia solar, que acionam solenoides latching, leem sensores locais e executam lógica fail-safe autônoma.
- **Repetidores** (opcionais): nós de infraestrutura que apenas retransmitem o canal da fazenda.

Princípios de projeto:

1. **Fail-safe primeiro**: nenhuma falha de comunicação pode resultar em válvula aberta indefinidamente. O pior cenário admissível é "não irrigou".
2. **Autonomia local**: a inteligência de segurança mora na estação; o rádio é meio de comando e sincronização, nunca dependência.
3. **Convergência automática**: configurações dessincronizadas se corrigem sozinhas (mecanismo de epoch). Presença física só é exigida no pareamento.
4. **Offline por padrão**: o sistema é 100% funcional sem internet.

## 2. Hardware assumido (contexto, fora do escopo)

| Item | Especificação |
|---|---|
| MCU / rádio | ESP32 + LoRa (firmware Meshtastic com módulo custom), na própria placa |
| Alimentação da estação | Bateria chumbo-ácido 12 V / 6 Ah + painel solar 20 Wp |
| Válvulas | Solenoides DC *latching* (pulso de inversão de polaridade via ponte H) |
| Alcance | Distâncias grandes; enlaces de vários km com repetidores em pontos altos |
| Faixa de rádio | 902–928 MHz (região ANATEL, livre de licença) |

O consumo contínuo do ESP32 em escuta é aceito no balanço energético definido acima.

## 3. Arquitetura de firmware

### 3.1 Firmware único com papel (role)

Um único binário. O firmware é organizado em um **núcleo comum a todos os papéis** (a "pilha cliente", idêntica em qualquer nó) mais módulos ativados pelo `role`, que cobre exclusivamente o que depende do hardware presente na placa — este sim imutável.

**Núcleo comum (ativo em todos os papéis):**

- Protocolo de aplicação, criptografia, anti-replay e versionamento (§4);
- Persistência em NVS com epoch e escrita atômica (§5);
- Pareamento, vínculo, allowlist e reset de fábrica (§6);
- Captive portal de campo, incluindo a aba de rede para comandar o sistema a partir de qualquer nó (§7.2);
- Modos instalador e pesquisa de cobertura (§8.4–§8.5);
- Heartbeat/telemetria, mini-log local, LED de status e botão multifunção.

**Módulos ativados pelo campo `role` (persistido em NVS, definido no provisionamento) — apenas vinculações de hardware e o papel de coordenação delas decorrente:**

- `GATEWAY` (placa com alimentação de fonte, RTC e entradas do modo híbrido): leitura das entradas físicas 24VAC, motor de cronograma, coordenação da rede (envio de comandos, cobrança de ACK, reconciliação de epoch), painel web completo e log de auditoria central.
- `ESTACAO` (placa com ponte H, sensores e tamper): acionamento de válvulas/GPOs com timer fail-safe local, leitura de sensores e intertravamentos locais.
- `REPETIDOR` (sem I/O de aplicação): apenas retransmissão do canal; não entra na allowlist de comando.
- `SERVICO` (ESP32 + rádio, sem I/O de aplicação; bateria própria): equipamento de manutenção itinerante multi-cliente — cofre de credenciais, troca de canal em runtime, varredura, leitura/escrita de configuração e pareamento em campo (§11).

Trocar o papel de um nó não exige recompilar — apenas reprovisionar (respeitando o pin map da placa em questão).

Protocolo, criptografia, formato de mensagens e esquema de NVS são compartilhados entre papéis. As mensagens carregam um número de **versão de protocolo**; mismatch gera alerta e rejeição segura.

### 3.2 Integração com o Meshtastic

- Comunicação de aplicação via **portnum privado** (faixa 256–511).
- Canal privado com **PSK AES-256 própria por fazenda**, gerada aleatoriamente no primeiro boot do gateway (nunca chave padrão).
- Payloads binários compactos (≤ ~200 bytes úteis por pacote), respeitando o airtime do LoRa.

### 3.3 Particionamento de flash (preparação para OTA)

A atualização OTA em si permanece fora do escopo (§9), mas o particionamento é adotado **desde já**, pois alterá-lo depois exige reflash físico de todas as placas em campo:

| Partição | Tipo | Finalidade |
|---|---|---|
| `nvs` | dados | Configurações, credenciais, contadores e sequências (§5.5) |
| `otadata` | dados | Seleção da partição de aplicação ativa |
| `app0` / `app1` | aplicação (duplas, mesmo tamanho) | Firmware ativo + slot de atualização |
| `littlefs` | dados | Arquivos estáticos do painel/portal (HTML/CSS/JS) |
| `coredump` | dados | Diagnóstico pós-pânico (consultável pelo portal) |

Regras:

- **Rollback automático**: imagem nova entra como *pendente* e só é confirmada como válida após um boot saudável — inicialização completa mais a primeira transmissão de heartbeat (estação) ou painel no ar (gateway). Sem confirmação, o bootloader retorna à partição anterior no reset seguinte.
- O flash via USB de fábrica/bancada usa o mesmo layout e o mesmo formato de imagem que o futuro OTA usará (formato já preparado para assinatura), evitando migração de formato depois.
- **UI e firmware desacoplados**: atualizar somente os arquivos do painel/portal (`littlefs`) não toca a aplicação — mudanças cosméticas de interface sem risco nem rollback.

## 4. Protocolo de aplicação



### 4.1 Tipos de mensagem

Nas direções abaixo, **CTRL** designa o nó controlador da transação: o gateway em operação normal, ou o nó `SERVICO` quando presente (§11).

| Tipo | Direção | Conteúdo principal |
|---|---|---|
| `CMD_VALVULA` | CTRL → EST | id da válvula, ação (abrir/fechar), **duração máxima (s)**, seq |
| `CMD_GPO` | CTRL → EST | id da saída de propósito geral, ação, duração máxima opcional, seq |
| `ACK` | EST → CTRL | seq confirmado, estado atual, tensão de bateria, epoch de config |
| `HEARTBEAT` | EST → GW | estado das válvulas/GPOs, Vbat, Vpainel, SNR/RSSI, contador de reboots + última causa, epoch, flags (tamper, modo seguro, sensores) |
| `SET_CONFIG` | CTRL → EST | bloco de configuração (fragmentável), epoch alvo, CRC |
| `GET_CONFIG` | CTRL → EST | solicita config vigente (resposta com epoch + CRC) |
| `EVENTO` | EST → GW | alerta assíncrono: bateria baixa, tamper, sensor, acionamento manual, reboot |
| `PAIR_ANNOUNCE` / `PAIR_GRANT` | EST ↔ CTRL | fluxo de pareamento (ver §6) |
| `PING_SURVEY` | qualquer | beacon do modo pesquisa de cobertura (§8.5) e **sonda de varredura** do nó de serviço (§11.4); respondida por qualquer nó do canal com nodeinfo mínimo (id, nome, role, epoch, Vbat, versão) |
| `RESYNC_SEQ` | CTRL → EST | solicita o `last_seq` que o receptor registrou para o remetente; resposta permite ao controlador retomar a numeração (§11.5) |

### 4.2 Regras obrigatórias

- **Fail-safe embutido no comando**: toda abertura carrega a duração máxima. A estação fecha sozinha ao expirar o timer **local**, independentemente do rádio. Teto absoluto compilado no firmware: **120 min** — nenhuma configuração ou comando ultrapassa. Valor efetivo = `min(comando, config_local, teto_firmware)`.
- **Anti-replay**: número de sequência monotônico **por remetente**, persistido no receptor (`last_seq[remetente]`). Pacote com seq já visto ou antigo é descartado e registrado. Um controlador que perdeu seu contador o recupera via `RESYNC_SEQ` (§11.5).
- **ACK de aplicação**: o gateway reenvia comandos não confirmados até N tentativas (configurável) e gera alerta em caso de falha.
- **Renovação**: um comando de abrir/ligar recebido com a saída **já ativa** não é erro — reinicia o timer fail-safe local com a nova duração. É o mecanismo que permite ao gateway estender com segurança um acionamento em curso (usado pelos grupos hidráulicos, §8.13).
- **Rate limiting no receptor**: máximo de N comandos/min aceitos (proteção contra flood lógico e bugs do gateway).
- **Validação de origem**: o receptor aceita comandos quando o remetente é o **gateway ao qual está vinculado** (§6) **ou** quando o pacote traz a marca `SERVICE_MAGIC` do nó de serviço (§11.5). Como o pacote só chega decifrado a quem possui a PSK do canal, **a posse da PSK constitui a autoridade de administração**. O gateway só processa mensagens de nós registrados ou do nó de serviço.
- **Bateria mínima**: abaixo do limiar configurado, a estação **recusa abrir** (responde NACK com motivo) mas **sempre aceita fechar**.

### 4.3 Comportamento sob negação de serviço (flood/jamming)

- Comando de abrir não entregue → ciclo perdido, alerta de falha de ACK no gateway.
- Comando de fechar não entregue → irrelevante: o timer local fecha a válvula.
- Ausência de heartbeats → alerta de estação muda (§8.3).
- Telemetria de utilização de canal e piso de ruído (SNR/RSSI) exposta no painel para diagnóstico de interferência.

## 5. Modelo de configuração e persistência

### 5.1 Camada 1 — Perfil de hardware (pin map), por nó

O firmware define funções lógicas (slots); a configuração mapeia pinos a slots. `-1` = função ausente. Funções múltiplas usam arrays pareados.

```json
{
  "schema": 1,
  "board": "estacao-v2",
  "pins": {
    "hbridge_a":   [25, 32],
    "hbridge_b":   [26, 33],
    "gpo":         [27],
    "btn_multi":   0,
    "led_status":  2,
    "vbat_adc":    34,
    "vpainel_adc": 35,
    "tamper":      14
  },
  "sensores": [
    { "id": 0, "nome": "pressao_linha", "tipo": "analogico", "pino": 36,
      "escala": { "adc_min": 300, "adc_max": 3800, "eng_min": 0.0, "eng_max": 10.0, "unidade": "bar" },
      "amostragem_s": 30 },
    { "id": 1, "nome": "nivel_reservatorio", "tipo": "digital", "pino": 39,
      "logica": "ativo_baixo", "debounce_ms": 200 }
  ],
  "pulse_ms": 60,
  "vbat_divisor": 4.6
}
```

Validações da UI: conflito de pino entre slots; GPIOs 34–39 do ESP32 são somente entrada (proibidos em funções de saída); pinos de boot/strapping sinalizados com aviso.

### 5.2 Camada 2 — Tabela de zonas (roteamento lógico), no gateway

A zona é a unidade que o cronograma e as entradas físicas enxergam. Resolve zona → (nó, índice de saída). O campo opcional `fonte` habilita o modo híbrido (disparo por entrada 24VAC do controlador comercial, lida por optoacoplador).

```json
{
  "schema": 1,
  "zonas": [
    { "id": 1, "nome": "Horta",    "no": "!a1b2c3d4", "tipo": "valvula", "indice": 0, "max_min": 45, "padrao_min": 20 },
    { "id": 2, "nome": "Pomar",    "no": "!a1b2c3d4", "tipo": "valvula", "indice": 1, "max_min": 60, "padrao_min": 30 },
    { "id": 3, "nome": "Pastagem", "no": "!e5f6a7b8", "tipo": "valvula", "indice": 0, "max_min": 90, "padrao_min": 45,
      "fonte": "gpio:27" },
    { "id": 4, "nome": "Portao curral", "no": "!e5f6a7b8", "tipo": "gpo", "indice": 0, "max_min": 1 }
  ]
}
```

### 5.3 Camada 3 — Registro de estações (identidade lógica)

O gateway mantém perfis lógicos de estação; o ID do nó Meshtastic é um **atributo substituível** do perfil. Troca de placa em campo = parear nó novo + associar ao perfil existente; o gateway empurra toda a configuração (via epoch). Inclui o **campo de coordenadas**, de preenchimento manual, para localização posterior.

```json
{
  "estacoes": [
    {
      "nome": "Pasto Norte",
      "no": "!e5f6a7b8",
      "coordenadas": { "lat": -22.123456, "lon": -47.654321, "descricao": "poste ao lado da caixa d'agua, 300 m apos a porteira 2" },
      "perfil_hw": { "ref": "camada 1" },
      "params": {
        "hb_min": 10,
        "retries": 3,
        "vbat_aviso": 12.2,
        "vbat_min_abrir": 11.8,
        "vbat_hibernacao": 11.5,
        "silencio_alerta_min": 35
      },
      "config_epoch": 17
    }
  ]
}
```

As coordenadas aparecem no painel (lista e detalhe da estação), no portal de campo do próprio nó e nos textos de alerta ("Estação Pasto Norte muda — lat -22.1234, lon -47.6543"), permitindo navegação por GPS até o equipamento.

### 5.4 Sincronização — epoch de configuração

1. Toda edição de config de estação no gateway incrementa `config_epoch`.
2. A estação persiste a config + epoch e reporta o epoch em **todo heartbeat e ACK**.
3. Gateway detecta `epoch_estacao < epoch_desejado` → reenvia automaticamente a config (com ACK) até convergir.
4. **Regra do maior epoch (edição fora de banda)**: se o gateway detectar `epoch_estacao > epoch_local`, **não sobrescreve** — a estação foi configurada por um nó de serviço (§11.6). O gateway executa `GET_CONFIG`, adota a configuração recebida, alinha seu contador e registra o fato na auditoria (origem `servico`).

Resultado: perdas de pacote, estações offline durante edições e nós recém-trocados convergem sem intervenção. O painel exibe o estado por estação: `sincronizada` / `pendente` / `inalcançável`.

### 5.5 Persistência segura (NVS)

- **Escrita atômica em dois passos**: config nova em chave de *staging* → validação por CRC → troca da chave ativa → limpeza. Queda de energia no meio nunca corrompe a config ativa; no boot, transação incompleta é finalizada ou descartada conforme o CRC.
- A estação **só envia o ACK de config após o commit em NVS** (ACK = gravado, não apenas recebido).
- **Config de fábrica embutida no binário**: NVS vazia/corrompida → boot em **modo seguro** (válvulas fechadas, GPOs inativos, só aceita pareamento), com flag no anúncio e no LED.
- Persistidos em NVS: role, credenciais (PSK, ID do gateway vinculado), pin map, parâmetros, sequência anti-replay, epoch, contador de reboots, ponteiros do log local.
- **Backup exportável**: o painel exporta um JSON único (estações, zonas, programas, intertravamentos, log recente, PSK cifrada por senha do usuário). Importável em um gateway substituto; as estações reconciliam por epoch.

## 6. Segurança e pareamento

Camadas independentes:

1. **PSK AES-256 por fazenda** (canal privado): pacotes de terceiros não decriptam. Isola fazendas vizinhas por criptografia.
2. **Vínculo nó ↔ gateway**: a estação grava o ID do seu gateway no pareamento e descarta comandos de qualquer outro remetente; o gateway mantém allowlist de estações adotadas. Permite inclusive duas fazendas do mesmo dono compartilharem PSK sem comando cruzado.
3. **Sequência anti-replay** por par.

Fluxo de adoção (posse física obrigatória):

1. Nó novo liga em modo fábrica (sem chave; não aceita comandos).
2. **Botão físico** no nó abre janela de pareamento de 2 min (anúncio `PAIR_ANNOUNCE`).
3. Usuário aprova no painel do gateway (confere ID/nome); o gateway responde com `PAIR_GRANT` contendo as credenciais (PSK da fazenda + ID do gateway) **pelo próprio rádio LoRa**, num canal de pareamento com chave conhecida. A exposição momentânea é aceita como risco tolerável: a janela dura 2 min, só existe após acionamento do botão físico no nó, e o gateway só a atende mediante aprovação explícita no painel.
4. Nó grava credenciais (commit atômico), reinicia no canal da fazenda; gateway o adiciona à allowlist e o associa a um perfil de estação (novo ou existente).

Regras complementares:

- **Reset de fábrica** (botão 10 s): apaga todas as credenciais e configs; o gateway marca a estação como removida.
- Repetidores recebem apenas a PSK; não entram na allowlist de comando.
- Portal Wi-Fi sempre com WPA2 + PIN de aplicação; AP desliga após inatividade.

## 7. Interfaces de usuário

### 7.1 Painel web do gateway

Servido pelo próprio ESP32 do gateway (LittleFS + endpoints JSON; página estática rica no navegador do cliente). Telas:

- **Estações**: registro, pareamento pendente, estado (bateria, painel, SNR, último contato, epoch, reboots), coordenadas, pin map (seção "avançado"), troca de nó do perfil, botão de teste de pulso (abre 10 s com fechamento automático).
- **Zonas**: tabela de roteamento com dropdowns (nó pareado → saída disponível), `max_min` e `padrao_min` por zona, tipo (válvula / GPO), fonte física opcional (modo híbrido).
- **Programas**: cronograma (dias, horários, sequência de zonas e durações). Só enxerga zonas.
- **Intertravamentos**: regras de bloqueio (ver §8.10).
- **Log de auditoria**: consulta com filtros (ver §8.9).

Toda mudança remota só é dada como aplicada após ACK com epoch confirmado; a UI mostra o estado da convergência.

### 7.2 Captive portal do nó (acesso em campo — disponível em todos os papéis)

Botão físico (pressão curta com nó já pareado, ou automático nos primeiros 10 min de um nó de fábrica) sobe AP Wi-Fi local (`Irrigacao-<nome>`); DNS cativo abre a página automaticamente. Abas:

- **Este nó**: estado vivo (bateria, painel, válvulas, sensores, tamper), config local, teste de pulso, coordenadas.
- **Rede**: envia comandos pelo rádio ao gateway e demais estações da fazenda (autenticado pela PSK/vínculo; a posse física do botão é a autorização). Permite operar o sistema inteiro a partir de qualquer nó em campo.
- **Instalador**: ver §8.4.

AP desliga após X min sem cliente. mDNS (`http://<nome>.local`) como alternativa ao portal cativo.

## 8. Funcionalidades (escopo desta versão)

### 8.1 Alertas de bateria em níveis

| Nível | Limiar (padrão, configurável) | Comportamento |
|---|---|---|
| Aviso | 12,2 V | `EVENTO` + destaque no painel |
| Crítico | 11,8 V | Recusa abrir válvulas (NACK com motivo); fechar sempre aceito; alerta |
| Hibernação | 11,5 V | Fecha tudo, desativa periféricos, heartbeat reduzido a 1/h; alerta; sai automaticamente ao recarregar acima de histerese (ex.: 12,4 V) |

Histerese obrigatória em todos os limiares para evitar oscilação de alertas. Tensão do painel também reportada no heartbeat (diagnóstico de geração).

### 8.2 Contador de reboots e causa

Contador persistente em NVS, incrementado a cada boot, com a última causa registrada (power-on, brownout, watchdog, pânico de software, reset manual). Reportado no heartbeat e exibido no painel. Taxa anômala de reboots (> N em 24 h) gera alerta — indica firmware instável ou alimentação degradada.

### 8.3 Alerta de estação muda

- Gateway monitora o intervalo desde o último heartbeat de cada estação; ausência por `silencio_alerta_min` (padrão: 3,5 × intervalo de heartbeat) → alerta com nome e **coordenadas** da estação.
- Distinção de degradação: tendência de queda de SNR ao longo de dias (média móvel) gera alerta preventivo de enlace degradando (antena, vegetação) antes da perda total.
- O painel de cada estação exibe "último contato há X min" permanentemente.

### 8.4 Modo instalador (no portal de campo)

Aba do captive portal com leitura ao vivo (atualização ~1 s) de RSSI/SNR do enlace com o gateway e com vizinhos visíveis, além de contagem de pacotes perdidos num teste de eco. Uso: apontamento de antena e escolha do ponto de instalação. Sai automaticamente após timeout para não poluir o airtime.

### 8.5 Modo pesquisa de cobertura (site survey)

Qualquer nó pode ser colocado temporariamente em modo beacon (`PING_SURVEY` a cada N s, com potência normal de operação) via portal ou painel. O gateway registra cada beacon recebido com SNR/RSSI e, se informado, a coordenada do ponto de teste (digitada no portal do nó itinerante). Resultado: tabela/lista de pontos testados × qualidade de sinal no painel, para planejar posições de estações e repetidores antes da instalação definitiva. O modo expira sozinho (timeout configurável).

### 8.6 Botão físico multifunção (na estação)

| Gesto | Ação |
|---|---|
| Pressão curta (nó pareado) | Sobe o captive portal (10 min) |
| Pressão dupla | **Acionamento manual local**: abre a válvula 0 pela duração padrão, com o mesmo timer fail-safe; nova pressão dupla fecha |
| Pressão longa (3 s) | Teste de pulso (abre 10 s, fecha automático) |
| Pressão de 10 s | Reset de fábrica (com confirmação por padrão de LED) |
| Nó de fábrica | Qualquer pressão abre a janela de pareamento |

Todo acionamento manual gera `EVENTO` para o log de auditoria (origem: `botao_fisico`).

### 8.7 LED de status (códigos de piscada)

| Padrão | Significado |
|---|---|
| 1 piscada / 5 s | Operação normal, pareado, sincronizado |
| 2 piscadas / 5 s | Sem contato com o gateway |
| 3 piscadas / 5 s | Config pendente (epoch atrasado) ou modo seguro |
| Piscada rápida contínua | Janela de pareamento aberta / portal ativo |
| Aceso fixo | Válvula ou GPO aberto |
| Padrão SOS | Bateria crítica / hibernação iminente |

Diagnóstico visual a distância sem celular. Padrões definidos em tabela única no firmware (fáceis de revisar).

### 8.8 Campo de coordenadas

Campo manual `coordenadas {lat, lon, descricao}` por estação (§5.3), editável no painel e no portal do nó. Exibido em listas, detalhes e em todo alerta referente à estação. A `descricao` livre complementa o GPS ("poste ao lado da caixa d'água"). Formato decimal (WGS84).

### 8.9 Log de auditoria (mandatório)

- **Toda ação registrada** com: timestamp (RTC), origem (`cronograma`, `painel`, `portal_campo:<no>`, `botao_fisico`, `entrada_fisica:<gpio>`, `intertravamento`, `failsafe_timer`, `sistema`), ator/nó, ação, alvo (zona/válvula/GPO/config), resultado (ok, NACK+motivo, timeout) e seq.
- Eventos registrados além de comandos: alertas de bateria, tamper, reboots, pareamentos, resets de fábrica, mudanças de config (com epoch), entradas/saídas de modo seguro e hibernação.
- Armazenamento no gateway: buffer circular em flash (dimensionado para meses de operação típica); exportável em CSV/JSON pelo painel; incluído no backup.
- Estações mantêm um mini-log circular local (últimos ~100 eventos) consultável pelo portal de campo — útil quando a estação esteve fora de alcance.
- O log é **somente-append** na perspectiva da UI (sem edição/apagamento seletivo).

### 8.10 Entradas de sensor (analógico/digital) e intertravamentos

Sensores declarados no pin map (§5.1):

- **Digital**: nível lógico com debounce e polaridade configuráveis (ex.: boia de nível, pressostato, sensor de chuva por contato).
- **Analógico**: leitura ADC com calibração linear de dois pontos (`adc_min/max → eng_min/max` + unidade) e período de amostragem (ex.: transdutor de pressão, sensor resistivo).

Estados/valores reportados no heartbeat e visíveis no painel e no portal.

**Intertravamentos** — regras avaliadas no gateway (visão global) e, quando envolvem sensor local da própria estação, também **replicadas localmente** (a estação bloqueia mesmo sem rádio):

```json
{
  "intertravamentos": [
    { "id": 1, "tipo": "sensor",
      "no": "!e5f6a7b8", "sensor": 1, "condicao": "ativo",
      "acao": "bloquear_abertura", "zonas": [1, 2, 3],
      "mensagem": "Reservatorio em nivel baixo" },
    { "id": 2, "tipo": "sensor",
      "no": "!a1b2c3d4", "sensor": 0, "condicao": "menor_que", "valor": 1.5,
      "acao": "fechar_e_bloquear", "zonas": ["*"],
      "mensagem": "Pressao de linha abaixo de 1,5 bar" },
    { "id": 3, "tipo": "simultaneidade", "max_saidas_abertas": 2 }
  ]
}
```

Regras: ações possíveis `bloquear_abertura` (impede novos ciclos) e `fechar_e_bloquear` (interrompe ciclos em curso); histerese configurável para condições analógicas; o cronograma **enfileira** zonas bloqueadas por simultaneidade em vez de sobrepor; todo bloqueio/desbloqueio vai ao log de auditoria com a regra e a leitura que o disparou.

### 8.11 Saída de propósito geral (GPO)

Saída digital genérica (relé/MOSFET) declarada no pin map e roteável como zona de tipo `gpo` (§5.2): portão, iluminação, bomba auxiliar, sirene. Herda todo o arcabouço das válvulas: comando com duração máxima opcional (`0` = biestável, permanece até comando contrário — exigindo confirmação extra na UI), ACK, log de auditoria, acionamento pelo painel, portal de campo e cronograma. GPOs biestáveis exibem o estado corrente de forma destacada no painel.

### 8.12 Alerta de violação (tamper)

Entrada digital dedicada no pin map (switch de tampa do gabinete). Abertura fora de uma **janela de manutenção** (ativável pelo painel ou automaticamente enquanto o portal de campo daquele nó está aberto) gera `EVENTO` imediato de alta prioridade, com nome e coordenadas da estação, e registro em auditoria. Estado do tamper presente em todo heartbeat (detecção mesmo se o evento original se perder). Opcional por estação (pino `-1` desativa).

### 8.13 Grupos hidráulicos (bomba / válvula mestre)

Atende setups em que um motor/bomba só opera corretamente com um número mínimo e máximo de válvulas abertas, exigindo sequenciamento coordenado com a bomba ligada. Introduz um invariante de segurança distribuído: **bomba ligada ⇒ pelo menos `min_abertas_com_bomba` válvulas confirmadamente abertas**.

#### Configuração

A bomba (ou válvula mestre) é uma zona `gpo` comum — pode estar em qualquer estação da mesh. O grupo agrupa bomba, zonas e parâmetros de comportamento:

```json
"grupos_hidraulicos": [
  { "id": 1, "nome": "Motor do poco",
    "bomba": { "zona": 9 },
    "zonas": [1, 2, 3, 4, 5],
    "min_abertas_com_bomba": 1,
    "max_abertas": 2,
    "transicao": "abrir_antes_de_fechar",
    "sobreposicao_s": 10,
    "partida_apos_abrir_s": 5,
    "parar_antes_de_fechar_s": 8,
    "funcionamento_min_min": 5,
    "max_partidas_hora": 6 }
]
```

- `transicao`: `abrir_antes_de_fechar` (padrão — durante a sobreposição o grupo admite momentaneamente `max_abertas + 1`) ou `fechar_antes_de_abrir` (para setups que não toleram a sobreposição; a proteção contra golpe de pressão passa a depender da proteção local, abaixo).
- `funcionamento_min_min` e `max_partidas_hora` protegem o motor de ciclagem curta: pedidos próximos no tempo mantêm a bomba ligada na ponte em vez de desligar/religar.

#### Execução

O cronograma continua enxergando zonas. Quando um programa toca zonas de um grupo, o gateway **serializa a execução** respeitando `max_abertas` e injeta a orquestração da bomba como uma **máquina de estados guiada por ACK** — nenhum passo ocorre sem a confirmação do anterior:

```
abrir V1 → [ACK] → aguardar partida_apos_abrir_s → ligar bomba → [ACK]
  → irrigar V1 …
  → abrir V2 → [ACK] → aguardar sobreposicao_s → fechar V1 → [ACK]
  → irrigar V2 … (repete)
  → fim da última zona: desligar bomba → [ACK]
  → aguardar parar_antes_de_fechar_s → fechar última válvula
```

A bomba nunca parte contra tudo fechado nem para com fluxo bloqueado. O comando de ligar a bomba embarca como duração máxima o **teto do ciclo completo do grupo** (fail-safe local também na bomba); o gateway a renova (§4.2) a cada transição de zona.

#### Matriz de falhas

| Falha | Reação do sistema |
|---|---|
| Abrir a próxima válvula falha (sem ACK após retries) | **Não fechar a corrente**: renová-la (reinicia o timer local), alertar, retentar a próxima. Impossível renovar antes do timer da corrente expirar → **desligar a bomba primeiro**, depois deixar a válvula fechar. |
| Fechar a anterior falha | Não viola o invariante (válvula a mais = pressão menor). Alerta; violação persistente de `max_abertas` → encerramento ordenado do grupo. |
| Gateway indisponível no meio do ciclo | Timers locais fecham válvulas e bomba (duração máxima embarcada). A janela entre expirações é coberta pela proteção local. |
| Estação reinicia com válvula aberta | Boot em estado fechado (§5.5); o heartbeat revela a mudança e o gateway desliga a bomba ou renova a sequência. |

#### Proteção local (última linha de defesa, sem rádio)

Recomenda-se um sensor de pressão (digital ou analógico, §8.10) na saída da bomba, **ligado à mesma estação que a aciona**, com intertravamentos replicados localmente: `pressão alta → fechar_e_bloquear` a zona da bomba (deadhead — tudo fechou) e `pressão baixa sustentada → fechar_e_bloquear` (operação a seco). Assim a orquestração por rádio cuida da *operação*, mas a *integridade do motor* nunca depende de um pacote chegar.

#### Unificação

A regra `simultaneidade` do §8.10 é o caso particular de um grupo hidráulico **sem bomba** (`max_abertas` apenas); implementações devem tratar ambas pelo mesmo mecanismo.

## 9. Fora do escopo desta versão (roadmap)

Registrado para não perder o histórico da discussão; **não implementar agora**: ajuste sazonal global, pausa por chuva, ciclo e infiltração, sensor de fluxo e detecção de vazamento, relatórios de consumo, atualização OTA (via Wi-Fi e via mesh)¹, mapa da mesh, gateway de backup, ponte para internet/MQTT/notificações, umidade de solo no cronograma, estação meteorológica, fertirrigação, cronograma local de contingência na estação.

¹ O particionamento de flash exigido pelo OTA já está adotado nesta versão (§3.3); resta ao futuro apenas o mecanismo de transferência e aplicação das imagens.

## 10. Parâmetros padrão consolidados

| Parâmetro | Padrão | Onde |
|---|---|---|
| Teto absoluto de abertura | 120 min (compilado) | Firmware da estação |
| Heartbeat | 10 min | Config da estação |
| Alerta de silêncio | 35 min (3,5 × HB) | Gateway |
| Retentativas de comando | 3 | Gateway |
| Pulso da ponte H | 60 ms | Pin map |
| Bateria: aviso / bloqueio / hibernação | 12,2 / 11,8 / 11,5 V | Config da estação |
| Rate limit de comandos | 10/min | Firmware da estação |
| Janela de pareamento | 2 min | Firmware |
| Timeout do portal Wi-Fi | 10 min sem cliente | Firmware |
| Reset de fábrica | botão 10 s | Firmware |
| Sobreposição na transição (grupo) | 10 s | Config do grupo hidráulico |
| Partida da bomba após abrir | 5 s | Config do grupo hidráulico |
| Parada da bomba antes de fechar | 8 s | Config do grupo hidráulico |
| Funcionamento mínimo da bomba | 5 min | Config do grupo hidráulico |
| Máx. de partidas da bomba | 6/h | Config do grupo hidráulico |
| Escuta da sonda de varredura | 4 s por cliente | Firmware do nó de serviço |
| Settle time após re-tune | 500 ms | Firmware do nó de serviço |

## 11. Equipamento de serviço (role `SERVICO`)

### 11.1 Definição

Nó portátil (ESP32 + rádio, sem I/O de aplicação, bateria própria) que atua como **gateway itinerante multi-cliente**: guarda as credenciais de vários clientes, sintoniza o canal de um deles, e lê/escreve configurações dos nós ao alcance, sem internet e sem contato físico com cada equipamento. Um único device atende todo o parque instalado.

**Modelo de autoridade adotado: a posse da PSK do canal é a autorização.** Não há PKI, assinatura, consentimento do cliente nem escopo de permissão — decisão explícita de projeto em favor da simplicidade. Consequência a registrar: quem detiver o device detém controle administrativo sobre todas as fazendas cujos perfis ele contém.

Herda integralmente o núcleo comum (§3.1): protocolo, `GET/SET_CONFIG`, epoch, portal Wi-Fi (que é a sua única UI — não requer tela), LED, botão. Pin map mínimo: LED de status, botão, ADC de bateria.

### 11.2 Cofre de clientes (armazenamento)

Reside em **LittleFS** (não em NVS, cujo espaço e modelo chave-valor são inadequados ao volume). Cartão SD é opcional, para import/export.

```
/clientes/index.json          → [{ id, nome, canal, gateway, arquivo }]
/clientes/<id>.json           → perfil completo do cliente
/clientes/<id>.seq            → contadores de sequência (arquivo separado)
/log/servico.jsonl            → log local append-only
```

Perfil de cliente:

```json
{
  "id": "fazenda-sp-01",
  "nome": "Sitio Boa Vista",
  "canal": { "nome": "bv-irrig", "psk_b64": "…", "modem_preset": "LONG_FAST" },
  "gateway": "!a1b2c3d4",
  "estacoes": [ { "no": "!e5f6a7b8", "nome": "Pasto Norte", "coordenadas": { "lat": -22.1, "lon": -47.6 } } ],
  "snapshot_epoch": { "!e5f6a7b8": 17 }
}
```

Regras de implementação:

- **Parsing em streaming** (um cliente por vez, direto do arquivo); jamais carregar o índice inteiro desserializado em RAM.
- Os contadores `seq` são gravados a cada comando e por isso vivem em **arquivo separado** (`<id>.seq`), evitando reescrever o perfil completo e desgastar a flash.
- Toda gravação usa o mesmo padrão de **staging + rename atômico** da NVS (§5.5): arquivo temporário validado antes de substituir o vigente.

### 11.3 Troca de canal em runtime

No Meshtastic o **slot de frequência deriva do nome do canal**: clientes com nomes distintos operam em frequências distintas. Selecionar um cliente exige, portanto, re-tunar o rádio (nome + PSK + `modem_preset`), não apenas trocar a chave.

- Se o firmware não aplicar a troca sem reinício, o comportamento aceito é **persistir o cliente ativo em NVS e reiniciar** (custo de poucos segundos, transparente para o técnico).
- Após o re-tune, aguardar o **settle time** (§10) com a fila de TX limpa antes de qualquer transmissão.
- Recomenda-se padronizar o `modem_preset` entre clientes; presets divergentes aumentam o tempo de varredura.

### 11.4 Varredura de rede

Como não existe escuta simultânea de múltiplas frequências, a varredura itera os clientes do índice: aplicar canal → emitir `PING_SURVEY` como **sonda** → escutar pela janela definida em §10 → registrar nós que responderam (id, nome, role, epoch, Vbat, versão, SNR/RSSI).

A sonda é usada porque o heartbeat (10 min) é longo demais para escuta passiva. Todos os papéis respondem à sonda com nodeinfo mínimo. Havendo GPS ou última posição conhecida, ordenar a varredura por proximidade.

### 11.5 Aceitação pelos nós e sequências

- **Marca de serviço**: comandos e configs do nó de serviço trazem `SERVICE_MAGIC` no cabeçalho. A validação de origem (§4.2) aceita o pacote por essa marca, dispensando o vínculo `gateway_id`.
- **`last_seq` por remetente**: o receptor mantém um contador por remetente, permitindo que gateway e nó de serviço comandem a mesma estação de forma independente.
- **`RESYNC_SEQ`**: quando o device não conhece seu contador para um par (cliente, nó) — device novo, perfil importado, arquivo `.seq` perdido — ele consulta o `last_seq` registrado no receptor e retoma a partir de `last_seq + 1`. Sem esse mecanismo o device fica permanentemente rejeitado por anti-replay.

### 11.6 Leitura e escrita de configuração

Duas rotas, e a UI do device deve deixar explícito qual está em uso:

1. **Via gateway (padrão)** — o device envia `SET_CONFIG` **ao gateway do cliente**, que incrementa o epoch e propaga pelo mecanismo normal (§5.4). Preserva fonte de verdade única; nenhuma divergência possível.
2. **Direta à estação (gateway inacessível)** — o device escreve na estação com `epoch = epoch_atual + 1` e registra a alteração como **pendente** no perfil do cliente. Ao reencontrar o gateway, este observa `epoch_estacao > epoch_local` e, pela **regra do maior epoch** (§5.4), puxa a config da estação via `GET_CONFIG`, adota-a e alinha seu contador — registrando em auditoria com origem `servico`.

Leitura (`GET_CONFIG`) é sempre direta e não altera epoch. O device pode exportar a config lida no formato de backup (§5.5), útil para clonar uma estação a ser substituída.

Demais operações disponíveis por nó ao alcance: teste de pulso, acionamento de zona, aprovação de pareamento (§6, substituindo o painel do gateway) e `RESYNC_SEQ`.

### 11.7 Import / export

Formato: o JSON de backup (§5.5) sob um envelope multi-cliente, incluindo `seq` e `snapshot_epoch` (um device provisionado a partir do arquivo já nasce com contadores válidos; defasagens são cobertas por `RESYNC_SEQ`).

Vias: portal Wi-Fi (`GET /export`, `POST /import` — caminho principal); cartão SD (`import.json` na raiz, aplicado no boot); USB serial (comandos `EXPORT`/`IMPORT`, uso de bancada).

Semântica: **merge por `id` de cliente** (sobrescreve perfis homônimos, preserva os demais), com modo `--replace` explícito. O arquivo é integralmente validado em área temporária antes de qualquer substituição.

### 11.8 Interface (portal do device)

O portal do núcleo comum ganha, no role `SERVICO`, três abas:

- **Clientes**: lista e seleção do cliente ativo (dispara re-tune/reinício), varredura, import/export.
- **Rede do cliente ativo**: nós detectados com estado (bateria, epoch, role, versão, SNR); por nó — ler config, escrever config, teste de pulso, acionar zona, aprovar pareamento, `RESYNC_SEQ`.
- **Log**: `servico.jsonl` local. O device não possui RTC: os registros carimbam `uptime` e adotam o timestamp do gateway quando disponível, cabendo ao gateway a autoridade temporal na mesclagem com sua auditoria.

### 11.9 Impacto no firmware do parque instalado

Os itens abaixo alteram estações e gateways e **devem estar presentes nos binários antes da distribuição em campo** — sem eles, o nó de serviço não é aceito pelo parque já instalado:

1. Aceitação de comando por `SERVICE_MAGIC` na validação de origem (§4.2).
2. `last_seq` por remetente e resposta a `RESYNC_SEQ` (§4.2, §11.5).
3. Regra do maior epoch com adoção via `GET_CONFIG` no gateway (§5.4).
4. Resposta à sonda `PING_SURVEY` com nodeinfo mínimo, em todos os papéis (§4.1).
