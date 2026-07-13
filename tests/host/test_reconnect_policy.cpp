#include "NivaloReconnectPolicy.h"

#include <stdint.h>
#include <iostream>
#include <limits>

namespace
{
int failures = 0;

void expect(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        failures++;
    }
}

void testInitialAttemptIsImmediateAtAnyUptime()
{
    NivaloReconnectPolicy policy;
    expect(policy.currentBackoffMs() == 1000U, "initial backoff is one second");
    expect(policy.isAttemptDue(0U), "initial attempt is due at boot");
    expect(policy.isAttemptDue(std::numeric_limits<uint32_t>::max()),
           "initial attempt is due after long uptime");
}

void testRetryTimingAndJitterBounds()
{
    NivaloReconnectPolicy policy;
    policy.onFailure(100U, 0U);
    expect(policy.nextAttemptAt() == 1100U, "zero entropy uses the base delay");
    expect(!policy.isAttemptDue(1099U), "retry is suppressed before its deadline");
    expect(policy.isAttemptDue(1100U), "retry is due at its deadline");
    expect(policy.currentBackoffMs() == 2000U, "backoff doubles after failure");

    policy.reset();
    policy.onFailure(100U, 250U);
    expect(policy.nextAttemptAt() == 1350U, "jitter may reach exactly 25 percent");

    policy.reset();
    policy.onFailure(100U, std::numeric_limits<uint32_t>::max());
    const uint32_t delay = policy.nextAttemptAt() - 100U;
    expect(delay >= 1000U && delay <= 1250U, "all entropy stays inside jitter bounds");
}

void testBackoffCapsAtSixtySeconds()
{
    NivaloReconnectPolicy policy;
    uint32_t now = 0U;
    const uint32_t expectedDelays[] = {1000U, 2000U, 4000U, 8000U, 16000U, 32000U, 60000U, 60000U};

    for (uint32_t expectedDelay : expectedDelays)
    {
        policy.onFailure(now, 0U);
        expect(policy.nextAttemptAt() - now == expectedDelay, "retry uses the expected capped delay");
        now = policy.nextAttemptAt();
    }
    expect(policy.currentBackoffMs() == 60000U, "backoff remains capped");
}

void testSuccessResetsBackoff()
{
    NivaloReconnectPolicy policy;
    policy.onFailure(0U, 0U);
    policy.onFailure(1000U, 0U);
    policy.onSuccess(3000U);

    expect(policy.currentBackoffMs() == 1000U, "success resets the base delay");
    expect(policy.isAttemptDue(3000U), "a later disconnect can reconnect immediately");

    policy.onFailure(3000U, 0U);
    expect(policy.nextAttemptAt() == 4000U, "failure after success starts at one second");
}

void testDeadlineSurvivesMillisRollover()
{
    NivaloReconnectPolicy policy;
    const uint32_t now = std::numeric_limits<uint32_t>::max() - 499U;
    policy.onFailure(now, 0U);

    expect(policy.nextAttemptAt() == 500U, "deadline wraps modulo 32 bits");
    expect(!policy.isAttemptDue(std::numeric_limits<uint32_t>::max()),
           "retry remains suppressed before rollover");
    expect(!policy.isAttemptDue(499U), "retry remains suppressed after rollover before deadline");
    expect(policy.isAttemptDue(500U), "retry becomes due after rollover at deadline");
}
} // namespace

int main()
{
    testInitialAttemptIsImmediateAtAnyUptime();
    testRetryTimingAndJitterBounds();
    testBackoffCapsAtSixtySeconds();
    testSuccessResetsBackoff();
    testDeadlineSurvivesMillisRollover();

    if (failures != 0)
    {
        std::cerr << failures << " reconnect policy assertion(s) failed\n";
        return 1;
    }
    std::cout << "reconnect policy tests passed\n";
    return 0;
}
