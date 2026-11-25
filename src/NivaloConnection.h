#ifndef NIVALO_CONNECTION_H
#define NIVALO_CONNECTION_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "NivaloReconnectPolicy.h"

// Builds the Last Will for the CONNECT attempt about to be made (a fresh envelope with that
// attempt's session epoch) in a buffer the builder owns, valid until the attempt returns. NULL
// skips the attempt.
typedef const char *(*NivaloWillBuilder)(void *context);

// Bounds on one blocking connect, so it stays inside the loop's watchdog budget. DNS adds up to
// 15 s that the Arduino core does not let us shorten.
static const uint32_t NivaloTcpConnectTimeoutS = 5U;
static const uint32_t NivaloTlsHandshakeTimeoutS = 10U;
static const uint16_t NivaloMqttSocketTimeoutS = 5U;

class NivaloConnection
{
public:
    NivaloConnection();
    static const char *defaultCaCertificate();
    void useTls(const char *caCertificate);
    void usePlaintext();
    // The packet buffer, allocated before any state changes; false when it cannot be.
    bool reserve(uint16_t bufferBytes);
    // MQTT 3.1.1, clean session, keepalive 60 s, Last Will on availability (QoS 1, retained).
    // Both downlink filters are subscribed at QoS 1 after CONNECT. False when a copy of the
    // settings could not be made.
    bool configure(const char *host, uint16_t port, const char *clientId,
                   const char *username, const char *password,
                   const char *commandsFilter, const char *otaFilter, const char *availabilityTopic,
                   NivaloWillBuilder willBuilder, void *willContext);
    void service(unsigned long now);
    bool connected();
    bool publish(const char *topic, const char *payload, bool retained = false);
    // Graceful: DISCONNECT, so the broker discards the Will.
    void disconnect();
    // Closes the transport without DISCONNECT, so the broker publishes the Will.
    void drop();
    PubSubClient &client();
    // True once after a CONNECT whose two SUBSCRIBE packets were both written. PubSubClient
    // cannot observe SUBACK, so this is the profile's point for the birth (section 8.1).
    bool takeJustConnected();
    unsigned long nextAttemptAt() const;
    unsigned long currentBackoffMs() const;

private:
    WiFiClient _plainClient;
    WiFiClientSecure _secureClient;
    PubSubClient _mqtt;
    NivaloReconnectPolicy _retryPolicy;
    String _host, _clientId, _username, _password, _commandsFilter, _otaFilter, _availabilityTopic;
    NivaloWillBuilder _willBuilder = NULL;
    void *_willContext = NULL;
    uint16_t _port = 8883;
    bool _plaintext = false;
    bool _justConnected = false;
};

#endif
