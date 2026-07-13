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
    unsigned long minimumValidEpochSeconds = 1704067200UL;
    uint8_t timeSyncMaximumAttempts = 3U;
    bool allowUnencryptedNvsForLocalDevelopment = false;
    const NivaloRuntimeCredentials *localDeveloperFixture = NULL;
    bool enableCliSerial = true;
    Stream *cliSerial = NULL;
};

class NivaloProvisioningStore
{
public:
    bool begin(bool allowUnencryptedDevelopment);
    bool load(NivaloRuntimeCredentials &credentials);
    bool commit(const NivaloRuntimeCredentials &credentials);
    bool loadPending(NivaloPendingClaimAttempt &attempt);
    bool savePending(const NivaloPendingClaimAttempt &attempt);
    bool clearPending();
    bool loadCliStaged(NivaloRuntimeCredentials &credentials);
    bool stageCli(const NivaloRuntimeCredentials &credentials);
    bool clearCliStaged();
    bool clear();
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

private:
    void handleCliSerial();
    void handleCliLine(char *line, size_t length);
    bool handleCliProvision(JsonObject root, const String &requestId);
    bool handleCliClaim(JsonObject root, const String &requestId);
    bool finishCliProvisioning(const NivaloRuntimeCredentials &replacement, bool stageFirst);
    void sendCliResponse(const char *schema, const String &requestId, bool ok, bool includeIdentity = false);
    void sendCliClaimResponse(const String &requestId, bool ok);
    void failCliClaim();
    void resetCliFrame();
    void startWifi(const String &ssid, const String &password);
    void startTimeSync(bool resetAttempts);
    void continueAfterTimeSync();
    bool clockPermitsTls() const;
    void startPortal();
    void stopPortal();
    void handlePortalRoot();
    void handlePortalSubmit();
    void handleNotFound();
    bool exchangeClaim();
    bool verifyExistingIdentity();
    bool createPendingAttempt(const String &claimCode, const String &ssid, const String &wifiPassword);
    String hardwareId() const;
    void setError(const char *message);

    NivaloProvisioningConfig _config;
    NivaloProvisioningStore _store;
    NivaloRuntimeCredentials _credentials, _pending;
    NivaloPendingClaimAttempt _claimAttempt;
    NivaloProvisioningState _state = NIVALO_PROVISIONING_STARTING;
    DNSServer _dns;
    WebServer _web;
    bool _portalRunning = false;
    bool _changingWifiOnly = false;
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
