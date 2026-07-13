#ifndef NIVALO_CLI_FRAME_H
#define NIVALO_CLI_FRAME_H

#include <stddef.h>
#include <stdint.h>

enum NivaloCliFrameResult
{
    NIVALO_CLI_FRAME_NONE,
    NIVALO_CLI_FRAME_READY,
    NIVALO_CLI_FRAME_DROPPED
};

class NivaloCliFrame
{
public:
    NivaloCliFrame(size_t maximumLength, uint32_t timeoutMs);
    NivaloCliFrameResult push(char character, uint32_t now, char *buffer);
    NivaloCliFrameResult expire(uint32_t now, char *buffer);
    void reset(char *buffer);
    size_t length() const { return _length; }

private:
    size_t _maximumLength;
    uint32_t _timeoutMs;
    size_t _length = 0U;
    uint32_t _startedAt = 0U;
    bool _active = false;
    bool _discarding = false;
    bool _sawCarriageReturn = false;
};

#endif
