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
String macAddress;
unsigned long lastTelemetryAt = 0;
bool deviceStarted = false;
int ledState = 0;

static int setLed(String argument)
{
    argument.trim();
    argument.toLowerCase();
    if (argument == "on" || argument == "1" || argument == "true")
    {
        ledState = 1;
    }
    else if (argument == "off" || argument == "0" || argument == "false")
    {
        ledState = 0;
    }
    else
    {
        return -1;
    }
#ifdef LED_BUILTIN
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
#endif
    return ledState;
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
    setLedMetadata.argumentExample = "\"on\"";
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
    provision.allowUnencryptedNvsForLocalDevelopment = true;
#else
    provision.allowUnencryptedNvsForLocalDevelopment = NIVALO_ALLOW_UNENCRYPTED_NVS_FOR_LOCAL_DEVELOPMENT != 0;
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
    if (provisioning.ready() && !deviceStarted) {
        const NivaloRuntimeCredentials &r=provisioning.credentials(); byte mac[6]; WiFi.macAddress(mac); char mt[13];
        snprintf(mt,sizeof(mt),"%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]); macAddress=mt;
        NivaloDeviceConfig c; c.mqtt.deviceId=r.deviceId.c_str(); c.mqtt.clientId=r.mqttClientId.c_str(); c.mqtt.username=r.mqttUsername.c_str();
        c.mqtt.password=r.mqttPassword.c_str(); c.mqtt.host=r.mqttHost.c_str(); c.mqtt.port=r.mqttPort; c.mqtt.firmwareVersion=NIVALO_IOT_FIRMWARE_VERSION;
        c.mqtt.hardwareName=NIVALO_EFFECTIVE_HARDWARE_NAME; c.mqtt.macAddress=macAddress; c.mqtt.transport=NIVALO_MQTT_TRANSPORT_TLS;
        c.mqtt.caCertificate=r.mqttCaCertificate.length()?r.mqttCaCertificate.c_str():NULL;
#if defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) && defined(NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM)
        c.mqtt.firmwareSigningKeys=firmwareSigningKeys; c.mqtt.firmwareSigningKeyCount=sizeof(firmwareSigningKeys)/sizeof(firmwareSigningKeys[0]);
#endif
        deviceStarted=device.begin(c);
    }
    if (!deviceStarted) { delay(2); return; }
    device.loop();

    if (millis() - lastTelemetryAt > 30000UL)
    {
        lastTelemetryAt = millis();
        device.publishRuntimeTelemetry();
        device.publishVariables();
    }
}
