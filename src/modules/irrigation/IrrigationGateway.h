#pragma once
// IrrigationGateway — Agregado das 6 tabelas/engines do lado gateway (Fase 4, Task 6).
// Decisão §1: este header é só um agregado; toda a lógica condicional vive em IrrigationModule.
// Os membros são públicos para que o módulo acesse diretamente sem getters desnecessários.

#include "modules/irrigation/CommandTracker.h"
#include "modules/irrigation/GatewayTables.h"
#include "modules/irrigation/MirrorMode.h"
#include "modules/irrigation/ProgramScheduler.h"
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
};
