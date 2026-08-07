#include "modules/irrigation/WeatherClient.h"
#include "configuration.h" // LOG_*, ARCH_ESP32

// ---------------------------------------------------------------------------
// Guard ESP32: todo código de rede fica aqui dentro.
// ---------------------------------------------------------------------------
#if defined(ARCH_ESP32)

#if __has_include(<WiFiClientSecure.h>)
#include <WiFiClientSecure.h>
#define WEATHER_HAS_TLS 1
#endif

#include <math.h>

// ---------------------------------------------------------------------------
// Constante de host
// ---------------------------------------------------------------------------
static const char *OM_HOST = "api.open-meteo.com";

// ---------------------------------------------------------------------------
// Helpers de parse JSON mínimo (sem ArduinoJson)
// ---------------------------------------------------------------------------

// Localiza a primeira ocorrência de `key` no corpo e devolve ponteiro para o
// caractere imediatamente após a chave (logo após o ':' ou '[' que abre o
// valor). Retorna nullptr se não encontrado.
static const char *findKey(const char *body, const char *key)
{
    const char *p = strstr(body, key);
    if (!p) return nullptr;
    p += strlen(key);
    // avança até o primeiro dígito, '-', '"' ou '[' (início do valor)
    while (*p && *p != ':' && *p != '[' && *p != '"' && *p != '-' && (*p < '0' || *p > '9')) p++;
    return p;
}

// Lê um valor escalar numérico após a chave `key` (ex: `"temperature_2m":21.3`).
// Devolve true e preenche *val. Falha silenciosamente (false) se chave ausente.
static bool readScalar(const char *body, const char *key, double *val)
{
    const char *p = findKey(body, key);
    if (!p) return false;
    // pula ':' e espaços
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (!*p) return false;
    char *end = nullptr;
    double v = strtod(p, &end);
    if (end == p) return false;
    *val = v;
    return true;
}

// Retorna ponteiro para o início do array numérico/string após a chave `key`
// (o caractere após o '[').  Ex: `"precipitation":[0,1.2,...]` → aponta para '0'.
static const char *findArray(const char *body, const char *key)
{
    const char *p = strstr(body, key);
    if (!p) return nullptr;
    p = strchr(p + strlen(key), '[');
    if (!p) return nullptr;
    return p + 1; // logo após '['
}

// Lê até `maxCount` elementos numéricos de um array JSON (avança por vírgulas).
// Preenche `out[0..n-1]` e devolve o número de elementos lidos.
// Para em ']' ou fim de string.
static int readNumArray(const char *arr, double *out, int maxCount)
{
    if (!arr) return 0;
    const char *p = arr;
    int n = 0;
    while (n < maxCount && *p && *p != ']') {
        // pula espaços, vírgulas, newlines
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
        if (!*p || *p == ']') break;
        char *end = nullptr;
        double v = strtod(p, &end);
        if (end == p) break; // caractere não-numérico inesperado
        out[n++] = v;
        p = end;
    }
    return n;
}

// Lê até `maxCount` strings ISO de um array JSON de strings (ex: "time":["2025-...","..."]).
// Cada string é copiada truncada em buf[n][bufLen].  Devolve número de itens lidos.
static int readStrArray(const char *arr, char (*out)[20], int maxCount)
{
    if (!arr) return 0;
    const char *p = arr;
    int n = 0;
    while (n < maxCount && *p && *p != ']') {
        // busca a próxima aspa de abertura
        p = strchr(p, '"');
        if (!p || !*(p + 1)) break;
        p++; // pula '"' de abertura
        const char *end = strchr(p, '"');
        if (!end) break;
        int len = (int)(end - p);
        if (len >= 20) len = 19;
        memcpy(out[n], p, len);
        out[n][len] = '\0';
        n++;
        p = end + 1;
    }
    return n;
}

// Extrai o valor string de uma chave escalar (ex: `"time":"2025-08-07T10:00"`).
// Copia até bufLen-1 chars em `buf`.  Retorna true se encontrou.
static bool readStrScalar(const char *body, const char *key, char *buf, int bufLen)
{
    const char *p = strstr(body, key);
    if (!p) return false;
    p = strchr(p + strlen(key), '"');
    if (!p) return false;
    p++; // pula '"' de abertura
    const char *end = strchr(p, '"');
    if (!end) return false;
    int len = (int)(end - p);
    if (len >= bufLen) len = bufLen - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Tamanho máximo dos arrays horários e diários
// past_hours=24 + forecast_hours=13 → ~37 pontos horários; daily ~8 dias
// ---------------------------------------------------------------------------
static constexpr int MAX_HOURLY = 48;
static constexpr int MAX_DAILY  = 10;

// ---------------------------------------------------------------------------
// parseWeather — espelha parseWeather() do mockup (Irrigacao Mobile.dc.html:2386-2413)
// ---------------------------------------------------------------------------
static bool parseWeather(const String &bodyStr, WeatherCache &out)
{
    const char *body = bodyStr.c_str();

    // -----------------------------------------------------------------------
    // 1. Localizar o bloco "current":{...} e extrair current.time
    //    A chave "current":{ pode ter campos em qualquer ordem; buscamos
    //    "time" dentro da sub-string delimitada pelo bloco current.
    // -----------------------------------------------------------------------
    char currentTime[20] = {0}; // ex: "2025-08-07T10:00"

    {
        // Encontrar início do objeto "current"
        const char *curBlock = strstr(body, "\"current\":");
        if (!curBlock) return false;
        curBlock = strchr(curBlock, '{');
        if (!curBlock) return false;

        // Encontrar fechamento do bloco (busca a próxima '}' de mesmo nível)
        // Para o formato Open-Meteo com poucos campos, a primeira '}' que fecha é suficiente.
        const char *curEnd = strchr(curBlock + 1, '}');
        if (!curEnd) return false;

        // Copiar o sub-bloco para buscar "time" dentro dele
        int blockLen = (int)(curEnd - curBlock + 1);
        if (blockLen > 512) blockLen = 512;
        char curBuf[512];
        memcpy(curBuf, curBlock, blockLen);
        curBuf[blockLen] = '\0';

        if (!readStrScalar(curBuf, "\"time\":", currentTime, sizeof(currentTime))) return false;
    }

    // -----------------------------------------------------------------------
    // 2. Ler array hourly.time e encontrar i = indexOf(currentTime)
    //    (fallback: i = último índice, como no JS)
    // -----------------------------------------------------------------------
    char hourlyTimes[MAX_HOURLY][20];
    int hTimeCount = 0;
    {
        // Localizar o bloco "hourly":{...} para restringir a busca
        const char *hrBlock = strstr(body, "\"hourly\":");
        if (!hrBlock) return false;
        hrBlock = strchr(hrBlock, '{');
        if (!hrBlock) return false;
        // Encontrar o array "time" dentro do bloco hourly
        const char *timeArr = strstr(hrBlock, "\"time\":");
        if (!timeArr) return false;
        const char *arrStart = strchr(timeArr, '[');
        if (!arrStart) return false;
        hTimeCount = readStrArray(arrStart + 1, hourlyTimes, MAX_HOURLY);
    }
    if (hTimeCount == 0) return false;

    // Encontrar índice i (hora corrente no array horário)
    // currentTime é "YYYY-MM-DDTHH:MM"; hourlyTimes tem o mesmo formato
    int i = -1;
    for (int k = 0; k < hTimeCount; k++) {
        if (strcmp(hourlyTimes[k], currentTime) == 0) { i = k; break; }
    }
    if (i < 0) i = hTimeCount - 1; // fallback: último, como no JS

    // -----------------------------------------------------------------------
    // 3. Ler arrays hourly (precipitação, probabilidade, umidade do solo)
    //    Limitados ao bloco "hourly":{...}; como é texto plano, buscamos as
    //    chaves dentro do bloco para evitar colisão com "daily".
    // -----------------------------------------------------------------------
    double hourlyPrecip[MAX_HOURLY] = {0};
    double hourlyProb[MAX_HOURLY]   = {0};
    double hourlySoil[MAX_HOURLY]   = {0};
    int hPrecipCount = 0, hProbCount = 0, hSoilCount = 0;

    {
        const char *hrBlock = strstr(body, "\"hourly\":");
        if (!hrBlock) return false;
        hrBlock = strchr(hrBlock, '{');
        if (!hrBlock) return false;
        // Fecha o bloco hourly ao encontrar o próximo objeto de nível raiz
        // (busca "daily":{ para delimitar — o hourly vem antes no response Open-Meteo)
        const char *hrEnd = strstr(hrBlock, "\"daily\":");
        // Se não encontrar "daily", usa o fim do body
        if (!hrEnd) hrEnd = body + strlen(body);
        int len = (int)(hrEnd - hrBlock);
        if (len > 8192) len = 8192; // teto de segurança

        // Sub-buffer para parsear apenas hourly
        char *hrBuf = (char *)malloc(len + 1);
        if (!hrBuf) return false;
        memcpy(hrBuf, hrBlock, len);
        hrBuf[len] = '\0';

        const char *p;
        p = findArray(hrBuf, "\"precipitation\":");
        hPrecipCount = readNumArray(p, hourlyPrecip, MAX_HOURLY);
        p = findArray(hrBuf, "\"precipitation_probability\":");
        hProbCount = readNumArray(p, hourlyProb, MAX_HOURLY);
        p = findArray(hrBuf, "\"soil_moisture_0_to_1cm\":");
        hSoilCount = readNumArray(p, hourlySoil, MAX_HOURLY);

        free(hrBuf);
    }

    if (hPrecipCount == 0) return false; // campo mandatório

    // -----------------------------------------------------------------------
    // 4. Calcular métricas horárias (espelha sum() e max() do JS)
    // -----------------------------------------------------------------------

    // chuvaPrevista12h = Σ precip[i+1 .. i+12]
    double chuvaPrevista12h = 0.0;
    for (int k = i + 1; k <= i + 12 && k < hPrecipCount; k++) {
        if (hourlyPrecip[k] > 0) chuvaPrevista12h += hourlyPrecip[k];
    }

    // probChuva = max(prob[i .. i+12])
    double probMax = 0.0;
    if (hProbCount > 0) {
        int from = (i >= 0) ? i : 0;
        for (int k = from; k <= from + 12 && k < hProbCount; k++) {
            if (hourlyProb[k] > probMax) probMax = hourlyProb[k];
        }
    }

    // chuvaAcum24h = Σ precip[i-23 .. i]
    double chuvaAcum24h = 0.0;
    {
        int from = i - 23;
        if (from < 0) from = 0;
        for (int k = from; k <= i && k < hPrecipCount; k++) {
            if (hourlyPrecip[k] > 0) chuvaAcum24h += hourlyPrecip[k];
        }
    }

    // umidadeSolo = soil[i] * 100 (se disponível)
    double umidadeSolo = -1.0;
    if (hSoilCount > 0 && i < hSoilCount) {
        umidadeSolo = hourlySoil[i] * 100.0;
    }

    // -----------------------------------------------------------------------
    // 5. Ler campos do objeto "current"
    // -----------------------------------------------------------------------
    double tempAtual = 0.0, umidadeRel = 0.0;
    {
        const char *curBlock = strstr(body, "\"current\":");
        if (!curBlock) return false;
        curBlock = strchr(curBlock, '{');
        if (!curBlock) return false;
        const char *curEnd = strchr(curBlock + 1, '}');
        if (!curEnd) return false;
        int len = (int)(curEnd - curBlock + 1);
        if (len > 512) len = 512;
        char curBuf[512];
        memcpy(curBuf, curBlock, len);
        curBuf[len] = '\0';

        double v = 0.0;
        if (!readScalar(curBuf, "\"temperature_2m\":", &v)) return false;
        tempAtual = v;
        if (!readScalar(curBuf, "\"relative_humidity_2m\":", &v)) return false;
        umidadeRel = v;
    }

    // -----------------------------------------------------------------------
    // 6. Ler campos do array "daily" e determinar dayIdx
    //    dayIdx = índice de todayStr (primeiros 10 chars de currentTime) em daily.time
    // -----------------------------------------------------------------------
    char todayStr[11] = {0};
    memcpy(todayStr, currentTime, 10); // "YYYY-MM-DD"
    todayStr[10] = '\0';

    double dailyTempMin[MAX_DAILY]   = {0};
    double dailyTempMax[MAX_DAILY]   = {0};
    double dailyVento[MAX_DAILY]     = {0};
    double dailyEt0[MAX_DAILY]       = {0};
    char   dailyTimes[MAX_DAILY][20];
    int dTimeCount = 0, dMinCount = 0, dMaxCount = 0, dVentoCount = 0, dEt0Count = 0;

    {
        const char *dlBlock = strstr(body, "\"daily\":");
        if (!dlBlock) return false;
        dlBlock = strchr(dlBlock, '{');
        if (!dlBlock) return false;

        // O bloco daily vai até o fechamento '}' do objeto raiz;
        // como é o último bloco, buscamos '}' contando nível de aninhamento.
        // Simplificação: buscamos o fim do body como limite superior.
        const char *dlEnd = body + strlen(body);
        int len = (int)(dlEnd - dlBlock);
        if (len > 4096) len = 4096;

        char *dlBuf = (char *)malloc(len + 1);
        if (!dlBuf) return false;
        memcpy(dlBuf, dlBlock, len);
        dlBuf[len] = '\0';

        // Ler daily.time (strings)
        const char *timeArr = strstr(dlBuf, "\"time\":");
        if (timeArr) {
            const char *arrStart = strchr(timeArr, '[');
            if (arrStart) dTimeCount = readStrArray(arrStart + 1, dailyTimes, MAX_DAILY);
        }

        const char *p;
        p = findArray(dlBuf, "\"temperature_2m_min\":");
        dMinCount = readNumArray(p, dailyTempMin, MAX_DAILY);
        p = findArray(dlBuf, "\"temperature_2m_max\":");
        dMaxCount = readNumArray(p, dailyTempMax, MAX_DAILY);
        p = findArray(dlBuf, "\"wind_gusts_10m_max\":");
        dVentoCount = readNumArray(p, dailyVento, MAX_DAILY);
        p = findArray(dlBuf, "\"et0_fao_evapotranspiration\":");
        dEt0Count = readNumArray(p, dailyEt0, MAX_DAILY);

        free(dlBuf);
    }

    // dayIdx: posição de todayStr em daily.time (fallback: último)
    int dayIdx = -1;
    for (int k = 0; k < dTimeCount; k++) {
        if (strncmp(dailyTimes[k], todayStr, 10) == 0) { dayIdx = k; break; }
    }
    if (dayIdx < 0) dayIdx = (dTimeCount > 0) ? dTimeCount - 1 : 0;

    // -----------------------------------------------------------------------
    // 7. Verificar dados mínimos obrigatórios para a decisão de supressão
    // -----------------------------------------------------------------------
    if (dMinCount == 0 || dMaxCount == 0) return false;
    if (dayIdx >= dMinCount || dayIdx >= dMaxCount) return false;

    // -----------------------------------------------------------------------
    // 8. Preencher WeatherCache (unidades centi)
    // -----------------------------------------------------------------------
    out = WeatherCache{}; // limpa

    // Decisão (campos mandatórios):
    out.chuvaPrevista12hCenti = (uint16_t)((int)roundf((float)(chuvaPrevista12h * 100.0)));
    out.probChuvaPct          = (uint8_t)((int)probMax); // já em %

    // Exibição:
    out.chuvaAcum24hCenti = (uint16_t)((int)roundf((float)(chuvaAcum24h * 100.0)));
    out.tempAtualCenti    = (int16_t)((int)roundf((float)(tempAtual * 100.0)));
    out.umidadeRelPct     = (uint8_t)((int)umidadeRel);
    out.umidadeSoloPct    = (umidadeSolo >= 0) ? (uint8_t)((int)roundf((float)umidadeSolo)) : 0;
    out.tempMinCenti      = (int16_t)((int)roundf((float)(dailyTempMin[dayIdx] * 100.0)));
    out.tempMaxCenti      = (int16_t)((int)roundf((float)(dailyTempMax[dayIdx] * 100.0)));
    out.ventoRajadaCenti  = (dVentoCount > dayIdx)
                                ? (uint16_t)((int)roundf((float)(dailyVento[dayIdx] * 100.0)))
                                : 0;
    out.et0Centi          = (dEt0Count > dayIdx)
                                ? (uint16_t)((int)roundf((float)(dailyEt0[dayIdx] * 100.0)))
                                : 0;

    return true;
}

// ---------------------------------------------------------------------------
// httpsGet — TLS leve (sem verificação de cert)
// ---------------------------------------------------------------------------
#if defined(WEATHER_HAS_TLS)
static bool httpsGet(const char *host, const String &path, String &body)
{
    WiFiClientSecure client;
    client.setInsecure();   // TLS mais leve: sem verificação de cert
    client.setTimeout(6);   // segundos

    if (!client.connect(host, 443)) {
        LOG_WARN("Weather", "connect fail");
        return false;
    }

    client.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
                 "\r\nConnection: close\r\n\r\n");

    // Pular cabeçalhos HTTP (lê até linha em branco)
    uint32_t t0 = millis();
    bool headersDone = false;
    body = "";
    while (client.connected() && millis() - t0 < 8000) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (!headersDone) {
                // linha em branco (só \r\n) indica fim dos headers
                if (line == "\r" || line.length() == 0) {
                    headersDone = true;
                }
            } else {
                body += line + "\n";
            }
        }
    }
    client.stop();
    return body.length() > 0 && headersDone;
}
#else
// WiFiClientSecure não disponível: no-op
static bool httpsGet(const char * /*host*/, const String & /*path*/, String & /*body*/)
{
    return false;
}
#endif // WEATHER_HAS_TLS

#endif // ARCH_ESP32

// ---------------------------------------------------------------------------
// pollNow — monta URL e executa fetch+parse
// ---------------------------------------------------------------------------
bool WeatherClient::pollNow(const WeatherConfig &cfg, uint32_t nowEpoch, WeatherCache &out)
{
#if defined(ARCH_ESP32)
    // Coordenadas com ~5 casas decimais
    char latBuf[16], lonBuf[16];
    snprintf(latBuf, sizeof(latBuf), "%.5f", (double)cfg.latE7 / 1e7);
    snprintf(lonBuf, sizeof(lonBuf), "%.5f", (double)cfg.lonE7 / 1e7);

    String path = String("/v1/forecast?latitude=") + latBuf +
                  "&longitude=" + lonBuf +
                  "&current=temperature_2m,relative_humidity_2m,precipitation,wind_gusts_10m" +
                  "&hourly=precipitation,precipitation_probability,soil_moisture_0_to_1cm" +
                  "&daily=temperature_2m_max,temperature_2m_min,wind_gusts_10m_max,et0_fao_evapotranspiration" +
                  "&past_hours=24&forecast_hours=13&timezone=auto";

    String body;
    if (!httpsGet(OM_HOST, path, body)) {
        LOG_WARN("Weather", "httpsGet falhou");
        out.isMock = true;
        return false;
    }

    WeatherCache parsed;
    if (!parseWeather(body, parsed)) {
        LOG_WARN("Weather", "parseWeather falhou");
        out.isMock = true;
        return false;
    }

    parsed.valid      = true;
    parsed.isMock     = false;
    parsed.fetchEpoch = nowEpoch;
    out = parsed;
    return true;
#else
    (void)cfg; (void)nowEpoch; (void)out;
    return false;
#endif // ARCH_ESP32
}

// ---------------------------------------------------------------------------
// tick — agendamento 2×/dia + seed pós-boot
// ---------------------------------------------------------------------------
bool WeatherClient::tick(const WeatherConfig &cfg, uint32_t nowLocalSecs, bool staUp, WeatherCache &out)
{
    if (!cfg.enabled || !staUp || nowLocalSecs == 0) return false;

#if defined(ARCH_ESP32)
    uint32_t hour = (nowLocalSecs / 3600) % 24;
    uint32_t day  = nowLocalSecs / 86400;
    uint32_t key  = day * 100 + hour;

    // Seed pós-boot: primeiro tick com WiFi up e hora válida → poll imediato.
    if (!seededAfterBoot) {
        seededAfterBoot = true;
        lastPollKey = key; // marca para não re-disparar na mesma hora
        return pollNow(cfg, nowLocalSecs, out);
    }

    // Poll nas horas configuradas, uma vez por ocorrência (chave muda a cada hora).
    bool scheduled = (hour == (uint32_t)cfg.pollHourA || hour == (uint32_t)cfg.pollHourB);
    if (scheduled && key != lastPollKey) {
        lastPollKey = key;
        return pollNow(cfg, nowLocalSecs, out);
    }
    return false;
#else
    (void)nowLocalSecs; (void)out;
    return false;
#endif // ARCH_ESP32
}
