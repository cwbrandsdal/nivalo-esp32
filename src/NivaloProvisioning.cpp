#include "NivaloProvisioning.h"
#include "NivaloCliClaimRecoveryPolicy.h"
#include "NivaloConnection.h"
#include "NivaloProvisioningTimePolicy.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <esp_flash_encrypt.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <new>
#include <time.h>


static bool isCanonicalUuid(const String &value)
{
    if (value.length() != 36U) return false;
    for (size_t i = 0; i < value.length(); i++)
    {
        if (i == 8U || i == 13U || i == 18U || i == 23U)
        {
            if (value[i] != '-') return false;
        }
        else if (!isxdigit((unsigned char)value[i])) return false;
    }
    return true;
}

static bool isLowerHexSha256(const String &value)
{
    if (value.length() != 64U) return false;
    for (size_t index = 0U; index < value.length(); ++index)
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) return false;
    return true;
}

static bool sha256Text(const String &value, String &output)
{
    uint8_t digest[32] = {0};
    if (mbedtls_sha256_ret(
            reinterpret_cast<const unsigned char *>(value.c_str()),
            value.length(), digest, 0) != 0) return false;
    char encoded[65] = {0};
    for (size_t index = 0U; index < sizeof(digest); ++index)
        snprintf(encoded + (index * 2U), 3U, "%02x", digest[index]);
    output = encoded;
    volatile uint8_t *wipe = digest;
    for (size_t index = 0U; index < sizeof(digest); ++index) wipe[index] = 0U;
    return true;
}

static bool constantTimeEqual(const String &left, const String &right)
{
    if (left.length() != right.length()) return false;
    unsigned char difference = 0U;
    for (size_t index = 0U; index < left.length(); ++index)
        difference |= static_cast<unsigned char>(left[index]) ^
                      static_cast<unsigned char>(right[index]);
    return difference == 0U;
}

bool NivaloRuntimeCredentials::valid() const
{
    return wifiSsid.length() > 0U && deviceId.length() > 0U && mqttHost.length() > 0U &&
           (mqttPort == 8883U || mqttPort == 8884U) && mqttClientId.length() > 0U && mqttUsername.length() > 0U &&
           mqttPassword.length() >= 16U &&
           (claimCodeSha256.length() == 0U || isLowerHexSha256(claimCodeSha256));
}
bool NivaloPendingClaimAttempt::valid() const
{
    return wifiSsid.length()>0U && claimCode.length()==8U && isCanonicalUuid(attemptId) && nonce.length()==32U &&
           publicKeyPem.length()>100U && privateKeyPem.length()>100U && mqttCredential.length()>=43U &&
           credentialSha256.length()==64U && signatureBase64.length()>20U;
}

static String serializeCredentials(const NivaloRuntimeCredentials &c)
{
    DynamicJsonDocument doc(4096);
    doc["wifiSsid"] = c.wifiSsid; doc["wifiPassword"] = c.wifiPassword;
    doc["deviceId"] = c.deviceId; doc["mqttHost"] = c.mqttHost; doc["mqttPort"] = c.mqttPort;
    doc["mqttClientId"] = c.mqttClientId; doc["mqttUsername"] = c.mqttUsername;
    doc["mqttPassword"] = c.mqttPassword; doc["mqttCaCertificate"] = c.mqttCaCertificate;
    doc["devicePrivateKeyPem"] = c.devicePrivateKeyPem;
    if (c.claimCodeSha256.length() > 0U) doc["claimCodeSha256"] = c.claimCodeSha256;
    String output; serializeJson(doc, output); return output;
}

static const char CliStageTombstone[] = "superseded";
static const char PendingClaimTombstone[] = "superseded";

static bool parseCredentials(const String &json, NivaloRuntimeCredentials &c)
{
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
    c.wifiSsid = doc["wifiSsid"] | ""; c.wifiPassword = doc["wifiPassword"] | "";
    c.deviceId = doc["deviceId"] | ""; c.mqttHost = doc["mqttHost"] | ""; c.mqttPort = doc["mqttPort"] | 0;
    c.mqttClientId = doc["mqttClientId"] | ""; c.mqttUsername = doc["mqttUsername"] | "";
    c.mqttPassword = doc["mqttPassword"] | ""; c.mqttCaCertificate = doc["mqttCaCertificate"] | "";
    c.devicePrivateKeyPem = doc["devicePrivateKeyPem"] | "";
    c.claimCodeSha256 = doc["claimCodeSha256"] | "";
    return c.valid();
}

static String serializePending(const NivaloPendingClaimAttempt &a)
{
    DynamicJsonDocument d(4096); d["wifiSsid"]=a.wifiSsid; d["wifiPassword"]=a.wifiPassword; d["claimCode"]=a.claimCode;
    d["attemptId"]=a.attemptId; d["nonce"]=a.nonce; d["publicKeyPem"]=a.publicKeyPem; d["privateKeyPem"]=a.privateKeyPem;
    d["mqttCredential"]=a.mqttCredential; d["credentialSha256"]=a.credentialSha256; d["signatureBase64"]=a.signatureBase64;
    String out; serializeJson(d,out); return out;
}
static bool parsePending(const String &json, NivaloPendingClaimAttempt &a)
{
    DynamicJsonDocument d(4096); if(deserializeJson(d,json)!=DeserializationError::Ok)return false;
    a.wifiSsid=d["wifiSsid"]|""; a.wifiPassword=d["wifiPassword"]|""; a.claimCode=d["claimCode"]|"";
    a.attemptId=d["attemptId"]|""; a.nonce=d["nonce"]|""; a.publicKeyPem=d["publicKeyPem"]|""; a.privateKeyPem=d["privateKeyPem"]|"";
    a.mqttCredential=d["mqttCredential"]|""; a.credentialSha256=d["credentialSha256"]|""; a.signatureBase64=d["signatureBase64"]|""; return a.valid();
}

static bool pendingIdentityWasCommitted(const NivaloRuntimeCredentials &credentials,
                                        const NivaloPendingClaimAttempt &attempt)
{
    return credentials.valid() && attempt.valid() &&
           credentials.mqttPassword == attempt.mqttCredential &&
           credentials.devicePrivateKeyPem == attempt.privateKeyPem;
}

bool NivaloProvisioningStore::begin(bool allowUnencryptedDevelopment)
{
    _protected = esp_flash_encryption_enabled();
    if (!_protected && !allowUnencryptedDevelopment) return false;
    _opened = _preferences.begin("nivalo-prov", false);
    return _opened;
}
bool NivaloProvisioningStore::storageProtected() const { return _protected; }
bool NivaloProvisioningStore::load(NivaloRuntimeCredentials &credentials)
{
    if (!_opened) return false;
    String active = _preferences.getString("active", "A");
    String value = _preferences.getString(active == "B" ? "slotB" : "slotA", "");
    return value.length() > 0U && parseCredentials(value, credentials);
}
bool NivaloProvisioningStore::commit(const NivaloRuntimeCredentials &credentials)
{
    if (!_opened || !credentials.valid()) return false;
    String current = _preferences.getString("active", "A");
    const char *next = current == "A" ? "B" : "A";
    const char *key = next[0] == 'B' ? "slotB" : "slotA";
    String encoded = serializeCredentials(credentials);
    if (_preferences.putString(key, encoded) != encoded.length() || _preferences.getString(key, "") != encoded)
        return false;
    if (_preferences.putString("active", next) != 1U || _preferences.getString("active", "") != next)
        return false;
    NivaloRuntimeCredentials committed;
    return load(committed) && serializeCredentials(committed) == encoded;
}
bool NivaloProvisioningStore::clear() { return _opened && _preferences.clear(); }
bool NivaloProvisioningStore::loadPending(NivaloPendingClaimAttempt &a) { return _opened && parsePending(_preferences.getString("pending",""),a); }
bool NivaloProvisioningStore::savePending(const NivaloPendingClaimAttempt &a)
{
    if(!_opened||!a.valid())return false; String encoded=serializePending(a);
    return _preferences.putString("pending",encoded)==encoded.length() && _preferences.getString("pending","")==encoded;
}
bool NivaloProvisioningStore::clearPending()
{
    if (!_opened) return false;
    const size_t length = sizeof(PendingClaimTombstone) - 1U;
    return _preferences.putString("pending", PendingClaimTombstone) == length &&
           _preferences.getString("pending", "") == PendingClaimTombstone;
}
bool NivaloProvisioningStore::loadCliStaged(NivaloRuntimeCredentials &credentials)
{
    return _opened && parseCredentials(_preferences.getString("cliStage", CliStageTombstone), credentials);
}
bool NivaloProvisioningStore::stageCli(const NivaloRuntimeCredentials &credentials)
{
    if (!_opened || !credentials.valid()) return false;
    const String encoded = serializeCredentials(credentials);
    return _preferences.putString("cliStage", encoded) == encoded.length() &&
           _preferences.getString("cliStage", "") == encoded;
}
bool NivaloProvisioningStore::clearCliStaged()
{
    if (!_opened) return false;
    const size_t length = sizeof(CliStageTombstone) - 1U;
    return _preferences.putString("cliStage", CliStageTombstone) == length &&
           _preferences.getString("cliStage", "") == CliStageTombstone;
}

NivaloProvisioning::NivaloProvisioning() : _web(80) {}
NivaloProvisioning::~NivaloProvisioning() { delete[] _cliLine; }

bool NivaloProvisioning::begin(const NivaloProvisioningConfig &config)
{
    _config = config;
    _cliSerial = _config.enableCliSerial ? (_config.cliSerial == NULL ? &Serial : _config.cliSerial) : NULL;
    if (_cliSerial != NULL && _cliLine == NULL)
        _cliLine = new (std::nothrow) char[NivaloCliProvisioningPolicy::MaximumLineLength + 1U];
    if (_cliSerial != NULL && _cliLine == NULL)
    {
        setError("CLI serial buffer allocation failed");
        return false;
    }
    bool storeAvailable = _store.begin(_config.allowUnencryptedNvsForLocalDevelopment);
    _cliProvisioningAllowed = storeAvailable && _config.localDeveloperFixture == NULL;
    if (_config.localDeveloperFixture != NULL)
    {
        _credentials = *_config.localDeveloperFixture;
        if (!_credentials.valid()) { setError("Local developer fixture is invalid"); return false; }
        startWifi(_credentials.wifiSsid, _credentials.wifiPassword); return true;
    }
    if (!storeAvailable)
    {
        setError("Encrypted NVS/flash encryption is required"); return false;
    }
    if (_config.setupButtonPin >= 0)
        pinMode(_config.setupButtonPin, _config.setupButtonActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);
    NivaloRuntimeCredentials cliStaged;
    if (_store.loadCliStaged(cliStaged))
    {
        if (!finishCliProvisioning(cliStaged, false))
        {
            setError("Interrupted CLI provisioning recovery failed");
            return false;
        }
        startWifi(_credentials.wifiSsid, _credentials.wifiPassword);
        return true;
    }
    bool hasCredentials = _store.load(_credentials);
    bool hasPendingAttempt = _store.loadPending(_claimAttempt);
    if (hasCredentials && hasPendingAttempt && pendingIdentityWasCommitted(_credentials, _claimAttempt))
    {
        // The A/B selector is the commit point. A reset can occur after it is
        // switched but before pending cleanup; never exchange that claim again.
        if (!_store.clearPending())
        {
            setError("Committed claim cleanup recovery failed");
            return false;
        }
        _claimAttempt = NivaloPendingClaimAttempt();
        startWifi(_credentials.wifiSsid, _credentials.wifiPassword);
    }
    else if (hasPendingAttempt)
    {
        _pendingClaimCode=_claimAttempt.claimCode; _pending.wifiSsid=_claimAttempt.wifiSsid; _pending.wifiPassword=_claimAttempt.wifiPassword;
        startWifi(_claimAttempt.wifiSsid,_claimAttempt.wifiPassword);
    }
    else if (hasCredentials) startWifi(_credentials.wifiSsid, _credentials.wifiPassword);
    else startPortal();
    return true;
}

void NivaloProvisioning::startWifi(const String &ssid, const String &password)
{
    stopPortal(); WiFi.mode(WIFI_STA); WiFi.begin(ssid.c_str(), password.c_str());
    _state = NIVALO_PROVISIONING_CONNECTING_WIFI; _stateStartedAt = millis();
    _wifiLostAt = 0U; _timeSyncFailures = 0U; _timeSyncRetryAt = 0U;
    _timeSyncWaitingToRetry = false;
}

bool NivaloProvisioning::clockPermitsTls() const
{
    return NivaloProvisioningTimePolicy::permitsTls(
        static_cast<int64_t>(time(NULL)),
        static_cast<uint64_t>(_config.minimumValidEpochSeconds));
}

void NivaloProvisioning::startTimeSync(bool resetAttempts)
{
    if (resetAttempts) _timeSyncFailures = 0U;
    _timeSyncRetryAt = 0U;
    _timeSyncWaitingToRetry = false;
    configTime(0, 0, _config.primaryNtpServer, _config.secondaryNtpServer);
    _state = NIVALO_PROVISIONING_SYNCING_TIME;
    _stateStartedAt = millis();
}

void NivaloProvisioning::continueAfterTimeSync()
{
    if (_changingWifiOnly)
    {
        _pending.deviceId = _credentials.deviceId; _pending.mqttHost = _credentials.mqttHost;
        _pending.mqttPort = _credentials.mqttPort; _pending.mqttClientId = _credentials.mqttClientId;
        _pending.mqttUsername = _credentials.mqttUsername; _pending.mqttPassword = _credentials.mqttPassword;
        _pending.mqttCaCertificate = _credentials.mqttCaCertificate;
        _pending.devicePrivateKeyPem = _credentials.devicePrivateKeyPem;
        // A claim receipt authenticates only the exact Wi-Fi request that was
        // durably acknowledged. Any later credential mutation invalidates it.
        _pending.claimCodeSha256 = "";
        if (verifyExistingIdentity() && _store.commit(_pending))
        {
            _credentials = _pending; _state = NIVALO_PROVISIONING_READY;
        }
        else
        {
            _lastError = "New Wi-Fi could not validate the existing MQTT TLS identity";
            startPortal();
        }
    }
    else if (_pendingClaimCode.length() > 0U) _state = NIVALO_PROVISIONING_CLAIMING;
    else _state = NIVALO_PROVISIONING_READY;
    _stateStartedAt = millis();
}

void NivaloProvisioning::loop()
{
    handleCliSerial();
    if (_portalRunning) { _dns.processNextRequest(); _web.handleClient(); }
    if (_config.setupButtonPin >= 0)
    {
        bool pressed = digitalRead(_config.setupButtonPin) == (_config.setupButtonActiveLow ? LOW : HIGH);
        if (pressed && _buttonPressedAt == 0U) _buttonPressedAt = millis();
        if (!pressed) _buttonPressedAt = 0U;
        if (pressed && millis() - _buttonPressedAt >= _config.setupButtonHoldMs) { _buttonPressedAt = 0U; enterSetupMode(); }
    }
    if (_state == NIVALO_PROVISIONING_CONNECTING_WIFI)
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            startTimeSync(true);
        }
        else if (millis() - _stateStartedAt >= _config.wifiConnectTimeoutMs)
        {
            failCliClaim();
            startPortal();
        }
    }
    else if (_state == NIVALO_PROVISIONING_SYNCING_TIME)
    {
        const unsigned long now = millis();
        if (WiFi.status() != WL_CONNECTED)
        {
            if (_wifiLostAt == 0U) _wifiLostAt = now;
            else if (NivaloProvisioningTimePolicy::elapsed(now, _wifiLostAt, _config.wifiConnectTimeoutMs))
            {
                failCliClaim();
                startPortal();
            }
            return;
        }
        _wifiLostAt = 0U;
        if (clockPermitsTls())
        {
            continueAfterTimeSync();
        }
        else if (_timeSyncWaitingToRetry)
        {
            if (NivaloProvisioningTimePolicy::retryDue(now, _timeSyncRetryAt)) startTimeSync(false);
        }
        else if (NivaloProvisioningTimePolicy::elapsed(now, _stateStartedAt, _config.timeSyncTimeoutMs))
        {
            ++_timeSyncFailures;
            if (!NivaloProvisioningTimePolicy::mayRetry(_timeSyncFailures, _config.timeSyncMaximumAttempts))
            {
                _lastError = "Clock synchronization timed out; TLS was not attempted";
                failCliClaim();
                startPortal();
            }
            else
            {
                _timeSyncRetryAt = now + NivaloProvisioningTimePolicy::retryDelay(
                    _timeSyncFailures,
                    _config.timeSyncRetryBaseMs,
                    _config.timeSyncRetryMaximumMs);
                _timeSyncWaitingToRetry = true;
            }
        }
    }
    else if (_state == NIVALO_PROVISIONING_CLAIMING)
    {
        if ((long)(millis()-_claimRetryAt)<0) return;
        if (exchangeClaim() && verifyExistingIdentity() && _store.commit(_pending))
        {
            _credentials = _pending;
            if (!_store.clearPending())
            {
                _lastError = "Committed claim cleanup failed";
                failCliClaim();
                _state = NIVALO_PROVISIONING_ERROR;
            }
            else
            {
                _pendingClaimCode = "";
                _claimAttempt = NivaloPendingClaimAttempt();
                _state = NIVALO_PROVISIONING_READY;
                if (_cliClaimRequestId.length() > 0U)
                {
                    sendCliClaimResponse(_cliClaimRequestId, true);
                    _cliClaimRequestId = "";
                }
            }
        }
        else if (++_claimFailures >= 5U)
        {
            failCliClaim();
            startPortal();
        }
        else { _claimRetryAt=millis()+min(60000UL,5000UL*(1UL<<(_claimFailures-1U))); }
    }
    else if (_state == NIVALO_PROVISIONING_READY)
    {
        if (WiFi.status() == WL_CONNECTED) _wifiLostAt = 0U;
        else if (_wifiLostAt == 0U) _wifiLostAt = millis();
        else if (millis() - _wifiLostAt >= _config.wifiConnectTimeoutMs)
        {
            _changingWifiOnly = true;
            startPortal();
        }
    }
}

void NivaloProvisioning::resetCliFrame()
{
    _cliFrame.reset(_cliLine);
}

void NivaloProvisioning::handleCliSerial()
{
    if (_cliRestartAt != 0U && (long)(millis() - _cliRestartAt) >= 0)
    {
        if (_cliSerial != NULL) _cliSerial->flush();
        ESP.restart();
        return;
    }
    if (_cliSerial == NULL || _cliLine == NULL) return;
    _cliFrame.expire(millis(), _cliLine);

    size_t processed = 0U;
    while (_cliSerial->available() > 0 && processed++ < 256U)
    {
        const int next = _cliSerial->read();
        if (next < 0) break;
        const NivaloCliFrameResult result = _cliFrame.push(static_cast<char>(next), millis(), _cliLine);
        if (result == NIVALO_CLI_FRAME_READY)
        {
            const size_t length = _cliFrame.length();
            _cliLine[length] = '\0';
            handleCliLine(_cliLine, length);
            resetCliFrame();
            if (_cliRestartAt != 0U) return;
        }
        else if (result == NIVALO_CLI_FRAME_DROPPED) resetCliFrame();
    }
}

void NivaloProvisioning::handleCliLine(char *line, size_t length)
{
    DynamicJsonDocument request(4096);
    if (deserializeJson(request, line, length) != DeserializationError::Ok || !request.is<JsonObject>()) return;
    JsonObject root = request.as<JsonObject>();
    if (!root["schema"].is<const char *>() || !root["requestId"].is<const char *>()) return;
    const String schema = root["schema"].as<String>();
    const String requestId = root["requestId"].as<String>();
    if (!NivaloCliProvisioningPolicy::isRequestId(requestId.c_str(), requestId.length())) return;

    if (schema == "nivalo.cli.identify.v1")
    {
        if (root.size() != 2U)
        {
            sendCliResponse("nivalo.cli.identify.v1", requestId, false);
            return;
        }
        sendCliResponse("nivalo.cli.identify.v1", requestId, true, true);
    }
    else if (schema == "nivalo.cli.provision.v1")
    {
        const bool ok = handleCliProvision(root, requestId);
        sendCliResponse("nivalo.cli.provision.v1", requestId, ok);
        if (ok) _cliRestartAt = millis() + 500UL;
    }
    else if (schema == "nivalo.cli.claim.v1")
    {
        if (!handleCliClaim(root, requestId))
            sendCliClaimResponse(requestId, false);
    }
}

bool NivaloProvisioning::handleCliClaim(JsonObject root, const String &requestId)
{
    if (!_cliProvisioningAllowed || _cliClaimRequestId.length() > 0U || root.size() != 5U ||
        !root["expectedHardwareId"].is<const char *>() ||
        !root["wifi"].is<JsonObject>() || !root["claim"].is<JsonObject>()) return false;
    JsonObject wifi = root["wifi"].as<JsonObject>();
    JsonObject claim = root["claim"].as<JsonObject>();
    if (wifi.size() != 2U || claim.size() != 1U ||
        !wifi["ssid"].is<const char *>() || !wifi["password"].is<const char *>() ||
        !claim["code"].is<const char *>()) return false;

    const String expectedHardwareId = root["expectedHardwareId"].as<String>();
    const String wifiSsid = wifi["ssid"].as<String>();
    const String wifiPassword = wifi["password"].as<String>();
    String claimCode = claim["code"].as<String>();
    claimCode.toUpperCase();
    if (!NivaloCliProvisioningPolicy::isHardwareId(
            expectedHardwareId.c_str(), expectedHardwareId.length()) ||
        expectedHardwareId != hardwareId() ||
        !NivaloCliProvisioningPolicy::isWifiSsid(wifiSsid.c_str(), wifiSsid.length()) ||
        !NivaloCliProvisioningPolicy::isWifiPassword(wifiPassword.c_str(), wifiPassword.length()) ||
        !NivaloCliProvisioningPolicy::isClaimCode(claimCode.c_str(), claimCode.length())) return false;

    String claimReceipt;
    if (!sha256Text(claimCode, claimReceipt)) return false;
    if (_credentials.valid())
    {
        const bool matches = isLowerHexSha256(_credentials.claimCodeSha256) &&
                             constantTimeEqual(_credentials.claimCodeSha256, claimReceipt) &&
                             _credentials.wifiSsid == wifiSsid &&
                             constantTimeEqual(_credentials.wifiPassword, wifiPassword);
        claimReceipt = "";
        const bool readyState = _state == NIVALO_PROVISIONING_READY;
        const bool recoveryRequired =
            _state == NIVALO_PROVISIONING_STARTING ||
            _state == NIVALO_PROVISIONING_SETUP_PORTAL ||
            _state == NIVALO_PROVISIONING_ERROR;
        const NivaloCliClaimRecoveryAction recoveryAction =
            NivaloCliClaimRecoveryPolicy::resumeAction(
                readyState,
                recoveryRequired,
                WiFi.status() == WL_CONNECTED,
                clockPermitsTls());
        if (!matches || recoveryAction == NIVALO_CLI_CLAIM_REJECT_REPLAY ||
            !_store.clearPending()) return false;
        _claimAttempt = NivaloPendingClaimAttempt();
        _pendingClaimCode = "";
        _changingWifiOnly = false;
        switch (recoveryAction)
        {
        case NIVALO_CLI_CLAIM_REJECT_REPLAY:
            return false;
        case NIVALO_CLI_CLAIM_CONNECT_WIFI:
            startWifi(_credentials.wifiSsid, _credentials.wifiPassword);
            break;
        case NIVALO_CLI_CLAIM_SYNC_TIME:
            startTimeSync(true);
            break;
        case NIVALO_CLI_CLAIM_READY:
            _state = NIVALO_PROVISIONING_READY;
            break;
        case NIVALO_CLI_CLAIM_PRESERVE_STATE:
        default:
            break;
        }
        sendCliClaimResponse(requestId, true);
        return true;
    }
    claimReceipt = "";

    _pending = NivaloRuntimeCredentials();
    _pending.wifiSsid = wifiSsid;
    _pending.wifiPassword = wifiPassword;
    _pendingClaimCode = claimCode;
    const bool reuseCliPendingAttempt = _claimAttempt.valid() && claimCode == _claimAttempt.claimCode;
    if (reuseCliPendingAttempt)
    {
        _claimAttempt.wifiSsid = wifiSsid;
        _claimAttempt.wifiPassword = wifiPassword;
    }
    else if (!createPendingAttempt(claimCode, wifiSsid, wifiPassword)) return false;
    if (!_store.savePending(_claimAttempt)) return false;

    _changingWifiOnly = false;
    _claimFailures = 0U;
    _claimRetryAt = 0U;
    _cliClaimRequestId = requestId;
    startWifi(wifiSsid, wifiPassword);
    return true;
}

bool NivaloProvisioning::handleCliProvision(JsonObject root, const String &requestId)
{
    (void)requestId;
    if (!_cliProvisioningAllowed || _cliClaimRequestId.length() > 0U || root.size() != 4U ||
        !root["wifi"].is<JsonObject>() || !root["mqtt"].is<JsonObject>()) return false;
    JsonObject wifi = root["wifi"].as<JsonObject>();
    JsonObject mqtt = root["mqtt"].as<JsonObject>();
    if (wifi.size() != 2U || mqtt.size() != 7U ||
        !wifi["ssid"].is<const char *>() || !wifi["password"].is<const char *>() ||
        !mqtt["deviceId"].is<const char *>() || !mqtt["host"].is<const char *>() ||
        !mqtt["port"].is<unsigned int>() || !mqtt["useTls"].is<bool>() ||
        !mqtt["clientId"].is<const char *>() || !mqtt["username"].is<const char *>() ||
        !mqtt["password"].is<const char *>()) return false;

    const String wifiSsid = wifi["ssid"].as<String>();
    const String wifiPassword = wifi["password"].as<String>();
    const String deviceId = mqtt["deviceId"].as<String>();
    const String mqttHost = mqtt["host"].as<String>();
    const unsigned int mqttPort = mqtt["port"].as<unsigned int>();
    const String mqttClientId = mqtt["clientId"].as<String>();
    const String mqttUsername = mqtt["username"].as<String>();
    const String mqttPassword = mqtt["password"].as<String>();
    if (!NivaloCliProvisioningPolicy::isWifiSsid(wifiSsid.c_str(), wifiSsid.length()) ||
        !NivaloCliProvisioningPolicy::isWifiPassword(wifiPassword.c_str(), wifiPassword.length()) ||
        !NivaloCliProvisioningPolicy::isCanonicalUuid(deviceId.c_str(), deviceId.length()) ||
        !NivaloCliProvisioningPolicy::isMqttHost(mqttHost.c_str(), mqttHost.length()) ||
        mqttPort > 65535U || !NivaloCliProvisioningPolicy::isTlsPort(static_cast<uint16_t>(mqttPort)) ||
        mqtt["useTls"].as<bool>() != true ||
        !NivaloCliProvisioningPolicy::isMqttIdentity(mqttClientId.c_str(), mqttClientId.length()) ||
        !NivaloCliProvisioningPolicy::isMqttIdentity(mqttUsername.c_str(), mqttUsername.length()) ||
        !NivaloCliProvisioningPolicy::isMqttPassword(mqttPassword.c_str(), mqttPassword.length())) return false;

    NivaloRuntimeCredentials replacement;
    replacement.wifiSsid = wifiSsid;
    replacement.wifiPassword = wifiPassword;
    replacement.deviceId = deviceId;
    replacement.mqttHost = mqttHost;
    replacement.mqttPort = static_cast<uint16_t>(mqttPort);
    replacement.mqttClientId = mqttClientId;
    replacement.mqttUsername = mqttUsername;
    replacement.mqttPassword = mqttPassword;
    if (!replacement.valid() || !finishCliProvisioning(replacement, true)) return false;

    _pending = NivaloRuntimeCredentials();
    _claimAttempt = NivaloPendingClaimAttempt();
    _pendingClaimCode = "";
    return true;
}

bool NivaloProvisioning::finishCliProvisioning(const NivaloRuntimeCredentials &replacement, bool stageFirst)
{
    struct Operations
    {
        NivaloProvisioningStore &store;
        const NivaloRuntimeCredentials &replacement;
        NivaloRuntimeCredentials committed;

        bool stage() { return store.stageCli(replacement); }
        bool commitActive() { return store.commit(replacement); }
        bool supersedePendingClaim() { return store.clearPending(); }
        bool verifyActive()
        {
            return store.load(committed) &&
                   serializeCredentials(committed) == serializeCredentials(replacement);
        }
        bool clearStage() { return store.clearCliStaged(); }
    } operations = {_store, replacement, NivaloRuntimeCredentials()};

    if (!replacement.valid()) return false;
    const bool completed = stageFirst
                               ? NivaloCliTransaction::start(operations)
                               : NivaloCliTransaction::resume(operations);
    if (!completed) return false;
    _credentials = operations.committed;
    return true;
}

void NivaloProvisioning::sendCliResponse(const char *schema, const String &requestId, bool ok, bool includeIdentity)
{
    if (_cliSerial == NULL) return;
    StaticJsonDocument<384> response;
    response["schema"] = schema;
    response["requestId"] = requestId;
    response["ok"] = ok;
    if (ok && includeIdentity)
    {
        uint8_t mac[6] = {0};
        if (esp_efuse_mac_get_default(mac) != ESP_OK)
        {
            response["ok"] = false;
        }
        else
        {
            char macAddress[18];
            snprintf(macAddress, sizeof(macAddress), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            response["macAddress"] = macAddress;
            response["hardwareId"] = hardwareId();
        }
    }
    serializeJson(response, *_cliSerial);
    _cliSerial->write('\n');
}

void NivaloProvisioning::sendCliClaimResponse(const String &requestId, bool ok)
{
    if (_cliSerial == NULL) return;
    StaticJsonDocument<256> response;
    response["schema"] = "nivalo.cli.claim.v1";
    response["requestId"] = requestId;
    response["ok"] = ok;
    if (ok)
    {
        const String currentHardwareId = hardwareId();
        if (!_credentials.valid() || !isCanonicalUuid(_credentials.deviceId) ||
            !NivaloCliProvisioningPolicy::isHardwareId(
                currentHardwareId.c_str(), currentHardwareId.length()))
        {
            response["ok"] = false;
        }
        else
        {
            response["hardwareId"] = currentHardwareId;
            response["deviceId"] = _credentials.deviceId;
        }
    }
    serializeJson(response, *_cliSerial);
    _cliSerial->write('\n');
    _cliSerial->flush();
}

void NivaloProvisioning::failCliClaim()
{
    if (_cliClaimRequestId.length() == 0U) return;
    sendCliClaimResponse(_cliClaimRequestId, false);
    _cliClaimRequestId = "";
}

void NivaloProvisioning::startPortal()
{
    // Entering local setup terminates any in-flight serial claim. This also
    // covers explicit setup-button and factory-reset interruptions.
    failCliClaim();
    WiFi.disconnect(false, false); WiFi.mode(WIFI_AP);
    String ap = String(_config.softApPrefix) + "-" + hardwareId().substring(hardwareId().length() - 6U);
    WiFi.softAP(ap.c_str()); _dns.start(53, "*", WiFi.softAPIP());
    if (!_portalRunning)
    {
        _web.on("/", HTTP_GET, [this]() { handlePortalRoot(); });
        _web.on("/configure", HTTP_POST, [this]() { handlePortalSubmit(); });
        _web.onNotFound([this]() { handleNotFound(); }); _web.begin();
    }
    _portalRunning = true; _state = NIVALO_PROVISIONING_SETUP_PORTAL; _stateStartedAt = millis();
}
void NivaloProvisioning::stopPortal() { if (_portalRunning) { _dns.stop(); _web.stop(); WiFi.softAPdisconnect(true); _portalRunning = false; } }
void NivaloProvisioning::enterSetupMode() { _changingWifiOnly = _credentials.valid(); startPortal(); }
bool NivaloProvisioning::factoryReset() { bool ok = _store.clear(); _credentials = NivaloRuntimeCredentials(); _changingWifiOnly = false; startPortal(); return ok; }

void NivaloProvisioning::handlePortalRoot()
{
    const char page[] = "<!doctype html><meta name=viewport content='width=device-width'><h1>Nivalo setup</h1>"
        "<form method=post action=/configure><label>Wi-Fi SSID<input name=ssid maxlength=32 required></label>"
        "<label>Wi-Fi password<input name=wifi type=password maxlength=63></label>"
        "<label>One-use claim code<input name=claim type=password maxlength=8></label><button>Connect</button></form>";
    _web.send(200, "text/html", page);
}
void NivaloProvisioning::handlePortalSubmit()
{
    String ssid = _web.arg("ssid"), password = _web.arg("wifi"), claim = _web.arg("claim"); claim.toUpperCase();
    if (ssid.length() == 0U || ssid.length() > 32U || password.length() > 63U ||
        (_changingWifiOnly ? claim.length() != 0U :
         !NivaloCliProvisioningPolicy::isClaimCode(claim.c_str(), claim.length())))
    {
        _web.send(400, "text/plain", "Invalid setup values");
        return;
    }
    _pending = NivaloRuntimeCredentials(); _pending.wifiSsid = ssid; _pending.wifiPassword = password; _pendingClaimCode = claim;
    if (!_changingWifiOnly)
    {
        bool reusePendingAttempt = _claimAttempt.valid() && claim == _claimAttempt.claimCode;
        if (reusePendingAttempt)
        {
            _claimAttempt.wifiSsid = ssid;
            _claimAttempt.wifiPassword = password;
        }
        else if (!createPendingAttempt(claim, ssid, password))
        {
            _web.send(500, "text/plain", "Could not secure pending claim"); return;
        }
        if (!_store.savePending(_claimAttempt))
        {
            _web.send(500, "text/plain", "Could not secure pending claim"); return;
        }
    }
    if (!_changingWifiOnly) { _claimFailures = 0U; _claimRetryAt = 0U; }
    _web.send(202, "text/plain", "Connecting; this setup network will close."); startWifi(ssid, password);
}
void NivaloProvisioning::handleNotFound() { _web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true); _web.send(302, "text/plain", ""); }

String NivaloProvisioning::hardwareId() const
{
    uint64_t mac = ESP.getEfuseMac(); char value[32]; snprintf(value, sizeof(value), "esp32-%04lx%08lx", (unsigned long)(mac >> 32U), (unsigned long)mac); return value;
}

bool NivaloProvisioning::createPendingAttempt(const String &claimCode, const String &ssidValue, const String &wifiPasswordValue)
{
    mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context random; mbedtls_pk_context key;
    mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&random); mbedtls_pk_init(&key);
    const char personalization[] = "nivalo-device-claim";
    int result = mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy,
        (const unsigned char *)personalization, sizeof(personalization) - 1U);
    if (result == 0) result = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (result == 0) result = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &random);
    unsigned char publicPem[512], privatePem[512];
    if (result == 0) result = mbedtls_pk_write_pubkey_pem(&key, publicPem, sizeof(publicPem));
    if (result == 0) result = mbedtls_pk_write_key_pem(&key, privatePem, sizeof(privatePem));
    uint8_t nonceBytes[16] = {0}, credentialBytes[32] = {0};
    if (result == 0) result = mbedtls_ctr_drbg_random(&random, nonceBytes, sizeof(nonceBytes));
    if (result == 0) result = mbedtls_ctr_drbg_random(&random, credentialBytes, sizeof(credentialBytes));
    char nonceText[33]; for (size_t i=0;i<16;i++) snprintf(nonceText+i*2,3,"%02x",nonceBytes[i]);
    unsigned char credentialBase64[64]; size_t credentialLength=0;
    if(result==0) result=mbedtls_base64_encode(credentialBase64,sizeof(credentialBase64),&credentialLength,credentialBytes,sizeof(credentialBytes));
    if(result==0) credentialBase64[credentialLength]='\0';
    String credential=(char*)credentialBase64; credential.replace("+","-"); credential.replace("/","_"); credential.replace("=","");
    uint8_t credentialHash[32] = {0}; char credentialHashText[65] = {0};
    if(result==0) result=mbedtls_sha256_ret((const unsigned char*)credential.c_str(),credential.length(),credentialHash,0);
    for(size_t i=0;i<32;i++) snprintf(credentialHashText+i*2,3,"%02x",credentialHash[i]);
    uint32_t a=esp_random(),b=esp_random(),c=esp_random(),d=esp_random(); char attempt[37];
    snprintf(attempt,sizeof(attempt),"%08lx-%04lx-%04lx-%04lx-%04lx%08lx",(unsigned long)a,(unsigned long)(b>>16),
        (unsigned long)((b&0x0fff)|0x4000),(unsigned long)(((c>>16)&0x3fff)|0x8000),(unsigned long)(c&0xffff),(unsigned long)d);
    String manifest = String("NIVALO-DEVICE-CLAIM-V1\n") + attempt + "\n" + claimCode + "\n" + hardwareId() + "\n" + nonceText + "\n" + credentialHashText + "\n";
    uint8_t hash[32], der[80]; size_t derLength=0, base64Length=0; unsigned char base64[128];
    if (result == 0) result = mbedtls_sha256_ret((const unsigned char *)manifest.c_str(), manifest.length(), hash, 0);
    if (result == 0) result = mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), der, &derLength, mbedtls_ctr_drbg_random, &random);
    if (result == 0) result = mbedtls_base64_encode(base64, sizeof(base64), &base64Length, der, derLength);
    if (result == 0) {
        base64[base64Length]='\0'; _claimAttempt.wifiSsid=ssidValue; _claimAttempt.wifiPassword=wifiPasswordValue;
        _claimAttempt.claimCode=claimCode; _claimAttempt.attemptId=attempt; _claimAttempt.nonce=nonceText;
        _claimAttempt.publicKeyPem=(char*)publicPem; _claimAttempt.privateKeyPem=(char*)privatePem;
        _claimAttempt.mqttCredential=credential; _claimAttempt.credentialSha256=credentialHashText; _claimAttempt.signatureBase64=(char*)base64;
    }
    mbedtls_pk_free(&key); mbedtls_ctr_drbg_free(&random); mbedtls_entropy_free(&entropy); return result == 0 && _claimAttempt.valid();
}

bool NivaloProvisioning::exchangeClaim()
{
    if (!clockPermitsTls()) { _lastError="Clock is not synchronized; claim HTTPS was not attempted"; return false; }
    if (_config.claimUrl == NULL || strncmp(_config.claimUrl, "https://", 8) != 0 || !_claimAttempt.valid()) { _lastError="Pending claim is invalid"; return false; }
    DynamicJsonDocument request(3072); request["attemptId"]=_claimAttempt.attemptId; request["claimCode"]=_claimAttempt.claimCode;
    request["mqttCredential"]=_claimAttempt.mqttCredential; request["hardware"]["hardwareId"]=hardwareId();
    request["hardware"]["hardwareType"]=_config.hardwareType; request["proof"]["algorithm"]="ecdsa-p256-sha256";
    request["proof"]["nonce"]=_claimAttempt.nonce; request["proof"]["credentialSha256"]=_claimAttempt.credentialSha256;
    request["proof"]["publicKeyPem"]=_claimAttempt.publicKeyPem; request["proof"]["value"]=_claimAttempt.signatureBase64;
    String body; serializeJson(request, body);
    WiFiClientSecure tls; tls.setCACert(_config.claimCaCertificate == NULL ? NivaloConnection::defaultCaCertificate() : _config.claimCaCertificate);
    HTTPClient http; http.setConnectTimeout(5000); http.setTimeout(5000); http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(tls, _config.claimUrl)) { _lastError="Claim HTTPS setup failed"; return false; }
    http.addHeader("Content-Type", "application/json"); int status=http.POST((uint8_t*)body.c_str(), body.length()); body="";
    if (status != 200 && status != 201) { http.end(); _lastError="Claim exchange rejected"; return false; }
    String response=http.getString(); http.end(); DynamicJsonDocument doc(4096);
    DeserializationError parseError = deserializeJson(doc,response);
    JsonVariant mqtt = doc["mqtt"];
    if (parseError != DeserializationError::Ok || !doc.is<JsonObject>() || doc.size() != 2U ||
        !doc["deviceId"].is<const char *>() || !isCanonicalUuid(doc["deviceId"].as<String>()) ||
        !mqtt.is<JsonObject>() || (mqtt.size() != 5U && mqtt.size() != 6U) ||
        (mqtt.size() == 6U && !mqtt.containsKey("caCertificatePem")) ||
        !mqtt["host"].is<const char *>() || !mqtt["clientId"].is<const char *>() || !mqtt["username"].is<const char *>() ||
        !mqtt["port"].is<int>() || (mqtt["port"].as<int>() != 8883 && mqtt["port"].as<int>() != 8884) ||
        !mqtt["useTls"].is<bool>() || mqtt["useTls"].as<bool>() != true ||
        (mqtt.containsKey("caCertificatePem") && !mqtt["caCertificatePem"].isNull() && !mqtt["caCertificatePem"].is<const char *>()) ||
        mqtt.containsKey("password") || doc.containsKey("claimCode") || doc.containsKey("claimSecret"))
    { _lastError="Claim response invalid"; return false; }
    _pending.deviceId=doc["deviceId"]|""; _pending.mqttHost=doc["mqtt"]["host"]|""; _pending.mqttPort=doc["mqtt"]["port"]|0;
    _pending.mqttClientId=doc["mqtt"]["clientId"]|""; _pending.mqttUsername=doc["mqtt"]["username"]|"";
    _pending.mqttPassword=_claimAttempt.mqttCredential; _pending.mqttCaCertificate=doc["mqtt"]["caCertificatePem"]|"";
    _pending.devicePrivateKeyPem=_claimAttempt.privateKeyPem;
    if (!sha256Text(_claimAttempt.claimCode, _pending.claimCodeSha256))
    {
        _lastError="Claim response receipt could not be secured";
        return false;
    }
    if (!_pending.valid() || _pending.deviceId.length() != 36U) { _lastError="Claim response identity invalid"; return false; } return true;
}

bool NivaloProvisioning::verifyExistingIdentity()
{
    if (!clockPermitsTls()) { _lastError="Clock is not synchronized; MQTT TLS was not attempted"; return false; }
    WiFiClientSecure tls;
    tls.setCACert(_pending.mqttCaCertificate.length() > 0U ? _pending.mqttCaCertificate.c_str() : NivaloConnection::defaultCaCertificate());
    PubSubClient mqtt(tls);
    mqtt.setServer(_pending.mqttHost.c_str(), _pending.mqttPort);
    mqtt.setSocketTimeout(3);
    bool connected = mqtt.connect(_pending.mqttClientId.c_str(), _pending.mqttUsername.c_str(), _pending.mqttPassword.c_str());
    if (connected) mqtt.disconnect();
    return connected;
}
void NivaloProvisioning::setError(const char *message) { _lastError=message; _state=NIVALO_PROVISIONING_ERROR; }
