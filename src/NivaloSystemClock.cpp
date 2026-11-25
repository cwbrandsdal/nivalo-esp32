#include "NivaloSystemClock.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <string.h>

namespace
{
// Every completed SNTP synchronisation, with the monotonic time it happened at.
struct TimeSync
{
    int64_t utcMs;
    int64_t monotonicUs;
};
portMUX_TYPE syncLock = portMUX_INITIALIZER_UNLOCKED;
TimeSync syncQueue[4];
uint8_t syncCount = 0U;

NivaloClock systemClock;
NivaloTrustedSyncObserver observer = NULL;
void *observerContext = NULL;

void onTimeSync(struct timeval *tv)
{
    if (tv == NULL)
    {
        return;
    }
    TimeSync sync;
    sync.monotonicUs = esp_timer_get_time();
    sync.utcMs = (int64_t)tv->tv_sec * 1000LL + (int64_t)tv->tv_usec / 1000LL;
    portENTER_CRITICAL(&syncLock);
    if (syncCount == sizeof(syncQueue) / sizeof(syncQueue[0]))
    {
        memmove(&syncQueue[0], &syncQueue[1], sizeof(TimeSync) * (syncCount - 1U));
        syncCount--;
    }
    syncQueue[syncCount++] = sync;
    portEXIT_CRITICAL(&syncLock);
}

bool takeTimeSync(TimeSync *sync)
{
    bool taken = false;
    portENTER_CRITICAL(&syncLock);
    if (syncCount > 0U)
    {
        *sync = syncQueue[0];
        memmove(&syncQueue[0], &syncQueue[1], sizeof(TimeSync) * (syncCount - 1U));
        syncCount--;
        taken = true;
    }
    portEXIT_CRITICAL(&syncLock);
    return taken;
}

struct TimeSyncObserver
{
    TimeSyncObserver() { sntp_set_time_sync_notification_cb(onTimeSync); }
} timeSyncObserver;
} // namespace

NivaloClock &nivaloSystemClock()
{
    return systemClock;
}

int64_t nivaloMonotonicUs()
{
    return esp_timer_get_time();
}

void nivaloPumpSystemClock()
{
    // Runs on the loop task (provisioning and the device); the queue is the only shared state.
    TimeSync sync;
    while (takeTimeSync(&sync))
    {
        if (systemClock.onSync(sync.utcMs, sync.monotonicUs) && observer != NULL)
        {
            observer(observerContext, sync.utcMs, sync.monotonicUs);
        }
    }
}

bool nivaloSystemClockValid()
{
    nivaloPumpSystemClock();
    return systemClock.valid(esp_timer_get_time());
}

void nivaloSetTrustedSyncObserver(NivaloTrustedSyncObserver trustedObserver, void *context)
{
    observer = trustedObserver;
    observerContext = context;
}
