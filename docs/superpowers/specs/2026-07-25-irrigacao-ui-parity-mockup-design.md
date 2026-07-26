# Irrigação — Paridade de UI com o mockup + wizard de 1º boot (design)

**Data:** 2026-07-25 · **Branch:** `sistema-irrigacao` · **Escopo:** frontend (painel + portal) + endpoints de suporte + wizard de provisionamento.

Mockup-alvo: `myfork/Irrigacao Mobile.dc.html` (painel do gateway, iOS frame 390×780, paleta oklch, cards arredondados, nav de 5 itens + "Mais"). A UI real deve reproduzir a aparência **de dentro do frame** (sem a moldura de iPhone falsa, que é só presentation do design-doc).

Decisões do usuário (brainstorming 2026-07-25): escopo = **painel + portal**; navegação = **5 itens + Mais**; wizard de papel = **só 1º boot** (some após gravar; trocar papel = reset de fábrica); fidelidade = **total** (endpoints novos); wizard = **end-to-end** (UI + firmware).

---

## A. Navegação e telas

### Painel (role GATEWAY)
Barra inferior 11 → **5 abas**: `Visão Geral · Estações · Zonas · Programas · Mais`.

- **Mais** = tela-menu; cards tap→sub-rota, agrupando as funções raras/secundárias: Grupos, Intertravamentos, Sensores, GPO, Tamper, Log, Cobertura, **Sistema**.
- Sub-rotas **reusam os `render*()` atuais** + back-link ("‹ Mais"); a aba Mais permanece ativa. `app.js`: `RENDER` ganha `mais` e `sistema`; `show()` guarda a sub-rota para o back.
- **Sistema** (nova): PIN de aplicação, Backup (⬇ hoje no header → migra para cá), exportar chave da fazenda (base64 + nome do canal, com PIN, §11.2).

### Portal (todos os papéis)
Restyle no mesmo idioma visual (tokens/cards/nav do mockup). Abas atuais mantidas (Este nó / Rede / Cobertura + svc-only reveladas por papel). Ganha o **wizard** (§D).

## B. Visão Geral — fidelidade total

Cards do mockup, consumindo os endpoints de §C:
- Linha de stats: Estações · Em execução · Alertas.
- **Em execução agora**: etapa atual, "restam N min", grupo/programa, barra de progresso (`elapsedPct`). Só com RTC/execução ativa.
- **Pareamento pendente** (dado já em `/overview`).
- **Alertas não reconhecidos**: lista + botão "Reconhecer" (por item e/ou todos).
- **Próximas execuções**: 1–3 próximas do scheduler (só com RTC).

Sem RTC → blocos de agenda/progresso vazios; mantém a nota "sem relógio — cronograma inativo" já existente.

## C. Endpoints novos (painel) — puros native-tested + cola CI-only

Padrão idêntico ao vigente: lógica pura em `IrrigationWebApi` (suite `test_irrigation_webapi`); cola HTTP em `IrrigationWebEndpoints.cpp` (excluída do build nativo em `variants/native/portduino.ini`).

| Rota | Fonte no firmware | Builder/parser puro |
|---|---|---|
| `GET /api/irrigation/alerts` | `AlertCenter`/`StationMonitor` (iterar alertas ativos) | `buildAlerts(...)` |
| `POST /api/irrigation/alerts/ack` | reusa `lastAckAllMs` (ACK_ALERT) + ack por id se viável | `parseAlertAck(...)` |
| `GET /api/irrigation/schedule/upcoming?n=3` | `ProgramScheduler` + clock local | `buildUpcoming(programs, zones, nowLocal, n)` |
| `GET /api/irrigation/overview` **estendido** | scheduler (etapa/elapsed) + `HydraulicGroupEngine::status` | `buildRunning(...)` (campos `running*`, `elapsedPct`, `grupoNome`, `programNome`) |

`buildUpcoming` recebe o relógio como parâmetro (puro/determinístico → testável no host). Acessores novos no `IrrigationModule`/engines conforme necessário (ex.: `ProgramScheduler` expor próximas ocorrências; `HydraulicGroupEngine` expor etapa/elapsed).

## D. Wizard de 1º boot (escolha de papel) — end-to-end

**Objetivo:** funcionalidade "de uma vez só" (escolher Gateway/Estação/Serviço/Repetidor) aparece **apenas** enquanto o nó não foi provisionado.

- **Detecção de "provisionado" = flag de runtime**, não bit persistido: `provisioned = (settings carregadas do store no boot)`. Um nó de fábrica não tem settings salvas → `loadIrrigationSettings` falha → defaults + safe mode → `provisioned=false` → wizard. Após o wizard gravar settings → próximo boot carrega → `provisioned=true` → wizard some. **Não toca ABI** (settings v5 176B intactos) e **evita o foot-gun** de nós de campo re-exibirem o wizard após atualização de firmware (um bit em `hwFlags` leria 0 em blobs antigos).
- **Endpoint:** `POST /api/portal/provision { role, farmName? }` → grava `settings.role` (+ nome/`farmName` se aplicável); se `role==GATEWAY`, dispara geração da PSK da fazenda (reusa o caminho de 1º-boot do gateway); commit atômico; reboot em ~3 s (espelha `applyRetune`/`commitPairing`). `parseProvision` puro native-tested; validação de `role ∈ {0,1,2,3}`.
- **Superfície = portal** (não o painel): um nó de fábrica ainda não é gateway, logo não serve o painel; o portal é a UI universal de campo (auto-sobe nos primeiros 10 min de um nó de fábrica, §7.2). O node-state do portal passa a reportar `provisioned`; `app.js` do portal mostra a tela-cheia do wizard quando `provisioned==false` e esconde as abas normais.
- **UI:** tela cheia — título "Configurar dispositivo", seleção de papel (segmented/radio), campo "Nome da fazenda" só quando Gateway, botão "Continuar" → `POST /provision` → aviso de reboot. Após provisionar, nunca reaparece. Trocar papel = reset de fábrica (limpa settings → `provisioned=false`).

## E. Portal — restyle

Envolver os forms atuais em `.card` com headers; aplicar paleta/nav do mockup; manter a revelação das svc-tabs por papel e toda a lógica/endpoints existentes. Sem mudança de comportamento.

## F. Testes e verificação

- **Puros (host, Docker):** `buildAlerts`, `parseAlertAck`, `buildUpcoming`, `buildRunning`, `parseProvision` → casos em `test_irrigation_webapi` (ou suite nova `test_provision` se ficar grande). Suíte nativa completa deve seguir GREEN.
- **CI/banca-only:** endpoints, `ContentHandler`, frontend (painel + portal), wizard end-to-end — não compilam no nativo. **Banca 2+ nós** exigida antes de campo: reboot/geração-de-PSK/troca-de-papel do wizard, alertas e agenda por rádio.
- Aditivo: protocolo VERSION=1, ABI settings v5 (176B) intactos. Sem tipo de wire novo.

## G. Decomposição (ordem de implementação)

1. **Firmware — provisionamento:** flag runtime `provisioned` + `POST /api/portal/provision` + `parseProvision` + `provisioned` no node-state do portal.
2. **Firmware — overview:** acessores de engines + `buildAlerts`/`parseAlertAck`/`buildUpcoming`/`buildRunning` + rotas CI-only.
3. **Frontend painel:** nav 5+Mais, tela Mais, tela Sistema, Visão Geral redesenhada consumindo §C.
4. **Frontend portal:** restyle + tela do wizard.

**Fora de escopo:** moldura iOS falsa, animações, VIA_GATEWAY/aprovar-pareamento (follow-ons herdados da Fase 8).

---

## Auto-revisão (self-review)

- Placeholders: nenhum TBD pendente; acessores exatos de engine ficam para o plano/implementação (ProgramScheduler/HydraulicGroupEngine podem precisar de getters — tratado na §C/G).
- Consistência: `provisioned` runtime (não-ABI) reconcilia §D com "não bumpar v5"; supera a proposta original do bit em `hwFlags`.
- Escopo: 4 partes coesas, ordenáveis; passível de execução incremental. Sem decomposição em specs separados.
- Ambiguidade: fidelidade "total" definida por endpoints concretos (§C); wizard só no portal (§D) — explícito.
