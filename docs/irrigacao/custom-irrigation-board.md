# Board `custom_irrigation`

Placa-alvo própria do sistema de irrigação. Hoje é **eletricamente uma Heltec WiFi
LoRa 32 V2, sem GPS** (bancada). Evolui para o hardware custom editando um ficheiro:
`variants/esp32/custom_irrigation/variant.h`.

## Build / flash

```
pio run -e custom_irrigation            # compila
pio run -e custom_irrigation -t upload  # grava na Heltec de bancada
```

O env stock `heltec-v2_0` fica intacto.

## Mapa de pinos — seed Heltec V2 (sem GPS)

| Função | GPIO | Nota |
|---|---|---|
| Válvula 0 abre / fecha | 17 / 23 | H-bridge (latch) |
| Válvula 1 abre / fecha | 22 / 32 | H-bridge (latch) |
| Entrada digital 0 / 1 | 38 / 39 | input-only, **pull-up externo**, ativo-baixo |
| Tamper | 37 | input-only, ativo-baixo |
| GPO 0 (relé/bomba) | 33 | livre por não ter GPS |
| Botão multifunção | 0 | PRG onboard (partilhado c/ botão do sistema) |
| LED de estado | 25 | onboard (partilhado c/ LED do sistema) |
| Sensor 0 (opcional) | 36 | ADC1, off por default |

O mapa é aplicado como **default de 1º boot** (sem config persistida). Depois de
provisionar (wizard/portal), o blob salvo tem prioridade e o mapa da board é ignorado.

## RTC

A board não tem RTC. Para agenda semanal com hora real, ligar um **DS3231 externo** ao
barramento I2C (SDA=GPIO4, SCL=GPIO15, partilhado com o OLED) — o módulo RTC do
Meshtastic auto-deteta. Sem RTC o sistema continua operacional (mirror/manual; scheduler
ocioso sem hora), conforme spec §5.

## Caveats de bancada (Heltec V2)

- Botão (GPIO0) e LED (GPIO25) são partilhados com o botão/LED do sistema Meshtastic;
  gestos de irrigação e ações do sistema coexistem.
- Entradas 38/39/37 são input-only → **exigem pull-up externo**.
- Só cabem 2 válvulas latch; GPO1 e sensores extra ficam para a board real.

## Re-pinar para a board real (L298N + relé FINDER)

1. Editar `variant.h`: trocar `board` (platformio.ini), os defines de HARDWARE e o bloco
   `IRRIGATION_*` pelos pinos do L298N (válvula0 = IN1/IN2, válvula1 = IN3/IN4) e do relé.
2. `pio run -e custom_irrigation -t upload`.
3. Provisionar papel no wizard de 1º boot.
