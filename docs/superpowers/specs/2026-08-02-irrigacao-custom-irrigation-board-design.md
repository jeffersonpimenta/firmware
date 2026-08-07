# Design — variant `custom_irrigation` (board própria, seed Heltec V2)

Data: 2026-08-02 · Branch: `sistema-irrigacao`

## Objetivo

Ter uma placa-alvo própria (`custom_irrigation`) que **hoje é eletricamente uma Heltec
WiFi LoRa 32 V2** para bancada, mas que evolui para o hardware custom (KiCad
`RT/Autom/Hardware/RX`, base L298N + relé FINDER) apenas editando **um ficheiro**:
`variants/esp32/custom_irrigation/variant.h`. Ao dar boot sem config persistida, o nó
já arranca com um mapa de pinos de irrigação válido (latch, entradas, saídas, botão,
LED, tamper), dispensando o passo de portal para testar na bancada.

## Decisões (fechadas com o utilizador)

- **Sem GPS na bancada** → GPIO33 e GPIO36 ficam livres (habilita 1 GPO + 1 sensor
  analógico).
- **2 válvulas latch** iniciais (não 8). Heltec V2 não tem GPIO livre para mais.
- **Novo variant `custom_irrigation`**, seed = pinos Heltec V2. Não mexe no variant
  `heltec_v2` de stock.
- **Aditivo total**: sem mudança de ABI (`IrrigationSettings` v6 / 180 B intacto) nem de
  protocolo (VERSION=1). Defaults só preenchem a struct em RAM no 1º boot.
- **RTC**: a board ainda não tem RTC. Usa DS3231 externo no barramento I2C
  (SDA 4 / SCL 15), auto-detetado pelo módulo RTC do Meshtastic — **não** entra no blob
  de irrigação. Só documentado. O caminho "gateway sem RTC" (scheduler ocioso gracioso)
  já existe.

## Arquitetura

Três peças, todas aditivas:

### 1. Variant `variants/esp32/custom_irrigation/`

- `platformio.ini` — `[env:custom_irrigation]`, `extends = esp32_base`,
  `board = heltec_wifi_lora_32_V2` (hardware Heltec por agora),
  `build_flags` inclui `-I variants/esp32/custom_irrigation`, `-D CUSTOM_IRRIGATION`
  e `-D HELTEC_V2_0` (reusa defines de HW já presentes no firmware).
  `custom_meshtastic_hw_model` = **255 (PRIVATE_HW)** com slug/nome próprios (evita
  colidir com o model 5 da Heltec; ajustável quando registar a board).
- `variant.h` — conteúdo:
  1. Bloco de **hardware** copiado do `heltec_v2/variant.h` (LoRa SX1276, OLED, VEXT,
     LED, BUTTON, ADC de bateria). Sem `GPS_RX/TX` (sem GPS).
  2. Bloco de **mapa de irrigação** — macros `IRRIGATION_PIN_*` (ver tabela). Este é o
     único bloco que o utilizador re-pina para a board real.

### 2. Mapper `src/modules/irrigation/IrrigationBoardDefaults.h`

Header puro (sem deps de ESP):

```cpp
void applyBoardIrrigationDefaults(IrrigationSettings &s);
```

- Se as macros `IRRIGATION_PIN_*` estiverem definidas (build `custom_irrigation`),
  preenche os campos de pino da struct (`pinsHbridgeA/B`, `pinsDigitalIn`,
  `digitalInActiveLow`, `pinsGpo`, `pinBtn`, `pinLed`, `pinTamper`, `hwFlags`,
  `numValves`, `sensores[0]` opcional). Mexe **só** em campos de pino/contagem — nunca
  em `role`, `boundGateway`, `configEpoch`, tabelas.
- Se as macros não existirem (qualquer outro build, incl. nativo), é **no-op** → nós
  não-custom continuam a ser provisionados por portal/gateway como hoje.

### 3. Hook em `loadIrrigationSettingsOrDefault()` (IrrigationModule.cpp:84)

```cpp
IrrigationSettings s;
if (!loadIrrigationSettings(s))      // sem blob persistido = 1º boot / pós-factory-reset
    applyBoardIrrigationDefaults(s); // aplica mapa da board
```

Chave: só quando `loadIrrigationSettings` devolve `false`. Depois de provisionar
(wizard §6 / portal), o blob salvo carrega e os defaults são ignorados — **nunca**
sobrescreve um mapa configurado em campo. Os pinos entram em RAM antes do provision,
logo persistem quando o wizard grava a role.

## Mapa de pinos — Heltec V2 (seed, sem GPS)

| Função | Campo | GPIO | Nota |
|---|---|---|---|
| Válvula 0 abre/fecha | `pinsHbridgeA[0]`/`B[0]` | **17 / 23** | output-safe |
| Válvula 1 abre/fecha | `pinsHbridgeA[1]`/`B[1]` | **22 / 32** | output-safe |
| Entrada digital 0 | `pinsDigitalIn[0]` | **38** | input-only, pull-up externo |
| Entrada digital 1 | `pinsDigitalIn[1]` | **39** | input-only, pull-up externo |
| Tamper | `pinTamper` | **37** | input-only; `hwFlags` bit0 = ativo-baixo |
| GPO 0 (relé/bomba) | `pinsGpo[0]` | **33** | output-safe (livre por não ter GPS) |
| GPO 1 | `pinsGpo[1]` | **-1** | sem pino seguro livre na Heltec; ativar na board real |
| Botão multifunção | `pinBtn` | **0** | botão PRG onboard |
| LED de estado | `pinLed` | **25** | LED onboard |
| Sensor 0 (opcional) | `sensores[0].pino` | **36** | ADC1, input-only; **off por default**, exemplo comentado |

`numValves = 2`, `role = ESTACAO`, `pulseMs = 60`, `digitalInActiveLow` = bits 0 e 1
(entradas com pull-up externo → ativo-baixo). RTC = DS3231 externo em I2C (não no blob).

Pinos ocupados pelo firmware Heltec V2 (não usar): LoRa 5/19/27/18 + 26/14/35/34;
OLED 4/15 + reset 16; VEXT 21; LED 25; BUTTON 0; bateria 13. Strapping a evitar como
saída: 2, 12.

## Mapa de pinos — board custom (RX, template)

Bloco espelho no mesmo `variant.h`, atrás de comentário, para preencher quando a board
fechar. Referência do KiCad: latch via **L298N** (Válvula0 = IN1/IN2 → OUT1/2;
Válvula1 = IN3/IN4 → OUT3/4), **relé FINDER SPDT** = um GPO, DIO0–5 para
entradas/tamper/botão. Marcado `// TODO: confirmar em RX.kicad_sch` (board sem RTC
ainda; layout pode mudar).

## Build

```
pio run -e custom_irrigation                    # compila
pio run -e custom_irrigation -t upload           # grava na Heltec de bancada
```

Stock `heltec-v2_0` fica intacto.

## Testes

- Novo suite nativo `test/test_irrigation_board_defaults` (bump
  `test/native-suite-count` 62→63). O nativo **não** tem as macros do variant, então o
  teste define `IRRIGATION_PIN_*` localmente antes de incluir o header e verifica:
  - mapa Heltec → pinos esperados nos campos certos;
  - sentinelas `-1` intactas onde não há macro (GPO1, válvulas 2–7);
  - nenhum campo fora do conjunto de pino/contagem é mutado (role/boundGateway/epoch
    preservados);
  - no-op quando as macros não existem.
- Suite nativa completa deve manter-se GREEN (baseline 953; ABI intacta).
- Build ESP32 `custom_irrigation` no CI/bancada (o variant só compila fora do nativo).

## Fora de escopo / follow-ups

- Preencher pinos reais da board custom (depende do KiCad final + decisão de RTC).
- Registar `hw_model` definitivo da board no protobuf do Meshtastic.
- GPO1 / 2ª bateria / sensores extra → quando a board tiver pinos.
- Bancada 2+ nós: comportamento de rádio (pareamento, config por rádio) não é
  native-testável — validar em bancada.

## Como re-pinar para a board real (fluxo do utilizador)

1. Editar `variants/esp32/custom_irrigation/variant.h`: trocar `board`, os defines de HW
   (LoRa/OLED conforme a board) e o bloco `IRRIGATION_PIN_*` pelos pinos do L298N/relé.
2. `pio run -e custom_irrigation -t upload`.
3. Provisionar papel no wizard de 1º boot; o mapa de pinos já vem do variant.
