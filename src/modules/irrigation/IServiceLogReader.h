#pragma once
#include <cstddef>

namespace IrrigationWeb
{

// Leitor injetável do servico.jsonl (§11.2). Entrega cada linha (já-JSON, NUL-terminada),
// da mais antiga p/ mais nova, limitado às últimas maxLines. Header próprio p/ evitar ciclo
// de include entre IProfileStore.h (produz o leitor) e ServicePortalApi.h (consome em buildServiceLog).
struct IServiceLogReader {
    virtual void forEachLine(size_t maxLines, void *ctx, void (*cb)(void *, const char *line)) = 0;
    virtual ~IServiceLogReader() = default;
};

} // namespace IrrigationWeb
