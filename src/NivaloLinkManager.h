#ifndef NIVALO_LINK_MANAGER_H
#define NIVALO_LINK_MANAGER_H

#include "NivaloLinkSpiTransport.h"

class NivaloLinkManager
{
public:
    bool begin(const NivaloLinkPinMap &pins) { return _transport.begin(pins); }
    NivaloLinkSpiTransport &transport() { return _transport; }

    unsigned long lastPoll = 0U;
    unsigned long lastHeartbeat = 0U;
    unsigned long lastInvalidFrameReport = 0U;
    unsigned long backoffUntil = 0U;
    uint32_t invalidFrameCount = 0U;

private:
    NivaloLinkSpiTransport _transport;
};

#endif
