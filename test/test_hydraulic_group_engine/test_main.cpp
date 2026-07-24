#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/HydraulicGroupEngine.h"
#include <unity.h>

using State = HydraulicGroupEngine::State;

static const uint32_t NODE = 0xAABBCCDD;

void setUp(void) {}
void tearDown(void) {}

// Monta ZoneTable: válvulas 1,2 (tipo valvula, index 0/1) + bomba zona 9 (gpo, index 0), todas em NODE.
static void seedZones(ZoneTable &z)
{
    Zone v1; v1.id = 1; v1.node = NODE; v1.tipo = 0; v1.index = 0; v1.maxMin = 120; z.upsert(v1);
    Zone v2; v2.id = 2; v2.node = NODE; v2.tipo = 0; v2.index = 1; v2.maxMin = 120; z.upsert(v2);
    Zone pb; pb.id = 9; pb.node = NODE; pb.tipo = 1; pb.index = 0; pb.maxMin = 120; z.upsert(pb);
}

static HydraulicGroup grp()
{
    HydraulicGroup g;
    g.id = 1; g.bombaZoneId = 9;
    g.zoneIds[0] = 1; g.zoneIds[1] = 2; g.zoneCount = 2;
    g.minOpen = 1; g.maxOpen = 1; g.transicao = 0; // abrir_antes_de_fechar
    g.overlapS = 10; g.startAfterOpenS = 5; g.stopBeforeCloseS = 8;
    g.minRunMin = 0; g.maxStartsHour = 0; // sem bridging/rate p/ este teste
    return g;
}

// helper: dá 1 tick e devolve o único emit esperado (falha se != 1).
static GroupEmit tick1(HydraulicGroupEngine &e, const HydraulicGroupTable &t, const ZoneTable &z, uint32_t ms)
{
    GroupEmit out[4];
    size_t n = e.tick(t, z, ms, out, 4);
    TEST_ASSERT_EQUAL_UINT32(1, n);
    return out[0];
}

static void tickNone(HydraulicGroupEngine &e, const HydraulicGroupTable &t, const ZoneTable &z, uint32_t ms)
{
    GroupEmit out[4];
    TEST_ASSERT_EQUAL_UINT32(0, e.tick(t, z, ms, out, 4));
}

static void test_happy_path_min1()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();

    // Cronograma manda abrir V1 por 600 s.
    e.setDesired(1, 1, true, 600);

    // tick 1: abre V1.
    GroupEmit em = tick1(e, t, z, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, em.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, em.action);
    TEST_ASSERT_EQUAL_UINT16(600, em.durationS);
    TEST_ASSERT_EQUAL_HEX32(NODE, em.node);
    e.noteSent(NODE, 1, 1, 501);

    // sem ACK ainda -> nada.
    tickNone(e, t, z, 1000);
    e.onAck(NODE, 501); // V1 confirmada

    // ainda dentro de startAfterOpenS (5 s) -> nada.
    tickNone(e, t, z, 2000);

    // 5 s depois: liga bomba (zona 9, gpo, index 0), dur = 600+120.
    GroupEmit pump = tick1(e, t, z, 6000);
    TEST_ASSERT_EQUAL_UINT8(9, pump.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, pump.action);
    TEST_ASSERT_EQUAL_UINT8(1, pump.tipo);
    TEST_ASSERT_EQUAL_UINT16(720, pump.durationS);
    e.noteSent(NODE, 9, 1, 502);
    e.onAck(NODE, 502);

    // RUNNING, desejo satisfeito -> nada.
    tickNone(e, t, z, 6000);
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));
    TEST_ASSERT_TRUE(e.pumpOn(1));

    // Transição: cronograma fecha V1, abre V2 (mesmo tick).
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);

    // abrir_antes_de_fechar: abre V2 primeiro.
    GroupEmit o2 = tick1(e, t, z, 100000);
    TEST_ASSERT_EQUAL_UINT8(2, o2.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, o2.action);
    e.noteSent(NODE, 2, 1, 503);
    e.onAck(NODE, 503);

    // durante a sobreposição (10 s) -> nada.
    tickNone(e, t, z, 105000);

    // após overlap: fecha V1.
    GroupEmit c1 = tick1(e, t, z, 110001);
    TEST_ASSERT_EQUAL_UINT8(1, c1.zoneId);
    TEST_ASSERT_EQUAL_UINT8(0, c1.action);
    e.noteSent(NODE, 1, 0, 504);
    e.onAck(NODE, 504);
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));

    // Fim: cronograma fecha V2 (última).
    e.setDesired(1, 2, false, 0);

    // desliga bomba primeiro.
    GroupEmit poff = tick1(e, t, z, 700000);
    TEST_ASSERT_EQUAL_UINT8(9, poff.zoneId);
    TEST_ASSERT_EQUAL_UINT8(0, poff.action);
    e.noteSent(NODE, 9, 0, 505);
    e.onAck(NODE, 505);
    TEST_ASSERT_FALSE(e.pumpOn(1));

    // drena parar_antes_de_fechar_s (8 s) -> nada.
    tickNone(e, t, z, 702000);

    // fecha última válvula.
    GroupEmit clast = tick1(e, t, z, 708001);
    TEST_ASSERT_EQUAL_UINT8(2, clast.zoneId);
    TEST_ASSERT_EQUAL_UINT8(0, clast.action);
    e.noteSent(NODE, 2, 0, 506);
    e.onAck(NODE, 506);

    tickNone(e, t, z, 708001);
    TEST_ASSERT_EQUAL(State::IDLE, e.stateOf(1));
}

static void test_fechar_antes_de_abrir()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.transicao = 1; // fechar_antes_de_abrir
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();

    e.setDesired(1, 1, true, 600);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1); // abre V1
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2); // bomba
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));

    // transição: fecha V1 ANTES de abrir V2 (sem overlap).
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);
    GroupEmit c1 = tick1(e, t, z, 100000);
    TEST_ASSERT_EQUAL_UINT8(1, c1.zoneId);
    TEST_ASSERT_EQUAL_UINT8(0, c1.action); // fecha primeiro
    e.noteSent(NODE, 1, 0, 3); e.onAck(NODE, 3);
    GroupEmit o2 = tick1(e, t, z, 100000);
    TEST_ASSERT_EQUAL_UINT8(2, o2.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, o2.action); // abre depois
}

static void test_min_open_2()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.minOpen = 2; g.maxOpen = 2;
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();

    e.setDesired(1, 1, true, 600);
    e.setDesired(1, 2, true, 600);
    // abre as 2 válvulas antes da bomba.
    GroupEmit a = tick1(e, t, z, 1000); e.noteSent(NODE, a.zoneId, 1, 1); e.onAck(NODE, 1);
    GroupEmit b = tick1(e, t, z, 1000); e.noteSent(NODE, b.zoneId, 1, 2); e.onAck(NODE, 2);
    TEST_ASSERT_TRUE((a.zoneId == 1 && b.zoneId == 2) || (a.zoneId == 2 && b.zoneId == 1));
    TEST_ASSERT_EQUAL_UINT8(1, a.action);
    TEST_ASSERT_EQUAL_UINT8(1, b.action);
    // só então a bomba.
    GroupEmit pump = tick1(e, t, z, 6000);
    TEST_ASSERT_EQUAL_UINT8(9, pump.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, pump.action);
    e.noteSent(NODE, 9, 1, 3);
    e.onAck(NODE, 3);
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));
    TEST_ASSERT_TRUE(e.pumpOn(1));
}

static void test_renova_bomba_na_transicao()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();

    e.setDesired(1, 1, true, 600);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    GroupEmit p1 = tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    TEST_ASSERT_EQUAL_UINT16(720, p1.durationS);

    // transição V1->V2 com nova duração menor.
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 300);
    GroupEmit o2 = tick1(e, t, z, 100000); e.noteSent(NODE, 2, 1, 3); e.onAck(NODE, 3); // abre V2
    GroupEmit c1 = tick1(e, t, z, 110001); e.noteSent(NODE, 1, 0, 4); e.onAck(NODE, 4); // fecha V1
    // renovação: bomba re-enviada com dur = 300+120 = 420.
    GroupEmit pr = tick1(e, t, z, 110001);
    TEST_ASSERT_EQUAL_UINT8(9, pr.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, pr.action);
    TEST_ASSERT_EQUAL_UINT16(420, pr.durationS);
}

static void test_open_next_falha_renova_corrente()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();
    // liga V1 (600 s) + bomba
    e.setDesired(1, 1, true, 600);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    // transição: abre V2 mas FALHA (sem ACK após retries)
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);
    GroupEmit o2 = tick1(e, t, z, 100000); e.noteSent(NODE, 2, 1, 3);
    e.onCmdFailed(NODE, 2, 1);
    // V1 aberta em t=1000 por 600 s -> expira ~601000ms; agora 100000 -> longe do prazo -> renova V1.
    GroupEmit renov = tick1(e, t, z, 100000);
    TEST_ASSERT_EQUAL_UINT8(1, renov.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, renov.action);
    HydraulicGroupEngine::GroupAlert al;
    bool got = false;
    while (e.takeAlert(al)) if (al.code == HydraulicGroupEngine::GA_OPEN_FAIL_RENEW) got = true;
    TEST_ASSERT_TRUE(got);
}

static void test_open_next_falha_sem_renovar_desliga_bomba()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();
    e.setDesired(1, 1, true, 30); // dur curta: 30 s
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1); // V1 expira ~31000ms
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);
    tick1(e, t, z, 25000); e.noteSent(NODE, 2, 1, 3);
    e.onCmdFailed(NODE, 2, 1); // perto do prazo de V1 (31000)
    GroupEmit poff = tick1(e, t, z, 30000);
    TEST_ASSERT_EQUAL_UINT8(9, poff.zoneId); // desliga a bomba primeiro
    TEST_ASSERT_EQUAL_UINT8(0, poff.action);
    TEST_ASSERT_EQUAL(State::PUMP_OFF_WAIT, e.stateOf(1));
    HydraulicGroupEngine::GroupAlert al;
    bool got = false;
    while (e.takeAlert(al)) if (al.code == HydraulicGroupEngine::GA_OPEN_FAIL_PUMPOFF) got = true;
    TEST_ASSERT_TRUE(got);
}

static void test_close_prev_falha_alerta()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();
    e.setDesired(1, 1, true, 600);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);
    tick1(e, t, z, 100000); e.noteSent(NODE, 2, 1, 3); e.onAck(NODE, 3); // abre V2
    GroupEmit c1 = tick1(e, t, z, 110001); e.noteSent(NODE, 1, 0, 4);   // fecha V1
    e.onCmdFailed(NODE, 1, 0); // fechar V1 falha
    HydraulicGroupEngine::GroupAlert al; bool got = false;
    while (e.takeAlert(al)) if (al.code == HydraulicGroupEngine::GA_CLOSE_FAIL) got = true;
    TEST_ASSERT_TRUE(got);
    // invariante seguro: grupo segue com bomba ligada (válvula a mais = pressão menor).
    TEST_ASSERT_TRUE(e.pumpOn(1));
}

static void test_bridging_sem_nova_partida()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.minRunMin = 10; g.maxStartsHour = 6;
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();
    e.setDesired(1, 1, true, 60);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2); // 1 partida
    // V1 deixa de ser desejada; gap curto -> bomba fica ligada (ponte).
    e.setDesired(1, 1, false, 0);
    tickNone(e, t, z, 120000); // 2 min depois, dentro de minRunMin(10 min): bomba NÃO desliga
    TEST_ASSERT_TRUE(e.pumpOn(1));
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));
    // chega V2 -> retoma sem nova partida.
    e.setDesired(1, 2, true, 60);
    GroupEmit o2 = tick1(e, t, z, 130000);
    TEST_ASSERT_EQUAL_UINT8(2, o2.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, o2.action);
}

static void test_max_partidas_defer()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.minRunMin = 0; g.maxStartsHour = 1; // só 1 partida/h
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();
    // 1ª partida ok
    e.setDesired(1, 1, true, 60);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    // encerra tudo
    e.setDesired(1, 1, false, 0);
    tick1(e, t, z, 70000); e.noteSent(NODE, 9, 0, 3); e.onAck(NODE, 3); // bomba off
    tick1(e, t, z, 80000); e.noteSent(NODE, 1, 0, 4); e.onAck(NODE, 4); // fecha V1
    TEST_ASSERT_EQUAL(State::IDLE, e.stateOf(1));
    // 2ª demanda: partida bloqueada (1/h) -> DEFER, válvula fica fechada.
    e.setDesired(1, 2, true, 60);
    tickNone(e, t, z, 90000); // não abre V2 (não abre válvula sem bomba)
    TEST_ASSERT_EQUAL(State::DEFERRED, e.stateOf(1));
    HydraulicGroupEngine::GroupAlert al; bool got = false;
    while (e.takeAlert(al)) if (al.code == HydraulicGroupEngine::GA_DEFER_RATE) got = true;
    TEST_ASSERT_TRUE(got);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_happy_path_min1);
    RUN_TEST(test_fechar_antes_de_abrir);
    RUN_TEST(test_min_open_2);
    RUN_TEST(test_renova_bomba_na_transicao);
    RUN_TEST(test_open_next_falha_renova_corrente);
    RUN_TEST(test_open_next_falha_sem_renovar_desliga_bomba);
    RUN_TEST(test_close_prev_falha_alerta);
    RUN_TEST(test_bridging_sem_nova_partida);
    RUN_TEST(test_max_partidas_defer);
    exit(UNITY_END());
}
void loop() {}
