#pragma once
#include <stddef.h>
#include <stdint.h>

enum InterlockCond : uint8_t { COND_ATIVO = 0, COND_INATIVO = 1, COND_MENOR_QUE = 2, COND_MAIOR_QUE = 3 };
enum InterlockAcao : uint8_t { ACAO_BLOQUEAR_ABERTURA = 0, ACAO_FECHAR_E_BLOQUEAR = 1 };
enum InterlockTipo : uint8_t { IL_SENSOR = 0, IL_SIMULTANEIDADE = 1 };

// Avalia a condição de uma regra sobre uma leitura, com histerese latched.
// `active`/`valueCenti` vêm do sensor (digital usa active; analógico usa valueCenti).
// `latched` é o estado retido pelo chamador (entra e sai por referência).
// Retorna o novo estado de disparo (true = condição satisfeita agora).
bool evalCondition(uint8_t condicao, bool active, int32_t valueCenti,
                   int32_t thresholdCenti, uint16_t histCenti, bool &latched);
