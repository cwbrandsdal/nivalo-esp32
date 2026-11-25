#include "NivaloJournal.h"

#include <string.h>

namespace
{
class Writer
{
public:
    Writer(uint8_t *output, size_t capacity) : _output(output), _capacity(capacity) {}
    void byte(uint8_t value)
    {
        if (_output == NULL || _length >= _capacity)
        {
            _failed = true;
            return;
        }
        _output[_length++] = value;
    }
    void u32(uint32_t value)
    {
        for (int i = 0; i < 4; i++)
        {
            byte((uint8_t)(value >> (8 * i)));
        }
    }
    void bytes(const uint8_t *data, size_t length)
    {
        for (size_t i = 0U; i < length; i++)
        {
            byte(data[i]);
        }
    }
    void text(const char *value, size_t width)
    {
        const size_t length = value == NULL ? 0U : strnlen(value, width);
        for (size_t i = 0U; i < width; i++)
        {
            byte(i < length ? (uint8_t)value[i] : 0U);
        }
    }
    size_t finish()
    {
        const uint32_t crc = nivaloCrc32(_output, _length);
        u32(crc);
        return _failed ? 0U : _length;
    }

private:
    uint8_t *_output;
    size_t _capacity;
    size_t _length = 0U;
    bool _failed = false;
};

class Reader
{
public:
    Reader(const uint8_t *data, size_t length) : _data(data), _length(length) {}
    uint8_t byte()
    {
        if (_data == NULL || _position >= _length)
        {
            _failed = true;
            return 0U;
        }
        return _data[_position++];
    }
    uint32_t u32()
    {
        uint32_t value = 0U;
        for (int i = 0; i < 4; i++)
        {
            value |= (uint32_t)byte() << (8 * i);
        }
        return value;
    }
    void bytes(uint8_t *output, size_t length)
    {
        if (_data == NULL || _length - _position < length || _position > _length)
        {
            _failed = true;
            memset(output, 0, length);
            return;
        }
        memcpy(output, _data + _position, length);
        _position += length;
    }
    // width bytes into output, which holds width + 1 characters.
    void text(char *output, size_t width)
    {
        bytes(reinterpret_cast<uint8_t *>(output), width);
        output[width] = '\0';
    }
    bool failed() const { return _failed; }
    size_t position() const { return _position; }

private:
    const uint8_t *_data;
    size_t _length;
    size_t _position = 0U;
    bool _failed = false;
};

// Common framing: three header bytes, the body, then a CRC-32 of everything before it.
bool framed(const uint8_t *data, size_t length, char kind)
{
    if (data == NULL || length < 7U || data[0] != 'N' || data[1] != (uint8_t)kind || data[2] != 1U)
    {
        return false;
    }
    const uint32_t stored = (uint32_t)data[length - 4U] | ((uint32_t)data[length - 3U] << 8U) |
                            ((uint32_t)data[length - 2U] << 16U) | ((uint32_t)data[length - 1U] << 24U);
    return stored == nivaloCrc32(data, length - 4U);
}

bool validStatus(uint8_t status) { return status >= NIVALO_ACK_ACCEPTED && status <= NIVALO_ACK_INDETERMINATE; }
} // namespace

uint32_t nivaloCrc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0U; data != NULL && i < length; i++)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1U) ^ (0xEDB88320UL & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

NivaloResult nivaloResult(NivaloAckStatus status, NivaloAckCode code)
{
    NivaloResult result;
    result.status = (uint8_t)status;
    result.code = (uint8_t)code;
    return result;
}

const char *nivaloAckStatusName(uint8_t status)
{
    switch (status)
    {
    case NIVALO_ACK_ACCEPTED: return "accepted";
    case NIVALO_ACK_RUNNING: return "running";
    case NIVALO_ACK_SUCCEEDED: return "succeeded";
    case NIVALO_ACK_FAILED: return "failed";
    case NIVALO_ACK_INDETERMINATE: return "indeterminate";
    default: return NULL;
    }
}

const char *nivaloAckCodeName(uint8_t code)
{
    switch (code)
    {
    case NIVALO_CODE_EXPIRED: return "expired";
    case NIVALO_CODE_CLOCK_UNSYNCED: return "clock_unsynced";
    case NIVALO_CODE_INVALID_COMMAND: return "invalid_command";
    case NIVALO_CODE_NOT_ALLOWED: return "not_allowed";
    case NIVALO_CODE_ID_CONFLICT: return "id_conflict";
    case NIVALO_CODE_STORAGE_ERROR: return "storage_error";
    case NIVALO_CODE_OTA_REJECTED: return "ota_rejected";
    case NIVALO_CODE_HANDLER_FAILED: return "handler_failed";
    case NIVALO_CODE_EXECUTION_UNKNOWN: return "execution_unknown";
    case NIVALO_CODE_REPLAY_SUPPRESSED: return "replay_suppressed";
    default: return NULL;
    }
}

NivaloJournal nivaloFreshJournal(const char *deviceId)
{
    NivaloJournal journal;
    if (nivaloIsUuid(deviceId))
    {
        memcpy(journal.deviceId, deviceId, 37U);
    }
    return journal;
}

size_t nivaloEncodeJournal(const NivaloJournal &journal, uint8_t *output, size_t capacity)
{
    Writer writer(output, capacity);
    writer.byte('N');
    writer.byte('J');
    writer.byte(1U);
    writer.text(journal.deviceId, 36U);
    writer.u32(journal.highWater);
    writer.byte(journal.hasLast ? 1U : 0U);
    writer.u32(journal.hasLast ? journal.seq : 0U);
    writer.text(journal.hasLast ? journal.commandId : "", 36U);
    writer.byte(journal.hasLast ? journal.operationClass : 0U);
    if (journal.hasLast)
    {
        writer.bytes(journal.fingerprint, sizeof(journal.fingerprint));
    }
    else
    {
        const uint8_t zeros[32] = {0};
        writer.bytes(zeros, sizeof(zeros));
    }
    writer.byte(journal.hasLast ? journal.phase : 0U);
    writer.byte(journal.hasLast ? journal.result.status : 0U);
    writer.byte(journal.hasLast ? journal.result.code : 0U);
    return writer.finish();
}

bool nivaloDecodeJournal(const uint8_t *data, size_t length, NivaloJournal *journal)
{
    if (journal == NULL || length != 124U || !framed(data, length, 'J'))
    {
        return false;
    }
    Reader reader(data + 3, length - 7U);
    NivaloJournal decoded;
    reader.text(decoded.deviceId, 36U);
    decoded.highWater = reader.u32();
    const uint8_t hasLast = reader.byte();
    decoded.seq = reader.u32();
    reader.text(decoded.commandId, 36U);
    decoded.operationClass = reader.byte();
    reader.bytes(decoded.fingerprint, sizeof(decoded.fingerprint));
    decoded.phase = reader.byte();
    decoded.result.status = reader.byte();
    decoded.result.code = reader.byte();
    if (reader.failed() || !nivaloIsUuid(decoded.deviceId) || hasLast > 1U)
    {
        return false;
    }
    decoded.hasLast = hasLast == 1U;
    if (decoded.hasLast)
    {
        if (decoded.seq != decoded.highWater || decoded.seq == 0U || !nivaloIsUuid(decoded.commandId) ||
            decoded.operationClass > NIVALO_OPERATION_OTA ||
            (decoded.phase != NIVALO_PHASE_CLAIMED && decoded.phase != NIVALO_PHASE_DONE))
        {
            return false;
        }
        if (decoded.phase == NIVALO_PHASE_DONE &&
            (!validStatus(decoded.result.status) || decoded.result.code > NIVALO_CODE_REPLAY_SUPPRESSED))
        {
            return false;
        }
    }
    else if (decoded.highWater != 0U)
    {
        return false;
    }
    *journal = decoded;
    return true;
}

size_t nivaloEncodePending(const NivaloPendingRecord &pending, uint8_t *output, size_t capacity)
{
    Writer writer(output, capacity);
    writer.byte('N');
    writer.byte('P');
    writer.byte(1U);
    writer.u32(pending.commandSeq);
    writer.text(pending.productId, 36U);
    writer.u32(pending.securityVersion);
    writer.u32(pending.sizeBytes);
    writer.text(pending.sha256, 64U);
    return writer.finish();
}

bool nivaloDecodePending(const uint8_t *data, size_t length, NivaloPendingRecord *pending)
{
    if (pending == NULL || length != 119U || !framed(data, length, 'P'))
    {
        return false;
    }
    Reader reader(data + 3, length - 7U);
    NivaloPendingRecord decoded;
    decoded.commandSeq = reader.u32();
    reader.text(decoded.productId, 36U);
    decoded.securityVersion = reader.u32();
    decoded.sizeBytes = reader.u32();
    reader.text(decoded.sha256, 64U);
    if (reader.failed() || decoded.commandSeq == 0U || !nivaloIsUuid(decoded.productId) ||
        decoded.securityVersion == 0U || decoded.sizeBytes == 0U || !nivaloIsLowerHex(decoded.sha256, 64U))
    {
        return false;
    }
    *pending = decoded;
    return true;
}

size_t nivaloEncodeFloor(const NivaloFloorRecord &floor, uint8_t *output, size_t capacity)
{
    Writer writer(output, capacity);
    writer.byte('N');
    writer.byte('F');
    writer.byte(1U);
    writer.text(floor.productId, 36U);
    writer.u32(floor.floor);
    return writer.finish();
}

bool nivaloDecodeFloor(const uint8_t *data, size_t length, NivaloFloorRecord *floor)
{
    if (floor == NULL || length != 47U || !framed(data, length, 'F'))
    {
        return false;
    }
    Reader reader(data + 3, length - 7U);
    NivaloFloorRecord decoded;
    reader.text(decoded.productId, 36U);
    decoded.floor = reader.u32();
    if (reader.failed() || !nivaloIsUuid(decoded.productId))
    {
        return false;
    }
    *floor = decoded;
    return true;
}
