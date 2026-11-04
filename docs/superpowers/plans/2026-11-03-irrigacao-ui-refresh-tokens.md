# Irrigação Web UI — Design-Token Refresh Conformance Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the device-served web UI in `data/irrigacao/` fully conform to the design-token spec in `docs/ui-reference/Webserver UI Refresh.dc.html` / `PROMPT_CLAUDE_CODE.md`, without adding/removing screens.

**Architecture:** All screens already exist and function. This is a *conformance refactor*: (1) every color literal lives only in `:root` (tokenize CSS rules + eliminate hardcoded colors in JS templates), (2) no `box-shadow`, (3) no `style="…"` in JS templates except sparkline bar-height and progress-bar width, (4) `style.css` ≤ 30 KB, (5) a single `api.js` data layer with `mock.js` behind `?mock=1`. Tokenizing shrinks CSS (long `oklch(...)` literals → short `var(--x)`), which funds the size budget.

**Tech Stack:** Vanilla JS (no build), one `style.css`, system fonts, `oklch` tokens. Offline; LittleFS-served.

## Global Constraints (verbatim from spec)

- Roda offline. Sem web fonts, CDN, frameworks ou build step. Vanilla JS + um CSS, fontes do sistema.
- `style.css` ≤ 30 KB (30720 bytes). Primitivos compartilhados; nada de regras duplicadas por tela.
- Todo acesso a dados passa por `api.js`. As telas nunca chamam `fetch` direto. `mock.js` implementa a mesma interface, ativado por `?mock=1`.
- Escopo = o que está na referência. Não acrescentar telas/campos/funções.
- Nenhuma cor fora de `:root` (CSS nem strings de template JS). Sem `box-shadow`. Separação só por borda.
- **Font floor EXCEPTION (user decision 2026-11-03):** keep the mockup's 10.5–11px chip/timestamp/tab sizes; the "≥12px" criterion is waived in favor of visual fidelity.
- Work on branch `sistema-irrigacao`. Commit per task. Do NOT push to `fork` unless the user asks.
- **Windows/UTF-8:** files contain Portuguese accents. Every edit MUST preserve UTF-8 **without BOM**. After each task run `node --check` on changed `.js` and confirm `git diff --stat` shows a sane line delta (not a full-file rewrite from mojibake). See memory `subagent-windows-encoding-corruption`.

## Acceptance Gates (run from `data/irrigacao/`)

- **G1 colors:** `grep -nE "#[0-9a-fA-F]{3,6}\b|rgba?\(|oklch\(" app.js portal/app.js mock.js` → **0 matches**. For `style.css`, every `oklch(`/`rgba(` occurrence is inside the `:root{…}` block only.
- **G2 inline style:** `grep -noE "style=" app.js portal/app.js` → only the whitelisted sparkline/progress sites remain (documented in Task 6).
- **G3 box-shadow:** `grep -c "box-shadow" style.css app.js portal/app.js` → **0** everywhere.
- **G4 size:** `wc -c style.css` → **≤ 30720**.
- **G5 syntax:** `node --check app.js && node --check portal/app.js && node --check api.js && node --check mock.js` → all OK.
- **G6 mock flows:** with `?mock=1`, gateway + portal render and the spec's interaction flows work (manual/bench — browser).
- **G7 responsive:** no horizontal overflow at 360px and 480px (manual/bench).

---

## File Structure

- Create `data/irrigacao/api.js` — single data layer. Houses `getJson`/`postJson` + resource functions for **both** `/api/irrigation/*` (gateway) and `/api/portal/*` (portal). Loaded by both `index.html` and `portal/index.html`.
- Modify `data/irrigacao/index.html` + `portal/index.html` — conditional `?mock=1` loader + add `<script src="api.js">` / `"../api.js"`.
- Modify `data/irrigacao/mock.js` — auto-activate on `?mock=1` (keep manual path); it continues to patch `window.fetch`, so `api.js` fetch calls hit mock data (same interface, zero duplication).
- Modify `data/irrigacao/app.js` — remove embedded `getJson`/`postJson`, call `api.*`; replace all hardcoded colors / inline styles / box-shadows with token-based CSS classes.
- Modify `data/irrigacao/portal/app.js` — same, replacing its `j()` helper.
- Modify `data/irrigacao/style.css` — add token set below; replace every in-rule literal with `var(...)`; define missing `--muted-2`, `--radius-stat`; absorb new component classes; trim to ≤30 KB.

### Token additions to `:root` (exact)

Add these after the existing tokens (they are each used ≥1× today as raw literals):

```css
--green-08: oklch(0.47 0.1 150 / 0.08);
--green-12: oklch(0.47 0.1 150 / 0.12);
--green-35: oklch(0.47 0.1 150 / 0.35);
--green-50: oklch(0.47 0.1 150 / 0.5);
--green-spark: oklch(0.47 0.1 150 / 0.5);
--amber-10: oklch(0.65 0.15 75 / 0.1);
--amber-15: oklch(0.65 0.15 75 / 0.15);
--amber-40: oklch(0.65 0.15 75 / 0.4);
--red-30: oklch(0.55 0.16 30 / 0.3);
--red-40: oklch(0.55 0.16 30 / 0.4);
--red-08: oklch(0.55 0.16 30 / 0.08);
--red-12: oklch(0.55 0.16 30 / 0.12);
--field: oklch(0.97 0.003 100);
--tank-fill: oklch(0.47 0.1 150 / 0.35);
--tank-border: oklch(0.8 0.006 100);
--chip-neutral-bg: oklch(0.9 0.006 100);
--scrim: oklch(0.22 0.008 100 / 0.4);
--muted-2: oklch(0.6 0.008 100);
--radius-stat: 12px;
```

Notes:
- `--green-soft` (0.1) already exists — use it; do NOT add a `--green-10`.
- `rgba(0,0,0,0.4)` (`.sheet-backdrop`) → `var(--scrim)`.
- `oklch(0.4 0.006 100)` (lvl-pill-chip color, lvl-delcancel) → existing `var(--neutral-ink)`.
- `oklch(0.55 0.14 230)` (`.gi-supress`) → existing `var(--blue-ink)`.
- `var(--radius-stat)` and `var(--muted-2)` are referenced today but undefined — defining them fixes live bugs.

---

## Task 1: `api.js` data layer + `?mock=1` wiring

**Files:**
- Create: `data/irrigacao/api.js`
- Modify: `data/irrigacao/index.html`, `data/irrigacao/portal/index.html`, `data/irrigacao/mock.js`
- Modify: `data/irrigacao/app.js` (remove its `getJson`/`postJson` defs; they now come from `api.js`), `data/irrigacao/portal/app.js` (remove its `j()` def)

**Interfaces — Produces (global `window`, no modules/build):**
- `getJson(path)` → `Promise<any>` (GET JSON; path relative to each app's API base)
- `postJson(path, body)` → `Promise<any>` (POST JSON)
- `api` object with resource fns used by callers, e.g. `api.overview()`, `api.zones()`, `api.command(body)`, `api.portalNode()`, etc. — **mirror the exact endpoint set the two apps already call** (enumerate from current `app.js`/`portal/app.js`; do not invent new endpoints).

- [ ] **Step 1:** Read current `getJson`/`postJson` in `app.js` (~lines 11–25, base `/api/irrigation`) and `j()` in `portal/app.js` (~lines 24–30, base `/api/portal`). List every distinct endpoint string each app calls.
- [ ] **Step 2:** Create `api.js` defining `getJson`/`postJson` (identical behavior/signatures to the originals so call sites are unchanged) plus an `api` namespace with one function per endpoint found. Keep the two API bases: gateway fns hit `/api/irrigation/*`, portal fns hit `/api/portal/*`. Pure fetch — no mock logic here.
- [ ] **Step 3:** In `mock.js`, add at top: auto-activate when `new URLSearchParams(location.search).has('mock')` (keep the existing manual activation too). mock.js keeps patching `window.fetch`; since `api.js` uses fetch, mock data flows through unchanged.
- [ ] **Step 4:** `index.html`: before `<script src="app.js">`, add `<script src="api.js"></script>`; and a tiny conditional loader that injects `mock.js` first when `?mock=1`. `portal/index.html`: same with `../api.js` and `../mock.js`. Remove the "descomente para testar" manual comment lines (superseded by `?mock=1`).
- [ ] **Step 5:** In `app.js` delete the local `getJson`/`postJson` definitions (now global from `api.js`). In `portal/app.js` delete local `j()` and replace its call sites with `getJson`/`postJson` (or keep a one-line `const j = getJson;` alias to minimize churn — acceptable). Leave all screen logic otherwise untouched.
- [ ] **Step 6 (test):** Run `node --check api.js app.js portal/app.js mock.js` → OK. Run `grep -nE "fetch\(" app.js portal/app.js` → **0** (all fetch lives in api.js/mock.js). Confirm `index.html`/`portal/index.html` reference `api.js`.
- [ ] **Step 7 (commit):** `git add -A data/irrigacao && git commit` — `refactor(ui): extrai camada de dados api.js e ativa mock via ?mock=1`.

---

## Task 2: CSS tokenization + fix undefined vars (owns `style.css`)

**Files:** Modify `data/irrigacao/style.css`

- [ ] **Step 1:** Add the token block above into `:root` (keep it one line, minified style, consistent with the file).
- [ ] **Step 2:** Replace every in-rule color literal with its `var(...)` per the mapping table above. Sweep the whole file. Common ones: `oklch(0.47 0.1 150 / 0.12)`→`var(--green-12)`, `/ 0.08)`→`var(--green-08)`, `/ 0.5)`→`var(--green-50)`, `/ 0.35)`→`var(--tank-fill)`; amber `/0.15`→`var(--amber-15)`, `/0.1`→`var(--amber-10)`, `/0.4`→`var(--amber-40)`, bare `oklch(0.65 0.15 75)`→`var(--amber)`; red `/0.08`→`var(--red-08)`, `/0.12`→`var(--red-12)`, `/0.3`→`var(--red-30)`, `/0.4`→`var(--red-40)`; `oklch(0.97 0.003 100)`→`var(--field)`; `oklch(0.9 0.006 100)`→`var(--chip-neutral-bg)`; `oklch(0.8 0.006 100)`→`var(--tank-border)`; `oklch(0.4 0.006 100)`→`var(--neutral-ink)`; `oklch(0.55 0.14 230)`→`var(--blue-ink)`; `rgba(0,0,0,0.4)`→`var(--scrim)`. `.pill.active` literal `oklch(0.47 0.1 150 / 0.1)`→`var(--green-soft)`.
- [ ] **Step 3:** Confirm no `box-shadow` exists in style.css (grep = 0 today; keep it that way).
- [ ] **Step 4 (test):** `grep -nE "oklch\(|rgba?\(|#[0-9a-fA-F]{3,6}\b" style.css` → **every match is inside the leading `:root{…}`** (since file is minified onto few lines, visually verify the only colored substring segment is the `:root` block). `grep -c "var(--muted-2)\|var(--radius-stat)" style.css` → both now resolve (defined). `wc -c style.css` → record new size (expected to **drop** vs 31263).
- [ ] **Step 5 (commit):** `refactor(ui): tokeniza cores do style.css, define --muted-2/--radius-stat`.

---

## Task 3: app.js de-inline — overview, stations, zones (+ needed classes)

**Files:** Modify `data/irrigacao/app.js`, `data/irrigacao/style.css`

**Scope:** `renderOverview`, `renderStations` + station sheet, `renderZones` + zone edit form. Convert every `style="…"` and hardcoded color in these functions to CSS classes. **Reuse existing classes first** (the file already has `.card.stat`, `.card.station`, `.card.zone`, `.pairbox`, `.sheet*`, `.pills`, `.seg`, `.oput`, `.btn.*`, `.card.redbox`, etc.). Only add a new class when no existing one fits; add it to `style.css` using tokens (never a raw literal).

- [ ] **Step 1:** Grep the target functions for `style=`, hardcoded `oklch(`/`#fff`/`rgba(`, and `box-shadow`. For each, pick an existing class or define a token-based one.
- [ ] **Step 2:** Rewrite templates to use `class="…"`. Replace `#fff`→ the `--on-accent`/`--card` class. Remove `box-shadow` (spec: borders only). The one allowed inline style here is the **progress-bar width** in the "Em execução" card — keep `style="width:${pct}%"` on the fill element only; its color/height come from a class.
- [ ] **Step 3 (test):** `node --check app.js`. Grep these functions' line ranges: no `oklch(`/`rgba(`/`#`, no `box-shadow`, no `style=` except the progress-fill width. `git diff --stat` sane (no encoding blow-up).
- [ ] **Step 4 (commit):** `refactor(ui): classes por token em overview/estações/zonas`.

---

## Task 4: app.js de-inline — programs + Mais forms

**Files:** Modify `data/irrigacao/app.js`, `data/irrigacao/style.css`

**Scope:** `renderPrograms` + program edit form (days grid, steps, switch), plus the `Mais` menu (`renderMais`) and `renderSistema`. Same reuse-first rule (`.card.program`, `.switch`, `.card.step`, `.btn.stepadd`, `.card.navrow`, `.seg`, `.pills`).

- [ ] **Step 1:** Grep scope for `style=`/colors/`box-shadow`; map to classes.
- [ ] **Step 2:** Rewrite to classes; add token-based classes to style.css only where missing.
- [ ] **Step 3 (test):** `node --check app.js`; grep scope clean; diff stat sane.
- [ ] **Step 4 (commit):** `refactor(ui): classes por token em programas/mais/sistema`.

---

## Task 5: app.js de-inline — Mais subscreens

**Files:** Modify `data/irrigacao/app.js`, `data/irrigacao/style.css`

**Scope:** `routeAcionamento`, `renderGruposInterlocks`, `renderNiveis`, `renderMeteo`, `renderSensores`, `renderGpo`, `renderTamper`, `renderAuditLog`, `renderRadio`, `renderWifi`, `renderHorario`. These hold most of the remaining inline styles and the sub-12px `11px` font-sizes. **Font floor is waived** — keep the mockup's small sizes, but move them from inline `style=` into classes (so G2 passes) using the existing size scale (`10.5/11.5/12.5px` classes already exist in many places; reuse or add).

- [ ] **Step 1:** Grep scope for `style=`/colors/`box-shadow`. Note the sparkline (radio) bar-height is the **whitelisted** dynamic inline style — keep `style="height:${h}%"` on spark bars only; color via `.fp-spark > i` class.
- [ ] **Step 2:** Rewrite to classes; add token-based classes where missing.
- [ ] **Step 3 (test):** `node --check app.js`; grep scope: no colors/box-shadow; `style=` only on spark bar height. Diff stat sane.
- [ ] **Step 4 (commit):** `refactor(ui): classes por token nas subtelas de Mais`.

---

## Task 6: portal/app.js de-inline

**Files:** Modify `data/irrigacao/portal/app.js`, `data/irrigacao/style.css`

**Scope:** all 16 inline `style=` + 1 color in `portal/app.js`. Reuse the `.field-portal .fp-*` class family already defined. The **whitelisted** dynamic inline styles for the whole project are: (a) sparkline `height:` on `.fp-spark > i`, (b) progress-bar `width:` on the gateway "Em execução" fill. Everything else becomes classes.

- [ ] **Step 1:** Grep `portal/app.js` for `style=`/`oklch(`/`rgba(`/`#`/`box-shadow`.
- [ ] **Step 2:** Rewrite to `.fp-*` classes (add token-based ones only if missing).
- [ ] **Step 3 (test):** `node --check portal/app.js`; `grep -nE "oklch\(|rgba?\(|#[0-9a-fA-F]{3,6}\b|box-shadow" portal/app.js` → 0; `grep -n "style=" portal/app.js` → only whitelisted spark/progress. Diff stat sane.
- [ ] **Step 4 (commit):** `refactor(ui): classes por token no portal`.

---

## Task 7: Final gates — size, full grep sweep, responsive

**Files:** Modify `data/irrigacao/style.css` (trim only if needed)

- [ ] **Step 1 (G1):** `grep -nE "#[0-9a-fA-F]{3,6}\b|rgba?\(|oklch\(" app.js portal/app.js mock.js` → 0. `style.css`: all color literals inside `:root` only.
- [ ] **Step 2 (G2):** `grep -noE "style=" app.js portal/app.js` → only spark-height + progress-width sites.
- [ ] **Step 3 (G3):** `grep -c "box-shadow" style.css app.js portal/app.js` → 0.
- [ ] **Step 4 (G4):** `wc -c style.css` → ≤ 30720. If over: remove duplicate rules, merge selectors sharing declarations, drop redundant properties (no behavior change). Re-check.
- [ ] **Step 5 (G5):** `node --check api.js app.js portal/app.js mock.js` → OK.
- [ ] **Step 6 (G6/G7):** Serve `data/irrigacao/` and open `index.html?mock=1` and `portal/index.html?mock=1` at 360px & 480px; walk the spec flows (pairing approve, zone open/close, zone+program CRUD, alert ack, log; portal pulse open/close, GPO confirm, zone-via-net, survey start/stop, save coords, roles estação/repetidor/serviço/wizard). Record any overflow/regressions and fix. **If no browser is available, mark G6/G7 as bench-pending and report.**
- [ ] **Step 7 (commit):** `chore(ui): fecha gates de conformidade (tamanho/cores/inline/shadow)`.

---

## Self-Review notes

- Spec coverage: criteria G1–G5 are mechanically verified; G6/G7 need a browser (bench). Screen/field scope unchanged (no additions) ✔.
- Font floor: intentionally waived per user (2026-11-03) — keep mockup 10.5–11px, but all sizes must live in CSS classes, not inline, so G2 still holds.
- Encoding: every JS-touching task ends with `node --check` + diff-stat sanity to catch subagent UTF-8 corruption.
- Order: Task 1 (api) independent; Task 2 (tokens) must precede 3–6 (they consume tokens); Tasks 3→4→5 serialize on `app.js`; Task 6 on `portal/app.js`; all CSS edits serialize on `style.css` so run 2→3→4→5→6→7 in order.
