#include "NivaloDevice.h"

#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <stdlib.h>

namespace
{
static constexpr size_t MqttPacketBufferSize = 4096U;
}

static bool copied(String &target, const char *source)
{
    target = source;
    return source != NULL && target.length() == strlen(source);
}

static bool isProductionMqttHost(const char *host)
{
    if (host == NULL)
    {
        return false;
    }
    String normalized(host);
    normalized.toLowerCase();
    while (normalized.endsWith("."))
    {
        normalized.remove(normalized.length() - 1U);
    }
    return normalized == "mqtt.nivalo.io" || normalized.endsWith(".mqtt.nivalo.io");
}

static bool parseIpv4Address(const char *host, uint8_t octets[4])
{
    if (host == NULL || octets == NULL)
    {
        return false;
    }

    size_t octetIndex = 0U;
    unsigned int value = 0U;
    size_t digitCount = 0U;
    for (const char *cursor = host;; cursor++)
    {
        char current = *cursor;
        if (current >= '0' && current <= '9')
        {
            value = value * 10U + (unsigned int)(current - '0');
            digitCount++;
            if (value > 255U || digitCount > 3U)
            {
                return false;
            }
            continue;
        }

        if ((current == '.' || current == '\0') && digitCount > 0U && octetIndex < 4U)
        {
            octets[octetIndex++] = (uint8_t)value;
            value = 0U;
            digitCount = 0U;
            if (current == '\0')
            {
                return octetIndex == 4U;
            }
            continue;
        }

        return false;
    }
}


static bool isHexDigit(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static bool isLocalIpv6Address(const char *host)
{
    if (host == NULL)
    {
        return false;
    }
    if (strcmp(host, "::1") == 0)
    {
        return true;
    }

    const char *separator = strchr(host, ':');
    if (separator == NULL || separator == host || (size_t)(separator - host) > 4U)
    {
        return false;
    }
    for (const char *cursor = host; *cursor != '\0'; cursor++)
    {
        if (!isHexDigit(*cursor) && *cursor != ':' && *cursor != '.')
        {
            return false;
        }
    }

    char firstHextetText[5] = {0};
    memcpy(firstHextetText, host, (size_t)(separator - host));
    unsigned long firstHextet = strtoul(firstHextetText, NULL, 16);
    return (firstHextet & 0xFE00UL) == 0xFC00UL ||
           (firstHextet & 0xFFC0UL) == 0xFE80UL;
}

static bool isLocalBrokerHost(const char *host)
{
    if (host == NULL || strlen(host) == 0U)
    {
        return false;
    }

    String normalized(host);
    normalized.toLowerCase();
    while (normalized.endsWith("."))
    {
        normalized.remove(normalized.length() - 1U);
    }

    if (normalized == "localhost" ||
        normalized.endsWith(".local") || normalized.endsWith(".lan") ||
        normalized.endsWith(".home.arpa") || isLocalIpv6Address(normalized.c_str()))
    {
        return true;
    }

    uint8_t octets[4];
    if (parseIpv4Address(normalized.c_str(), octets))
    {
        return octets[0] == 10U || octets[0] == 127U ||
               (octets[0] == 172U && octets[1] >= 16U && octets[1] <= 31U) ||
               (octets[0] == 192U && octets[1] == 168U) ||
               (octets[0] == 169U && octets[1] == 254U);
    }

    if (normalized.indexOf('.') < 0)
    {
        return true;
    }

    return false;
}

NivaloDevice::NivaloDevice() : _clock(nivaloSystemClock()), _platform(*this), _executor(_platform)
{
    _hardSerial = NULL;
    _baud = 0;
    _mqttPort = 8883;
    _mqttTransport = NIVALO_MQTT_TRANSPORT_TLS;
}

boolean NivaloDevice::begin(HardwareSerial &hardSerial, unsigned long baud)
{
    _hardSerial = &hardSerial;
    boolean ok = begin(baud);
    _hardSerial = &hardSerial;
    (void)init(baud);
    return ok;
}

boolean NivaloDevice::begin(unsigned long baud)
{
    _statusPixel.begin();
    _otaPins.configure(_pins.ota);
    _otaPins.releasePins();
    _baud = baud;

#if NIVALO_HAS_SECONDARY_MCU
    return _link.begin(_pins.link);
#else
    return true;
#endif
}

boolean NivaloDevice::begin(const NivaloDeviceConfig &config)
{
    if (config.watchdogEnabled && config.watchdogTimeoutSeconds < NIVALO_MIN_WATCHDOG_SECONDS)
    {
        Serial.println("Nivalo: watchdogTimeoutSeconds must be at least 50 (one connect can block about 45 s)");
        return false;
    }
    _pins = config.pins;
    _hardSerial = config.secondarySerial;
    _watchdogEnabled = config.watchdogEnabled;
    if (_watchdogEnabled)
    {
        esp_task_wdt_init(config.watchdogTimeoutSeconds, true);
        esp_task_wdt_add(NULL);
    }
    bool deviceReady = config.secondarySerial != NULL
                           ? begin(*config.secondarySerial, config.secondaryBaud)
                           : begin(config.secondaryBaud);
    if (deviceReady && beginMqtt(config.mqtt))
    {
        return true;
    }
    // A device that did not start is not looped: unsubscribe the task so a caller that waits and
    // retries is not reset by the watchdog.
    if (_watchdogEnabled)
    {
        esp_task_wdt_delete(NULL);
        _watchdogEnabled = false;
    }
    return false;
}

boolean NivaloDevice::beginMqtt(const NivaloMqttConfig &config)
{
    if (config.deviceId == NULL || !nivaloIsUuid(config.deviceId) ||
        config.clientId == NULL || strlen(config.clientId) == 0U ||
        config.username == NULL || strlen(config.username) == 0U ||
        config.password == NULL || strlen(config.password) == 0U ||
        config.host == NULL || strlen(config.host) == 0U || config.port == 0U)
    {
        Serial.println("MQTT configuration rejected: required value missing or device ID not canonical");
        return false;
    }
    NivaloFirmwareInfo firmware;
    firmware.version = config.firmwareVersion;
    firmware.hardware = config.hardwareName;
    if (!nivaloValidFirmwareInfo(firmware))
    {
        Serial.println("MQTT configuration rejected: firmware version and hardware must be 1-64 bytes of text");
        return false;
    }
    NivaloOrigin origin;
    if (config.apiOrigin != NULL && !nivaloParseHttpsOrigin(config.apiOrigin, false, &origin))
    {
        Serial.println("MQTT configuration rejected: apiOrigin must be https://host[:port]");
        return false;
    }

    if (config.transport == NIVALO_MQTT_TRANSPORT_PLAINTEXT_LOCAL)
    {
        if (isProductionMqttHost(config.host) || !isLocalBrokerHost(config.host))
        {
            Serial.println("MQTT configuration rejected: plaintext is restricted to local brokers");
            return false;
        }
        _connection.usePlaintext();
    }
    else if (config.transport == NIVALO_MQTT_TRANSPORT_TLS)
    {
        if (config.caCertificate != NULL && strlen(config.caCertificate) == 0U)
        {
            Serial.println("MQTT configuration rejected: CA certificate is empty");
            return false;
        }
        _connection.useTls(config.caCertificate != NULL ? config.caCertificate : NivaloConnection::defaultCaCertificate());
    }
    else
    {
        Serial.println("MQTT configuration rejected: unsupported transport");
        return false;
    }

    for (size_t i = 0; i < config.firmwareSigningKeyCount; i++)
    {
        if (config.firmwareSigningKeys == NULL ||
            config.firmwareSigningKeys[i].keyId == NULL ||
            config.firmwareSigningKeys[i].keyId[0] == '\0' ||
            config.firmwareSigningKeys[i].publicKeyPem == NULL ||
            config.firmwareSigningKeys[i].publicKeyPem[0] == '\0')
        {
            Serial.println("MQTT configuration rejected: invalid firmware signing key entry");
            return false;
        }
        for (size_t prior = 0; prior < i; prior++)
        {
            if (strcmp(config.firmwareSigningKeys[prior].keyId, config.firmwareSigningKeys[i].keyId) == 0)
            {
                Serial.println("MQTT configuration rejected: duplicate firmware signing keyId");
                return false;
            }
        }
    }

    // Every allocation that can fail comes before the boot counter advances, so a failed start
    // changes no durable state and can simply be retried.
    _sessionReady = false;
    if (!_connection.reserve(MqttPacketBufferSize))
    {
        Serial.println("Nivalo: MQTT packet buffer could not be allocated");
        return false;
    }
    if (!copied(_deviceId, config.deviceId) || !copied(_clientId, config.clientId) ||
        !copied(_username, config.username) || !copied(_password, config.password) ||
        !copied(_mqttHost, config.host) || !copied(_firmwareVersion, config.firmwareVersion) ||
        !copied(_hardwareName, config.hardwareName) ||
        !copied(_apiOrigin, config.apiOrigin == NULL ? "" : config.apiOrigin) || !_topics.configure(_deviceId.c_str()))
    {
        Serial.println("Nivalo: MQTT settings could not be copied (out of memory)");
        return false;
    }

    _statusPixel.set(255, 0, 0);

    _mqttPort = config.port;
    _mqttTransport = config.transport;
    _firmware.version = _firmwareVersion.c_str();
    _firmware.hardware = _hardwareName.c_str();
    _apiCaCertificate = config.apiCaCertificate;
    _telemetryBufferCapacity = min((uint8_t)8U, config.telemetryBufferCapacity);

    // The SNTP observer is installed before setup(); syncs made during provisioning are queued.
    if (config.configureClock)
    {
        configTime(0, 0, config.primaryNtpServer == NULL ? "pool.ntp.org" : config.primaryNtpServer,
                   config.secondaryNtpServer == NULL ? "time.cloudflare.com" : config.secondaryNtpServer);
    }
    // Trusted synchronisations from now on also tighten the active operation's deadline.
    nivaloSetTrustedSyncObserver(&NivaloDevice::onTrustedSync, this);
    pumpClock();

    if (!_connection.configure(_mqttHost.c_str(), _mqttPort, _clientId.c_str(), _username.c_str(), _password.c_str(),
                               _topics.commandsFilter(), _topics.otaFilter(), _topics.availability(),
                               &NivaloDevice::buildWill, this))
    {
        Serial.println("Nivalo: MQTT settings could not be copied (out of memory)");
        return false;
    }

    // Persistent protocol state. The boot counter is advanced once per boot, before any MQTT
    // connection, and the epoch it starts never repeats for this device ID (section 8.1).
    if (!_store.begin())
    {
        Serial.println("Nivalo: device state storage could not be opened");
        return false;
    }
    uint32_t boot = 0U;
    const NivaloStoreRead bootRead = _store.loadBoot(&boot);
    if (bootRead != NIVALO_STORE_OK || boot >= 0xFFFFFFFEUL || !_store.writeBoot(boot + 1U))
    {
        Serial.println("Nivalo: boot counter missing or unwritable; the device must be re-claimed");
        return false;
    }
    _epoch.boot = boot + 1U;
    _epoch.connection = 0U;

    // OTA: the running image's own identity, the committed floor of its product, the slot size.
    const NivaloImageIdentity &running = nivaloRunningImageIdentity();
    const esp_partition_t *inactive = esp_ota_get_next_update_partition(NULL);
    _ota.keys = config.firmwareSigningKeys;
    _ota.keyCount = config.firmwareSigningKeyCount;
    _ota.productId = running.productId;
    _ota.runningSecurityVersion = running.securityVersion;
    uint32_t floor = 0U;
    const NivaloStoreRead floorRead = _store.loadFloor(running.productId, &floor);
    _ota.committedFloor = floor;
    _ota.floorReadable = floorRead != NIVALO_STORE_CORRUPT;
    if (!_ota.floorReadable)
    {
        Serial.println("Nivalo: committed OTA floor unreadable; OTA is disabled and the record is kept");
    }
    _ota.maxImageBytes = inactive == NULL ? 0U : inactive->size;
    _ota.apiOrigin = config.apiOrigin == NULL ? NULL : _apiOrigin.c_str();

    // Reconcile a claimed operation before connecting (section 11.4 step 4).
    NivaloJournal journal;
    const NivaloStoreRead journalRead = _store.loadJournal(&journal);
    NivaloPendingRecord pending;
    const NivaloStoreRead pendingRead = _store.loadPending(&pending);
    if (pendingRead == NIVALO_STORE_CORRUPT)
    {
        (void)_store.clearPending();
    }
    _executor.begin(_deviceId.c_str(), journalRead == NIVALO_STORE_OK ? &journal : NULL,
                    pendingRead == NIVALO_STORE_OK ? &pending : NULL, &_ota, running);
    if (_executor.blocked())
    {
        Serial.println("Nivalo: command journal missing or unusable; commands are refused until re-claimed");
    }
    NivaloCatalogState catalog;
    _catalogReady = false;
    _catalogRead = _store.loadCatalog(&catalog);
    if (_catalogRead == NIVALO_STORE_OK)
    {
        _catalog = catalog;
    }

    _connection.client().setCallback([this](char *topic, byte *payload, unsigned int length)
                                     { this->mqttCallback(topic, payload, length); });
    _sessionReady = true;
    return true;
}

void NivaloDevice::setColor(NivaloStatusColor color)
{
    switch (color)
    {
    case NIVALO_STATUS_RED:
        _statusPixel.set(255, 0, 0);
        break;
    case NIVALO_STATUS_GREEN:
        _statusPixel.set(0, 255, 0);
        break;
    case NIVALO_STATUS_YELLOW:
        _statusPixel.set(255, 255, 0);
        break;
    case NIVALO_STATUS_PULSE_BLUE:
        _statusPixel.set(52, 204, 235);
        break;
    default:
        break;
    }
}
