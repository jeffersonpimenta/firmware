#pragma once
#if !MESHTASTIC_EXCLUDE_WEBSERVER

namespace httpsserver
{
class HTTPServer;
}

// Registra as rotas /api/portal/service/* (Fase 8c, §11.8, gated role==SERVICO). Chamada por ContentHandler.
void registerIrrigationServicePortalHandlers(httpsserver::HTTPServer *server);

#endif
