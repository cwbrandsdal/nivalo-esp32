#include "NivaloDevice.h"

// PubSubClient hands over its own buffer and reuses it as soon as the callback returns. The
// downlink is scanned, fingerprinted and copied here, inside the callback, and executed later
// from loop(): the executor never runs inside PubSubClient and the PUBACK goes out at once.
void NivaloDevice::mqttCallback(char *topic, byte *message, unsigned int length)
{
    if (_inboxFull)
    {
        // Not consumed: the gateway republishes a live operation every 30 s.
        return;
    }
    if (nivaloParseDownlink(_topics, topic, message, length, &_inbox) == NIVALO_DOWNLINK_IDENTIFIED)
    {
        _inboxFull = true;
    }
}

void NivaloDevice::processInbox()
{
    if (!_inboxFull)
    {
        return;
    }
    if (_executorBusyLoop)
    {
        // Called while an operation is running (OTA download): the executor only answers
        // duplicates from its journal or drops higher sequences; it starts nothing.
        _executor.deliver(_inbox);
        _inboxFull = false;
        drainAcks();
        return;
    }
    // The executing operation must outlive the inbox slot, which refills during a download.
    _working = _inbox;
    _inboxFull = false;
    _executorBusyLoop = true;
    _executor.deliver(_working);
    _executorBusyLoop = false;
    drainAcks();
}

void NivaloDevice::serviceWhileBusy()
{
    feedWatchdog();
    serviceConnection();
    processInbox();
    drainAcks();
    serviceHeartbeat();
}

bool NivaloDevice::Platform::writeJournal(const NivaloJournal &journal)
{
    return _device._store.writeJournal(journal);
}

bool NivaloDevice::Platform::writePending(const NivaloPendingRecord &pending)
{
    return _device._store.writePending(pending);
}

bool NivaloDevice::Platform::clearPending()
{
    return _device._store.clearPending();
}

bool NivaloDevice::Platform::writeFloor(uint32_t floor)
{
    return _device._store.writeFloor(_device._ota.productId, floor);
}

NivaloStoreRead NivaloDevice::Platform::readPending(NivaloPendingRecord *pending)
{
    return _device._store.loadPending(pending);
}

NivaloStoreRead NivaloDevice::Platform::readFloor(uint32_t *floor)
{
    return _device._store.loadFloor(_device._ota.productId, floor);
}

NivaloClockReading NivaloDevice::Platform::readClock()
{
    _device.pumpClock();
    return _device._clock.read(_device.monotonicUs());
}

void NivaloDevice::Platform::publishAck(const NivaloAck &ack)
{
    char bytes[AckEnvelopeCapacity];
    uint16_t length = 0U;
    if (!_device.publishAck(ack, bytes, sizeof(bytes), &length))
    {
        // Built but not sent: the retry sends the same bytes. Not built (no clock): built later.
        _device.queueAck(ack, bytes, length);
    }
}
