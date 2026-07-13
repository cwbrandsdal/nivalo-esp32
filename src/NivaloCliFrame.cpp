#include "NivaloCliFrame.h"

NivaloCliFrame::NivaloCliFrame(size_t maximumLength, uint32_t timeoutMs)
    : _maximumLength(maximumLength), _timeoutMs(timeoutMs)
{
}

NivaloCliFrameResult NivaloCliFrame::push(char character, uint32_t now, char *buffer)
{
    if (buffer == NULL) return NIVALO_CLI_FRAME_DROPPED;
    if (character == '\n')
        return !_discarding && _length > 0U ? NIVALO_CLI_FRAME_READY : NIVALO_CLI_FRAME_DROPPED;
    if (!_active)
    {
        _active = true;
        _startedAt = now;
    }
    if (character == '\r')
    {
        if (_sawCarriageReturn) _discarding = true;
        _sawCarriageReturn = true;
        return NIVALO_CLI_FRAME_NONE;
    }
    if (_sawCarriageReturn || character == '\0' || _discarding || _length >= _maximumLength)
    {
        _discarding = true;
        return NIVALO_CLI_FRAME_NONE;
    }
    buffer[_length++] = character;
    return NIVALO_CLI_FRAME_NONE;
}

NivaloCliFrameResult NivaloCliFrame::expire(uint32_t now, char *buffer)
{
    if (!_active || now - _startedAt < _timeoutMs) return NIVALO_CLI_FRAME_NONE;
    reset(buffer);
    return NIVALO_CLI_FRAME_DROPPED;
}

void NivaloCliFrame::reset(char *buffer)
{
    if (buffer != NULL)
    {
        volatile char *wipe = buffer;
        for (size_t index = 0U; index < _length; ++index) wipe[index] = '\0';
    }
    _length = 0U;
    _startedAt = 0U;
    _active = false;
    _discarding = false;
    _sawCarriageReturn = false;
}
