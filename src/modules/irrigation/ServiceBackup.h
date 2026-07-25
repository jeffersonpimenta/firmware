#pragma once
#include <cstddef>
#include <cstdint>

namespace IrrigationService {

size_t base64Encode(const uint8_t *in, size_t n, char *out, size_t cap);
int base64Decode(const char *in, size_t n, uint8_t *out, size_t cap);

} // namespace IrrigationService
