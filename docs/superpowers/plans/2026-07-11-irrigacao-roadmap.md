# Irrigação LoRa Mesh — Roadmap de implementação

Spec: `myfork/especificacao-irrigacao-mesh.md` (v0.1). Spec cobre múltiplos subsistemas independentes; cada fase abaixo vira um plano próprio e entrega software funcional e testável sozinho.

| Fase | Escopo (seções da spec) | Plano |
|---|---|---|
| 1 | Núcleo do protocolo + estação fail-safe: mensagens binárias no portnum privado, anti-replay, rate limit, ValveController com timer local, ACK, heartbeat, role em settings (§3.1, §3.2, §4) | `2026-07-11-irrigacao-fase1-nucleo.md` |
| 2 | Persistência NVS atômica, epoch de config, `SET_CONFIG`/`GET_CONFIG` fragmentável, regra do maior epoch, modo seguro (§5) | concluída (2026-07-12, plano 2026-07-11-irrigacao-fase2-config.md; regra do maior epoch movida p/ Fase 4 — lado gateway) |
| 3 | Pareamento, vínculo nó↔gateway, allowlist, reset de fábrica, botão multifunção, LED de status (§6, §8.6, §8.7) | futuro |
| 4 | Gateway: registro de estações, zonas, programas/cronograma, retries de ACK, alertas (bateria, estação muda, reboots) (§5.2–§5.3, §8.1–§8.3) | futuro |
| 5 | Painel web do gateway + captive portal de campo (LittleFS, endpoints JSON) (§7) | futuro |
| 6 | Sensores, intertravamentos (global + réplica local), GPO, tamper, log de auditoria (§8.9–§8.12) | futuro |
| 7 | Grupos hidráulicos (máquina de estados bomba/válvulas, matriz de falhas) (§8.13) | futuro |
| 8 | Role SERVICO: cofre LittleFS, re-tune de canal, varredura, `RESYNC_SEQ`, import/export; modo híbrido 24VAC; survey/instalador (§8.4–§8.5, §11) | futuro |
| 9 | Particionamento de flash p/ OTA nas variants alvo (§3.3) | futuro |

Regras globais da spec que valem em todas as fases: teto absoluto de abertura 120 min compilado; fail-safe local sempre; payload ≤ ~200 bytes; versão de protocolo em toda mensagem; mismatch = rejeição segura.

Referencia de UI (Fase 5): mockup do painel/portal em `myfork/Irrigacao Mobile.dc.html` (design doc HTML, 2026-07-11) - base visual das telas do painel web e captive portal.

Requisito reforçado pelo usuário (2026-07-11) — modo híbrido/espelho (§5.2 `fonte`): o sistema deve poder operar SOMENTE espelhando as entradas físicas de um controlador externo (réplica das saídas 24VAC lidas por optoacoplador), sem cronograma próprio. Isso deve ser configurável por entrada, incluindo a polaridade do estado ativo (active-high / active-low). Entra na Fase 4 (gateway/zonas) e no pin map da Fase 2 (campo de polaridade por entrada física).
