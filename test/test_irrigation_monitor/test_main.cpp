#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/StationMonitor.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_batteryLevelsWithHysteresis()
{
    StationMonitor m;
    Alert out[2];
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1250, 0, 1000, out)); // normal
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1210, 0, 2000, out)); // < 12,2
    TEST_ASSERT_EQUAL(AlertType::BATT_AVISO, out[0].type);
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1215, 0, 3000, out)); // dentro da histerese: nada
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1170, 0, 4000, out)); // < 11,8
    TEST_ASSERT_EQUAL(AlertType::BATT_CRITICO, out[0].type);
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1140, 0, 5000, out)); // < 11,5
    TEST_ASSERT_EQUAL(AlertType::BATT_HIBERNACAO, out[0].type);
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1190, 0, 6000, out)); // subiu mas < crítico+hist? 1190 >= 1180+20? não (1200) → continua hibern.? sobe p/ crítico? ver regra
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1250, 0, 7000, out)); // >= 1220+20 → recuperou
    TEST_ASSERT_EQUAL(AlertType::BATT_RECUPEROU, out[0].type);
}

static void test_silenceFiresOnceAndBackOnline()
{
    StationMonitor m;
    Alert out[2], a;
    m.onHeartbeat(0x11, 1250, 0, 1000, out);
    TEST_ASSERT_FALSE(m.checkSilence(0x11, 60000, 50000, a));
    TEST_ASSERT_TRUE(m.checkSilence(0x11, 60000, 62000, a));
    TEST_ASSERT_EQUAL(AlertType::SILENT, a.type);
    TEST_ASSERT_FALSE(m.checkSilence(0x11, 60000, 70000, a)); // 1× só
    int n = m.onHeartbeat(0x11, 1250, 0, 80000, out);         // voltou
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(AlertType::BACK_ONLINE, out[0].type);
}

static void test_silenceNeverHeardDoesNotFire()
{
    StationMonitor m;
    Alert a;
    TEST_ASSERT_FALSE(m.checkSilence(0x99, 60000, 100000, a));
}

static void test_rebootAnomaly()
{
    StationMonitor m;
    Alert out[2];
    m.onHeartbeat(0x11, 1250, 10, 1000, out);                          // baseline 10
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1250, 14, 2000, out)); // +4
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1250, 16, 3000, out)); // +6 > 5
    TEST_ASSERT_EQUAL(AlertType::REBOOT_ANOMALY, out[0].type);
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1250, 18, 4000, out)); // 1× por janela
}

static void test_alertRing()
{
    AlertCenter c;
    for (uint32_t i = 0; i < 40; i++)
        c.push({AlertType::SILENT, i, 0, i});
    TEST_ASSERT_EQUAL_UINT(AlertCenter::MAX, c.count());
    TEST_ASSERT_EQUAL_UINT32(39, c.at(0).node);                    // mais recente
    TEST_ASSERT_EQUAL_UINT32(8, c.at(AlertCenter::MAX - 1).node);  // mais antigo restante
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_batteryLevelsWithHysteresis);
    RUN_TEST(test_silenceFiresOnceAndBackOnline);
    RUN_TEST(test_silenceNeverHeardDoesNotFire);
    RUN_TEST(test_rebootAnomaly);
    RUN_TEST(test_alertRing);
    exit(UNITY_END());
}

void loop() {}
