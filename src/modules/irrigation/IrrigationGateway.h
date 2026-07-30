#pragma once
// IrrigationGateway — Agregado das 6 tabelas/engines do lado gateway (Fase 4, Task 6).
// Decisão §1: este header é só um agregado; toda a lógica condicional vive em IrrigationModule.
// Os membros são públicos para que o módulo acesse diretamente sem getters desnecessários.

#include "modules/irrigation/CommandTracker.h"
#include "modules/irrigation/GatewayTables.h"
#include "modules/irrigation/HydraulicGroupEngine.h"
#include "modules/irrigation/HydraulicGroupTable.h"
#include "modules/irrigation/LevelControlEngine.h"
#include "modules/irrigation/LevelControlTable.h"
#include "modules/irrigation/InterlockEngine.h"
#include "modules/irrigation/InterlockTable.h"
#include "modules/irrigation/MirrorMode.h"
#include "modules/irrigation/OpenGate.h"
#include "modules/irrigation/ProgramScheduler.h"
#include "modules/irrigation/SensorNameTable.h"
#include "modules/irrigation/StationMonitor.h"
#include "modules/irrigation/StationTelemetryCache.h"

struct IrrigationGateway {
    ZoneTable zones;
    StationRegistry stations;
    ProgramScheduler scheduler;
    CommandTracker tracker;
    StationMonitor monitor;
    AlertCenter alerts;
    MirrorMode mirror;
    StationTelemetryCache telemetry;
    // Fase 6b: motor de intertravamentos e controle de abertura simultânea.
    InterlockTable interlocks;
    InterlockEngine interlockEngine;
    OpenGate openGate;
    // Fase 6b Task 18: nomes de sensores configurados pelo operador via painel.
    SensorNameTable sensorNames;
    // Fase 7a: grupos hidráulicos (bomba/válvula) — orquestração + config.
    HydraulicGroupTable groups;
    HydraulicGroupEngine groupEngine;
    // Controle de nível por boia (enchimento automático).
    LevelControlTable levels;
    LevelControlEngine levelEngine;
};
