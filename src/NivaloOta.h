#ifndef NIVALO_OTA_H
#define NIVALO_OTA_H

#include <Arduino.h>
#include <HTTPClient.h>
#include "NivaloLinkSpiTransport.h"

#if NIVALO_HAS_SECONDARY_MCU
#include <Adafruit_DAP.h>

using NivaloStm32Dap = Adafruit_DAP_STM32;
#endif

struct NivaloOtaPinMap
{
    uint8_t swdio = 12;
    uint8_t swclk = 14;
    uint8_t swreset = 4;
};

class NivaloOtaManager
{
public:
    void configure(const NivaloOtaPinMap &pins) { _pins = pins; }
    void releasePins();
#if NIVALO_HAS_SECONDARY_MCU
    NivaloStm32Dap &dap() { return _dap; }
#endif
    const NivaloOtaPinMap &pins() const { return _pins; }
    uint8_t *buffer() { return _buffer; }
    size_t bufferSize() const { return sizeof(_buffer); }
private:
    NivaloOtaPinMap _pins;
    uint8_t _buffer[16U * 1024U] __attribute__((aligned(4)));
#if NIVALO_HAS_SECONDARY_MCU
    NivaloStm32Dap _dap;
#endif
};

class NivaloOtaSession
{
public:
    NivaloOtaSession(NivaloLinkSpiTransport &link, NivaloOtaManager &ota, HTTPClient &http);
    ~NivaloOtaSession();
    void closeDownload();
private:
    NivaloLinkSpiTransport &_link;
    NivaloOtaManager &_ota;
    HTTPClient &_http;
    bool _downloadClosed = false;
};

#endif
