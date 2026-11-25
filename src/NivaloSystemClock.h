#ifndef NIVALO_SYSTEM_CLOCK_H
#define NIVALO_SYSTEM_CLOCK_H

// The device clock of specs/cloud-mqtt-v1.1.md section 9, one per process. Every SNTP
// synchronisation since boot is observed (the observer is installed before setup()) and judged
// once, so provisioning (claim HTTPS, MQTT identity check), the MQTT session and OTA downloads all
// open TLS under the same validity. A retained wall clock from before a warm restart never counts.

#include <stdint.h>

#include "NivaloClock.h"

typedef void (*NivaloTrustedSyncObserver)(void *context, int64_t utcMs, int64_t monotonicUs);

// The shared clock. Fold pending synchronisations with nivaloPumpSystemClock() before reading it.
NivaloClock &nivaloSystemClock();
// Folds queued synchronisations into the clock; each trusted one is passed to the observer.
void nivaloPumpSystemClock();
// Pumps, then answers validity now.
bool nivaloSystemClockValid();
int64_t nivaloMonotonicUs();
// One observer (the device's executor, for its deadline). NULL removes it.
void nivaloSetTrustedSyncObserver(NivaloTrustedSyncObserver observer, void *context);

#endif
