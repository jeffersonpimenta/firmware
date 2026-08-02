#include "Arduino.h"
#include "TestUtil.h"

// Simula o variant custom_irrigation (seed Heltec V2, sem GPS): define as macros
// ANTES de incluir o header do mapper.
#define IRRIGATION_NUM_VALVES        2
#define IRRIGATION_PIN_VALVE0_A      17
#define IRRIGATION_PIN_VALVE0_B      23
#define IRRIGATION_PIN_VALVE1_A      22
#define IRRIGATION_PIN_VALVE1_B      32
#define IRRIGATION_PIN_DIN0          38
#define IRRIGATION_PIN_DIN1          39
#define IRRIGATION_DIN_ACTIVE_LOW    0x03
#define IRRIGATION_PIN_TAMPER        37
#define IRRIGATION_TAMPER_ACTIVE_LOW 1
#define IRRIGATION_PIN_GPO0          33
#define IRRIGATION_PIN_BTN           0
#define IRRIGATION_PIN_LED           25

#include "modules/irrigation/IrrigationBoardDefaults.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_heltec_map_pins()
{
    IrrigationSettings s; // defaults (pinos -1)
    s.numValves = 5;      // sentinela: prova que o mapper escreve numValves (default do struct já é 2)
    applyBoardIrrigationDefaults(s);
    TEST_ASSERT_EQUAL_INT8(2, s.numValves);
    TEST_ASSERT_EQUAL_INT8(17, s.pinsHbridgeA[0]);
    TEST_ASSERT_EQUAL_INT8(23, s.pinsHbridgeB[0]);
    TEST_ASSERT_EQUAL_INT8(22, s.pinsHbridgeA[1]);
    TEST_ASSERT_EQUAL_INT8(32, s.pinsHbridgeB[1]);
    TEST_ASSERT_EQUAL_INT8(38, s.pinsDigitalIn[0]);
    TEST_ASSERT_EQUAL_INT8(39, s.pinsDigitalIn[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, s.digitalInActiveLow);
    TEST_ASSERT_EQUAL_INT8(37, s.pinTamper);
    TEST_ASSERT_EQUAL_UINT8(0x01, s.hwFlags & 0x01);
    TEST_ASSERT_EQUAL_INT8(33, s.pinsGpo[0]);
    TEST_ASSERT_EQUAL_INT8(0, s.pinBtn);
    TEST_ASSERT_EQUAL_INT8(25, s.pinLed);
}

static void test_sentinels_untouched()
{
    IrrigationSettings s;
    applyBoardIrrigationDefaults(s);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsHbridgeA[2]); // válvula não mapeada
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsHbridgeB[7]);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsGpo[1]);       // GPO1 não mapeado
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsDigitalIn[2]); // entrada não mapeada
}

static void test_non_pin_fields_preserved()
{
    IrrigationSettings s;
    s.role = (uint8_t)IrrigationRole::GATEWAY;
    s.boundGateway = 0xAABBCCDD;
    s.configEpoch = 42;
    applyBoardIrrigationDefaults(s);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)IrrigationRole::GATEWAY, s.role);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDD, s.boundGateway);
    TEST_ASSERT_EQUAL_UINT32(42, s.configEpoch);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_heltec_map_pins);
    RUN_TEST(test_sentinels_untouched);
    RUN_TEST(test_non_pin_fields_preserved);
    exit(UNITY_END());
}
void loop() {}
