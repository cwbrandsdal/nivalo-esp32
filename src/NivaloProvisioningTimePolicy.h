#ifndef NIVALO_PROVISIONING_TIME_POLICY_H
#define NIVALO_PROVISIONING_TIME_POLICY_H

#include <stdint.h>

class NivaloProvisioningTimePolicy
{
public:
    static bool permitsTls(int64_t epochSeconds, uint64_t minimumValidEpochSeconds)
    {
        return epochSeconds >= 0 && static_cast<uint64_t>(epochSeconds) >= minimumValidEpochSeconds;
    }

    static bool elapsed(uint32_t now, uint32_t startedAt, uint32_t intervalMs)
    {
        return static_cast<uint32_t>(now - startedAt) >= intervalMs;
    }

    static bool retryDue(uint32_t now, uint32_t retryAt)
    {
        return static_cast<int32_t>(now - retryAt) >= 0;
    }

    static bool mayRetry(uint8_t failedAttempts, uint8_t maximumAttempts)
    {
        return failedAttempts < maximumAttempts;
    }

    static uint32_t retryDelay(uint8_t failedAttempts, uint32_t baseDelayMs, uint32_t maximumDelayMs)
    {
        uint32_t delay = baseDelayMs;
        for (uint8_t attempt = 1U; attempt < failedAttempts && delay < maximumDelayMs; ++attempt)
        {
            if (delay > maximumDelayMs / 2U) return maximumDelayMs;
            delay *= 2U;
        }
        return delay > maximumDelayMs ? maximumDelayMs : delay;
    }
};

#endif
