#ifndef NIVALO_LINK_COMMAND_POLL_POLICY_H
#define NIVALO_LINK_COMMAND_POLL_POLICY_H

#include <stdint.h>

class NivaloLinkCommandPollPolicy
{
public:
    static const uint32_t FallbackPollMs = 1000U;

    NivaloLinkCommandPollPolicy();

    void reset();
    void onCommandExchange(uint32_t now);
    bool shouldDeferPoll(uint32_t now, bool dataReady) const;
    void onPollStarted();

    bool awaitingDataReady() const;

private:
    uint32_t _commandExchangeAt;
    bool _awaitingDataReady;
};

#endif
