#include <Arduino.h>
#include <WiFi.h>
#include <NivaloDevice.h>
#include <NivaloProvisioning.h>

#if __has_include("nivalo_config.h")
#include "nivalo_config.h"
#else
#include "nivalo_config.example.h"
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
    Serial.begin(115200);
#ifdef LED_BUILTIN
    pinMode(LED_BUILTIN, OUTPUT);
#endif
    if (!device.function("setLed", setLed) || !device.variable("ledState", &ledState))
    {
        Serial.println("SDK function/variable registration failed");
    }
    NivaloProvisioningConfig provision;
    provision.claimUrl = NIVALO_DEVICE_CLAIM_URL; provision.hardwareType = NIVALO_IOT_HARDWARE_NAME;
    provision.setupButtonPin = NIVALO_SETUP_BUTTON_PIN;
    provision.allowUnencryptedNvsForLocalDevelopment = NIVALO_ALLOW_UNENCRYPTED_NVS_FOR_LOCAL_DEVELOPMENT != 0;
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
        c.mqtt.hardwareName=NIVALO_IOT_HARDWARE_NAME; c.mqtt.macAddress=macAddress; c.mqtt.transport=NIVALO_MQTT_TRANSPORT_TLS;
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
