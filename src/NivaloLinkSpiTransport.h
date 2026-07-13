#ifndef NIVALO_LINK_SPI_TRANSPORT_H
#define NIVALO_LINK_SPI_TRANSPORT_H

#include <Arduino.h>
#include <SPI.h>

static constexpr size_t NIVALO_LINK_SPI_FRAME_SIZE = 1024U;
static constexpr size_t NIVALO_LINK_HEADER_SIZE = 24U;
static constexpr size_t NIVALO_LINK_MAX_PAYLOAD = NIVALO_LINK_SPI_FRAME_SIZE - NIVALO_LINK_HEADER_SIZE;

enum NivaloLinkFrameType : uint8_t
{
    NIVALO_LINK_FRAME_IDLE = 0x00,
    NIVALO_LINK_FRAME_HELLO = 0x01,
    NIVALO_LINK_FRAME_POLL = 0x02,
    NIVALO_LINK_FRAME_COMMAND = 0x10,
    NIVALO_LINK_FRAME_COMMAND_STATUS = 0x11,
    NIVALO_LINK_FRAME_TELEMETRY = 0x20,
    NIVALO_LINK_FRAME_EVENT = 0x21,
    NIVALO_LINK_FRAME_DEFINITION = 0x22,
    NIVALO_LINK_FRAME_DEFINITIONS_BEGIN = 0x23,
    NIVALO_LINK_FRAME_DEFINITIONS_END = 0x24,
    NIVALO_LINK_FRAME_HEARTBEAT = 0x30,
    NIVALO_LINK_FRAME_ERROR = 0x7F
};

struct NivaloLinkReceivedFrame
{
    bool valid;
    uint8_t type;
    uint16_t seq;
    uint16_t ack;
    size_t payloadLength;
    char payload[NIVALO_LINK_MAX_PAYLOAD + 1U];
};

struct NivaloLinkPinMap
{
    uint8_t sck = 5;
    uint8_t miso = 19;
    uint8_t mosi = 18;
    uint8_t chipSelect = 33;
    uint8_t dataReady = 26;
};

class NivaloLinkSpiTransport
{
public:
    bool begin(const NivaloLinkPinMap &pins = NivaloLinkPinMap());
    void setPaused(bool paused);
    bool isPaused() const;
    bool dataReady() const;
    bool exchange(uint8_t type, const char *jsonPayload, NivaloLinkReceivedFrame *received);
    bool poll(NivaloLinkReceivedFrame *received);
    const char *lastError() const;
    void formatLastRxPrefix(char *output, size_t outputLength, size_t byteCount = 8U) const;

private:
    void buildFrame(uint8_t type, const char *jsonPayload);
    bool parseFrame(NivaloLinkReceivedFrame *received);
    bool parseFrameAt(NivaloLinkReceivedFrame *received, size_t frameOffset);
    uint32_t crc32(const uint8_t *data, size_t length, uint32_t crc = 0xFFFFFFFFUL) const;
    uint32_t finishCrc32(uint32_t crc) const;
    void setLastError(const char *message);

    bool _started = false;
    bool _paused = false;
    uint16_t _nextSeq = 1;
    uint16_t _lastReceivedSeq = 0;
    const char *_lastError = "";
    uint8_t _txFrame[NIVALO_LINK_SPI_FRAME_SIZE];
    uint8_t _rxFrame[NIVALO_LINK_SPI_FRAME_SIZE];
    NivaloLinkPinMap _pins;
};

#endif
