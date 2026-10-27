# Fonte de Acionamento — UI (companion)

> Data: 2026-10-27 · Branch: `sistema-irrigacao`
> **Depende de:** [`2026-10-27-irrigacao-fonte-de-acionamento-unificada-design.md`](./2026-10-27-irrigacao-fonte-de-acionamento-unificada-design.md)
> (núcleo firmware — fonte da verdade do contrato). Firmware mergeia primeiro.

> **Status: esqueleto.** O design da UI ainda vai ser brainstormado em sessão
> própria. Este arquivo só fixa a amarração com o núcleo e lista o que a UI
> precisa resolver. Não implementar a partir dele ainda.

## Por que existe

O refactor da Fonte de Acionamento (spec de firmware) muda o contrato que a UI
consome. Esta companion garante que a mudança de UI seja rastreável e amarrada
ao núcleo, sem inchar a spec de firmware.

## Contrato herdado do firmware

Ver tabela "Specs relacionadas e contrato com a UI" na spec de firmware. Resumo
do que muda na superfície que a UI toca:

- Config por entrada passa a `digitalInMode[i]` (`InputMode`) + `digitalInActiveLow`.
- `pinsRemoteLed[2]` e `digitalInLedIdx` **removidos** da UI.
- Tabela de Espelho funde na tabela única de Fonte de Acionamento
  (espelho = preset `FOLLOW`).
- `RemoteTrigger`/validação ganham `action` explícito.

## Questões a brainstormar (ainda abertas)

1. Seletor de `InputMode` por entrada: dropdown único, ou "nível/borda" +
   sub-opção de ação? Como sinalizar os modos **desencorajados**
   (`EDGE_TOGGLE`/`EDGE_OPEN`/`EDGE_CLOSE`) sem travar o usuário?
2. Migração visual: a tela de Espelho some ou vira um preset dentro da Fonte de
   Acionamento? Como reaproveitar o que o usuário já configurou (greenfield —
   provavelmente zera).
3. Feedback: a UI deve mostrar o estado de comando (inflight/ok/fail) já que o
   LED físico é transiente? Reaproveita o estado real da zona que o portal já
   exibe?
4. Onde vive: portal AP, web do gateway, app — paridade com as telas de hoje.
5. Polaridade/optocoupler: expor o bit `digitalInActiveLow` por entrada de forma
   legível ("ativo-alto/ativo-baixo" vs "normal/invertido").

## Fora de escopo

- Núcleo firmware, protocolo, config on-disk — na spec de firmware.
