#include "StationMonitor.h"

// ============================================================================
// AlertCenter implementation
// ============================================================================

void AlertCenter::push(const Alert &a)
{
    ring[writeIdx] = a;
    writeIdx = (writeIdx + 1) % MAX;
    if (numAlerts < MAX) {
        numAlerts++;
    }
}

size_t AlertCenter::count() const
{
    return numAlerts;
}

const Alert &AlertCenter::at(size_t i) const
{
    // Index 0 = most recent
    // Most recent is at (writeIdx - 1) mod MAX
    size_t actualIdx = (writeIdx + MAX - 1 - i) % MAX;
    return ring[actualIdx];
}

// ============================================================================
// StationMonitor implementation
// ============================================================================

StationMonitor::StationState *StationMonitor::findOrCreate(uint32_t node, uint32_t nowMs)
{
    // Try to find existing
    for (size_t i = 0; i < MAX; i++) {
        if (stations[i].node == node) {
            return &stations[i];
        }
    }

    // Try to create in first empty slot
    for (size_t i = 0; i < MAX; i++) {
        if (stations[i].node == 0) {
            stations[i].node = node;
            stations[i].lastHeardMs = nowMs;
            stations[i].level = 0; // normal
            stations[i].silentFired = false;
            stations[i].everHeard = false;
            return &stations[i];
        }
    }

    // All slots full, cannot add
    return nullptr;
}

StationMonitor::StationState *StationMonitor::find(uint32_t node) const
{
    for (size_t i = 0; i < MAX; i++) {
        if (stations[i].node == node) {
            return const_cast<StationState *>(&stations[i]);
        }
    }
    return nullptr;
}

uint8_t StationMonitor::calculateLevel(uint16_t vbatCentiV) const
{
    // Calculate level based on simple thresholds
    if (vbatCentiV < HIBER_CV) {
        return 3; // hibernacao
    } else if (vbatCentiV < CRITICO_CV) {
        return 2; // critico
    } else if (vbatCentiV < AVISO_CV) {
        return 1; // aviso
    }
    return 0; // normal
}

int StationMonitor::onHeartbeat(uint32_t node, uint16_t vbatCentiV, uint16_t rebootCount, uint32_t nowMs, Alert out[2])
{
    StationState *state = findOrCreate(node, nowMs);
    if (!state) {
        return 0; // Cannot create more stations
    }

    int alertCount = 0;
    state->lastHeardMs = nowMs;

    if (!state->everHeard) {
        state->everHeard = true;
        state->silentFired = false;
        // Initialize level from current reading without firing an alert
        state->level = calculateLevel(vbatCentiV);
    } else {
        // Clear silent fired on heartbeat (back online)
        if (state->silentFired) {
            state->silentFired = false;
            out[alertCount].type = AlertType::BACK_ONLINE;
            out[alertCount].node = node;
            out[alertCount].arg = 0;
            out[alertCount].atMs = nowMs;
            alertCount++;
        }
    }

    // ========== Battery monitoring ==========
    // Hysteresis logic:
    // Down: immediate transition when crossing level threshold going down
    // Up: only transition when reaching level_threshold + HYST_CV

    // Check if level should go DOWN (immediate) using simple level calculation
    uint8_t simpleLevel = calculateLevel(vbatCentiV);

    if (simpleLevel > state->level) {
        // Battery voltage went below threshold - transition DOWN immediately (level number increases)
        state->level = simpleLevel;
        AlertType levelAlert;
        switch (simpleLevel) {
            case 1:
                levelAlert = AlertType::BATT_AVISO;
                break;
            case 2:
                levelAlert = AlertType::BATT_CRITICO;
                break;
            case 3:
                levelAlert = AlertType::BATT_HIBERNACAO;
                break;
            default:
                levelAlert = AlertType::NONE;
        }
        if (levelAlert != AlertType::NONE) {
            out[alertCount].type = levelAlert;
            out[alertCount].node = node;
            out[alertCount].arg = vbatCentiV;
            out[alertCount].atMs = nowMs;
            alertCount++;
        }
    } else {
        // Check for upward recovery with hysteresis
        // Upward recovery requires crossing current_level_threshold + HYST_CV

        if (state->level == 1) {
            // Currently aviso, go normal if >= AVISO+HYST
            if (vbatCentiV >= AVISO_CV + HYST_CV) {
                state->level = 0;
                out[alertCount].type = AlertType::BATT_RECUPEROU;
                out[alertCount].node = node;
                out[alertCount].arg = vbatCentiV;
                out[alertCount].atMs = nowMs;
                alertCount++;
            }
        } else if (state->level == 2) {
            // Currently critico, check recovery paths
            if (vbatCentiV >= AVISO_CV + HYST_CV) {
                // Full recovery to normal
                state->level = 0;
                out[alertCount].type = AlertType::BATT_RECUPEROU;
                out[alertCount].node = node;
                out[alertCount].arg = vbatCentiV;
                out[alertCount].atMs = nowMs;
                alertCount++;
            } else if (vbatCentiV >= CRITICO_CV + HYST_CV) {
                // Recover to aviso (no alert on intermediate recovery)
                state->level = 1;
            }
        } else if (state->level == 3) {
            // Currently hibernacao, check recovery paths
            if (vbatCentiV >= AVISO_CV + HYST_CV) {
                // Full recovery to normal
                state->level = 0;
                out[alertCount].type = AlertType::BATT_RECUPEROU;
                out[alertCount].node = node;
                out[alertCount].arg = vbatCentiV;
                out[alertCount].atMs = nowMs;
                alertCount++;
            } else if (vbatCentiV >= CRITICO_CV + HYST_CV) {
                // Recover to critico (no alert on intermediate recovery)
                state->level = 2;
            }
            // Note: if vbatCentiV >= HIBER_CV + HYST_CV but below CRITICO_CV + HYST_CV, we stay in level 3
        }
    }

    // ========== Reboot monitoring ==========
    uint32_t windowAgeMs = nowMs - state->rebootWindowStartMs;
    if (windowAgeMs > 24 * 60 * 60 * 1000u || state->rebootBase == 0) {
        // Start new window
        state->rebootWindowStartMs = nowMs;
        state->rebootBase = rebootCount;
        state->anomalyFired = false;
    } else {
        // Check if anomaly
        uint16_t rebootDelta = rebootCount - state->rebootBase;
        if (rebootDelta > REBOOT_LIMIT_24H && !state->anomalyFired) {
            state->anomalyFired = true;
            out[alertCount].type = AlertType::REBOOT_ANOMALY;
            out[alertCount].node = node;
            out[alertCount].arg = rebootCount;
            out[alertCount].atMs = nowMs;
            alertCount++;
        }
    }

    return alertCount;
}

bool StationMonitor::checkSilence(uint32_t node, uint32_t silencioMs, uint32_t nowMs, Alert &out)
{
    StationState *state = find(node);
    if (!state || !state->everHeard) {
        return false; // Never heard or not found
    }

    uint32_t elapsed = nowMs - state->lastHeardMs;
    if (elapsed > silencioMs && !state->silentFired) {
        state->silentFired = true;
        out.type = AlertType::SILENT;
        out.node = node;
        out.arg = 0;
        out.atMs = nowMs;
        return true;
    }

    return false;
}

uint32_t StationMonitor::lastHeardMs(uint32_t node) const
{
    const StationState *state = find(node);
    if (!state) {
        return 0;
    }
    return state->lastHeardMs;
}
