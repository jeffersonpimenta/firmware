#pragma once
#if !MESHTASTIC_EXCLUDE_WEBSERVER
#include <HTTPServer.hpp>

// Registra as rotas /api/irrigation/* no HTTPServer do Meshtastic (chamado uma vez
// por servidor em ContentHandler::registerHandlers). Guardado por MESHTASTIC_EXCLUDE_WEBSERVER
// para que o build nativo (que exclui o webserver) veja uma TU vazia.
void registerIrrigationHandlers(httpsserver::HTTPServer *server);
#endif
