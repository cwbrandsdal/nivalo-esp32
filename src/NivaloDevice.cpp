#include <Arduino.h>
#include "NivaloDevice.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

namespace
{
static constexpr size_t UartMessageJsonCapacity = 2048;

// Every completed SNTP synchronisation, with the monotonic time it happened at (section 9). The
// observer is installed before setup(), so synchronisations made during provisioning count.
struct TimeSync
{
    int64_t utcMs;
    int64_t monotonicUs;
};
portMUX_TYPE syncLock = portMUX_INITIALIZER_UNLOCKED;
TimeSync syncQueue[4];
uint8_t syncCount = 0U;

void onTimeSync(struct timeval *tv)
{
    if (tv == NULL)
    {
        return;
    }
    TimeSync sync;
    sync.monotonicUs = esp_timer_get_time();
    sync.utcMs = (int64_t)tv->tv_sec * 1000LL + (int64_t)tv->tv_usec / 1000LL;
    portENTER_CRITICAL(&syncLock);
    if (syncCount == sizeof(syncQueue) / sizeof(syncQueue[0]))
    {
        memmove(&syncQueue[0], &syncQueue[1], sizeof(TimeSync) * (syncCount - 1U));
        syncCount--;
    }
    syncQueue[syncCount++] = sync;
    portEXIT_CRITICAL(&syncLock);
}

bool takeTimeSync(TimeSync *sync)
{
    bool taken = false;
    portENTER_CRITICAL(&syncLock);
    if (syncCount > 0U)
    {
        *sync = syncQueue[0];
        memmove(&syncQueue[0], &syncQueue[1], sizeof(TimeSync) * (syncCount - 1U));
        syncCount--;
        taken = true;
    }
    portEXIT_CRITICAL(&syncLock);
    return taken;
}

struct TimeSyncObserver
{
    TimeSyncObserver() { sntp_set_time_sync_notification_cb(onTimeSync); }
} timeSyncObserver;
} // namespace

int64_t NivaloDevice::monotonicUs() const
{
    return esp_timer_get_time();
}

void NivaloDevice::pumpClock()
{
    TimeSync sync;
    while (takeTimeSync(&sync))
    {
        if (_clock.onSync(sync.utcMs, sync.monotonicUs))
        {
            _executor.onTrustedSync(sync.utcMs, sync.monotonicUs);
        }
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
    if (_watchdogEnabled)
    {
        esp_task_wdt_reset();
    }
    _statusPixel.pulse();
    serviceConnection();
    if (!_connection.connected())
    {
        _statusPixel.set(255, 0, 0);
    }
    processInbox();
    drainAcks();
    serviceHeartbeat();
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

bool NivaloDevice::buildWill(void *context, String &will)
{
    NivaloDevice &device = *static_cast<NivaloDevice *>(context);
    if (!device.clockValid() || !device.advanceConnection())
    {
        return false;
    }
    char payload[128];
    char messageId[37];
    const size_t length = nivaloEndPayload(device._epoch, false, payload, sizeof(payload));
    device.nextMessageId(messageId);
    const size_t envelope = nivaloBuildEnvelope(device._deviceId.c_str(), messageId,
                                                device._clock.read(device.monotonicUs()).utcMs, NULL, NULL, payload,
                                                length, device._envelope, sizeof(device._envelope));
    if (length == 0U || envelope == 0U)
    {
        return false;
    }
    will = device._envelope;
    return true;
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
        _connection.disconnect();
        return;
    }
    publishCatalog();
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
    if (_connection.connected() && clockValid())
    {
        char payload[128];
        const size_t length = nivaloEndPayload(_epoch, true, payload, sizeof(payload));
        if (length > 0U)
        {
            (void)publishEnvelope(_topics.availability(), payload, length, NULL, NULL, true);
            _connection.client().loop();
        }
    }
    _connection.disconnect();
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
