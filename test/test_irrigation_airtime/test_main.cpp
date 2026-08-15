#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationAirtime.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <unity.h>

using namespace IrrigationAirtime;
using namespace IrrigationProto;

void setUp(void) {}
void tearDown(void) {}

static void test_priority_command_is_high()
{
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_HIGH, priorityForType(MSG_CMD_VALVULA, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_HIGH, priorityForType(MSG_CMD_GPO, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_HIGH, priorityForType(MSG_REMOTE_CMD, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_HIGH, priorityForType(MSG_REMOTE_TRIGGER, false));
}

static void test_priority_ack_and_hb_and_survey()
{
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_RESPONSE, priorityForType(MSG_ACK, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_BACKGROUND, priorityForType(MSG_HEARTBEAT, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_BACKGROUND, priorityForType(MSG_PING_SURVEY, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_DEFAULT, priorityForType(MSG_SET_CONFIG, false));
}

static void test_priority_event_critical_vs_routine()
{
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_ALERT, priorityForType(MSG_EVENTO, true));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_DEFAULT, priorityForType(MSG_EVENTO, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_ALERT, priorityForType(MSG_CMD_MAINT, false));
}

static void test_isGatedType()
{
    TEST_ASSERT_TRUE(isGatedType(MSG_HEARTBEAT));
    TEST_ASSERT_TRUE(isGatedType(MSG_PING_SURVEY));
    TEST_ASSERT_FALSE(isGatedType(MSG_CMD_VALVULA));
    TEST_ASSERT_FALSE(isGatedType(MSG_ACK));
    TEST_ASSERT_FALSE(isGatedType(MSG_EVENTO));
}

static void test_isCriticalEvent()
{
    TEST_ASSERT_TRUE(isCriticalEvent(EV_TAMPER));
    TEST_ASSERT_FALSE(isCriticalEvent(EV_MANUAL_OPEN));
    TEST_ASSERT_FALSE(isCriticalEvent(EV_PAIRED));
}

static void test_hbBackoffFactor_bands()
{
    TEST_ASSERT_EQUAL_UINT8(1, hbBackoffFactor(0.0f));
    TEST_ASSERT_EQUAL_UINT8(1, hbBackoffFactor(24.9f));
    TEST_ASSERT_EQUAL_UINT8(2, hbBackoffFactor(25.0f));
    TEST_ASSERT_EQUAL_UINT8(2, hbBackoffFactor(39.9f));
    TEST_ASSERT_EQUAL_UINT8(4, hbBackoffFactor(40.0f));
    TEST_ASSERT_EQUAL_UINT8(4, hbBackoffFactor(59.9f));
    TEST_ASSERT_EQUAL_UINT8(8, hbBackoffFactor(60.0f));
    TEST_ASSERT_EQUAL_UINT8(8, hbBackoffFactor(100.0f));
}

static void test_hbWindowMs_caps_at_30s()
{
    TEST_ASSERT_EQUAL_UINT32(15000, hbWindowMs(60000));   // 60s/4
    TEST_ASSERT_EQUAL_UINT32(30000, hbWindowMs(600000));  // 10min/4 -> cap 30s
    TEST_ASSERT_EQUAL_UINT32(0, hbWindowMs(0));
}

static void test_hbJitterOffset_deterministic_in_window()
{
    uint32_t w = 30000;
    uint32_t a = hbJitterOffsetMs(0x11111111, 5, w);
    uint32_t b = hbJitterOffsetMs(0x11111111, 5, w);
    TEST_ASSERT_EQUAL_UINT32(a, b);        // determinístico
    TEST_ASSERT_LESS_THAN_UINT32(w, a);    // dentro da janela

    uint32_t c = hbJitterOffsetMs(0x11111111, 6, w);
    TEST_ASSERT_NOT_EQUAL(a, c);           // muda com epoch

    uint32_t d = hbJitterOffsetMs(0x22222222, 5, w);
    TEST_ASSERT_NOT_EQUAL(a, d);           // muda com nodeNum

    TEST_ASSERT_EQUAL_UINT32(0, hbJitterOffsetMs(0x11111111, 5, 0)); // janela 0
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_priority_command_is_high);
    RUN_TEST(test_priority_ack_and_hb_and_survey);
    RUN_TEST(test_priority_event_critical_vs_routine);
    RUN_TEST(test_isGatedType);
    RUN_TEST(test_isCriticalEvent);
    RUN_TEST(test_hbBackoffFactor_bands);
    RUN_TEST(test_hbWindowMs_caps_at_30s);
    RUN_TEST(test_hbJitterOffset_deterministic_in_window);
    UNITY_END();
}

void loop() {}
