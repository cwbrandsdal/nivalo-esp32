#include "NivaloLinkSpiTransport.h"

static constexpr uint32_t NIVALO_LINK_SPI_CLOCK_HZ = 500000UL;
static constexpr uint16_t NIVALO_LINK_SPI_CS_SETUP_US = 10U;
static constexpr uint16_t NIVALO_LINK_SPI_CS_HOLD_US = 10U;
static constexpr uint16_t NIVALO_LINK_SPI_INTERFRAME_US = 1000U;

static constexpr size_t NIVALO_LINK_MAGIC_OFFSET = 0U;
static constexpr size_t NIVALO_LINK_VERSION_OFFSET = 2U;
static constexpr size_t NIVALO_LINK_TYPE_OFFSET = 3U;
static constexpr size_t NIVALO_LINK_FLAGS_OFFSET = 4U;
static constexpr size_t NIVALO_LINK_HEADER_LEN_OFFSET = 5U;
static constexpr size_t NIVALO_LINK_SEQ_OFFSET = 6U;
static constexpr size_t NIVALO_LINK_ACK_OFFSET = 8U;
static constexpr size_t NIVALO_LINK_PAYLOAD_LEN_OFFSET = 10U;
static constexpr size_t NIVALO_LINK_RESERVED16_OFFSET = 12U;
static constexpr size_t NIVALO_LINK_CRC_OFFSET = 16U;
static constexpr size_t NIVALO_LINK_RESERVED_OFFSET = 20U;
static constexpr uint8_t NIVALO_LINK_VERSION = 1U;

static void writeLe16(uint8_t *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t)(value & 0xFFU);
    buffer[offset + 1U] = (uint8_t)(value >> 8U);
}

static void writeLe32(uint8_t *buffer, size_t offset, uint32_t value)
{
    buffer[offset] = (uint8_t)(value & 0xFFUL);
    buffer[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFUL);
    buffer[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFUL);
    buffer[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFUL);
}

static uint16_t readLe16(const uint8_t *buffer, size_t offset)
{
    return (uint16_t)buffer[offset] | ((uint16_t)buffer[offset + 1U] << 8U);
}

static uint32_t readLe32(const uint8_t *buffer, size_t offset)
{
    return (uint32_t)buffer[offset] |
           ((uint32_t)buffer[offset + 1U] << 8U) |
           ((uint32_t)buffer[offset + 2U] << 16U) |
           ((uint32_t)buffer[offset + 3U] << 24U);
}

bool NivaloLinkSpiTransport::begin(const NivaloLinkPinMap &pins)
{
    _pins = pins;
    SPI.begin(_pins.sck, _pins.miso, _pins.mosi, _pins.chipSelect);
    pinMode(_pins.chipSelect, OUTPUT);
    digitalWrite(_pins.chipSelect, HIGH);
    pinMode(_pins.dataReady, INPUT_PULLDOWN);
    memset(_txFrame, 0, sizeof(_txFrame));
    memset(_rxFrame, 0, sizeof(_rxFrame));
    _started = true;
    _paused = false;
    setLastError("");
    return true;
}

void NivaloLinkSpiTransport::setPaused(bool paused)
{
    _paused = paused;
    digitalWrite(_pins.chipSelect, HIGH);
}

bool NivaloLinkSpiTransport::isPaused() const
{
    return _paused;
}

bool NivaloLinkSpiTransport::dataReady() const
{
    return digitalRead(_pins.dataReady) == HIGH;
}

bool NivaloLinkSpiTransport::exchange(uint8_t type, const char *jsonPayload, NivaloLinkReceivedFrame *received)
{
    if (received != NULL)
    {
        received->valid = false;
        received->type = NIVALO_LINK_FRAME_IDLE;
        received->seq = 0;
        received->ack = 0;
        received->payloadLength = 0;
        received->payload[0] = '\0';
    }

    if (!_started)
    {
        setLastError("nivalolink spi not started");
        return false;
    }

    if (_paused)
    {
        setLastError("nivalolink spi paused");
        return false;
    }

    size_t payloadLength = jsonPayload == NULL ? 0U : strlen(jsonPayload);
    if (payloadLength > NIVALO_LINK_MAX_PAYLOAD)
    {
        setLastError("nivalolink payload too large");
        return false;
    }

    buildFrame(type, jsonPayload);

    SPI.beginTransaction(SPISettings(NIVALO_LINK_SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(_pins.chipSelect, LOW);
    delayMicroseconds(NIVALO_LINK_SPI_CS_SETUP_US);
    SPI.transferBytes(_txFrame, _rxFrame, NIVALO_LINK_SPI_FRAME_SIZE);
    delayMicroseconds(NIVALO_LINK_SPI_CS_HOLD_US);
    digitalWrite(_pins.chipSelect, HIGH);
    delayMicroseconds(NIVALO_LINK_SPI_INTERFRAME_US);
    SPI.endTransaction();

    if (received != NULL)
    {
        (void)parseFrame(received);
    }

    return true;
}

bool NivaloLinkSpiTransport::poll(NivaloLinkReceivedFrame *received)
{
    return exchange(NIVALO_LINK_FRAME_POLL, NULL, received);
}

const char *NivaloLinkSpiTransport::lastError() const
{
    return _lastError;
}

void NivaloLinkSpiTransport::formatLastRxPrefix(char *output, size_t outputLength, size_t byteCount) const
{
    static const char hex[] = "0123456789abcdef";
    if (output == NULL || outputLength == 0U)
    {
        return;
    }

    size_t maxBytes = (outputLength - 1U) / 2U;
    if (byteCount > maxBytes)
    {
        byteCount = maxBytes;
    }
    if (byteCount > NIVALO_LINK_SPI_FRAME_SIZE)
    {
        byteCount = NIVALO_LINK_SPI_FRAME_SIZE;
    }

    for (size_t i = 0; i < byteCount; i++)
    {
        output[i * 2U] = hex[(_rxFrame[i] >> 4U) & 0x0FU];
        output[i * 2U + 1U] = hex[_rxFrame[i] & 0x0FU];
    }
    output[byteCount * 2U] = '\0';
}

void NivaloLinkSpiTransport::buildFrame(uint8_t type, const char *jsonPayload)
{
    memset(_txFrame, 0, sizeof(_txFrame));

    size_t payloadLength = jsonPayload == NULL ? 0U : strlen(jsonPayload);
    _txFrame[NIVALO_LINK_MAGIC_OFFSET] = 'M';
    _txFrame[NIVALO_LINK_MAGIC_OFFSET + 1U] = 'L';
    _txFrame[NIVALO_LINK_VERSION_OFFSET] = NIVALO_LINK_VERSION;
    _txFrame[NIVALO_LINK_TYPE_OFFSET] = type;
    _txFrame[NIVALO_LINK_FLAGS_OFFSET] = 0U;
    _txFrame[NIVALO_LINK_HEADER_LEN_OFFSET] = NIVALO_LINK_HEADER_SIZE;
    writeLe16(_txFrame, NIVALO_LINK_SEQ_OFFSET, _nextSeq++);
    writeLe16(_txFrame, NIVALO_LINK_ACK_OFFSET, _lastReceivedSeq);
    writeLe16(_txFrame, NIVALO_LINK_PAYLOAD_LEN_OFFSET, (uint16_t)payloadLength);
    writeLe16(_txFrame, NIVALO_LINK_RESERVED16_OFFSET, 0U);
    writeLe32(_txFrame, NIVALO_LINK_CRC_OFFSET, 0UL);
    memset(&_txFrame[NIVALO_LINK_RESERVED_OFFSET], 0, 4U);

    if (payloadLength > 0U)
    {
        memcpy(&_txFrame[NIVALO_LINK_HEADER_SIZE], jsonPayload, payloadLength);
    }

    uint32_t crc = crc32(_txFrame, NIVALO_LINK_HEADER_SIZE);
    crc = crc32(&_txFrame[NIVALO_LINK_HEADER_SIZE], payloadLength, crc);
    writeLe32(_txFrame, NIVALO_LINK_CRC_OFFSET, finishCrc32(crc));
}

bool NivaloLinkSpiTransport::parseFrame(NivaloLinkReceivedFrame *received)
{
    if (received == NULL)
    {
        return false;
    }

    received->valid = false;
    received->payload[0] = '\0';

    if (parseFrameAt(received, 0U))
    {
        return true;
    }

    if (_rxFrame[0] == 0U && parseFrameAt(received, 1U))
    {
        return true;
    }

    return false;
}

bool NivaloLinkSpiTransport::parseFrameAt(NivaloLinkReceivedFrame *received, size_t frameOffset)
{
    if (received == NULL || frameOffset + NIVALO_LINK_HEADER_SIZE > NIVALO_LINK_SPI_FRAME_SIZE)
    {
        return false;
    }

    const uint8_t *frame = &_rxFrame[frameOffset];

    if (frame[NIVALO_LINK_MAGIC_OFFSET] != 'M' || frame[NIVALO_LINK_MAGIC_OFFSET + 1U] != 'L')
    {
        setLastError("bad magic");
        return false;
    }

    if (frame[NIVALO_LINK_VERSION_OFFSET] != NIVALO_LINK_VERSION)
    {
        setLastError("unsupported version");
        return false;
    }

    if (frame[NIVALO_LINK_HEADER_LEN_OFFSET] != NIVALO_LINK_HEADER_SIZE)
    {
        setLastError("bad header length");
        return false;
    }

    uint16_t payloadLength = readLe16(frame, NIVALO_LINK_PAYLOAD_LEN_OFFSET);
    if (payloadLength > NIVALO_LINK_MAX_PAYLOAD || frameOffset + NIVALO_LINK_HEADER_SIZE + payloadLength > NIVALO_LINK_SPI_FRAME_SIZE)
    {
        setLastError("bad payload length");
        return false;
    }

    uint32_t receivedCrc = readLe32(frame, NIVALO_LINK_CRC_OFFSET);
    uint8_t headerCopy[NIVALO_LINK_HEADER_SIZE];
    memcpy(headerCopy, frame, sizeof(headerCopy));
    writeLe32(headerCopy, NIVALO_LINK_CRC_OFFSET, 0UL);
    uint32_t crc = crc32(headerCopy, NIVALO_LINK_HEADER_SIZE);
    crc = crc32(&frame[NIVALO_LINK_HEADER_SIZE], payloadLength, crc);
    crc = finishCrc32(crc);
    if (crc != receivedCrc)
    {
        setLastError("bad crc");
        return false;
    }

    received->valid = true;
    received->type = frame[NIVALO_LINK_TYPE_OFFSET];
    received->seq = readLe16(frame, NIVALO_LINK_SEQ_OFFSET);
    received->ack = readLe16(frame, NIVALO_LINK_ACK_OFFSET);
    received->payloadLength = payloadLength;
    if (payloadLength > 0U)
    {
        memcpy(received->payload, &frame[NIVALO_LINK_HEADER_SIZE], payloadLength);
    }
    received->payload[payloadLength] = '\0';
    _lastReceivedSeq = received->seq;
    setLastError("");
    return true;
}

uint32_t NivaloLinkSpiTransport::crc32(const uint8_t *data, size_t length, uint32_t crc) const
{
    for (size_t i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8U; bit++)
        {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1UL));
            crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
        }
    }

    return crc;
}

uint32_t NivaloLinkSpiTransport::finishCrc32(uint32_t crc) const
{
    return crc ^ 0xFFFFFFFFUL;
}

void NivaloLinkSpiTransport::setLastError(const char *message)
{
    _lastError = message == NULL ? "" : message;
}
