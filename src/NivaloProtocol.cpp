#include "NivaloProtocol.h"

#include <esp_system.h>
#include <time.h>

void NivaloProtocol::beginClock(const char *primaryNtp, const char *secondaryNtp)
{
    configTime(0, 0, primaryNtp == NULL ? "pool.ntp.org" : primaryNtp,
               secondaryNtp == NULL ? "time.cloudflare.com" : secondaryNtp);
}

bool NivaloProtocol::timeValid() const
{
    return time(NULL) >= 1577836800; // 2020-01-01 UTC
}

String NivaloProtocol::timestamp() const
{
    if (!timeValid())
    {
        return "";
    }
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char output[32];
    strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return String(output);
}

String NivaloProtocol::messageId() const
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    uint32_t c = esp_random();
    uint32_t d = esp_random();
    char output[37];
    snprintf(output, sizeof(output), "%08lx-%04lx-%04lx-%04lx-%04lx%08lx",
             (unsigned long)a, (unsigned long)((b >> 16) & 0xFFFFUL),
             (unsigned long)((b & 0x0FFFUL) | 0x4000UL),
             (unsigned long)(((c >> 16) & 0x3FFFUL) | 0x8000UL),
             (unsigned long)(c & 0xFFFFUL), (unsigned long)d);
    return String(output);
}
