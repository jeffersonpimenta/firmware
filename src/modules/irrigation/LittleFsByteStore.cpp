#include "modules/irrigation/LittleFsByteStore.h"
#include "FSCommon.h"
#include "DebugConfiguration.h"
#include <string.h>

LittleFsByteStore::LittleFsByteStore(const char *path, size_t fixedSize)
    : path_(path), fixedSize_(fixedSize)
{
}

bool LittleFsByteStore::ensureAllocated()
{
#ifdef FSCom
    // Verifica tamanho atual.
    {
        auto f = FSCom.open(path_, FILE_O_READ);
        if (f) {
            size_t sz = f.size();
            f.close();
            if (sz >= fixedSize_)
                return true; // já alocado
        }
    }

    // Cria ou reescreve o arquivo com zeros até fixedSize_.
    // Usa FILE_O_WRITE ("w") — cria se não existe, trunca se existe mas curto.
    auto f = FSCom.open(path_, FILE_O_WRITE);
    if (!f) {
        LOG_ERROR("LittleFsByteStore: não foi possível criar %s", path_);
        return false;
    }

    // Escreve em blocos de 256 bytes para não explodir a stack.
    static constexpr size_t CHUNK = 256;
    uint8_t zeros[CHUNK];
    memset(zeros, 0, sizeof(zeros));

    size_t remaining = fixedSize_;
    bool ok = true;
    while (remaining > 0) {
        size_t toWrite = remaining < CHUNK ? remaining : CHUNK;
        if (f.write(zeros, toWrite) != toWrite) {
            ok = false;
            break;
        }
        remaining -= toWrite;
    }
    f.close();

    if (!ok) {
        LOG_ERROR("LittleFsByteStore: erro ao preencher %s", path_);
        FSCom.remove(path_);
    }
    return ok;
#else
    return false;
#endif
}

bool LittleFsByteStore::read(size_t off, void *dst, size_t n) const
{
#ifdef FSCom
    if (off + n > fixedSize_)
        return false;
    // "r+" — leitura e escrita sem truncar; exige que o arquivo já exista (garantido por ensureAllocated).
    auto f = FSCom.open(path_, "r+");
    if (!f)
        return false;
    if (!f.seek((uint32_t)off)) {
        f.close();
        return false;
    }
    size_t got = f.read(static_cast<uint8_t *>(dst), n);
    f.close();
    return got == n;
#else
    (void)off; (void)dst; (void)n;
    return false;
#endif
}

bool LittleFsByteStore::write(size_t off, const void *src, size_t n)
{
#ifdef FSCom
    if (off + n > fixedSize_)
        return false;
    auto f = FSCom.open(path_, "r+");
    if (!f)
        return false;
    if (!f.seek((uint32_t)off)) {
        f.close();
        return false;
    }
    size_t written = f.write(static_cast<const uint8_t *>(src), n);
    f.close();
    return written == n;
#else
    (void)off; (void)src; (void)n;
    return false;
#endif
}
