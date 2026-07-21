# Irrigação LoRa Mesh — Roadmap de implementação

Spec: `myfork/especificacao-irrigacao-mesh.md` (v0.1). Spec cobre múltiplos subsistemas independentes; cada fase abaixo vira um plano próprio e entrega software funcional e testável sozinho.

| Fase | Escopo (seções da spec) | Plano |
|---|---|---|
| 1 | Núcleo do protocolo + estação fail-safe: mensagens binárias no portnum privado, anti-replay, rate limit, ValveController com timer local, ACK, heartbeat, role em settings (§3.1, §3.2, §4) | `2026-07-11-irrigacao-fase1-nucleo.md` |
| 2 | Persistência NVS atômica, epoch de config, `SET_CONFIG`/`GET_CONFIG` fragmentável, regra do maior epoch, modo seguro (§5) | concluída (2026-07-12, plano 2026-07-11-irrigacao-fase2-config.md; regra do maior epoch movida p/ Fase 4 — lado gateway) |
| 3 | Pareamento, vínculo nó↔gateway, allowlist, reset de fábrica, botão multifunção, LED de status (§6, §8.6, §8.7) | concluída (2026-07-12, plano 2026-07-12-irrigacao-fase3-pareamento.md; aprovação física no gateway antecipa o painel da Fase 5; PSK de canal não é apagada no reset de fábrica — limitação até a Fase 5) |
| 4 | Gateway: registro de estações, zonas, programas/cronograma, retries de ACK, alertas (bateria, estação muda, reboots) (§5.2–§5.3, §8.1–§8.3) | concluída (2026-07-13, plano 2026-07-12-irrigacao-fase4-gateway.md; edição de zonas/programas/espelho via UI chega na Fase 5; intertravamentos na Fase 6; máquina guiada por ACK na Fase 7) |
| 5 | Painel web do gateway + captive portal de campo (LittleFS, endpoints JSON) (§7) | concluída (5a painel gateway 2026-07-15, plano 2026-07-15-irrigacao-fase5a-painel-gateway.md; 5b portal de campo 2026-07-17, plano 2026-07-17-irrigacao-fase5b-portal-no.md; aba Instalador §8.4 cortada — firmware separado) |
| 6 | Sensores, intertravamentos (global + réplica local), GPO, tamper, log de auditoria (§8.9–§8.12) | dividida em 6a/6b. **6a (estação)** concluída (2026-07-21, plano 2026-07-20-irrigacao-fase6a-estacao.md): settings ABI v4 (sensores/GPO/tamper/coords), SensorSampler, GpoController, AuditLog + mini-log persistente, heartbeat com bloco de sensores + envio antecipado, EV_TAMPER, endpoints/UI do portal. **6b (gateway)** futuro: motor de intertravamentos global + réplica local, log de auditoria dimensionado p/ meses (CSV/JSON, backup), janela de manutenção via painel, nomes de sensor, UI do painel, enfileiramento por simultaneidade |
| 7 | Grupos hidráulicos (máquina de estados bomba/válvulas, matriz de falhas) (§8.13) | futuro |
| 8 | Role SERVICO: cofre LittleFS, re-tune de canal, varredura, `RESYNC_SEQ`, import/export; modo híbrido 24VAC; survey/instalador (§8.4–§8.5, §11) | futuro |
| 9 | Particionamento de flash p/ OTA nas variants alvo (§3.3) | futuro |

Regras globais da spec que valem em todas as fases: teto absoluto de abertura 120 min compilado; fail-safe local sempre; payload ≤ ~200 bytes; versão de protocolo em toda mensagem; mismatch = rejeição segura.

Referencia de UI (Fase 5): mockup do painel/portal em `myfork/Irrigacao Mobile.dc.html` (design doc HTML, 2026-07-11) - base visual das telas do painel web e captive portal.

Requisito reforçado pelo usuário (2026-07-11) — modo híbrido/espelho (§5.2 `fonte`): o sistema deve poder operar SOMENTE espelhando as entradas físicas de um controlador externo (réplica das saídas 24VAC lidas por optoacoplador), sem cronograma próprio. Isso deve ser configurável por entrada, incluindo a polaridade do estado ativo (active-high / active-low). Entra na Fase 4 (gateway/zonas) e no pin map da Fase 2 (campo de polaridade por entrada física).

Requisito reforçado pelo usuário (2026-07-12) — modo espelhamento (§5.2 `fonte`, Fase 4): quando ativo, TODA configuração e TODOS os intertravamentos são bypassados — a estação escrava replica cegamente o estado da entrada do mestre. O bypass é configurável pela UI; a UI também permite associar cada entrada do mestre (gateway híbrido, opto 24VAC) a qualquer saída de qualquer escravo (mapeamento entrada→(nó,saída) via dropdown). Polaridade ativo-alto/baixo por entrada (requisito 2026-07-11) continua valendo. Nota de engenharia: o teto fail-safe de 120 min compilado (§4.2) permanece mesmo em espelhamento, salvo decisão explícita do usuário em contrário — a renovação periódica do comando cobre acionamentos longos.

Requisito do usuário (2026-07-12) — exportar chave da fazenda: função de visualizar a PSK do canal primário (base64 + nome do canal) para cadastro no cofre do device de serviço (§11.2). Fase 3: pressão longa no botão do gateway despeja no console serial. Fase 5: mesma função exposta no painel web (com PIN).
