#include "NivaloProvisioning.h"
#include "NivaloConnection.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <esp_flash_encrypt.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>


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

bool NivaloRuntimeCredentials::valid() const
{
    return wifiSsid.length() > 0U && deviceId.length() > 0U && mqttHost.length() > 0U &&
           (mqttPort == 8883U || mqttPort == 8884U) && mqttClientId.length() > 0U && mqttUsername.length() > 0U &&
           mqttPassword.length() >= 16U;
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
    String output; serializeJson(doc, output); return output;
}

static bool parseCredentials(const String &json, NivaloRuntimeCredentials &c)
{
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
    c.wifiSsid = doc["wifiSsid"] | ""; c.wifiPassword = doc["wifiPassword"] | "";
    c.deviceId = doc["deviceId"] | ""; c.mqttHost = doc["mqttHost"] | ""; c.mqttPort = doc["mqttPort"] | 0;
    c.mqttClientId = doc["mqttClientId"] | ""; c.mqttUsername = doc["mqttUsername"] | "";
    c.mqttPassword = doc["mqttPassword"] | ""; c.mqttCaCertificate = doc["mqttCaCertificate"] | "";
    c.devicePrivateKeyPem = doc["devicePrivateKeyPem"] | "";
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
    return _preferences.putString("active", next) == 1U;
}
bool NivaloProvisioningStore::clear() { return _opened && _preferences.clear(); }
bool NivaloProvisioningStore::loadPending(NivaloPendingClaimAttempt &a) { return _opened && parsePending(_preferences.getString("pending",""),a); }
bool NivaloProvisioningStore::savePending(const NivaloPendingClaimAttempt &a)
{
    if(!_opened||!a.valid())return false; String encoded=serializePending(a);
    return _preferences.putString("pending",encoded)==encoded.length() && _preferences.getString("pending","")==encoded;
}
bool NivaloProvisioningStore::clearPending() { return _opened && _preferences.remove("pending"); }

NivaloProvisioning::NivaloProvisioning() : _web(80) {}

bool NivaloProvisioning::begin(const NivaloProvisioningConfig &config)
{
    _config = config;
    if (_config.localDeveloperFixture != NULL)
    {
        _credentials = *_config.localDeveloperFixture;
        if (!_credentials.valid()) { setError("Local developer fixture is invalid"); return false; }
        startWifi(_credentials.wifiSsid, _credentials.wifiPassword); return true;
    }
    if (!_store.begin(_config.allowUnencryptedNvsForLocalDevelopment))
    {
        setError("Encrypted NVS/flash encryption is required"); return false;
    }
    if (_config.setupButtonPin >= 0)
        pinMode(_config.setupButtonPin, _config.setupButtonActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);
    bool hasCredentials = _store.load(_credentials);
    bool hasPendingAttempt = _store.loadPending(_claimAttempt);
    if (hasCredentials && hasPendingAttempt && pendingIdentityWasCommitted(_credentials, _claimAttempt))
    {
        // The A/B selector is the commit point. A reset can occur after it is
        // switched but before pending cleanup; never exchange that claim again.
        _store.clearPending();
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
}

void NivaloProvisioning::loop()
{
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
            if (_changingWifiOnly)
            {
                _pending.deviceId = _credentials.deviceId; _pending.mqttHost = _credentials.mqttHost;
                _pending.mqttPort = _credentials.mqttPort; _pending.mqttClientId = _credentials.mqttClientId;
                _pending.mqttUsername = _credentials.mqttUsername; _pending.mqttPassword = _credentials.mqttPassword;
                _pending.mqttCaCertificate = _credentials.mqttCaCertificate;
                _pending.devicePrivateKeyPem = _credentials.devicePrivateKeyPem;
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
        else if (millis() - _stateStartedAt >= _config.wifiConnectTimeoutMs) startPortal();
    }
    else if (_state == NIVALO_PROVISIONING_CLAIMING)
    {
        if ((long)(millis()-_claimRetryAt)<0) return;
        if (exchangeClaim() && verifyExistingIdentity() && _store.commit(_pending))
        {
            _credentials = _pending; _pendingClaimCode = ""; _store.clearPending(); _claimAttempt=NivaloPendingClaimAttempt(); _state = NIVALO_PROVISIONING_READY;
        }
        else if (++_claimFailures >= 5U) { startPortal(); }
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

void NivaloProvisioning::startPortal()
{
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
        (!_changingWifiOnly && claim.length() != 8U)) { _web.send(400, "text/plain", "Invalid setup values"); return; }
    for (size_t i = 0; i < claim.length(); i++) if (!((claim[i] >= 'A' && claim[i] <= 'Z') || (claim[i] >= '2' && claim[i] <= '9'))) { _web.send(400, "text/plain", "Invalid setup values"); return; }
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
    if (!_pending.valid() || _pending.deviceId.length() != 36U) { _lastError="Claim response identity invalid"; return false; } return true;
}

bool NivaloProvisioning::verifyExistingIdentity()
{
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
