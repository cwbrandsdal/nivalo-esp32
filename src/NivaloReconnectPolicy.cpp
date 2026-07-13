#include "NivaloReconnectPolicy.h"

#include <stdint.h>

NivaloReconnectPolicy::NivaloReconnectPolicy()
{
    reset();
}

void NivaloReconnectPolicy::reset()
{
    _nextAttemptAt = 0U;
    _backoffMs = InitialBackoffMs;
    _attemptImmediately = true;
}

bool NivaloReconnectPolicy::isAttemptDue(uint32_t now) const
{
    return _attemptImmediately || (now - _nextAttemptAt) < 0x80000000UL;
}

void NivaloReconnectPolicy::onFailure(uint32_t now, uint32_t entropy)
{
    const uint32_t jitterWindow = _backoffMs / 4U + 1U;
    const uint32_t jitter = entropy % jitterWindow;
    _nextAttemptAt = now + _backoffMs + jitter;
    _attemptImmediately = false;

    if (_backoffMs >= MaximumBackoffMs / 2U)
    {
        _backoffMs = MaximumBackoffMs;
    }
    else
    {
        _backoffMs *= 2U;
    }
}

void NivaloReconnectPolicy::onSuccess(uint32_t now)
{
    _nextAttemptAt = now;
    _backoffMs = InitialBackoffMs;
    _attemptImmediately = true;
}

uint32_t NivaloReconnectPolicy::nextAttemptAt() const
{
    return _nextAttemptAt;
}

uint32_t NivaloReconnectPolicy::currentBackoffMs() const
{
    return _backoffMs;
}
