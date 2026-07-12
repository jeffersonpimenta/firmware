#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/Allowlist.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include "modules/irrigation/Pairing.h"
#include <string.h>
#include <unity.h>

using State = StationPairing::State;

void setUp(void) {}
void tearDown(void) {}

static IrrigationProto::PairGrant makeGrant()
{
    IrrigationProto::PairGrant g = {};
    for (int i = 0; i < 32; i++)
        g.psk[i] = (uint8_t)i;
    g.nameLen = 4;
    memcpy(g.channelName, "farm", 5);
    g.gatewayId = 0xa1b2c3d4;
    return g;
}

static void test_station_windowLifecycleAndAnnounceCadence()
{
    StationPairing sp;
    TEST_ASSERT_EQUAL(State::IDLE, sp.state());
    TEST_ASSERT_FALSE(sp.announceDue(0));

    sp.openWindow(1000);
    TEST_ASSERT_EQUAL(State::WINDOW, sp.state());
    TEST_ASSERT_TRUE(sp.announceDue(1000));   // primeiro imediato
    TEST_ASSERT_FALSE(sp.announceDue(5000));  // <10 s
    TEST_ASSERT_TRUE(sp.announceDue(11000));  // 10 s depois
    sp.tick(1000 + 2 * 60 * 1000 + 1);        // janela expira
    TEST_ASSERT_EQUAL(State::IDLE, sp.state());
    TEST_ASSERT_FALSE(sp.announceDue(200000));
}

static void test_station_grantOnlyInsideWindow()
{
    StationPairing sp;
    TEST_ASSERT_FALSE(sp.onGrant(makeGrant(), 0)); // IDLE: ignora
    sp.openWindow(1000);
    TEST_ASSERT_TRUE(sp.onGrant(makeGrant(), 2000));
    TEST_ASSERT_EQUAL(State::COMMITTED, sp.state());
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, sp.grant().gatewayId);
    TEST_ASSERT_FALSE(sp.announceDue(3000)); // após commit, para de anunciar
}

static void test_gateway_windowAndCooldown()
{
    GatewayPairing gp;
    TEST_ASSERT_FALSE(gp.approveAnnounce(0x11, 0)); // janela fechada
    gp.openWindow(1000);
    TEST_ASSERT_TRUE(gp.windowOpen());
    TEST_ASSERT_TRUE(gp.approveAnnounce(0x11, 2000));
    TEST_ASSERT_FALSE(gp.approveAnnounce(0x11, 4000)); // cooldown 5 s
    TEST_ASSERT_TRUE(gp.approveAnnounce(0x22, 4000));  // outro nó, independente
    TEST_ASSERT_TRUE(gp.approveAnnounce(0x11, 8000));  // cooldown venceu
    gp.tick(1000 + 2 * 60 * 1000 + 1);
    TEST_ASSERT_FALSE(gp.windowOpen());
    TEST_ASSERT_FALSE(gp.approveAnnounce(0x33, 200000));
}

static void test_allowlist_addRemoveContainsIdempotent()
{
    Allowlist al;
    TEST_ASSERT_TRUE(al.add(0x11));
    TEST_ASSERT_TRUE(al.add(0x11)); // idempotente
    TEST_ASSERT_EQUAL_UINT(1, al.count());
    TEST_ASSERT_TRUE(al.contains(0x11));
    TEST_ASSERT_TRUE(al.remove(0x11));
    TEST_ASSERT_FALSE(al.contains(0x11));
    TEST_ASSERT_FALSE(al.remove(0x11));
}

static void test_allowlist_fullRejects()
{
    Allowlist al;
    for (uint32_t i = 1; i <= Allowlist::MAX; i++)
        TEST_ASSERT_TRUE(al.add(i));
    TEST_ASSERT_FALSE(al.add(999));
    TEST_ASSERT_EQUAL_UINT(Allowlist::MAX, al.count());
}

static void test_allowlist_serializeRoundTripAndRejectsGarbage()
{
    Allowlist al;
    al.add(0x11);
    al.add(0x22);
    uint8_t buf[128];
    size_t n = al.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    Allowlist copy;
    TEST_ASSERT_TRUE(copy.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(2, copy.count());
    TEST_ASSERT_TRUE(copy.contains(0x22));

    buf[0] ^= 0xFF; // magic corrompido
    Allowlist bad;
    TEST_ASSERT_FALSE(bad.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(0, bad.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_station_windowLifecycleAndAnnounceCadence);
    RUN_TEST(test_station_grantOnlyInsideWindow);
    RUN_TEST(test_gateway_windowAndCooldown);
    RUN_TEST(test_allowlist_addRemoveContainsIdempotent);
    RUN_TEST(test_allowlist_fullRejects);
    RUN_TEST(test_allowlist_serializeRoundTripAndRejectsGarbage);
    exit(UNITY_END());
}

void loop() {}
