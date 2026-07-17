#pragma once
#if !MESHTASTIC_EXCLUDE_WEBSERVER

namespace httpsserver
{
class HTTPServer;
}

// Registra as rotas /api/portal/* no HTTPServer do Meshtastic (Fase 5b). Chamada por ContentHandler.
void registerIrrigationPortalHandlers(httpsserver::HTTPServer *server);

#endif
