#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ValveController.h"
#include <unity.h>

class MockDriver : public IValveDriver
{
  public:
    int opens[ValveController::MAX_VALVES] = {};
    int closes[ValveController::MAX_VALVES] = {};
    void pulse(uint8_t index, bool open) override
    {
        if (open)
            opens[index]++;
        else
            closes[index]++;
    }
};

void setUp(void) {}
void tearDown(void) {}

static void test_open_pulsesOnceAndTracksState()
{
    MockDriver d;
    ValveController vc(d, 2);
    TEST_ASSERT_EQUAL(ValveController::Result::OK, vc.open(0, 60, 0, 1000));
    TEST_ASSERT_TRUE(vc.isOpen(0));
    TEST_ASSERT_EQUAL_INT(1, d.opens[0]);
    TEST_ASSERT_EQUAL_UINT8(0b01, vc.stateBitmap());
}

static void test_failsafeTimer_closesOnExpiry()
{
    MockDriver d;
    ValveController vc(d, 2);
    vc.open(0, 60, 0, 1000);
    vc.tick(1000 + 59999);
    TEST_ASSERT_TRUE(vc.isOpen(0));
    vc.tick(1000 + 60000);
    TEST_ASSERT_FALSE(vc.isOpen(0));
    TEST_ASSERT_EQUAL_INT(1, d.closes[0]);
}

static void test_renewal_restartsTimerWithoutRepulse()
{
    MockDriver d;
    ValveController vc(d, 2);
    vc.open(0, 60, 0, 1000);
    // Renovação (spec §4.2): abrir de novo estende, não repulsa
    TEST_ASSERT_EQUAL(ValveController::Result::OK, vc.open(0, 60, 0, 50000));
    TEST_ASSERT_EQUAL_INT(1, d.opens[0]);
    vc.tick(1000 + 60000); // timer antigo já teria expirado
    TEST_ASSERT_TRUE(vc.isOpen(0));
    vc.tick(50000 + 60000);
    TEST_ASSERT_FALSE(vc.isOpen(0));
}

static void test_durationClampedToCompiledCeiling()
{
    MockDriver d;
    ValveController vc(d, 1);
    vc.open(0, 999999, 0, 0); // pede ~11 dias
    vc.tick(ValveController::MAX_OPEN_SECONDS * 1000u - 1);
    TEST_ASSERT_TRUE(vc.isOpen(0));
    vc.tick(ValveController::MAX_OPEN_SECONDS * 1000u);
    TEST_ASSERT_FALSE(vc.isOpen(0)); // teto de 120 min venceu
}

static void test_configMaxTightensDuration()
{
    MockDriver d;
    ValveController vc(d, 1);
    vc.open(0, 3600, 600, 0); // config local limita a 10 min
    vc.tick(600 * 1000u);
    TEST_ASSERT_FALSE(vc.isOpen(0));
}

static void test_batteryLockout_blocksOpenAllowsClose()
{
    MockDriver d;
    ValveController vc(d, 1);
    vc.open(0, 60, 0, 0);
    vc.setBatteryLockout(true);
    TEST_ASSERT_EQUAL(ValveController::Result::BATTERY_LOW, vc.open(0, 60, 0, 1000));
    TEST_ASSERT_EQUAL(ValveController::Result::OK, vc.close(0)); // fechar sempre aceito
    TEST_ASSERT_FALSE(vc.isOpen(0));
}

static void test_invalidIdAndZeroDuration_rejected()
{
    MockDriver d;
    ValveController vc(d, 2);
    TEST_ASSERT_EQUAL(ValveController::Result::INVALID_ID, vc.open(2, 60, 0, 0));
    TEST_ASSERT_EQUAL(ValveController::Result::ZERO_DURATION, vc.open(0, 0, 0, 0));
    TEST_ASSERT_EQUAL(ValveController::Result::INVALID_ID, vc.close(5));
}

static void test_tickHandlesMillisRollover()
{
    MockDriver d;
    ValveController vc(d, 1);
    vc.open(0, 60, 0, 0xFFFFF000u); // expira depois do wrap de millis()
    vc.tick(0xFFFFFF00u);
    TEST_ASSERT_TRUE(vc.isOpen(0));
    vc.tick(0xFFFFF000u + 60000); // wrapped
    TEST_ASSERT_FALSE(vc.isOpen(0));
}

static void test_closeAll()
{
    MockDriver d;
    ValveController vc(d, 3);
    vc.open(0, 60, 0, 0);
    vc.open(2, 60, 0, 0);
    vc.closeAll();
    TEST_ASSERT_EQUAL_UINT8(0, vc.stateBitmap());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_open_pulsesOnceAndTracksState);
    RUN_TEST(test_failsafeTimer_closesOnExpiry);
    RUN_TEST(test_renewal_restartsTimerWithoutRepulse);
    RUN_TEST(test_durationClampedToCompiledCeiling);
    RUN_TEST(test_configMaxTightensDuration);
    RUN_TEST(test_batteryLockout_blocksOpenAllowsClose);
    RUN_TEST(test_invalidIdAndZeroDuration_rejected);
    RUN_TEST(test_tickHandlesMillisRollover);
    RUN_TEST(test_closeAll);
    exit(UNITY_END());
}

void loop() {}
