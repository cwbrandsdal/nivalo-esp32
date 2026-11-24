#ifndef NIVALO_CLOCK_H
#define NIVALO_CLOCK_H

// Device clock of specs/cloud-mqtt-v1.1.md section 9. UTC is only ever derived from the last
// trusted SNTP synchronisation plus the monotonic timer; the system clock is not trusted.

#include <stdint.h>

struct NivaloClockReading
{
    bool valid = false;
    int64_t utcMs = 0;
    int64_t monotonicUs = 0;
};

class NivaloClock
{
public:
    static const int64_t MaxSyncAgeUs = 21600LL * 1000000LL; // 6 h
    static const int64_t MaxBackwardStepMs = 2000;

    // A completed SNTP synchronisation that set utcMs at monotonicUs. Returns true when it is
    // trusted: the first one of this boot, one that steps back by at most 2 s, or one that reaches
    // the last trusted instant again after a larger backward step.
    bool onSync(int64_t utcMs, int64_t monotonicUs);
    NivaloClockReading read(int64_t monotonicUs) const;
    bool valid(int64_t monotonicUs) const;
    bool synced() const { return _synced; }

private:
    bool _synced = false;
    bool _stepped = false;
    int64_t _syncUtcMs = 0;
    int64_t _syncMonotonicUs = 0;
    int64_t _trustedInstantMs = 0;
};

// Monotonic deadline of an active operation (section 9): set at the claim, then only ever made
// earlier by later trusted readings.
class NivaloDeadline
{
public:
    void start(int64_t expiresAtMs, const NivaloClockReading &claim);
    void observe(int64_t utcMs, int64_t monotonicUs);
    // A fresh reading immediately before an effect: false once the clock is invalid, the
    // monotonic timer has reached the deadline or UTC has reached expiresAt.
    bool permits(const NivaloClockReading &reading, bool *clockInvalid);
    bool active() const { return _active; }
    void stop() { _active = false; }
    int64_t deadlineUs() const { return _deadlineUs; }

private:
    bool _active = false;
    int64_t _expiresAtMs = 0;
    int64_t _deadlineUs = 0;
};

#endif
