#ifndef NIVALO_LINK_MANAGER_H
#define NIVALO_LINK_MANAGER_H

#include "NivaloLinkSpiTransport.h"

class NivaloLinkManager
{
public:
    bool begin(const NivaloLinkPinMap &pins)
    {
        if (!_transport.begin(pins))
        {
            return false;
        }
        helloPending = true;
        lastHello = 0U;
        lastPoll = 0U;
        lastHeartbeat = 0U;
        lastInvalidFrameReport = 0U;
        backoffUntil = 0U;
        invalidFrameCount = 0U;
        return true;
    }
    NivaloLinkSpiTransport &transport() { return _transport; }

    bool helloPending = true;
    unsigned long lastHello = 0U;
    unsigned long lastPoll = 0U;
    unsigned long lastHeartbeat = 0U;
    unsigned long lastInvalidFrameReport = 0U;
    unsigned long backoffUntil = 0U;
    uint32_t invalidFrameCount = 0U;

private:
    NivaloLinkSpiTransport _transport;
};

#endif
