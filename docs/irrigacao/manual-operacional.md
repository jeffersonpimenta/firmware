# Manual Operacional — Sistema de Irrigação LoRa

**Guia prático para operadores de campo e gerentes de sistema.**

---

## 1. Visão Geral

Sistema automático de irrigação via LoRa mesh (rádio de longo alcance, sem internet).

**Componentes:**
- **Gateway** — computador central (painel web + cronograma)
- **Estações** — válvulas remotas (autossuficientes, com failsafe local)
- **Nó de serviço** — equipamento portátil para diagnóstico e configuração

**Princípio:** Se o rádio cair, o sistema **não trava**. Failsafe garante fechamento automático.

---

## 2. Operação Diária

### 2.1 Acesso ao painel web

1. **Na rede local** (WiFi do gateway):
   - Abra navegador
   - Acesse `http://192.168.4.1`
   - Painel aparece (sem login, rede privada)

2. **Remotamente** (com internet):
   - Necessário VPN ou port forwarding
   - Solicite ao administrador

### 2.2 Visão Geral (aba principal)

Mostra status em tempo real:

```
┌─────────────────────────────────────┐
│ Estações: 3        Em execução: 1  │
│ Zona 1 · 25 min    Alertas: 0     │
│                                     │
│ [com relógio] [⬇ Backup]           │
└─────────────────────────────────────┘
```

- **Estações**: total de nós pareados
- **Em execução**: zona irrigando agora + tempo restante
- **Alertas**: problemas detectados (bateria baixa, tamper, etc)
- **Relógio**: com=cronograma ativo, sem=modo manual apenas

### 2.3 Aba Estações

Lista todos os nós e status de comunicação:

```
Horta Norte    há 45 s  · 12.5 V  · sincronizada ✓
Pomar          há 3 min · 11.0 V  · pendente ⚠
Pastagem       há 12 s  · 13.2 V  · sincronizada ✓
```

- **Tempo**: quanto faz que recebeu o último sinal
- **Tensão**: bateria (normal ≥ 12V, aviso < 12V, crítica < 11.8V)
- **Sincronização**: 
  - ✓ sincronizada = respondeu aos últimos comandos
  - ⚠ pendente = últimas tentativas falharam
  - ✗ inalcançável = sem sinal há muito tempo

**Ação:** Clique na estação → detalhes + troubleshooting.

---

## 3. Zonas — Comando Manual

Aba **Zonas** = comando direto de válvulas/saídas.

```
Horta
  Est. Horta Norte · saída 0 · Válvula
  [Editar] [Abrir] [Fechar]

Pomar
  Est. Horta Norte · saída 1 · Válvula
  [Editar] [Abrir] [Fechar]
```

### Abrir zona:
1. Clique **[Abrir]**
2. Sistema envia comando + duração máxima de segurança
3. Estação abre e inicia timer local (failsafe)
4. Painel mostra "em execução" com tempo restante

**Duração máxima:** Configurada no painel (padrão 45 min). Nunca ultrapassa 120 min (teto do firmware).

### Fechar zona:
1. Clique **[Fechar]**
2. Comando prioritário (sempre aceito, até com bateria baixa)
3. Estação fecha imediatamente

---

## 4. Programas — Cronograma Automático

Aba **Programas** = irrigações automáticas por hora/dia.

### Criar programa:

1. **[+ Novo programa]**
2. Preencha:
   - **Dias**: dom, seg, ter... (multiplos)
   - **Horário**: ex. 06:00 (hora de início)
   - **Sequência**: lista de etapas (zona + duração)

Exemplo:
```
Todos os dias · 06:00
  Etapa 1: Horta    20 min
  Etapa 2: Pomar    30 min
  Etapa 3: Pastagem 45 min
  Total: 95 min
```

### Execução automática:

- **06:00** → Horta abre (20 min)
- **06:20** → Horta fecha, Pomar abre (30 min)
- **06:50** → Pomar fecha, Pastagem abre (45 min)
- **07:35** → Pastagem fecha. Programa termina.

**Se Gateway sem relógio:** cronograma inativo (painel avisa). Use modo manual.

**Se rádio cai durante programa:** 
- Estação fecha sozinha ao expirar timer local
- Próxima etapa fica adiada
- Ao reconectar, gateway retoma

---

## 5. Grupos Hidráulicos — Coordenação de Bombas

Aba **Grupos** = controle de bombas + limitação simultânea.

Cenário: Farm com bomba única, mas 4 zonas. Problema: bomba não aguenta 2+ zonas abertas ao mesmo tempo.

**Solução:** Grupo com máx 1 zona aberta.

### Criar grupo:

1. **[+ Novo grupo]**
2. Preencha:
   - **Nome**: ex. "Grupo Principal"
   - **Bomba**: qual zona aciona a bomba (GPO)
   - **Zonas membro**: quais zonas o grupo controla
   - **Máx abertas**: 1 (só uma zona por vez)
   - **Min abertas**: 1 (evita bomba ligar com zero)

### Operação:

```
Operador abre zona 1 (via painel):
  → Grupo verifica: abertas=0 < máx=1 ✓ Autoriza
  → Zona 1 abre
  → Bomba liga (automático)
  
Operador tenta abrir zona 2:
  → Grupo verifica: abertas=1 = máx=1 ✗ Bloqueado
  → Botão fica vermelho "adiado"
  → Fila espera zona 1 fechar
  
Zona 1 completa (20 min):
  → Zona 1 fecha
  → Grupo detecta: abertas=0
  → Auto-abre zona 2 da fila
  → Bomba continua ligada (proteção: mín 5 min)
```

---

## 6. Sensores — Monitoramento

Aba **Sensores** = leitura de sensores locais (pressão, nível, etc).

```
Estação Horta Norte (0x1a2b3c4d)
┌───────────────────────────────────┐
│ Idx │ Nome            │ Tipo      │ Valor      │
├─────┼─────────────────┼───────────┼────────────┤
│ 0   │ Pressão linha   │ Analógico │ 7.50 bar   │
│ 1   │ Nível           │ Digital   │ ativo      │
└───────────────────────────────────┘

Pode editar nome de cada sensor (ex.: "Pressão clorador").
```

**Uso:** Diagnóstico de problemas. Ex.: "Pressão caiu = vazamento?".

---

## 7. Intertravamentos — Segurança Automática

Aba **Intertravam.** = regras automáticas (se X, então fazer Y).

Exemplo 1: **Proteção de pressão**
```
Tipo: Sensor
Estação: Horta Norte
Sensor: 0 (pressão)
Condição: < 4.0 bar
Ação: Bloquear abertura de zonas 1, 2
Mensagem: "Pressão do sistema baixa"
```

Operação:
- Pressão normal (8.0 bar) → zonas 1,2 abertem livremente
- Pressão cai pra 3.5 bar → regra ativa, botões "Abrir" desabilitados
- Pode fechar manualmente (close sempre funciona)
- Pressão sobe pra 4.5 bar → regra desativa, "Abrir" habilita novamente

Exemplo 2: **Limite simultâneo**
```
Tipo: Simultaneidade
Máx zonas abertas: 2 (em todo o gateway)
Ação: Bloquear novas aberturas
Afetar: Todas
```

---

## 8. Pareamento — Adicionar Nó Novo

### Estação chega nova (primeira vez):

1. **Na estação**: segure botão de pareamento **3 segundos**
   - LED pisca amarelo (modo pareamento)
   - Transmite anúncio de pareamento

2. **No painel gateway**:
   ```
   Pareamento pendente — nó 0x1a2b3c4d · expira em 45s
   [Aprovar] [Rejeitar]
   ```
   Clique **[Aprovar]**

3. **Estação** recebe aprovação:
   - Salva configuração segura (PSK)
   - Reboot automático (3s)
   - Volta online sincronizada

4. **Gateway** adiciona nó à allowlist:
   - Agora aceita comandos do nó
   - Envia configuração automática

✓ **Pareado e operacional.**

**Se expirar sem aprovação:** nó volta a modo pareamento. Repita desde o passo 1.

---

## 9. Troubleshooting — Problemas Comuns

### Estação "pendente" (amarelo) há muito tempo

**Causa:** Comunicação intermitente, rádio distante, interferência.

**Ação:**
1. Verifique sinal WiFi/LoRa (SNR/RSSI na aba Cobertura)
2. Mova nó perto do gateway (teste)
3. Verifique bateria (tensão na aba Estações)
4. Se persistir: reboot estação (desligue 10s)

### Comando rejeitado (badge vermelho)

**Causa:** Bateria crítica, intertravamento ativo, zona já aberta, ou falta de comunicação.

**Ação:**
1. Aba Intertravam. → verifica se regra bloqueando
2. Aba Estações → verifica tensão (< 11.8V = crítica)
3. Tenta novamente (rádio pode estar congestionado)

### Programa não executou na hora

**Causa:** Gateway sem relógio real. Painel avisa "sem relógio".

**Ação:**
1. Gateway precisa relógio (RTC ou NTP)
2. Enquanto isso: use modo manual (abrir/fechar direto)

### Sensor lê sempre 0

**Causa:** Pino solto, sensor desconectado, ou parado.

**Ação:**
1. Verifique conexão física do sensor
2. Reboot estação
3. Se persiste: sensor pode estar queimado

### Tamper "ativo" (red)

**Causa:** Caixa da estação foi aberta.

**Ação:**
1. Verifique se há roubo/vandalismo
2. Se legítimo (manutenção): clique "Abrir janela" (10 min)
3. Estação ignora tamper enquanto manutenção ativa
4. Ao expirar: tamper reativa automaticamente

---

## 10. Manutenção de Campo

### Checklist semanal:

- [ ] Painel web: todos os nós "sincronizados"?
- [ ] Aba Estações: tensões > 12V?
- [ ] Aba Log: algum erro recorrente?
- [ ] Aba Cobertura: SNR/RSSI razoáveis (> -110 dBm)?

### Se nó fica offline permanentemente:

1. Desligue estação 10 segundos
2. Religue
3. Aguarde ~30s para reconectar
4. Se ainda offline: pode estar com bateria completamente descarregada

### Backup de configuração:

- **[⬇ Backup]** no painel
- Baixa JSON com toda a configuração (PSK, zonas, programas, etc)
- Guarde em local seguro (nuvem, pen drive)
- Útil para restaurar se gateway cair

---

## 11. Segurança Básica

### PSK (chave do sistema)

- Gerada aleatoriamente no primeiro boot do gateway
- **Nunca divulgue** para rádios não-autorizados
- Backup contém PSK (guarde em seguro)

### Allowlist

- Gateway só aceita comandos de nós pareados
- Novo nó recebe comando apenas após aprovação operador
- Rejeita automaticamente nó desconhecido

### Failsafe (proteção contra travamento)

- Toda abertura têm timeout local (máx 120 min)
- Se rádio falha, estação fecha sozinha
- Nunca fica aberta indefinidamente

---

## 12. Contatos e Suporte

- **Administrador de sistema:** [contato]
- **Técnico de campo:** [contato]
- **Log de problemas:** Aba Log (exportar CSV se necessário)

---

**Última atualização:** 2026-07-26  
**Versão firmware:** 8.2+
