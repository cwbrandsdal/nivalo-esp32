#include "NivaloLinkCommandPollPolicy.h"

NivaloLinkCommandPollPolicy::NivaloLinkCommandPollPolicy()
{
    reset();
}

void NivaloLinkCommandPollPolicy::reset()
{
    _commandExchangeAt = 0U;
    _awaitingDataReady = false;
}

void NivaloLinkCommandPollPolicy::onCommandExchange(uint32_t now)
{
    _commandExchangeAt = now;
    _awaitingDataReady = true;
}

bool NivaloLinkCommandPollPolicy::shouldDeferPoll(uint32_t now, bool dataReady) const
{
    if (!_awaitingDataReady || dataReady)
    {
        return false;
    }
    return (uint32_t)(now - _commandExchangeAt) < FallbackPollMs;
}

void NivaloLinkCommandPollPolicy::onPollStarted()
{
    _awaitingDataReady = false;
}

bool NivaloLinkCommandPollPolicy::awaitingDataReady() const
{
    return _awaitingDataReady;
}
