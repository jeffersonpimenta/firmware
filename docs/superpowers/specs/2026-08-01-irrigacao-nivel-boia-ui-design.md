# Design — Melhoria da UI de controle de nível (boia)

- **Data:** 2026-08-01
- **Branch:** sistema-irrigacao
- **Arquivos:** `data/irrigacao/app.js` (`renderNiveis`), `data/irrigacao/style.css`, `data/irrigacao/mock.js`, `data/irrigacao/test_mock.js`
- **Backend:** sem mudança. Endpoints `/levels`, `/levels/delete`, `/stations`, `/zones`, `/sensors` já existem.

## Problema

O painel "Controle de nível" (`renderNiveis`, `app.js:1434`) é pouco intuitivo e visualmente destoa do resto do app:

1. **Visual:** usa tabela genérica (`.log-table`) + form plano (`.form`), ignorando o vocabulário de componentes do app (cards, chips, `.fld/.finput`).
2. **Semântica:** controles confusos —
   - Dropdown "Liga quando boia" oferece `ativo (nível baixo)` × `inativo (nível baixo)`; **ambos** dizem "nível baixo".
   - Nó da boia digitado como hex cru (`0x1a2b3c4d`).
   - Índice de sensor e zona/bomba como números crus, sem nome.

## Domínio

Controle de nível por boia = **enchimento automático de reservatório**. Uma boia (float switch, sensor digital) no tanque. Reservatório baixo → bomba liga pra encher; cheio → bomba desliga.

Campos da regra (`LevelRule`, imutável neste trabalho):

| Campo | Significado |
|-------|-------------|
| `sensorNode` + `sensorIdx` | qual boia: nó da estação + índice de sensor (0..3) |
| `ligaQuandoAtivo` | polaridade: bomba liga quando a boia lê ATIVA (`true`) ou INATIVA (`false`) |
| `targetZoneId` | qual zona/bomba acionar |
| `minOnS` / `minOffS` | anti-curto-ciclo (tempo mínimo ligada / desligada) |
| `staleTimeoutS` | failsafe: se a boia fica sem sinal por esse tempo → desliga bomba + alerta |
| `mensagem` | texto de alerta customizado (≤23 chars) |

O estado escolhido em `ligaQuandoAtivo` sempre corresponde fisicamente a "reservatório baixo" (é quando a bomba precisa encher). Isso desfaz a ambiguidade do dropdown.

## Escopo

- **Dentro:** reescrever `renderNiveis()` (lista em cards + form em frase), CSS mínimo, mock consistente.
- **Fora (YAGNI):** status ao vivo (boia ativa agora? bomba ligada?), qualquer mudança de endpoint/backend, campo manual de nó (dropdowns cobrem o caso; fallback hex só para exibição de nós não cadastrados).

## Solução

### 1. Carregamento de dados

`renderNiveis()` carrega em paralelo:

```js
const [rules, stations, zones, sensors] = await Promise.all([
  getJson('/levels'), getJson('/stations').catch(() => []),
  getJson('/zones'),  getJson('/sensors').catch(() => []),
]);
```

Resolução de nomes:
- Estação (boia): `stationName(stations, node)` (já existe, `app.js:204`).
- Zona/bomba: nome via `/zones` (`{id, name, node, index, tipo}`).
- Sensor: novo helper `sensorName(sensors, node, idx)` → nome do sensor em `/sensors[node].sensores[idx].nome`, fallback `s{idx}`.

### 2. Lista — card por regra

Reusa `.card` + `.chip`. Cada regra:

```
{nome estação} · boia "{nome sensor | s{idx}}"
liga bomba  ▸  {nome zona}
quando boia ATIVA (= reservatório baixo)        // ou INATIVA
[chip: min {on}/{off}]  [chip: sem sinal {timeout} ⚠]
                                   [Editar] [Excluir]
```

- Tempos formatados legível (ex.: `5m`/`90s`) via helper existente se houver, senão `{n}s`.
- Vazio: `<div class="empty">Nenhuma regra de nível.</div>`.
- `Excluir` mantém `confirm()` + `postJson('/levels/delete')` atuais.

### 3. Form em frase (nova / editar)

Container `.card.form` com a frase preenchível (`.lvl-sentence`):

> A bomba **[Zona ▾]** liga quando a boia **[Estação ▾]**·**[Sensor ▾]** estiver **[ativa ▾]** → reservatório baixo.

- **Zona** ← `/zones`: `<option value=id>name</option>`.
- **Estação** ← `/stations`: `<option value=node>name</option>` (fallback hex se sem nome).
- **Sensor** ← sensores **digitais** (`tipo===0`) do nó selecionado, de `/sensors`; **recarrega** ao trocar Estação. `<option value=idx>nome|s{idx}</option>`.
- **Polaridade** ← `ativa`(1) / `inativa`(0).

Bloco recolhível `▸ Ajustes avançados` (`.lvl-adv`, escondido por padrão):
- Min ligada (s) — default 30
- Min desligada (s) — default 30
- Falha se sem sinal (s) — default 90
- Mensagem de alerta (≤23) — opcional

Estado/ID:
- ID oculto (`id=0` aloca no backend).
- `Editar` num card preenche todos os campos do form (inclui ID e abre "Ajustes avançados" se valores diferem do default) e rola até o form.
- `Salvar` monta o mesmo body atual e chama `postJson('/levels')`; sucesso → `renderNiveis()`; erro → `alert()` com `body.errors` (igual hoje).

Validação client-side leve: Estação e Sensor obrigatórios (senão o Salvar avisa). `sensorNode` sai do value da Estação (número), sem parse de hex manual.

### 4. CSS

Adições mínimas em `style.css`:
- `.lvl-sentence` — layout do form em frase (selects inline, quebra natural, alinhamento vertical dos `<select>` com o texto).
- `.lvl-adv` — bloco avançado recolhível (toggle via classe `.hidden` já existente) + estilo do disclosure `▸/▾`.

Reusa `.card`, `.chip`, `.sec-title`, `.fld`, `.finput`, `.frow`, `.btn`, `.empty`, `.hidden`.

### 5. Mock (`mock.js`)

Requisito do usuário: o commit/push deve incluir os dados de mock da nova função.

- `STATE.levels`: 2–3 regras cujos `sensorNode`/`sensorIdx` existem em `STATE.sensors` (e o nó em `STATE.stations`) e cujo `targetZoneId` existe em `STATE.zones` — para que cards e dropdowns resolvam nomes reais.
- Garantir consistência entre os 4 endpoints (`/levels`, `/stations`, `/zones`, `/sensors`): pelo menos uma estação com sensor **digital** nomeado servindo de boia.
- Cobrir ambas polaridades (uma regra `ligaQuandoAtivo:true`, outra `false`) e um `staleTimeoutS` distinto, pra exercitar os chips.
- Atualizar `test_mock.js` se ele asserta forma de `/levels`.

## Testes / verificação

- Suíte nativa C++ (`./bin/run-tests.sh`) não cobre a UI JS — deve continuar GREEN (sem toque em `src/`).
- Verificação manual via `mock.js` (abrir `data/irrigacao/index.html` com include de mock): renderizar cards, criar/editar/excluir regra, trocar estação e ver sensores recarregarem, expandir "Ajustes avançados".
- `test_mock.js` roda o mock isolado; estender se necessário para a forma de `/levels`.

## Commit

Um commit com `app.js` + `style.css` + `mock.js` (+ `test_mock.js` se tocado). Mensagem no padrão `feat(irrigation): nível — UI em cards + form em frase (boia)`. Push na branch `sistema-irrigacao`.
