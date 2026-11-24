#include <Arduino.h>
#include "NivaloDevice.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

namespace
{
static constexpr size_t UartMessageJsonCapacity = 2048;
} // namespace

int64_t NivaloDevice::monotonicUs() const
{
    return esp_timer_get_time();
}

void NivaloDevice::onTrustedSync(void *context, int64_t utcMs, int64_t monotonicUs)
{
    static_cast<NivaloDevice *>(context)->_executor.onTrustedSync(utcMs, monotonicUs);
}

void NivaloDevice::pumpClock()
{
    // Trusted synchronisations reach the executor through onTrustedSync, whoever pumps.
    nivaloPumpSystemClock();
}

void NivaloDevice::feedWatchdog()
{
    if (_watchdogEnabled)
    {
        esp_task_wdt_reset();
    }
}

bool NivaloDevice::clockValid() const
{
    return _clock.valid(esp_timer_get_time());
}

bool NivaloDevice::needsReclaim() const
{
    return !_sessionReady || _executor.blocked();
}

bool NivaloDevice::nextMessageId(char output[37])
{
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    nivaloFormatUuidV4(random, output);
    return true;
}

void NivaloDevice::loop()
{
    feedWatchdog();
    _statusPixel.pulse();
    serviceConnection();
    if (!_connection.connected())
    {
        _statusPixel.set(255, 0, 0);
    }
    serviceCompletion();
    processInbox();
    drainAcks();
    serviceHeartbeat();
    serviceCatalogRetry();
#if NIVALO_HAS_SECONDARY_MCU
    drainNivaloLink();
#endif
    serviceUart();
}

bool NivaloDevice::advanceConnection()
{
    // (boot, connection) never repeats: before the connection counter would wrap, a new boot
    // epoch is persisted.
    if (_epoch.connection == 0xFFFFFFFFUL)
    {
        if (_epoch.boot >= 0xFFFFFFFEUL || !_store.writeBoot(_epoch.boot + 1U))
        {
            _sessionReady = false;
            return false;
        }
        _epoch.boot++;
        _epoch.connection = 0U;
    }
    _epoch.connection++;
    return true;
}

const char *NivaloDevice::buildWill(void *context)
{
    // Built straight into the device's own envelope buffer, which nothing else touches before
    // CONNECT has been written; no heap copy can fail in between.
    NivaloDevice &device = *static_cast<NivaloDevice *>(context);
    if (!device.clockValid() || !device.advanceConnection())
    {
        return NULL;
    }
    device.feedWatchdog();
    char payload[128];
    char messageId[37];
    const size_t length = nivaloEndPayload(device._epoch, false, payload, sizeof(payload));
    device.nextMessageId(messageId);
    const size_t envelope = nivaloBuildEnvelope(device._deviceId.c_str(), messageId,
                                                device._clock.read(device.monotonicUs()).utcMs, NULL, NULL, payload,
                                                length, device._envelope, sizeof(device._envelope));
    return length == 0U || envelope == 0U ? NULL : device._envelope;
}

void NivaloDevice::serviceConnection()
{
    pumpClock();
    if (!_sessionReady)
    {
        return;
    }
    // Without a valid clock the device opens no TLS/MQTT connection (section 9).
    if (!_connection.connected() && !clockValid())
    {
        return;
    }
    _connection.service(millis());
    if (_connection.takeJustConnected())
    {
        onConnected();
    }
}

void NivaloDevice::onConnected()
{
    _statusPixel.set(52, 204, 235);
    _heartbeatSeq = 0U;
    char payload[128];
    const size_t length = nivaloBirthPayload(_epoch, payload, sizeof(payload));
    if (length == 0U || !publishEnvelope(_topics.availability(), payload, length, NULL, &_firmware, true))
    {
        Serial.println("Nivalo: birth publish failed; reconnecting");
        // No DISCONNECT: the broker publishes the registered Will.
        _connection.drop();
        return;
    }
    (void)publishCatalog();
    _executor.republishResult();
    drainAcks();
    flushTelemetryBuffer();
    _nextHeartbeatUs = monotonicUs() + (int64_t)NivaloV11::HeartbeatIntervalMs * 1000LL;
}

void NivaloDevice::serviceHeartbeat()
{
    if (!_connection.connected() || !clockValid())
    {
        return;
    }
    const int64_t now = monotonicUs();
    if (now < _nextHeartbeatUs)
    {
        return;
    }
    // 600 +/- 30 s on the monotonic timer.
    const int64_t jitterMs = (int64_t)(esp_random() % (2U * NivaloV11::HeartbeatJitterMs + 1U)) -
                             (int64_t)NivaloV11::HeartbeatJitterMs;
    _nextHeartbeatUs = now + ((int64_t)NivaloV11::HeartbeatIntervalMs + jitterMs) * 1000LL;
    if (_heartbeatSeq >= 0xFFFFFFFEUL)
    {
        return;
    }
    _heartbeatSeq++;
    NivaloHealth health;
    health.uptimeS = (uint32_t)(now / 1000000LL);
    health.heapTotalB = ESP.getHeapSize();
    health.heapFreeB = min((uint32_t)ESP.getFreeHeap(), health.heapTotalB);
    if (WiFi.status() == WL_CONNECTED)
    {
        const int32_t rssi = WiFi.RSSI();
        health.hasRssi = rssi <= 0 && rssi >= -127;
        health.wifiRssiDbm = rssi;
    }
    health.droppedBatches = _droppedBatches;
    char payload[256];
    const size_t length = nivaloHeartbeatPayload(_epoch, _heartbeatSeq, health, payload, sizeof(payload));
    if (length > 0U)
    {
        (void)publishEnvelope(_topics.availability(), payload, length, NULL, NULL, false);
    }
    // A lost terminal ACK reaches the server with the next heartbeat, from RAM.
    _executor.republishResult();
}

void NivaloDevice::shutdown()
{
    bool ended = false;
    if (_connection.connected() && clockValid())
    {
        char payload[128];
        const size_t length = nivaloEndPayload(_epoch, true, payload, sizeof(payload));
        ended = length > 0U && publishEnvelope(_topics.availability(), payload, length, NULL, NULL, true);
    }
    if (ended)
    {
        // The retained end was written: DISCONNECT discards the Will (section 8.1).
        _connection.disconnect();
    }
    else
    {
        // Without the end, closing the transport lets the broker publish the prebuilt Will.
        _connection.drop();
    }
}

void NivaloDevice::serviceUart()
{
    while (hwAvailable() > 0)
    {
        String uartLine = _hardSerial->readStringUntil('\n');
        uartLine.trim();
        if (uartLine.length() == 0)
        {
            continue;
        }

        DynamicJsonDocument doc(UartMessageJsonCapacity);
        DeserializationError err = deserializeJson(doc, uartLine);
        if (err != DeserializationError::Ok)
        {
            Serial.print("UART JSON parse failed: ");
            Serial.println(err.c_str());
            continue;
        }
        const String commandName = doc["command"].as<String>();
        if (commandName == "publish")
        {
            String output;
            serializeJson(doc, output);
            const char *eventName = doc["payload"]["eventName"] | "stm32.publish";
            const char *eventData = doc["payload"]["eventData"] | output.c_str();
            publishEvent(eventName, eventData);
        }
        else if (commandName == "variable" || commandName == "telemetry")
        {
            JsonVariant payload = doc["payload"];
            JsonVariant payloadValue = payload["value"];
            const char *name = payload["name"] | "";
            if (strlen(name) == 0)
            {
                name = payload["variable"] | "";
            }
            const char *unit = payload["unit"] | "";
            String value;
            if (strlen(name) == 0 || payloadValue.isNull())
            {
                Serial.println("UART telemetry rejected: name or value missing");
                continue;
            }
            if (payloadValue.is<const char *>())
            {
                value = payloadValue.as<const char *>();
            }
            else
            {
                serializeJson(payloadValue, value);
            }
            publishTelemetry(name, value.c_str(), strlen(unit) == 0 ? NULL : unit);
        }
        else
        {
            // Secondary MCU definitions are not reported in protocol v1.1 (source "mcu" is reserved
            // for the later bridge profile).
            Serial.print("Unhandled UART command: ");
            Serial.println(commandName.substring(0, 48));
        }
    }
}
