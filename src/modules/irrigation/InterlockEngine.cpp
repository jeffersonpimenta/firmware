#include "modules/irrigation/InterlockEngine.h"

bool evalCondition(uint8_t condicao, bool active, int32_t valueCenti,
                   int32_t thresholdCenti, uint16_t histCenti, bool &latched)
{
    switch (condicao) {
    case COND_ATIVO:   latched = active;  break;
    case COND_INATIVO: latched = !active; break;
    case COND_MENOR_QUE:
        if (valueCenti < thresholdCenti) latched = true;
        else if (valueCenti >= thresholdCenti + (int32_t)histCenti) latched = false;
        break; // dentro da banda: mantém latched
    case COND_MAIOR_QUE:
        if (valueCenti > thresholdCenti) latched = true;
        else if (valueCenti <= thresholdCenti - (int32_t)histCenti) latched = false;
        break;
    default: latched = false; break;
    }
    return latched;
}
