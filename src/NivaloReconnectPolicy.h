#ifndef NIVALO_RECONNECT_POLICY_H
#define NIVALO_RECONNECT_POLICY_H

#include <stdint.h>

class NivaloReconnectPolicy
{
public:
    static const uint32_t InitialBackoffMs = 1000U;
    static const uint32_t MaximumBackoffMs = 60000U;

    NivaloReconnectPolicy();

    void reset();
    bool isAttemptDue(uint32_t now) const;
    void onFailure(uint32_t now, uint32_t entropy);
    void onSuccess(uint32_t now);

    uint32_t nextAttemptAt() const;
    uint32_t currentBackoffMs() const;

private:
    uint32_t _nextAttemptAt;
    uint32_t _backoffMs;
    bool _attemptImmediately;
};

#endif
