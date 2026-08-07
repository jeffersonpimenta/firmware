// custom_irrigation — placa própria de irrigação.
// SEED elétrico = Heltec WiFi LoRa 32 V2, SEM GPS (GPIO33/36 livres p/ irrigação).
// Para a board real: trocar `board` no platformio.ini, os defines de HARDWARE abaixo
// (LoRa/OLED/ADC conforme a board) e o bloco IRRIGATION_* pelos pinos do L298N/relé.

// ================= HARDWARE (Heltec V2, sem GPS) =================
#define HAS_GPS 0 // sem GPS → GPIO33 e GPIO36 ficam livres para irrigação

#ifndef USE_JTAG // gpio15 é TDO do JTAG
#define I2C_SDA 4
#define I2C_SCL 15
#endif

#define RESET_OLED 16
#define VEXT_ENABLE 21 // active low: alimenta OLED e boost da antena LoRa
#define LED_POWER 25   // LED onboard (partilhado com pinLed da irrigação)
#define BUTTON_PIN 0   // botão PRG onboard (partilhado com pinBtn da irrigação)

#define USE_RF95
#define LORA_DIO0 26
#ifndef USE_JTAG
#define LORA_RESET 14
#endif
#define LORA_DIO1 35
#define LORA_DIO2 34

#define ADC_MULTIPLIER 3.2 // divisor R12=100k / R10=220k
#define BATTERY_PIN 13
#define ADC_CHANNEL ADC2_GPIO13_CHANNEL
#define BAT_MEASURE_ADC_UNIT 2

// ================= MAPA DE IRRIGAÇÃO (seed Heltec V2) =================
// Consumido por src/modules/irrigation/IrrigationBoardDefaults.h no 1º boot.
#define IRRIGATION_NUM_VALVES        2
#define IRRIGATION_PIN_VALVE0_A      17 // válvula 0 — abre (H-bridge A)
#define IRRIGATION_PIN_VALVE0_B      23 // válvula 0 — fecha (H-bridge B)
#define IRRIGATION_PIN_VALVE1_A      22 // válvula 1 — abre
#define IRRIGATION_PIN_VALVE1_B      32 // válvula 1 — fecha
#define IRRIGATION_PIN_DIN0          38 // entrada digital 0 (input-only, pull-up externo)
#define IRRIGATION_PIN_DIN1          39 // entrada digital 1 (input-only, pull-up externo)
#define IRRIGATION_DIN_ACTIVE_LOW    0x03 // entradas 0 e 1 ativo-baixo (pull-up externo)
#define IRRIGATION_PIN_TAMPER        37 // tamper (input-only)
#define IRRIGATION_TAMPER_ACTIVE_LOW 1
#define IRRIGATION_PIN_GPO0          33 // saída de nível (relé/bomba) — livre por não ter GPS
#define IRRIGATION_PIN_BTN           0  // botão multifunção (PRG onboard)
#define IRRIGATION_PIN_LED           25 // LED de estado (onboard)
// Opcional — sensor analógico 0 no ADC1 (GPIO36, livre sem GPS). Descomentar p/ ativar:
// #define IRRIGATION_PIN_SENSOR0    36
// GPO1 e válvulas 2..7: sem pino seguro livre na Heltec V2 — ativar na board real.
