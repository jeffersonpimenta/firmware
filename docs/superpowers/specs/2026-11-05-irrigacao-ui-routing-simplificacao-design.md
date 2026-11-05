# Simplificação de routing e estrutura da UI de irrigação

Data: 2026-11-05 · Branch: `sistema-irrigacao`

## Problema

O acesso à UI de irrigação é confuso em todas as plataformas (firmware ESP32
real e bancada de teste). Sintomas relatados:

1. "O redirecionamento não funciona muito bem."
2. "Parece haver mais de uma UI pro nó."
3. "O gateway não funciona muito bem."
4. Estrutura de pastas/caminhos não-intuitiva.

### Causa-raiz (investigada no código)

- **Entrada única por papel já existe** via `data/irrigacao/route.js`: consulta
  `GET /api/portal/node` e redireciona para wizard / painel gateway / portal do
  nó conforme `provisioned` e `role`. O papel é gravado server-side pelo
  provisionamento (`POST /api/portal/provision {role}` → `portalProvision()`),
  lido de volta por `/api/portal/node`.
- **Bug principal — `route.js:23`:** `location.replace(target)` usa um path
  pelado e **descarta a query string**. Flags `?mock=1`, `?wizard=1`, `?role=N`
  morrem no redirect. Qualquer preview/mock que caia num redirect de papel
  quebra (carrega a SPA de destino sem mock → fetch real falha → tela vazia).
- **Entrada `/` divergente por plataforma:** o firmware (portal cativo)
  redireciona `/` para `/irrigacao/portal/index.html` e depois `route.js` pode
  saltar de novo para o painel (duplo-salto); a bancada aponta `/` para o painel
  gateway. Dois comportamentos para a mesma intenção.
- **Estrutura assimétrica:** o gateway vive na raiz (`/irrigacao/index.html`) e
  o nó numa subpasta (`/irrigacao/portal/index.html`); assets partilhados e
  ficheiros de dev (mock/test) misturados na raiz. Nomes misturam pt/en
  (`irrigacao` vs `irrigation`) e `portal` significa "nó", o que não é óbvio.

## Objetivo

Entrada única por papel, com **comportamento idêntico** em todas as plataformas,
e uma árvore de ficheiros/caminhos intuitiva. A lógica de routing deve viver num
só sítio. Sem regressão de funcionalidade das SPAs existentes.

Não-objetivos: fundir as duas SPAs numa só; reescrever o front-end; alterar o
protocolo mesh ou o armazenamento de config/papel.

## Arquitetura

### Modelo de entrada — bootstrap único

- `/irrigacao/index.html` passa a ser uma **página mínima de bootstrap**: só
  carrega `route.js` (e, em dev, o `mock.js`). Nenhuma SPA pesada carrega antes
  de o papel ser conhecido.
- `route.js` (em `shared/`) faz `GET /api/portal/node`, decide e redireciona
  **uma única vez** para a SPA correta:
  - `provisioned === false` → `/irrigacao/node/index.html` (wizard de 1º boot)
  - `role === GATEWAY (1)` → `/irrigacao/gateway/index.html`
  - caso contrário (estação/repetidor/serviço) → `/irrigacao/node/index.html`
- **A lógica de routing vive só no bootstrap.** As SPAs `gateway/` e `node/`
  deixam de incluir `route.js`. Isso elimina o duplo-salto e o cross-redirect
  entre páginas (cada SPA assume que o bootstrap já a escolheu).
- Em `catch` (offline/preview sem API), o bootstrap não redireciona — mantém
  uma mensagem/tela neutra; o dev usa flags explícitas (`?role=`) para forçar.

### Estrutura de pastas (agrupada por papel)

```
data/irrigacao/
  index.html              bootstrap (entrada "/")
  gateway/
    index.html  app.js
  node/                   (era portal/ — nó + serviço)
    index.html  app.js
  shared/
    style.css  api.js  route.js
  dev/                    (mock + testes; excluído do device)
    mock.js  test_mock.js  test_niveis.js  test_radio.js
```

Caminhos web simétricos e auto-descritivos:

- `/irrigacao/` → bootstrap (decide papel)
- `/irrigacao/gateway/` → console do gateway
- `/irrigacao/node/` → portal do nó/serviço
- `/irrigacao/shared/*` → assets partilhados

### Correções de bug e portabilidade

- `route.js`: **preservar `location.search`** ao redirecionar (concatenar a
  query atual ao alvo) — mock/wizard/role sobrevivem. Alvos sempre terminam em
  `index.html` explícito (evita depender de index-de-diretório, que o
  `ESP8266WebServer` não resolve e que no ESP32 exige tratamento extra).
- Todas as referências a assets passam a **absolutas** a partir da raiz web
  (`/irrigacao/shared/style.css`, `/irrigacao/gateway/app.js`, etc.). O gateway
  sai da raiz, então caminhos relativos quebrariam.
- Snippet de injeção de mock (em cada `index.html`) aponta para
  `/irrigacao/dev/mock.js` e continua a disparar só quando `?mock` está na URL.

### Firmware — `src/mesh/http/ContentHandler.cpp`

- Redirect do portal cativo: alvo passa de `/irrigacao/portal/index.html` para
  **`/irrigacao/index.html`** (o bootstrap decide o papel).
- Tratar `/` final de caminho de diretório como `index.html` (para que
  `/irrigacao/` e `/irrigacao/gateway/` sirvam). O serving multi-nível (wildcard
  reconstruído a partir da request crua) já existe e cobre `/irrigacao/node/app.js`.

### Build / dev — `bin/platformio-custom.py`

- O staging do LittleFS (`_stage_fs`) passa a **excluir a pasta `dev/` inteira**
  em vez da lista fixa `_DEV_ONLY` de 4 nomes. O device nunca leva mock/testes.
- Invariante documentada: `?mock=1` é só-dev. O snippet de injeção só dispara
  com `?mock` na URL; no device a pasta `dev/` não existe, logo é inerte.

## Fase 2 (opcional) — renomear APIs para casar os papéis

Executada **como commit separado, depois** da Fase 1 estar gravada e verificada
(raio isolado, bisectável, diff barulhento não polui a mudança estrutural).

Renomeação de prefixo (mecânica; sem contrato externo — UI e firmware versionam
juntos no mesmo flash):

```
/api/irrigation       → /api/gateway          (= pasta gateway/)
/api/portal           → /api/node             (= pasta node/)
/api/portal/service   → /api/node/service
```

Raio de impacto medido (~188 ocorrências, quase todas prefixo de string):

- `/api/irrigation` (92): `IrrigationWebEndpoints.cpp` (89 `registerNode`),
  `app.js` (3), headers (2).
- `/api/portal` (96): `IrrigationPortalEndpoints.cpp` (30),
  `ServicePortalEndpoints.cpp` (13), `portal/app.js`→`node/app.js` (29),
  `gateway app.js` (16), `ContentHandler.cpp` (4), `mock.js` (3), outros (5).

Riscos e mitigação: "esquecer 1 ref" → 404 em runtime → grep-verificar **zero**
refs antigas após a troca + suite nativa + smoke manual. Atualizar os regexes de
intercept em `mock.js` (3) e os testes que referenciam paths.

## Ferramenta de dev (NÃO versionada)

A bancada ESP8266 (`tools/ui-rig-esp8266/`) serve `data/irrigacao/**` estático
via AP próprio, para medir velocidade/uso geral da UI em hardware real. É
conveniência local, **fora dos commits**: adicionar `tools/ui-rig-esp8266/` ao
`.gitignore`. Após a Fase 1, alinhar localmente o `main.cpp` do rig para `/` →
`/irrigacao/index.html?mock=1` (o `route.js` corrigido preserva `?mock`). Esta
alteração do rig não é entregável.

## Tratamento de erros

- Bootstrap sem API (preview/offline): não redireciona; tela neutra. Flags
  `?role=`/`?wizard=` forçam estados em dev via mock.
- Asset ausente no device: comportamento atual do `ContentHandler` (fallback).
- Papel desconhecido/futuro: `route.js` cai no ramo "não-gateway" → portal do nó
  (degradação segura).

## Testes

- **Unit JS (`route.js`)**: extrair a decisão de alvo para função pura
  `decideTarget({provisioned, role, pathname, search})` e testar: cada papel,
  `provisioned=false`, preservação de query, não-redirect quando já no destino.
  Segue o padrão `test_*.js` existente (em `dev/`).
- **Verificação manual no rig**: `/` → UI certa; mock vivo; wizard
  (`?mock=1&wizard=1`); troca de papel (`?mock=1&role=1`).
- **Firmware**: build dos ambientes de irrigação + smoke manual do redirect
  cativo (`/` e sonda de captive-portal → bootstrap → SPA certa).
- **Fase 2**: re-rodar `test_irrigation_webapi`, `test_portal_api`,
  `test_service_portal`; grep-verificar zero refs de prefixo antigo.

## Plano de fases

1. **Fase 1 (núcleo):** nova árvore de pastas + bootstrap + fix de query em
   `route.js` + refs absolutas + `ContentHandler` (alvo cativo + trailing-slash)
   + staging `dev/` + unit de `route.js`. `.gitignore` do rig.
2. **Fase 2 (opcional):** renomear APIs (`/api/gateway`, `/api/node`).
