#ifndef NIVALO_CONNECTION_H
#define NIVALO_CONNECTION_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "NivaloReconnectPolicy.h"

// Builds the Last Will for the CONNECT attempt about to be made (a fresh envelope with that
// attempt's session epoch). Returning false skips the attempt.
typedef bool (*NivaloWillBuilder)(void *context, String &will);

class NivaloConnection
{
public:
    NivaloConnection();
    static const char *defaultCaCertificate();
    void useTls(const char *caCertificate);
    void usePlaintext();
    // MQTT 3.1.1, clean session, keepalive 60 s, Last Will on availability (QoS 1, retained).
    // Both downlink filters are subscribed at QoS 1 after CONNECT.
    void configure(const char *host, uint16_t port, const char *clientId,
                   const char *username, const char *password,
                   const char *commandsFilter, const char *otaFilter, const char *availabilityTopic,
                   NivaloWillBuilder willBuilder, void *willContext);
    void service(unsigned long now);
    bool connected();
    bool publish(const char *topic, const char *payload, bool retained = false);
    void disconnect();
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
    bool _justConnected = false;
};

#endif
