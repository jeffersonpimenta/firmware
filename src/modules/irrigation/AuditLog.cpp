#include "AuditLog.h"
#include "IrrigationProtocol.h"

// Tamanho fixo de um registro serializado: tsSecs(4) + origin(1) + action(1) + target(1) + result(1) + node(4) + seq(4) = 16 bytes
static constexpr size_t RECORD_SIZE = 16;
// Tamanho do cabeçalho: magic(4) + count(2) + reservado(2) = 8 bytes
static constexpr size_t HEADER_SIZE = 8;
// Tamanho do CRC final: 4 bytes
static constexpr size_t CRC_SIZE = 4;

void AuditLog::append(const AuditRecord &r)
{
    if (cap == 0)
        return;
    buf[writeIdx] = r;
    writeIdx = (writeIdx + 1) % cap;
    if (num < cap)
        num++;
}

const AuditRecord &AuditLog::at(size_t i) const
{
    // Registro sentinela para acesso fora dos limites (defensivo).
    // Chamadores devem respeitar size(); esta proteção evita UB silencioso.
    // OSThread é cooperativo/single-thread; static const compartilhado é seguro.
    static const AuditRecord ZERO{};
    if (i >= num)
        return ZERO;
    // writeIdx aponta para o próximo slot de escrita, portanto
    // (writeIdx - 1) mod cap é o mais recente (i=0).
    size_t idx = (writeIdx + cap - 1 - i) % cap;
    return buf[idx];
}

size_t AuditLog::serialize(uint8_t *out, size_t outCap) const
{
    // count é u16 on-disk: ring acima de 65535 serializa só os 65535 mais recentes
    size_t emit = num > 0xFFFF ? 0xFFFF : num;

    // Calcula espaço necessário: cabeçalho + registros + CRC
    size_t need = HEADER_SIZE + emit * RECORD_SIZE + CRC_SIZE;
    if (outCap < need)
        return 0;

    size_t off = 0;

    // magic (4 bytes LE)
    out[off++] = (uint8_t)(MAGIC & 0xFF);
    out[off++] = (uint8_t)((MAGIC >> 8) & 0xFF);
    out[off++] = (uint8_t)((MAGIC >> 16) & 0xFF);
    out[off++] = (uint8_t)((MAGIC >> 24) & 0xFF);

    // count (2 bytes LE)
    out[off++] = (uint8_t)((uint16_t)emit & 0xFF);
    out[off++] = (uint8_t)((uint16_t)(emit >> 8) & 0xFF);

    // reservado (2 bytes = 0)
    out[off++] = 0;
    out[off++] = 0;

    // Serializa registros do mais antigo para o mais recente.
    // at(emit-1) é o mais antigo dos emitidos, at(0) é o mais recente.
    for (size_t i = emit; i > 0; i--) {
        const AuditRecord &r = at(i - 1);

        // tsSecs (4 bytes LE)
        out[off++] = (uint8_t)(r.tsSecs & 0xFF);
        out[off++] = (uint8_t)((r.tsSecs >> 8) & 0xFF);
        out[off++] = (uint8_t)((r.tsSecs >> 16) & 0xFF);
        out[off++] = (uint8_t)((r.tsSecs >> 24) & 0xFF);

        // origin, action, target, result (1 byte cada)
        out[off++] = r.origin;
        out[off++] = r.action;
        out[off++] = r.target;
        out[off++] = r.result;

        // node (4 bytes LE)
        out[off++] = (uint8_t)(r.node & 0xFF);
        out[off++] = (uint8_t)((r.node >> 8) & 0xFF);
        out[off++] = (uint8_t)((r.node >> 16) & 0xFF);
        out[off++] = (uint8_t)((r.node >> 24) & 0xFF);

        // seq (4 bytes LE)
        out[off++] = (uint8_t)(r.seq & 0xFF);
        out[off++] = (uint8_t)((r.seq >> 8) & 0xFF);
        out[off++] = (uint8_t)((r.seq >> 16) & 0xFF);
        out[off++] = (uint8_t)((r.seq >> 24) & 0xFF);
    }

    // CRC32 sobre tudo que veio antes (magic + count + reservado + registros)
    uint32_t crc = IrrigationProto::crc32(out, off);
    out[off++] = (uint8_t)(crc & 0xFF);
    out[off++] = (uint8_t)((crc >> 8) & 0xFF);
    out[off++] = (uint8_t)((crc >> 16) & 0xFF);
    out[off++] = (uint8_t)((crc >> 24) & 0xFF);

    return off;
}

bool AuditLog::deserialize(const uint8_t *in, size_t n)
{
    // Valida tamanho mínimo: cabeçalho + CRC (sem registros)
    if (n < HEADER_SIZE + CRC_SIZE)
        return false;

    // Valida magic
    uint32_t magic = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if (magic != MAGIC)
        return false;

    // Valida count e consistência de tamanho
    uint16_t count = (uint16_t)in[4] | ((uint16_t)in[5] << 8);
    size_t expected = HEADER_SIZE + (size_t)count * RECORD_SIZE + CRC_SIZE;
    if (n != expected)
        return false;

    // Valida CRC (cobre tudo exceto os 4 bytes finais do CRC)
    uint32_t crcStored = (uint32_t)in[n - 4] | ((uint32_t)in[n - 3] << 8) | ((uint32_t)in[n - 2] << 16) | ((uint32_t)in[n - 1] << 24);
    uint32_t crcCalc = IrrigationProto::crc32(in, n - CRC_SIZE);
    if (crcCalc != crcStored)
        return false;

    // Tudo válido: limpa e reinsere os registros.
    // Se count > cap, insere do mais antigo para o mais recente;
    // o ring descarta os mais antigos e preserva os mais recentes.
    clear();

    size_t off = HEADER_SIZE;
    for (uint16_t i = 0; i < count; i++) {
        AuditRecord r;
        r.tsSecs = (uint32_t)in[off] | ((uint32_t)in[off + 1] << 8) | ((uint32_t)in[off + 2] << 16) | ((uint32_t)in[off + 3] << 24);
        r.origin = in[off + 4];
        r.action = in[off + 5];
        r.target = in[off + 6];
        r.result = in[off + 7];
        r.node = (uint32_t)in[off + 8] | ((uint32_t)in[off + 9] << 8) | ((uint32_t)in[off + 10] << 16) | ((uint32_t)in[off + 11] << 24);
        r.seq = (uint32_t)in[off + 12] | ((uint32_t)in[off + 13] << 8) | ((uint32_t)in[off + 14] << 16) | ((uint32_t)in[off + 15] << 24);
        append(r);
        off += RECORD_SIZE;
    }

    return true;
}

void AuditLog::clear()
{
    num = 0;
    writeIdx = 0;
}
