#include "NivaloDevice.h"

#include <esp_task_wdt.h>
#include <stdlib.h>

namespace
{
static constexpr size_t MqttPacketBufferSize = 4096U;
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

NivaloDevice::NivaloDevice()
{
    _hardSerial = NULL;
    _baud = 0;
    _mqttPort = 8883;
    _mqttTransport = NIVALO_MQTT_TRANSPORT_TLS;
    _firmwareSigningKeys = NULL;
    _firmwareSigningKeyCount = 0U;
    _preserveKnownGoodStm32Image = false;
    _knownGoodStm32ImageSizeBytes = 0U;
    _stm32GoldenImageSizeBytes = 0U;
    _destructiveStm32RecoveryAcceptancePending = false;
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
    _ota.configure(_pins.ota);
    _ota.releasePins();
    _baud = baud;

#if NIVALO_HAS_SECONDARY_MCU
    return _link.begin(_pins.link);
#else
    return true;
#endif
}

boolean NivaloDevice::begin(const NivaloDeviceConfig &config)
{
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
    return deviceReady && beginMqtt(config.mqtt);
}

boolean NivaloDevice::beginMqtt(
    const char *deviceId,
    const char *clientId,
    const char *username,
    const char *password,
    const char *host,
    uint16_t port,
    const char *firmwareVersion,
    const char *hardwareName,
    String macAddress)
{
    NivaloMqttConfig config;
    config.deviceId = deviceId;
    config.clientId = clientId;
    config.username = username;
    config.password = password;
    config.host = host;
    config.port = port;
    config.firmwareVersion = firmwareVersion;
    config.hardwareName = hardwareName;
    config.macAddress = macAddress;
    return beginMqtt(config);
}

boolean NivaloDevice::beginMqtt(const NivaloMqttConfig &config)
{
    if (config.deviceId == NULL || strlen(config.deviceId) == 0U ||
        config.clientId == NULL || strlen(config.clientId) == 0U ||
        config.username == NULL || strlen(config.username) == 0U ||
        config.password == NULL || strlen(config.password) == 0U ||
        config.host == NULL || strlen(config.host) == 0U || config.port == 0U ||
        config.firmwareVersion == NULL || strlen(config.firmwareVersion) == 0U ||
        config.hardwareName == NULL || strlen(config.hardwareName) == 0U)
    {
        Serial.println("MQTT configuration rejected: required value missing");
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

    _statusPixel.set(255, 0, 0);

    _macAddr = config.macAddress;
    _deviceId = config.deviceId;
    _clientId = config.clientId;
    _username = config.username;
    _password = config.password;
    _mqttHost = config.host;
    _mqttPort = config.port;
    _mqttTransport = config.transport;
    _firmwareSigningKeys = config.firmwareSigningKeys;
    _firmwareSigningKeyCount = config.firmwareSigningKeyCount;
    _preserveKnownGoodStm32Image = config.preserveKnownGoodStm32Image;
    _knownGoodStm32ImageSizeBytes = config.knownGoodStm32ImageSizeBytes;
    _stm32GoldenImagePath = config.stm32GoldenImagePath == NULL ? "" : config.stm32GoldenImagePath;
    _stm32GoldenImageSizeBytes = config.stm32GoldenImageSizeBytes;
    _stm32GoldenImageSha256 = config.stm32GoldenImageSha256 == NULL ? "" : config.stm32GoldenImageSha256;
    _destructiveStm32RecoveryAcceptancePending = config.destructiveStm32RecoveryAcceptanceOnce;
    _firmwareVersion = config.firmwareVersion;
    _hardwareName = config.hardwareName;
    _telemetryBufferCapacity = min((uint8_t)8U, config.telemetryBufferCapacity);
    if (config.configureClock)
    {
        _protocol.beginClock(config.primaryNtpServer, config.secondaryNtpServer);
    }

    snprintf(_telemetryTopic, sizeof(_telemetryTopic), "nivalo/v1/devices/%s/telemetry", _deviceId.c_str());
    snprintf(_stateTopic, sizeof(_stateTopic), "nivalo/v1/devices/%s/state", _deviceId.c_str());
    snprintf(_eventsTopic, sizeof(_eventsTopic), "nivalo/v1/devices/%s/events", _deviceId.c_str());
    snprintf(_definitionsTopic, sizeof(_definitionsTopic), "nivalo/v1/devices/%s/definitions", _deviceId.c_str());
    snprintf(_availabilityTopic, sizeof(_availabilityTopic), "nivalo/v1/devices/%s/availability", _deviceId.c_str());
    snprintf(_commandsTopic, sizeof(_commandsTopic), "nivalo/v1/devices/%s/commands/+", _deviceId.c_str());
    snprintf(_commandAckTopicPrefix, sizeof(_commandAckTopicPrefix), "nivalo/v1/devices/%s/commands/", _deviceId.c_str());
    snprintf(_legacyAckTopicPrefix, sizeof(_legacyAckTopicPrefix), "nivalo/v1/devices/%s/acks/", _deviceId.c_str());

    _connection.configure(_mqttHost.c_str(), _mqttPort, _clientId.c_str(), _username.c_str(), _password.c_str(),
                          _commandsTopic, _availabilityTopic);
    _connection.client().setBufferSize(MqttPacketBufferSize);
    _connection.client().setCallback([this](char *topic, byte *payload, unsigned int length)
                           { this->mqttCallback(topic, payload, length); });

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
