#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationSettings.h"
#include <new>
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// Monta uma imagem v1 (40 B) byte a byte, no layout documentado no header da v1.
static void buildV1Image(uint8_t *img)
{
    memset(img, 0, IRRIGATION_SETTINGS_V1_SIZE);
    uint32_t magic = IrrigationSettings::MAGIC;
    uint16_t version = 1;
    memcpy(img + 0, &magic, 4);
    memcpy(img + 4, &version, 2);
    img[6] = 1;                       // role = GATEWAY
    img[7] = 4;                       // numValves
    uint32_t gw = 0xa1b2c3d4;
    memcpy(img + 8, &gw, 4);          // boundGateway
    uint16_t hb = 15, vbat = 1200, maxOpen = 600;
    memcpy(img + 12, &hb, 2);
    memcpy(img + 14, &vbat, 2);
    memcpy(img + 16, &maxOpen, 2);
    img[18] = 6;                      // cmdRatePerMin  (offset 19 = padding)
    uint16_t pulse = 80;
    memcpy(img + 20, &pulse, 2);
    for (int i = 0; i < 8; i++) {
        img[22 + i] = (uint8_t)(int8_t)(i < 4 ? 10 + i : -1); // pinsHbridgeA
        img[30 + i] = (uint8_t)(int8_t)(i < 4 ? 20 + i : -1); // pinsHbridgeB
    }
}

static void test_migrate_v1_preservesFieldsAndDefaultsNew()
{
    uint8_t img[IRRIGATION_SETTINGS_V1_SIZE];
    buildV1Image(img);
    IrrigationSettings s;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(img, sizeof(img), s));
    TEST_ASSERT_EQUAL_UINT16(2, s.version);
    TEST_ASSERT_EQUAL_UINT8(1, s.role);
    TEST_ASSERT_EQUAL_UINT8(4, s.numValves);
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, s.boundGateway);
    TEST_ASSERT_EQUAL_UINT16(15, s.hbMinutes);
    TEST_ASSERT_EQUAL_UINT16(1200, s.vbatMinAbrirCentiV);
    TEST_ASSERT_EQUAL_UINT16(600, s.maxOpenConfigS);
    TEST_ASSERT_EQUAL_UINT8(6, s.cmdRatePerMin);
    TEST_ASSERT_EQUAL_UINT16(80, s.pulseMs);
    TEST_ASSERT_EQUAL_INT8(12, s.pinsHbridgeA[2]);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsHbridgeA[7]);
    TEST_ASSERT_EQUAL_INT8(22, s.pinsHbridgeB[2]);
    // Campos novos: defaults
    TEST_ASSERT_EQUAL_UINT32(0, s.configEpoch);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsDigitalIn[0]);
    TEST_ASSERT_EQUAL_UINT8(0, s.digitalInActiveLow);
}

static void test_migrate_v2_passthrough()
{
    IrrigationSettings in;
    in.configEpoch = 17;
    in.pinsDigitalIn[1] = 36;
    in.digitalInActiveLow = 0b0010;
    in.numValves = 3;
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings((const uint8_t *)&in, sizeof(in), out));
    TEST_ASSERT_EQUAL_UINT32(17, out.configEpoch);
    TEST_ASSERT_EQUAL_INT8(36, out.pinsDigitalIn[1]);
    TEST_ASSERT_EQUAL_UINT8(0b0010, out.digitalInActiveLow);
    TEST_ASSERT_EQUAL_UINT8(3, out.numValves);
}

static void test_migrate_rejectsBadInput()
{
    IrrigationSettings s;
    uint8_t img[IRRIGATION_SETTINGS_V1_SIZE];
    buildV1Image(img);

    TEST_ASSERT_FALSE(migrateIrrigationSettings(img, 10, s)); // curto

    img[0] ^= 0xFF; // magic errado
    TEST_ASSERT_FALSE(migrateIrrigationSettings(img, sizeof(img), s));
    img[0] ^= 0xFF;

    img[4] = 3; // versão desconhecida
    TEST_ASSERT_FALSE(migrateIrrigationSettings(img, sizeof(img), s));
}

static void test_migrate_v2_wrongSize_rejected()
{
    IrrigationSettings in;
    IrrigationSettings out;
    TEST_ASSERT_FALSE(migrateIrrigationSettings((const uint8_t *)&in, sizeof(in) - 1, out));
}

static void test_defaultStruct_bytesDeterministic()
{
    // Constrói duas instâncias sobre lixo de memória diferente: todos os 52 bytes
    // (incluindo padding explícito) devem ser idênticos — CRC estável p/ o gateway.
    alignas(IrrigationSettings) uint8_t rawA[sizeof(IrrigationSettings)];
    alignas(IrrigationSettings) uint8_t rawB[sizeof(IrrigationSettings)];
    memset(rawA, 0xAA, sizeof(rawA));
    memset(rawB, 0x55, sizeof(rawB));
    IrrigationSettings *a = new (rawA) IrrigationSettings();
    IrrigationSettings *b = new (rawB) IrrigationSettings();
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(IrrigationSettings));
    a->~IrrigationSettings();
    b->~IrrigationSettings();
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_migrate_v1_preservesFieldsAndDefaultsNew);
    RUN_TEST(test_migrate_v2_passthrough);
    RUN_TEST(test_migrate_rejectsBadInput);
    RUN_TEST(test_migrate_v2_wrongSize_rejected);
    RUN_TEST(test_defaultStruct_bytesDeterministic);
    exit(UNITY_END());
}

void loop() {}
