#include "NivaloLinkCommandPollPolicy.h"

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

void testNormalPollingIsInitiallyAllowed()
{
    NivaloLinkCommandPollPolicy policy;
    expect(!policy.awaitingDataReady(), "initial policy is not awaiting a command response");
    expect(!policy.shouldDeferPoll(0U, false), "normal unready polling remains allowed");
}

void testCommandWaitsForDataReady()
{
    NivaloLinkCommandPollPolicy policy;
    policy.onCommandExchange(100U);

    expect(policy.awaitingDataReady(), "command exchange begins the DATA_READY wait");
    expect(policy.shouldDeferPoll(100U, false), "immediate unready poll is deferred");
    expect(policy.shouldDeferPoll(1099U, false), "unready poll remains deferred before fallback");
    expect(!policy.shouldDeferPoll(100U, true), "DATA_READY permits an immediate response drain");
    expect(!policy.shouldDeferPoll(1100U, false), "periodic fallback is allowed at one second");
}

void testStartingPollClearsCommandWait()
{
    NivaloLinkCommandPollPolicy policy;
    policy.onCommandExchange(100U);
    policy.onPollStarted();

    expect(!policy.awaitingDataReady(), "a response poll clears the command wait");
    expect(!policy.shouldDeferPoll(101U, false), "later normal polls are not suppressed");
}

void testLaterCommandRestartsFallbackWindow()
{
    NivaloLinkCommandPollPolicy policy;
    policy.onCommandExchange(100U);
    policy.onCommandExchange(900U);

    expect(policy.shouldDeferPoll(1100U, false), "a later command restarts the fallback window");
    expect(!policy.shouldDeferPoll(1900U, false), "restarted fallback expires after one second");
}

void testFallbackSurvivesMillisRollover()
{
    NivaloLinkCommandPollPolicy policy;
    const uint32_t exchangeAt = std::numeric_limits<uint32_t>::max() - 499U;
    policy.onCommandExchange(exchangeAt);

    expect(policy.shouldDeferPoll(std::numeric_limits<uint32_t>::max(), false),
           "wait remains active before rollover");
    expect(policy.shouldDeferPoll(499U, false), "wait remains active before wrapped deadline");
    expect(!policy.shouldDeferPoll(500U, false), "fallback becomes due at wrapped deadline");
}
} // namespace

int main()
{
    testNormalPollingIsInitiallyAllowed();
    testCommandWaitsForDataReady();
    testStartingPollClearsCommandWait();
    testLaterCommandRestartsFallbackWindow();
    testFallbackSurvivesMillisRollover();

    if (failures != 0)
    {
        std::cerr << failures << " NivaloLink command poll assertion(s) failed\n";
        return 1;
    }
    std::cout << "NivaloLink command poll policy tests passed\n";
    return 0;
}
