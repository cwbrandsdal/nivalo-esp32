#ifndef NIVALO_DEVICE_H
#define NIVALO_DEVICE_H

#if (ARDUINO >= 100)
#include "Arduino.h"
#else
#include "WProgram.h"
#endif

#include "NivaloLinkSpiTransport.h"
#include "NivaloSdkRegistry.h"
#include "NivaloConnection.h"
#include "NivaloCommandResultCache.h"
#include "NivaloProtocol.h"
#include "NivaloLinkManager.h"
#include "NivaloOta.h"
#include "NivaloStatusPixel.h"
#include <ArduinoJson.h>

#ifndef NIVALO_HAS_SECONDARY_MCU
#define NIVALO_HAS_SECONDARY_MCU 0
#endif

// Compatibility publication ends after the protocol migration window on
// 2027-01-31 UTC. Set to 0 once every consumer accepts the canonical topic.
#ifndef NIVALO_PUBLISH_LEGACY_COMMAND_ACKS
#define NIVALO_PUBLISH_LEGACY_COMMAND_ACKS 1
#endif

typedef enum
{
	NIVALO_STATUS_INVALID = -1,		   // -1
	NIVALO_STATUS_SUCCESS = 0,		   // 0
	NIVALO_STATUS_OUT_OF_MEMORY,	   // 1
	NIVALO_STATUS_TIMEOUT,			   // 2
	NIVALO_STATUS_UNEXPECTED_PARAM,	   // 3
	NIVALO_STATUS_UNEXPECTED_RESPONSE, // 4
	NIVALO_STATUS_NO_RESPONSE,		   // 5
	NIVALO_STATUS_DEREGISTERED		   // 6
} nivalo_status_t;

typedef enum
{
	NIVALO_STATUS_GREEN,	
	NIVALO_STATUS_PULSE_BLUE,		  
	NIVALO_STATUS_RED,	
	NIVALO_STATUS_YELLOW
} NivaloStatusColor;

typedef enum
{
    NIVALO_MQTT_TRANSPORT_TLS = 0,
    NIVALO_MQTT_TRANSPORT_PLAINTEXT_LOCAL = 1
} NivaloMqttTransport;

struct NivaloMqttConfig
{
    const char *deviceId = NULL;
    const char *clientId = NULL;
    const char *username = NULL;
    const char *password = NULL;
    const char *host = NULL;
    uint16_t port = 8883;
    const char *firmwareVersion = NULL;
    const char *hardwareName = NULL;
    String macAddress;
    NivaloMqttTransport transport = NIVALO_MQTT_TRANSPORT_TLS;
    // NULL selects the library's built-in public CA bundle. A custom CA string
    // must remain valid for the lifetime of the MQTT connection.
    const char *caCertificate = NULL;
    // Firmware signing keys are public material. Supply the active key and,
    // during a rotation window, the immediately previous key. An empty trust
    // set is intentionally fail-closed: every firmware flash is rejected.
    const struct NivaloFirmwareSigningKey *firmwareSigningKeys = NULL;
    size_t firmwareSigningKeyCount = 0;
    // STM32 updates require either a known-good read-back snapshot or a
    // pre-provisioned golden image. Both options are disabled by default.
    bool preserveKnownGoodStm32Image = false;
    size_t knownGoodStm32ImageSizeBytes = 0;
    const char *stm32GoldenImagePath = NULL;
    size_t stm32GoldenImageSizeBytes = 0;
    const char *stm32GoldenImageSha256 = NULL;
    const char *primaryNtpServer = "pool.ntp.org";
    const char *secondaryNtpServer = "time.cloudflare.com";
    // Disable when the application already owns SNTP/timezone setup.
    bool configureClock = true;
    uint8_t telemetryBufferCapacity = 0;
};

struct NivaloFirmwareSigningKey
{
    const char *keyId;
    // PEM SubjectPublicKeyInfo for an ECDSA P-256 key. The caller owns the
    // storage and must keep it valid for the device lifetime.
    const char *publicKeyPem;
};

struct NivaloPinMap
{
    NivaloLinkPinMap link;
    NivaloOtaPinMap ota;
};

struct NivaloDeviceConfig
{
    NivaloMqttConfig mqtt;
    NivaloPinMap pins;
    HardwareSerial *secondarySerial = NULL;
    unsigned long secondaryBaud = 115200;
    bool watchdogEnabled = true;
    uint16_t watchdogTimeoutSeconds = 30;
};


class NivaloDevice
{
public:
    //  Constructor
    NivaloDevice();

    boolean begin(unsigned long baud = 115200);
    boolean begin(HardwareSerial &hardSerial, unsigned long baud = 115200);
    boolean begin(const NivaloDeviceConfig &config);
    boolean beginMqtt(const NivaloMqttConfig &config);
    [[deprecated("Use begin(const NivaloDeviceConfig&) or beginMqtt(const NivaloMqttConfig&)")]] boolean beginMqtt(
        const char *deviceId,
        const char *clientId,
        const char *username,
        const char *password,
        const char *host,
        uint16_t port,
        const char *firmwareVersion,
        const char *hardwareName,
        String macAddress);
    void setColor(NivaloStatusColor color);

    size_t publish(const char *eventName, const char *eventData);
    size_t publishTelemetry(const char *name, const char *value, const char *unit = NULL);
    size_t publishRuntimeTelemetry();
    size_t publishVariables();
    bool publishDefinitions();
    size_t publishUptimeTelemetry();
    size_t publishWifiSignalTelemetry();
    size_t publishHeapTelemetry();
    size_t publishEvent(const char *name, const char *data, const char *severity = "info");
    size_t publishAvailability(const char *status, const char *reason = NULL);

    bool function(const char *name, NivaloFunctionHandler handler);
    bool function(const char *name, NivaloFunctionHandler handler, const NivaloFunctionMetadata &metadata);
    bool variable(const char *name, int *reference, const char *unit = NULL);
    bool variable(const char *name, unsigned int *reference, const char *unit = NULL);
    bool variable(const char *name, long *reference, const char *unit = NULL);
    bool variable(const char *name, unsigned long *reference, const char *unit = NULL);
    bool variable(const char *name, float *reference, const char *unit = NULL);
    bool variable(const char *name, double *reference, const char *unit = NULL);
    bool variable(const char *name, bool *reference, const char *unit = NULL);
    bool variable(const char *name, String *reference, const char *unit = NULL);
    void onCommand(NivaloCommandHandler handler);

    void loop();

private:
    String _deviceId;
    String _clientId;
    String _username;
    String _password;
    String _mqttHost;
    String _firmwareVersion;
    String _hardwareName;
    uint16_t _mqttPort;
    NivaloMqttTransport _mqttTransport;
    const NivaloFirmwareSigningKey *_firmwareSigningKeys;
    size_t _firmwareSigningKeyCount;
    bool _preserveKnownGoodStm32Image;
    size_t _knownGoodStm32ImageSizeBytes;
    String _stm32GoldenImagePath;
    size_t _stm32GoldenImageSizeBytes;
    String _stm32GoldenImageSha256;
    NivaloSdkRegistry _sdkRegistry;
    NivaloCommandResultCache _sdkCommandResults;
    NivaloConnection _connection;
    NivaloProtocol _protocol;
    NivaloLinkManager _link;
    NivaloOtaManager _ota;
    NivaloStatusPixel _statusPixel;
    NivaloPinMap _pins;
    unsigned long _lastHeartbeat = 0U;
    bool _lastWillConfigured = false;
    bool _watchdogEnabled = false;
    uint8_t _telemetryBufferCapacity = 0U;
    struct BufferedTelemetry { String payload; } _telemetryBuffer[8];
    uint8_t _telemetryBufferHead = 0U;
    uint8_t _telemetryBufferCount = 0U;
    struct PendingCommandAckState
    {
        bool queued = false;
        char commandId[40];
        char status[16];
        char message[96];
        float progress = -1;
    } _pendingCommandAcks[8];
    struct PendingEventState
    {
        bool queued = false;
        char name[64];
        char data[512];
        char severity[16];
    } _pendingEvents[8];
    String _macAddr;
    char _telemetryTopic[150];
    char _stateTopic[150];
    char _eventsTopic[150];
    char _definitionsTopic[150];
    char _availabilityTopic[150];
    char _commandsTopic[150];
    char _commandAckTopicPrefix[150];
    char _legacyAckTopicPrefix[150];

    HardwareSerial *_hardSerial;

    unsigned long _baud;

    nivalo_status_t init(unsigned long baud = 115200);

    void mqttCallback(char *topic, byte *message, unsigned int length);
    void handleFirmwareCommand(const String &commandId, JsonObject arguments);
    void mqttReconnect();
    void serviceConnection();
    void flushTelemetryBuffer();
    bool queueCommandAckReport(const char *commandId, const char *status, const char *message, float progress = -1);
    bool queueEventReport(const char *name, const char *data, const char *severity = "info");
    void drainQueuedReports();
    void drainNivaloLink();
    void handleNivaloLinkFrame(const NivaloLinkReceivedFrame &frame);
    void publishNivaloLinkHeartbeat(const char *payload);
    bool dispatchSdkCommand(const char *commandId, const String &commandName, const String &arguments);
    void appendSdkDefinitions(JsonObject payload);
    void publishCommandAck(const char *commandId, const char *status, const char *message = NULL, float progress = -1);
    String getCommandIdFromTopic(const char *topic);
    String createMessageId();
    String createTimestamp();

    // UART Functions
    size_t hwPrint(const char *s);
    size_t hwWrite(const char c);
    int readAvailable(char *inString);
    char readChar(void);
    byte readByte(void);
    int hwAvailable(void);
    void beginSerial(unsigned long baud);
    void setTimeout(unsigned long timeout);
    bool find(char *target);
};

#endif
