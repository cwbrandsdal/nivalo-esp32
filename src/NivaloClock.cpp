#include "NivaloClock.h"

#include <stddef.h>

bool NivaloClock::onSync(int64_t utcMs, int64_t monotonicUs)
{
    if (!_synced)
    {
        _synced = true;
        _stepped = false;
        _syncUtcMs = utcMs;
        _syncMonotonicUs = monotonicUs;
        return true;
    }
    const int64_t projectedMs = _syncUtcMs + (monotonicUs - _syncMonotonicUs) / 1000;
    if (_stepped)
    {
        if (utcMs < _trustedInstantMs)
        {
            return false;
        }
        _stepped = false;
    }
    else if (utcMs < projectedMs - MaxBackwardStepMs)
    {
        // A backward step of more than 2 s: invalid until a synchronisation reaches the instant
        // the trusted timeline had reached when the step was observed.
        _stepped = true;
        _trustedInstantMs = projectedMs;
        return false;
    }
    _syncUtcMs = utcMs;
    _syncMonotonicUs = monotonicUs;
    return true;
}

bool NivaloClock::valid(int64_t monotonicUs) const
{
    return _synced && !_stepped && monotonicUs >= _syncMonotonicUs && monotonicUs - _syncMonotonicUs <= MaxSyncAgeUs;
}

NivaloClockReading NivaloClock::read(int64_t monotonicUs) const
{
    NivaloClockReading reading;
    reading.monotonicUs = monotonicUs;
    reading.valid = valid(monotonicUs);
    reading.utcMs = _synced ? _syncUtcMs + (monotonicUs - _syncMonotonicUs) / 1000 : 0;
    return reading;
}

void NivaloDeadline::start(int64_t expiresAtMs, const NivaloClockReading &claim)
{
    _active = true;
    _expiresAtMs = expiresAtMs;
    _deadlineUs = claim.monotonicUs + (expiresAtMs - claim.utcMs) * 1000;
}

void NivaloDeadline::observe(int64_t utcMs, int64_t monotonicUs)
{
    if (!_active)
    {
        return;
    }
    const int64_t candidate = monotonicUs + (_expiresAtMs - utcMs) * 1000;
    if (candidate < _deadlineUs)
    {
        _deadlineUs = candidate;
    }
}

bool NivaloDeadline::permits(const NivaloClockReading &reading, bool *clockInvalid)
{
    if (clockInvalid != NULL)
    {
        *clockInvalid = !reading.valid;
    }
    if (!_active || !reading.valid)
    {
        return false;
    }
    observe(reading.utcMs, reading.monotonicUs);
    return reading.monotonicUs < _deadlineUs;
}
