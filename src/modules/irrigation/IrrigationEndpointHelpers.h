#pragma once
#if !MESHTASTIC_EXCLUDE_WEBSERVER

// Helpers HTTP compartilhados pelos TUs de endpoint (IrrigationWebEndpoints,
// IrrigationPortalEndpoints, ServicePortalEndpoints). Antes duplicados idênticos
// nos três .cpp — uma correção de escaping/erro precisava ser aplicada 3×.

#include "modules/irrigation/IrrigationWebApi.h" // JsonWriter, ParseResult

#include <Arduino.h> // byte, size_t

// Mesma sequência de include do esp32_https_server usada por ContentHandler.cpp:
// "#undef str" antes dos headers do servidor (workaround gcc bug 57824).
#undef str
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>

namespace IrrigationWeb
{

inline void sendJson(httpsserver::HTTPResponse *res, const char *body, int status = 200)
{
    res->setStatusCode(status);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->print(body);
}

// Responde 400 com {"errors":[...]} a partir de um ParseResult que falhou.
inline void sendParseErrors(httpsserver::HTTPResponse *res, const ParseResult &pr)
{
    char err[256];
    JsonWriter w(err, sizeof(err));
    w.beginObject();
    w.key("errors");
    w.beginArray();
    for (uint8_t i = 0; i < pr.errorCount; i++)
        w.str(pr.errors[i].msg);
    w.endArray();
    w.endObject();
    if (w.done() == 0) {
        // fallback caso o buffer estoure
        sendJson(res, "{\"errors\":[\"erro\"]}", 400);
        return;
    }
    sendJson(res, err, 400);
}

// Lê o corpo da requisição. esp32_https_server expõe HTTPRequest::readBytes(byte*, size_t)
// que devolve o número de bytes lidos (mesmo idioma de handleAPIv1ToRadio em ContentHandler.cpp).
inline size_t readBody(httpsserver::HTTPRequest *req, char *buf, size_t cap)
{
    if (cap == 0)
        return 0;
    size_t n = req->readBytes(reinterpret_cast<byte *>(buf), cap - 1);
    if (n >= cap)
        n = cap - 1;
    buf[n] = '\0';
    return n;
}

} // namespace IrrigationWeb

#endif
