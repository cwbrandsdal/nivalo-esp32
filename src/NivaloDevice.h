#ifndef NIVALO_DEVICE_H
#define NIVALO_DEVICE_H

#if (ARDUINO >= 100)
#include "Arduino.h"
#else
#include "WProgram.h"
#endif

#include <ArduinoJson.h>
#include <esp_partition.h>

#include "NivaloArguments.h"
#include "NivaloCatalog.h"
#include "NivaloClock.h"
#include "NivaloConnection.h"
#include "NivaloDeviceStore.h"
#include "NivaloDownlink.h"
#include "NivaloExecutor.h"
#include "NivaloLinkManager.h"
#include "NivaloLinkSpiTransport.h"
#include "NivaloOta.h"
#include "NivaloOtaPolicy.h"
#include "NivaloSdkRegistry.h"
#include "NivaloStatusPixel.h"
#include "NivaloSystemClock.h"
#include "NivaloUplink.h"
#include "NivaloWire.h"

#ifndef NIVALO_HAS_SECONDARY_MCU
#define NIVALO_HAS_SECONDARY_MCU 0
#endif

typedef enum
{
	NIVALO_STATUS_INVALID = -1,
	NIVALO_STATUS_SUCCESS = 0,
	NIVALO_STATUS_OUT_OF_MEMORY,
	NIVALO_STATUS_TIMEOUT,
	NIVALO_STATUS_UNEXPECTED_PARAM,
	NIVALO_STATUS_UNEXPECTED_RESPONSE,
	NIVALO_STATUS_NO_RESPONSE,
	NIVALO_STATUS_DEREGISTERED
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
    // Firmware identity reported in every birth: text of 1-64 bytes each.
    const char *firmwareVersion = NULL;
    const char *hardwareName = NULL;
    NivaloMqttTransport transport = NIVALO_MQTT_TRANSPORT_TLS;
    // NULL selects the library's built-in public CA bundle. A custom CA string
    // must remain valid for the lifetime of the MQTT connection.
    const char *caCertificate = NULL;
    // Firmware signing keys are public material, selected by keyId. Supply the active key and,
    // during a rotation window, the immediately previous key. An empty trust set rejects every
    // OTA job.
    const NivaloFirmwareSigningKey *firmwareSigningKeys = NULL;
    size_t firmwareSigningKeyCount = 0;
    // Origin of the Nivalo API, for example "https://iot-api.nivalo.io". OTA images are
    // downloaded only from this origin; NULL rejects every OTA job.
    const char *apiOrigin = NULL;
    // CA for the API download; NULL selects the built-in public CA bundle.
    const char *apiCaCertificate = NULL;
    const char *primaryNtpServer = "pool.ntp.org";
    const char *secondaryNtpServer = "time.cloudflare.com";
    // Disable when the application already owns SNTP/timezone setup. Nivalo still observes
    // every SNTP synchronisation to judge clock validity.
    bool configureClock = true;
    // Telemetry messages kept in RAM while offline (0-8). Oldest dropped first.
    uint8_t telemetryBufferCapacity = 0;
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
    // At least NIVALO_MIN_WATCHDOG_SECONDS: one MQTT connect or OTA request blocks the loop for
    // up to about 45 s (DNS 15 s, TCP 5 s, TLS handshake 10 s, MQTT/HTTP response 5-8 s, request
    // write 5 s), and the loop cannot feed the watchdog inside it.
    uint16_t watchdogTimeoutSeconds = 60;
};

#define NIVALO_MIN_WATCHDOG_SECONDS 50

class NivaloDevice
{
public:
    NivaloDevice();

    boolean begin(unsigned long baud = 115200);
    boolean begin(HardwareSerial &hardSerial, unsigned long baud = 115200);
    boolean begin(const NivaloDeviceConfig &config);
    boolean beginMqtt(const NivaloMqttConfig &config);
    void setColor(NivaloStatusColor color);

    // --- Telemetry (batched; section 5) ---------------------------------------------------------
    // One reading published at once. value is a decimal number or, otherwise, raw text.
    size_t publishTelemetry(const char *name, const char *value, const char *unit = NULL);
    // Adds a reading to the current sample of the pending batch (opened at the time of its first
    // reading). nextTelemetrySample() closes the sample; flushTelemetry() publishes the batch.
    // A full batch is published automatically.
    bool addTelemetry(const char *name, double value, const char *unit = NULL);
    bool addTelemetryRaw(const char *name, const char *raw, const char *unit = NULL);
    void nextTelemetrySample();
    size_t flushTelemetry();
    // Every registered variable as one sample.
    size_t publishVariables();
    // uptime_ms, wifi_rssi and heap figures as one sample. Device health is also reported, free
    // of charge, in every availability heartbeat.
    size_t publishRuntimeTelemetry();

    // --- Events and state (section 6) -----------------------------------------------------------
    size_t publish(const char *eventName, const char *eventData);
    // data that is strict JSON becomes the event's data, anything else its raw text.
    size_t publishEvent(const char *name, const char *data, const char *severity = "info");
    // valueJson is strict JSON (for example "true", "21.5", "\"auto\"" or "null").
    size_t publishState(const char *name, const char *valueJson);

    // --- Capabilities (section 12) --------------------------------------------------------------
    // Register before the first connection; the catalog is sealed once it has been reported.
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
    // A handler that will return NIVALO_FUNCTION_PENDING reads its invocation token while it runs
    // (0 outside a handler call) and later reports the outcome with it, from any task:
    // completeFunction() only queues it; device.loop() journals it on the executor. Returns false
    // for a stale or unknown token or when a completion for it is already queued.
    uint32_t functionToken() const;
    bool completeFunction(uint32_t token, NivaloFunctionOutcome outcome);

    // --- Session --------------------------------------------------------------------------------
    // Publishes the retained graceful end and disconnects.
    void shutdown();
    bool clockValid() const;
    // No downlink is accepted until the device is re-claimed (journal missing, corrupt or
    // unwritable), or the boot counter is missing so no session can start.
    bool needsReclaim() const;

    void loop();

private:
    class Platform : public NivaloExecutorPlatform
    {
    public:
        explicit Platform(NivaloDevice &device) : _device(device) {}
        bool writeJournal(const NivaloJournal &journal) override;
        bool writePending(const NivaloPendingRecord &pending) override;
        bool clearPending() override;
        bool writeFloor(uint32_t floor) override;
        NivaloClockReading readClock() override;
        void publishAck(const NivaloAck &ack) override;
        bool hasFunction(const char *name) override;
        bool prepareFunction(const NivaloOperation &operation) override;
        NivaloFunctionOutcome invokePrepared(uint32_t token) override;
        void releasePrepared() override;
        NivaloRefreshOutcome refreshCatalog() override;
        NivaloDownloadOutcome downloadImage(const NivaloOperation &operation, uint32_t token) override;
        NivaloActivationOutcome activateImage() override;
        bool hashRunningSlot(uint32_t sizeBytes, char sha256Hex[65]) override;
        void restart() override;

    private:
        NivaloDevice &_device;
    };
    friend class Platform;

    static const char *buildWill(void *context);
    static void onTrustedSync(void *context, int64_t utcMs, int64_t monotonicUs);
    void feedWatchdog();
    void serviceCompletion();
    void serviceCatalogRetry();
    void mqttCallback(char *topic, byte *message, unsigned int length);
    void serviceConnection();
    void onConnected();
    void serviceHeartbeat();
    void serviceWhileBusy();
    void processInbox();
    void pumpClock();
    int64_t monotonicUs() const;
    bool nextMessageId(char output[37]);
    bool publishEnvelope(const char *topic, const char *payload, size_t length, const char *correlationId,
                         const NivaloFirmwareInfo *firmware, bool retained);
    // Builds (when bytes is empty) and publishes an ACK; the envelope stays in bytes for a retry.
    bool publishAck(const NivaloAck &ack, char *bytes, size_t capacity, uint16_t *length);
    void queueAck(const NivaloAck &ack, const char *bytes, uint16_t length);
    void drainAcks();
    bool publishBatch();
    void bufferTelemetry(const char *envelope, size_t length);
    void flushTelemetryBuffer();
    bool openSample();
    bool sealCatalog();
    NivaloRefreshOutcome publishCatalog();
    NivaloDownloadOutcome downloadFirmware(const NivaloOperation &operation);
    bool advanceConnection();

    // UART bridge input (secondary MCU over UART).
    void serviceUart();
    void drainNivaloLink();
    void handleNivaloLinkFrame(const NivaloLinkReceivedFrame &frame);
    void publishNivaloLinkHeartbeat(const char *payload);

    String _deviceId;
    String _clientId;
    String _username;
    String _password;
    String _mqttHost;
    String _firmwareVersion;
    String _hardwareName;
    String _apiOrigin;
    const char *_apiCaCertificate = NULL;
    uint16_t _mqttPort;
    NivaloMqttTransport _mqttTransport;
    NivaloFirmwareInfo _firmware;

    NivaloTopics _topics;
    // The process-wide clock (NivaloSystemClock.h), shared with provisioning.
    NivaloClock &_clock;
    NivaloDeviceStore _store;
    Platform _platform;
    NivaloExecutor _executor;
    NivaloOtaDevice _ota;
    NivaloSdkRegistry _sdkRegistry;
    NivaloCatalogState _catalog;
    // How the stored catalog read: MISSING is a first enrollment; CORRUPT (unreadable) is never
    // written over and blocks the refresh until a later read succeeds.
    NivaloStoreRead _catalogRead = NIVALO_STORE_CORRUPT;
    bool _catalogReady = false;
    // A refresh that did not complete is retried while connected; a catalog this firmware cannot
    // report (refused transition, too many entries) is not.
    bool _catalogRefreshPending = false;
    bool _catalogUnreportable = false;
    int64_t _catalogRetryAtUs = 0;

    // The generic command being executed: its handler and the arguments it receives, prepared
    // before the claim.
    NivaloPreparedArguments _prepared;
    const NivaloRegisteredFunction *_preparedFunction = NULL;
    // The asynchronous completion mailbox (written by any task, drained by loop()).
    volatile uint32_t _completionToken = 0U;
    volatile bool _completionQueued = false;
    volatile NivaloFunctionOutcome _completionOutcome = NIVALO_FUNCTION_UNKNOWN;
    NivaloConnection _connection;
    NivaloLinkManager _link;
    NivaloOtaManager _otaPins;
    NivaloStatusPixel _statusPixel;
    NivaloPinMap _pins;

    // Session epoch (section 8.1).
    bool _sessionReady = false;
    NivaloEpoch _epoch;
    uint32_t _heartbeatSeq = 0U;
    int64_t _nextHeartbeatUs = 0;

    // One downlink copied out of PubSubClient's buffer, handled on the next loop pass, and the
    // copy the executor works on while the inbox refills.
    NivaloOperation _inbox;
    NivaloOperation _working;
    bool _inboxFull = false;
    bool _executorBusyLoop = false;

    // ACKs waiting for a connection and a valid clock. An ACK whose publish was attempted keeps its
    // envelope, so a retry sends identical bytes (section 3); a deferred one is built when the
    // clock is valid.
    static constexpr size_t AckQueueCapacity = 6U;
    static constexpr size_t AckEnvelopeCapacity = 384U;
    struct QueuedAck
    {
        NivaloAck ack;
        uint16_t length = 0U;
        char bytes[AckEnvelopeCapacity] = {0};
    };
    QueuedAck _ackQueue[AckQueueCapacity];
    size_t _ackQueueCount = 0U;

    NivaloTelemetryBatch _batch;
    bool _newSample = true;
    uint32_t _droppedBatches = 0U;
    uint8_t _telemetryBufferCapacity = 0U;
    String _telemetryBuffer[8];
    uint8_t _telemetryBufferHead = 0U;
    uint8_t _telemetryBufferCount = 0U;
    char _envelope[NivaloV11::MaxUplinkBytes + 1U];
    // Event and state payloads, built before the envelope around them.
    char _payload[NivaloV11::MaxUplinkBytes];

    // OTA: the inactive slot written by the current job, for the single activation call.
    const esp_partition_t *_otaTarget = NULL;

    bool _watchdogEnabled = false;
    HardwareSerial *_hardSerial;
    unsigned long _baud;

    nivalo_status_t init(unsigned long baud = 115200);

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
