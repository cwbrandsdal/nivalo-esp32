#include "NivaloCommandResultCache.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    NivaloCommandResultCache cache;
    assert(cache.find("11111111-1111-1111-1111-111111111111") == NULL);
    assert(cache.remember(
        "11111111-1111-1111-1111-111111111111",
        "setPwm",
        "succeeded",
        "Function setPwm returned 50"));

    const NivaloCompletedCommand *first =
        cache.find("11111111-1111-1111-1111-111111111111");
    assert(first != NULL);
    assert(std::strcmp(first->commandName, "setPwm") == 0);
    assert(std::strcmp(first->status, "succeeded") == 0);
    assert(std::strcmp(first->message, "Function setPwm returned 50") == 0);

    assert(!cache.remember("", "setPwm", "succeeded", "bad"));
    assert(!cache.remember(
        "22222222-2222-2222-2222-222222222222",
        "",
        "succeeded",
        "bad"));

    for (int index = 0; index < 8; index++)
    {
        char id[40];
        std::snprintf(id, sizeof(id), "00000000-0000-0000-0000-%012d", index);
        assert(cache.remember(id, "turnOn", "succeeded", "Function turnOn returned 1"));
    }

    assert(cache.find("11111111-1111-1111-1111-111111111111") == NULL);
    assert(cache.find("00000000-0000-0000-0000-000000000000") != NULL);

    assert(cache.remember(
        "00000000-0000-0000-0000-000000000000",
        "differentName",
        "failed",
        "must not overwrite"));
    const NivaloCompletedCommand *unchanged =
        cache.find("00000000-0000-0000-0000-000000000000");
    assert(unchanged != NULL);
    assert(std::strcmp(unchanged->commandName, "turnOn") == 0);

    char longMessage[160];
    std::memset(longMessage, 'x', sizeof(longMessage) - 1U);
    longMessage[sizeof(longMessage) - 1U] = '\0';
    assert(cache.remember(
        "33333333-3333-3333-3333-333333333333",
        "genericHandler",
        "succeeded",
        longMessage));
    const NivaloCompletedCommand *truncated =
        cache.find("33333333-3333-3333-3333-333333333333");
    assert(truncated != NULL);
    assert(std::strlen(truncated->message) == NIVALO_COMMAND_MESSAGE_CAPACITY - 1U);
    return 0;
}
