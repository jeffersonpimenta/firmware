# Tela unificada "Enlace / Cobertura" — design

**Data:** 2026-08-11
**Branch:** `sistema-irrigacao`
**Arquivos:** `data/irrigacao/app.js` (único arquivo tocado; sem CSS novo)

## Contexto

O painel web do gateway de irrigação (SPA em `data/irrigacao/`) tem hoje duas
telas secundárias distintas no menu "Mais", com propósito e visual sobrepostos:

- **Malha / Enlace** (`renderMalha`, chave `malha`): cards por **nó conhecido**.
  Fonte `GET /stations`. Campos: nome, chip de qualidade (por SNR), SNR, RSSI,
  Bateria. Sem ação. Polled 3 s.
- **Cobertura** (`renderCobertura`, chave `cobertura`, site survey §8.5): tabela
  de **beacons de qualquer nó**. Fonte `GET /survey`. Campos: nó (hex), papel
  (Estação/Gateway/Repetidor/Serviço), coordenada GPS, SNR, RSSI, idade. Botão
  **Limpar** (`POST /survey/clear`). Polled 3 s.

Ambas mostram SNR+RSSI por nó e são diagnósticos de rádio. Divergem em fonte de
dados, campos e layout (cards vs tabela).

## Objetivo

Unificar as duas em **uma** entrada de menu e **uma** tela, com alternância por
controle segmentado. Mantém todas as funcionalidades atuais; nenhuma perda.

## Decisões (brainstorming)

1. **Modelo:** uma tela, duas abas/seções (datasets ficam separados sob um teto
   só). Não fundir por nó.
2. **Interação:** controle segmentado no topo (toggle) — mostra só uma vista por
   vez. Reusa o padrão `.seg wide` dentro de `.segrow` já existente (usado no
   toggle Válvula|Motor da tela de estações). Zero CSS novo.
3. **Nome do menu:** título **Enlace / Cobertura**, subtítulo *"SNR/RSSI dos nós
   e cobertura de sinal"*.
4. **Rótulos das abas:** **Enlace** | **Cobertura** (casam com o título; "Malha"
   some do rótulo visível).
5. **Sem aliases** das chaves antigas — limpeza total.

## Desenho

### 1. Menu (`renderMais`)

Remove as duas linhas `['cobertura', …]` e `['malha', …]`. Adiciona uma:

```js
['radio', 'Enlace / Cobertura', 'SNR/RSSI dos nós e cobertura de sinal'],
```

Posição: onde ficavam as duas antigas (bloco de diagnóstico de rádio).

### 2. `renderRadio()` — substitui `renderMalha` + `renderCobertura`

Estado de módulo (persiste entre re-renders de poll):

```js
let signalSeg = 'enlace'; // 'enlace' | 'cobertura'
```

Fluxo:

- Renderiza `.segrow` com dois `.seg wide` — **Enlace** | **Cobertura** — o ativo
  com classe `active` conforme `signalSeg`.
- Busca **só** o dataset da aba ativa:
  - `enlace` → `GET /stations` → corpo via `malhaCardsHtml(list)`
  - `cobertura` → `GET /survey` → corpo via `coberturaTableHtml(rows)`
- Injeta `segrow + corpo` no `#view`.
- Wire de eventos:
  - Clique em `.seg`: seta `signalSeg` e chama `renderRadio()` de novo.
  - `#cov-clear` (só existe no corpo de cobertura): `POST /survey/clear` +
    `renderRadio()`.

### 3. Helpers puros (extraídos do código atual, sem alterar a lógica)

- `malhaCardsHtml(list)`: corpo atual de `renderMalha` (map de cards + empty
  state "Nenhuma estação conhecida.").
- `coberturaTableHtml(rows)`: corpo atual de `renderCobertura` (tabela + linha do
  botão Limpar + empty state "Sem beacons recebidos.").

Retornam string HTML; não tocam o DOM. `renderRadio` faz fetch + inserção + wire.

### 4. Roteamento

- `RENDER.radio = renderRadio;`
- `SECTION_LABELS.radio = 'Enlace / Cobertura';`
- `POLLED.radio = 1;`
- Remove as chaves `malha` e `cobertura` dos três mapas (`RENDER`,
  `SECTION_LABELS`, `POLLED`).

### 5. Poll

`showSub('radio')` inicia poll de 3 s chamando `renderRadio()`, que re-busca só a
aba ativa. O toggle sobrevive porque o estado está no módulo. Sem inputs de texto
na tela → o poll não interrompe interação.

## Fora de escopo

- Fundir os dois datasets por nó.
- Qualquer mudança nos endpoints (`/stations`, `/survey`, `/survey/clear`).
- CSS novo.

## Riscos / notas

- Único arquivo alterado: `data/irrigacao/app.js`. Mudança de UI, sem impacto no
  firmware C++.
- Após remover `renderMalha`/`renderCobertura`, garantir que nenhuma outra
  referência às funções ou às chaves `malha`/`cobertura` permaneça no arquivo.
