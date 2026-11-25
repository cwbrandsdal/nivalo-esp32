#ifndef NIVALO_PROVISIONING_H
#define NIVALO_PROVISIONING_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>

#include "NivaloCliProvisioningPolicy.h"
#include "NivaloCliFrame.h"
#include "NivaloCliTransaction.h"
#include "NivaloDeviceStore.h"

struct NivaloRuntimeCredentials
{
    String wifiSsid, wifiPassword;
    String deviceId, mqttHost, mqttClientId, mqttUsername, mqttPassword, mqttCaCertificate;
    uint16_t mqttPort = 8883;
    String devicePrivateKeyPem, claimCodeSha256;
    bool valid() const;
};

struct NivaloPendingClaimAttempt
{
    String wifiSsid, wifiPassword, claimCode, attemptId, nonce;
    String publicKeyPem, privateKeyPem, mqttCredential, credentialSha256, signatureBase64;
    bool valid() const;
};

enum NivaloProvisioningState
{
    NIVALO_PROVISIONING_STARTING,
    NIVALO_PROVISIONING_CONNECTING_WIFI,
    NIVALO_PROVISIONING_SYNCING_TIME,
    NIVALO_PROVISIONING_SETUP_PORTAL,
    NIVALO_PROVISIONING_CLAIMING,
    NIVALO_PROVISIONING_READY,
    NIVALO_PROVISIONING_ERROR
};

struct NivaloProvisioningConfig
{
    const char *claimUrl = "https://iot-api.nivalo.io/v1/device-claims/exchange";
    const char *hardwareType = "esp32";
    const char *claimCaCertificate = NULL;
    const char *softApPrefix = "Nivalo-Setup";
    int setupButtonPin = -1;
    bool setupButtonActiveLow = true;
    unsigned long setupButtonHoldMs = 3000UL;
    unsigned long wifiConnectTimeoutMs = 20000UL;
    const char *primaryNtpServer = "pool.ntp.org";
    const char *secondaryNtpServer = "time.cloudflare.com";
    unsigned long timeSyncTimeoutMs = 15000UL;
    unsigned long timeSyncRetryBaseMs = 2000UL;
    unsigned long timeSyncRetryMaximumMs = 30000UL;
    uint8_t timeSyncMaximumAttempts = 3U;
    // A commissioned unit that lost its saved WLAN opens the setup access point and keeps retrying
    // the WLAN in the background, first after this delay, doubling up to the maximum.
    unsigned long wifiRetryBaseMs = 30000UL;
    unsigned long wifiRetryMaximumMs = 300000UL;
    // Accept plain (unencrypted) NVS for the Wi-Fi password, MQTT credentials and claim proof key.
    // Anyone with physical access can then read them; see the README. Without it, provisioning
    // runs only where NVS encryption is actually active.
    bool allowPlaintextNvs = false;
    const NivaloRuntimeCredentials *localDeveloperFixture = NULL;
    bool enableCliSerial = true;
    Stream *cliSerial = NULL;
};

class NivaloProvisioningStore
{
public:
    bool begin(bool allowPlaintextNvs);
    bool load(NivaloRuntimeCredentials &credentials);
    bool commit(const NivaloRuntimeCredentials &credentials);
    // The pending claim attempt, which is also the recovery marker of a claim commit: OK, MISSING
    // (never written, or superseded), CORRUPT or UNREADABLE (a failed read is never absence).
    NivaloStoreRead loadPending(NivaloPendingClaimAttempt &attempt);
    bool savePending(const NivaloPendingClaimAttempt &attempt);
    bool clearPending();
    // A staged CLI transaction (manual credentials for an identity whose protocol state exists).
    bool loadCliStaged(NivaloRuntimeCredentials &credentials);
    bool stageCli(const NivaloRuntimeCredentials &credentials);
    bool clearCliStaged();
    bool clear();
    // True only when the credential namespace is actually encrypted (NVS encryption with its key
    // partition, under flash encryption), never because of the flash-encryption eFuse alone.
    bool storageProtected() const;
private:
    Preferences _preferences;
    bool _opened = false;
    bool _protected = false;
};

class NivaloProvisioning
{
public:
    NivaloProvisioning();
    ~NivaloProvisioning();
    NivaloProvisioning(const NivaloProvisioning &) = delete;
    NivaloProvisioning &operator=(const NivaloProvisioning &) = delete;
    bool begin(const NivaloProvisioningConfig &config);
    void loop();
    void enterSetupMode();
    bool factoryReset();
    bool ready() const { return _state == NIVALO_PROVISIONING_READY; }
    NivaloProvisioningState state() const { return _state; }
    const NivaloRuntimeCredentials &credentials() const { return _credentials; }
    const char *lastError() const { return _lastError.c_str(); }
    // Whether credentials are stored in encrypted NVS (false on plain NVS).
    bool storageProtected() const { return _store.storageProtected(); }

private:
    void handleCliSerial();
    void handleCliLine(char *line, size_t length);
    bool handleCliProvision(JsonObject root, const String &requestId);
    bool handleCliClaim(JsonObject root, const String &requestId);
    bool handleCliSetupCode(JsonObject root);
    // The per-unit setup code from the factory namespace, which no reset clears.
    bool loadSetupCode(char code[17]);
    // Protocol state (command journal, boot counter) after a claim of deviceId: initialized when
    // it belongs to another identity, kept when it is deviceId's (an interrupted commit).
    bool prepareClaimedState(const String &deviceId);
    bool finishCliProvisioning(const NivaloRuntimeCredentials &replacement, bool stageFirst);
    // The protocol state of deviceId (journal and boot counter) reads back: the condition for any
    // claim recovery ACK or pending-marker cleanup.
    bool protocolStateReady(const String &deviceId);
    void sendCliResponse(const char *schema, const String &requestId, bool ok, bool includeIdentity = false);
    void sendCliClaimResponse(const String &requestId, bool ok);
    void failCliClaim();
    void resetCliFrame();
    void startWifi(const String &ssid, const String &password);
    void startTimeSync(bool resetAttempts);
    void continueAfterTimeSync();
    bool clockPermitsTls() const;
    // keepStation: a commissioned unit that lost its saved WLAN keeps retrying it (AP+STA).
    void startPortal(bool keepStation = false);
    void stopPortal();
    // Portal for a commissioned unit whose saved WLAN (or clock) is unavailable: Wi-Fi changes
    // only, with bounded background retries of the saved WLAN.
    void startOutagePortal();
    void serviceOutagePortal();
    void handlePortalRoot();
    void handlePortalSubmit();
    void handleNotFound();
    // The current HTTP client is a station of the setup access point (not the station-side LAN).
    bool fromSetupNetwork();
    void refuseOffSetupNetwork();
    bool exchangeClaim();
    bool verifyExistingIdentity();
    bool createPendingAttempt(const String &claimCode, const String &ssid, const String &wifiPassword);
    String hardwareId() const;
    void setError(const char *message);

    NivaloProvisioningConfig _config;
    NivaloProvisioningStore _store;
    NivaloDeviceStore _deviceState;
    NivaloRuntimeCredentials _credentials, _pending;
    NivaloPendingClaimAttempt _claimAttempt;
    NivaloProvisioningState _state = NIVALO_PROVISIONING_STARTING;
    DNSServer _dns;
    WebServer _web;
    bool _portalRunning = false;
    bool _dnsRunning = false;
    bool _changingWifiOnly = false;
    bool _outagePortal = false;
    bool _wifiAttempting = false;
    bool _webRoutes = false;
    unsigned long _wifiRetryAt = 0U;
    unsigned long _wifiRetryDelayMs = 0U;
    unsigned long _stateStartedAt = 0U;
    unsigned long _buttonPressedAt = 0U;
    unsigned long _wifiLostAt = 0U;
    uint8_t _timeSyncFailures = 0U;
    unsigned long _timeSyncRetryAt = 0U;
    bool _timeSyncWaitingToRetry = false;
    String _pendingClaimCode;
    uint8_t _claimFailures = 0U;
    unsigned long _claimRetryAt = 0U;
    String _lastError;
    String _cliClaimRequestId;
    Stream *_cliSerial = NULL;
    char *_cliLine = NULL;
    NivaloCliFrame _cliFrame = NivaloCliFrame(
        NivaloCliProvisioningPolicy::MaximumLineLength,
        NivaloCliProvisioningPolicy::FrameTimeoutMs);
    unsigned long _cliRestartAt = 0U;
    bool _cliProvisioningAllowed = false;
};

#endif
