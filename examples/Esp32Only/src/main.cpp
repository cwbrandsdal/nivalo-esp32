#include <Arduino.h>
#include <WiFi.h>
#include <NivaloDevice.h>
#include <NivaloProvisioning.h>

#if NIVALO_BROWSER_FLASH_ARTIFACT
#include "nivalo_browser_flash_config.h"
#if !defined(NIVALO_BROWSER_FLASH_ALLOW_UNENCRYPTED_NVS_FOR_EVALUATION) || \
    NIVALO_BROWSER_FLASH_ALLOW_UNENCRYPTED_NVS_FOR_EVALUATION != 1
#error "The current browser artifact is evaluation-only and requires its explicit storage policy."
#endif
#define NIVALO_EFFECTIVE_HARDWARE_NAME "adafruit-feather-esp32-browser-evaluation"
#else
#if __has_include("nivalo_config.h")
#include "nivalo_config.h"
#else
#include "nivalo_config.example.h"
#endif
#define NIVALO_EFFECTIVE_HARDWARE_NAME NIVALO_IOT_HARDWARE_NAME
#endif

#if NIVALO_BROWSER_FLASH_ARTIFACT && NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE
#error "Browser provisioning artifacts cannot contain a local developer fixture."
#endif

#include "nivalo_firmware_trust.h"

#ifndef NIVALO_ALLOW_PLAINTEXT_NVS
#define NIVALO_ALLOW_PLAINTEXT_NVS 0
#endif

// A staging or production image always carries its release trust anchor; a reserved test key
// (specs/cloud-mqtt-v1.1.md section 11.2) never builds into one.
#if (defined(NIVALO_ENVIRONMENT_STAGING) || defined(NIVALO_ENVIRONMENT_PRODUCTION)) && \
    (!defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) || !defined(NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM))
#error "This staging/production environment has no firmware trust anchor in nivalo_firmware_trust.h"
#endif
#if defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) || defined(NIVALO_FIRMWARE_PREVIOUS_KEY_ID)
static constexpr bool nivaloTestKeyId(const char *keyId)
{
    return keyId[0] == 't' && keyId[1] == 'e' && keyId[2] == 's' && keyId[3] == 't' && keyId[4] == '-';
}
#endif
#ifdef NIVALO_FIRMWARE_CURRENT_KEY_ID
static_assert(sizeof(NIVALO_FIRMWARE_CURRENT_KEY_ID) > 1U && !nivaloTestKeyId(NIVALO_FIRMWARE_CURRENT_KEY_ID),
              "release firmware key IDs never start with test-");
#endif
#ifdef NIVALO_FIRMWARE_PREVIOUS_KEY_ID
static_assert(sizeof(NIVALO_FIRMWARE_PREVIOUS_KEY_ID) > 1U && !nivaloTestKeyId(NIVALO_FIRMWARE_PREVIOUS_KEY_ID),
              "release firmware key IDs never start with test-");
#endif

// The environment (staging or production) is chosen by the PlatformIO environment, which sets
// NIVALO_PRODUCT_ID, NIVALO_SECURITY_VERSION and NIVALO_API_ORIGIN (see platformio.ini).
#ifndef NIVALO_API_ORIGIN
#error "NIVALO_API_ORIGIN must be set by the build environment"
#endif

NivaloDevice device;
NivaloProvisioning provisioning;
#if defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) && defined(NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM)
static const NivaloFirmwareSigningKey firmwareSigningKeys[] = {
    {NIVALO_FIRMWARE_CURRENT_KEY_ID, NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM},
#if defined(NIVALO_FIRMWARE_PREVIOUS_KEY_ID) && defined(NIVALO_FIRMWARE_PREVIOUS_PUBLIC_KEY_PEM)
    {NIVALO_FIRMWARE_PREVIOUS_KEY_ID, NIVALO_FIRMWARE_PREVIOUS_PUBLIC_KEY_PEM},
#endif
};
#endif
unsigned long lastTelemetryAt = 0;
// A start that failed (for example a temporary allocation failure) is retried after this delay;
// it changes no durable state.
static const unsigned long DeviceStartRetryMs = 30000UL;
unsigned long deviceStartAttemptAt = 0;
bool deviceStartAttempted = false;
bool deviceStarted = false;
bool ledState = false;

// setLed {"state": true|false}
static bool validSetLedArguments(JsonObjectConst arguments)
{
    return arguments.size() == 1U && arguments["state"].is<bool>();
}

static NivaloFunctionOutcome setLed(JsonObjectConst arguments)
{
    ledState = arguments["state"].as<bool>();
#ifdef LED_BUILTIN
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
#endif
    return NIVALO_FUNCTION_SUCCEEDED;
}

void setup()
{
#if NIVALO_BROWSER_FLASH_ARTIFACT
    nivaloRetainBrowserFlashDescriptor();
#endif
    Serial.begin(115200);
#ifdef LED_BUILTIN
    pinMode(LED_BUILTIN, OUTPUT);
#endif
    NivaloFunctionMetadata setLedMetadata;
    setLedMetadata.displayName = "Set LED";
    setLedMetadata.description = "Turns the built-in LED on or off.";
    setLedMetadata.argumentExample = "{\"state\":true}";
    setLedMetadata.returnType = "void";
    setLedMetadata.dangerLevel = "operational";
    setLedMetadata.validateArguments = validSetLedArguments;
    if (!device.function("setLed", setLed, setLedMetadata) || !device.variable("ledState", &ledState))
    {
        Serial.println("SDK function/variable registration failed");
    }
    NivaloProvisioningConfig provision;
    provision.claimUrl = NIVALO_DEVICE_CLAIM_URL; provision.hardwareType = NIVALO_EFFECTIVE_HARDWARE_NAME;
    provision.setupButtonPin = NIVALO_SETUP_BUTTON_PIN;
#if NIVALO_BROWSER_FLASH_ARTIFACT
    // A normally erased evaluation board has flash/NVS encryption disabled.
    // This explicit build is test-only; production artifacts must replace it
    // with a reviewed encrypted-board enablement path.
    provision.allowPlaintextNvs = true;
#else
    // Plain NVS on the owner's unencrypted boards (platformio.ini; README "Credential storage").
    provision.allowPlaintextNvs = NIVALO_ALLOW_PLAINTEXT_NVS != 0;
#endif
#if NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE
    static const NivaloRuntimeCredentials fixture = {NIVALO_DEV_WIFI_SSID, NIVALO_DEV_WIFI_PASSWORD, NIVALO_DEV_DEVICE_ID,
        NIVALO_DEV_MQTT_HOST, NIVALO_DEV_MQTT_CLIENT_ID, NIVALO_DEV_MQTT_USERNAME, NIVALO_DEV_MQTT_PASSWORD, "", NIVALO_DEV_MQTT_PORT, ""};
    provision.localDeveloperFixture = &fixture;
#endif
    provisioning.begin(provision);
}

void loop()
{
    provisioning.loop();
    if (provisioning.ready() && !deviceStarted &&
        (!deviceStartAttempted || millis() - deviceStartAttemptAt >= DeviceStartRetryMs)) {
        deviceStartAttempted = true;
        deviceStartAttemptAt = millis();
        const NivaloRuntimeCredentials &r = provisioning.credentials();
        NivaloDeviceConfig c;
        c.mqtt.deviceId = r.deviceId.c_str(); c.mqtt.clientId = r.mqttClientId.c_str(); c.mqtt.username = r.mqttUsername.c_str();
        c.mqtt.password = r.mqttPassword.c_str(); c.mqtt.host = r.mqttHost.c_str(); c.mqtt.port = r.mqttPort;
        c.mqtt.firmwareVersion = NIVALO_IOT_FIRMWARE_VERSION; c.mqtt.hardwareName = NIVALO_EFFECTIVE_HARDWARE_NAME;
        c.mqtt.transport = NIVALO_MQTT_TRANSPORT_TLS;
        c.mqtt.caCertificate = r.mqttCaCertificate.length() ? r.mqttCaCertificate.c_str() : NULL;
        c.mqtt.apiOrigin = NIVALO_API_ORIGIN;
#if defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) && defined(NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM)
        c.mqtt.firmwareSigningKeys = firmwareSigningKeys;
        c.mqtt.firmwareSigningKeyCount = sizeof(firmwareSigningKeys) / sizeof(firmwareSigningKeys[0]);
#endif
        // On failure begin() unsubscribes this task from the watchdog again, so the wait below
        // cannot reset the board.
        deviceStarted = device.begin(c);
        if (!deviceStarted)
        {
            Serial.println("Nivalo device could not start; retrying in 30 s");
        }
    }
    if (!deviceStarted) { delay(2); return; }
    device.loop();

    // Device health already travels in the free availability heartbeat; application readings
    // are billed per 16, so report them at the cadence the application needs.
    if (millis() - lastTelemetryAt > 300000UL)
    {
        lastTelemetryAt = millis();
        device.publishVariables();
    }
}
