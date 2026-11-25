#ifndef NIVALO_CLAIM_RESPONSE_H
#define NIVALO_CLAIM_RESPONSE_H

// The device-claim-v1 exchange response, checked whole before anything is probed or promoted.
// Arduino-free; the host tests drive the same code.

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

class NivaloClaimResponse
{
public:
    // device-claim-v1 bounds the response body.
    static const size_t MaxBytes = 3072U;

    NivaloClaimResponse();
    NivaloClaimResponse(const NivaloClaimResponse &) = delete;
    NivaloClaimResponse &operator=(const NivaloClaimResponse &) = delete;

    // Strict JSON of at most MaxBytes with the response schema: deviceId (canonical UUID) and mqtt
    // {host, port 8883|8884, useTls true, clientId, username, optional caCertificatePem}. Every
    // string is copied into this object: bytes may be wiped and freed as soon as it returns.
    bool parse(const char *bytes, size_t length);

    // Valid after a successful parse(), for the lifetime of this object.
    const char *deviceId() const;
    const char *mqttHost() const;
    uint16_t mqttPort() const;
    const char *mqttClientId() const;
    const char *mqttUsername() const;
    // "" when absent or null.
    const char *mqttCaCertificate() const;

private:
    DynamicJsonDocument _document;
    bool _valid = false;
};

#endif
