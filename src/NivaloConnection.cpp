#include "NivaloConnection.h"

#include <esp_system.h>
#include <esp_task_wdt.h>

namespace
{
// Public trust anchors for the default Let's Encrypt chain used by Nivalo.
// Source: https://letsencrypt.org/certificates/ (ISRG Root X1 and X2).
const char DefaultRootCa[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";
}

const char *NivaloConnection::defaultCaCertificate()
{
    return DefaultRootCa;
}


namespace
{
bool copied(String &target, const char *source)
{
    target = source;
    return source != NULL && target.length() == strlen(source);
}

size_t field(const String &value)
{
    return 2U + value.length();
}
} // namespace

NivaloConnection::NivaloConnection() : _mqtt(_secureClient) {}

void NivaloConnection::useTls(const char *caCertificate)
{
    _plaintext = false;
    _secureClient.setCACert(caCertificate);
    _secureClient.setTimeout(NivaloTcpConnectTimeoutS);
    _secureClient.setHandshakeTimeout(NivaloTlsHandshakeTimeoutS);
    _mqtt.setClient(_secureClient);
}

void NivaloConnection::usePlaintext()
{
    _plaintext = true;
    _plainClient.setTimeout(NivaloTcpConnectTimeoutS);
    _mqtt.setClient(_plainClient);
}

bool NivaloConnection::reserve(uint16_t bufferBytes)
{
    return _mqtt.setBufferSize(bufferBytes) && _mqtt.getBufferSize() == bufferBytes;
}

bool NivaloConnection::configure(const char *host, uint16_t port, const char *clientId,
                                 const char *username, const char *password,
                                 const char *commandsFilter, const char *otaFilter, const char *availabilityTopic,
                                 NivaloWillBuilder willBuilder, void *willContext)
{
    _willBuilder = NULL;
    if (!copied(_host, host) || !copied(_clientId, clientId) || !copied(_username, username) ||
        !copied(_password, password) || !copied(_commandsFilter, commandsFilter) || !copied(_otaFilter, otaFilter) ||
        !copied(_availabilityTopic, availabilityTopic))
    {
        return false;
    }
    _port = port;
    _willBuilder = willBuilder;
    _willContext = willContext;
    _mqtt.setServer(_host.c_str(), _port);
    _mqtt.setKeepAlive(60);
    // Bounds the CONNACK wait and every blocking read of one connect.
    _mqtt.setSocketTimeout(NivaloMqttSocketTimeoutS);
    _retryPolicy.reset();
    return true;
}

void NivaloConnection::service(unsigned long now)
{
    if (_mqtt.connected()) { _mqtt.loop(); return; }
    const uint32_t nowMs = static_cast<uint32_t>(now);
    if (_willBuilder == NULL || !_retryPolicy.isAttemptDue(nowMs)) { return; }
    const char *will = _willBuilder(_willContext);
    // The whole CONNECT (fixed header, variable header, client ID, Will topic and message, user
    // name, password) must fit the packet buffer, or PubSubClient would refuse it mid-write.
    const size_t connectBytes = will == NULL ? 0U : 5U + 10U + field(_clientId) + field(_availabilityTopic) +
                                                        2U + strlen(will) + field(_username) + field(_password);
    if (will == NULL || connectBytes > _mqtt.getBufferSize())
    {
        _retryPolicy.onFailure(nowMs, esp_random());
        return;
    }
    const bool ok = _mqtt.connect(_clientId.c_str(), _username.c_str(), _password.c_str(),
                                  _availabilityTopic.c_str(), 1, true, will, true);
    if (!ok)
    {
        _retryPolicy.onFailure(nowMs, esp_random());
        return;
    }
    // CONNECT took its bounded share of the watchdog budget; the SUBSCRIBE writes get their own.
    (void)esp_task_wdt_reset();
    const bool commands = _mqtt.subscribe(_commandsFilter.c_str(), 1);
    const bool ota = commands && _mqtt.subscribe(_otaFilter.c_str(), 1);
    if (!ota)
    {
        // Without both subscriptions the device would announce a session it cannot serve.
        _mqtt.disconnect();
        _retryPolicy.onFailure(nowMs, esp_random());
        return;
    }
    _retryPolicy.onSuccess(nowMs);
    _justConnected = true;
}

bool NivaloConnection::connected() { return _mqtt.connected(); }
bool NivaloConnection::publish(const char *topic, const char *payload, bool retained) { return _mqtt.connected() && _mqtt.publish(topic, payload, retained); }
void NivaloConnection::disconnect() { if (_mqtt.connected()) { _mqtt.disconnect(); } }
void NivaloConnection::drop()
{
    if (_plaintext) { _plainClient.stop(); } else { _secureClient.stop(); }
}
PubSubClient &NivaloConnection::client() { return _mqtt; }
bool NivaloConnection::takeJustConnected() { bool value = _justConnected; _justConnected = false; return value; }
unsigned long NivaloConnection::nextAttemptAt() const { return _retryPolicy.nextAttemptAt(); }
unsigned long NivaloConnection::currentBackoffMs() const { return _retryPolicy.currentBackoffMs(); }
