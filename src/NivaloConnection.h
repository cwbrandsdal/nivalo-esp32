#ifndef NIVALO_CONNECTION_H
#define NIVALO_CONNECTION_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

class NivaloConnection
{
public:
    NivaloConnection();
    static const char *defaultCaCertificate();
    void useTls(const char *caCertificate);
    void usePlaintext();
    void configure(const char *host, uint16_t port, const char *clientId,
                   const char *username, const char *password,
                   const char *commandsTopic, const char *availabilityTopic);
    void setLastWill(const String &payload);
    void service(unsigned long now);
    bool connected();
    bool publish(const char *topic, const char *payload, bool retained = false);
    PubSubClient &client();
    bool takeJustConnected();
    unsigned long nextAttemptAt() const;
    unsigned long currentBackoffMs() const;

private:
    void scheduleRetry(unsigned long now);
    WiFiClient _plainClient;
    WiFiClientSecure _secureClient;
    PubSubClient _mqtt;
    String _host, _clientId, _username, _password, _commandsTopic, _availabilityTopic, _lastWill;
    uint16_t _port = 8883;
    unsigned long _nextAttemptAt = 0U;
    unsigned long _backoffMs = 1000U;
    bool _justConnected = false;
};

#endif
