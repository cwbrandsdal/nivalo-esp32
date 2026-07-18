#ifndef NIVALO_LINK_RECEIVE_SEQUENCE_H
#define NIVALO_LINK_RECEIVE_SEQUENCE_H

#include <stdint.h>

class NivaloLinkReceiveSequence
{
public:
    void reset()
    {
        _hasSequence = false;
        _lastSequence = 0U;
    }

    bool observe(uint16_t sequence)
    {
        if (_hasSequence && sequence == _lastSequence)
        {
            return false;
        }

        _hasSequence = true;
        _lastSequence = sequence;
        return true;
    }

    uint16_t acknowledgment() const
    {
        return _hasSequence ? _lastSequence : 0U;
    }

private:
    bool _hasSequence = false;
    uint16_t _lastSequence = 0U;
};

#endif
