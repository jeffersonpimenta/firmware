#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/SensorSampler.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

struct FakeReader : ISensorReader {
    uint16_t adc[40] = {};
    bool level[40] = {};
    uint16_t readAdc(int8_t pin) override { return adc[pin]; }
    bool readLevel(int8_t pin) override { return level[pin]; }
};

static IrrigationSettings cfgAnalog()
{
    IrrigationSettings s;
    // pino=36, tipo=1(analógico), flags=0, amostragemS=30, debounceMs=0,
    // adcMin=1000, adcMax=3000, engMin=0, engMax=1000 (0..10,00 bar), unidade=1(bar), pad=0
    s.sensores[0] = {36, 1, 0, 30, 0, 1000, 3000, 0, 1000, 1, 0};
    return s;
}

static IrrigationSettings cfgDigital()
{
    IrrigationSettings s;
    // pino=39, tipo=0(digital), flags=1(ativo-baixo), amostragemS=0, debounceMs=200,
    // adcMin=0, adcMax=4095, engMin=0, engMax=0, unidade=0, pad=0
    s.sensores[0] = {39, 0, 1, 0, 200, 0, 4095, 0, 0, 0, 0};
    return s;
}

static void test_analog_calibration_and_clamp()
{
    SensorSampler sm;
    sm.configure(cfgAnalog());
    FakeReader rd;
    rd.adc[36] = 2000; // meio da faixa → 5,00 bar
    sm.tick(0, rd);
    IrrigationProto::SensorReading out[4];
    TEST_ASSERT_EQUAL_size_t(1, sm.readings(out));
    TEST_ASSERT_EQUAL_INT16(500, out[0].valueCenti);
    rd.adc[36] = 100; // abaixo de adcMin → clamp em engMin
    sm.tick(31000, rd);
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(0, out[0].valueCenti);
}

static void test_analog_sampling_period()
{
    SensorSampler sm;
    sm.configure(cfgAnalog());
    FakeReader rd;
    rd.adc[36] = 2000;
    sm.tick(0, rd);
    rd.adc[36] = 3000;
    sm.tick(10000, rd); // < 30 s: não re-amostra
    IrrigationProto::SensorReading out[4];
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(500, out[0].valueCenti);
    sm.tick(30000, rd); // período vencido
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(1000, out[0].valueCenti);
}

static void test_digital_debounce_and_polarity()
{
    SensorSampler sm;
    sm.configure(cfgDigital());
    FakeReader rd;
    rd.level[39] = true; // ativo-baixo: nível alto = inativo
    sm.tick(0, rd);
    sm.tick(300, rd); // estável 300 ms
    IrrigationProto::SensorReading out[4];
    TEST_ASSERT_EQUAL_size_t(1, sm.readings(out));
    TEST_ASSERT_EQUAL_INT16(0, out[0].valueCenti); // inativo
    rd.level[39] = false; // vai a ativo
    sm.tick(400, rd);     // ainda dentro do debounce
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(0, out[0].valueCenti);
    sm.tick(700, rd); // 300 ms estável > 200 ms
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(100, out[0].valueCenti); // ativo
}

static void test_early_heartbeat_hysteresis_and_ratelimit()
{
    // amostragemS = 1 para a linha do tempo curta do teste
    IrrigationSettings cfg = cfgAnalog();
    cfg.sensores[0].amostragemS = 1;
    SensorSampler sm;
    sm.configure(cfg);
    FakeReader rd;
    rd.adc[36] = 2000; // 5,00 bar
    sm.tick(0, rd);
    sm.noteReported(0); // heartbeat de boot reportou 5,00
    rd.adc[36] = 2010;  // ~5,05: dentro da banda de 2% (span 1000 → banda 20 centi)
    sm.tick(1000, rd);
    TEST_ASSERT_FALSE(sm.earlyHeartbeatDue(1001)); // sem mudança significativa
    rd.adc[36] = 2500; // 7,50: fora da banda → pendente
    sm.tick(2000, rd);
    TEST_ASSERT_FALSE(sm.earlyHeartbeatDue(2001));  // rate-limit: < 30 s do último reporte
    TEST_ASSERT_TRUE(sm.earlyHeartbeatDue(30000));  // 30 s decorridos → dispara
    sm.noteReported(30000);
    TEST_ASSERT_FALSE(sm.earlyHeartbeatDue(30001)); // pendência limpa após reporte
}

static void test_digital_change_arms_early_heartbeat()
{
    SensorSampler sm;
    sm.configure(cfgDigital());
    FakeReader rd;
    rd.level[39] = true;
    sm.tick(0, rd);
    sm.tick(300, rd);
    sm.noteReported(300);
    rd.level[39] = false;
    sm.tick(400, rd);
    sm.tick(700, rd); // estado estável mudou
    TEST_ASSERT_FALSE(sm.earlyHeartbeatDue(701)); // < 30 s do último reporte
    TEST_ASSERT_TRUE(sm.earlyHeartbeatDue(300 + 30000));
}

static void test_transient_settles_back_no_early_heartbeat()
{
    // Transiente que cruza a banda e volta antes do rate-limit NÃO dispara HB antecipado
    IrrigationSettings cfg = cfgAnalog();
    cfg.sensores[0].amostragemS = 1;
    SensorSampler sm;
    sm.configure(cfg);
    FakeReader rd;
    rd.adc[36] = 2000; // 5,00
    sm.tick(0, rd);
    sm.noteReported(0);
    rd.adc[36] = 2500; // 7,50: fora da banda
    sm.tick(1000, rd);
    rd.adc[36] = 2005; // ~5,02: de volta à banda
    sm.tick(2000, rd);
    TEST_ASSERT_FALSE(sm.earlyHeartbeatDue(30001)); // condição viva: nada significativo agora
    rd.adc[36] = 2500; // sai de novo e FICA fora
    sm.tick(3000, rd);
    TEST_ASSERT_TRUE(sm.earlyHeartbeatDue(30002));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_analog_calibration_and_clamp);
    RUN_TEST(test_analog_sampling_period);
    RUN_TEST(test_digital_debounce_and_polarity);
    RUN_TEST(test_early_heartbeat_hysteresis_and_ratelimit);
    RUN_TEST(test_digital_change_arms_early_heartbeat);
    RUN_TEST(test_transient_settles_back_no_early_heartbeat);
    exit(UNITY_END());
}

void loop() {}
