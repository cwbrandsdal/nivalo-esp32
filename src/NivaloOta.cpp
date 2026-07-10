#include "NivaloOta.h"

void NivaloOtaManager::releasePins()
{
#if NIVALO_HAS_SECONDARY_MCU
    pinMode(_pins.swdio, INPUT); pinMode(_pins.swclk, INPUT); pinMode(_pins.swreset, INPUT);
#endif
}

NivaloOtaSession::NivaloOtaSession(NivaloLinkSpiTransport &link, NivaloOtaManager &ota, HTTPClient &http)
    : _link(link), _ota(ota), _http(http)
{
    _link.setPaused(true); _ota.releasePins();
}

NivaloOtaSession::~NivaloOtaSession()
{
    closeDownload();
    _link.setPaused(false);
    _ota.releasePins();
}

void NivaloOtaSession::closeDownload()
{
    if (!_downloadClosed)
    {
        _http.end();
        _downloadClosed = true;
    }
}
