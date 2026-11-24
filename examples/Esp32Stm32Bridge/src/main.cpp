#include <WiFi.h>
#include <NivaloDevice.h>
#include <NivaloProvisioning.h>

#if __has_include("nivalo_config.h")
#include "nivalo_config.h"
#else
#include "nivalo_config.example.h"
#endif
#ifndef NIVALO_ALLOW_PLAINTEXT_NVS
#define NIVALO_ALLOW_PLAINTEXT_NVS 0
#endif

namespace
{
NivaloDevice device;
NivaloProvisioning provisioning;
bool deviceStartAttempted = false;
bool deviceStarted = false;

#if defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) && defined(NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM)
const NivaloFirmwareSigningKey firmwareSigningKeys[] = {
    {NIVALO_FIRMWARE_CURRENT_KEY_ID, NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM},
#if defined(NIVALO_FIRMWARE_PREVIOUS_KEY_ID) && defined(NIVALO_FIRMWARE_PREVIOUS_PUBLIC_KEY_PEM)
    {NIVALO_FIRMWARE_PREVIOUS_KEY_ID, NIVALO_FIRMWARE_PREVIOUS_PUBLIC_KEY_PEM},
#endif
};
#endif

bool startDevice(const NivaloRuntimeCredentials &runtime)
{
    NivaloDeviceConfig config;
    config.mqtt.deviceId = runtime.deviceId.c_str();
    config.mqtt.clientId = runtime.mqttClientId.c_str();
    config.mqtt.username = runtime.mqttUsername.c_str();
    config.mqtt.password = runtime.mqttPassword.c_str();
    config.mqtt.host = runtime.mqttHost.c_str();
    config.mqtt.port = runtime.mqttPort;
    config.mqtt.firmwareVersion = NIVALO_IOT_FIRMWARE_VERSION;
    config.mqtt.hardwareName = NIVALO_IOT_HARDWARE_NAME;
    config.mqtt.transport = NIVALO_MQTT_TRANSPORT_TLS;
    config.mqtt.caCertificate = runtime.mqttCaCertificate.length() > 0U
                                    ? runtime.mqttCaCertificate.c_str()
                                    : NULL;
    config.mqtt.apiOrigin = NIVALO_API_ORIGIN;

#if defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) && defined(NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM)
    config.mqtt.firmwareSigningKeys = firmwareSigningKeys;
    config.mqtt.firmwareSigningKeyCount = sizeof(firmwareSigningKeys) / sizeof(firmwareSigningKeys[0]);
#endif

    return device.begin(config);
}
} // namespace

void setup()
{
    Serial.begin(115200);

    NivaloProvisioningConfig config;
    config.claimUrl = NIVALO_DEVICE_CLAIM_URL;
    config.hardwareType = NIVALO_IOT_HARDWARE_NAME;
    config.setupButtonPin = NIVALO_SETUP_BUTTON_PIN;
    config.allowPlaintextNvs = NIVALO_ALLOW_PLAINTEXT_NVS != 0;

#if NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE
    static const NivaloRuntimeCredentials localFixture = {
        NIVALO_DEV_WIFI_SSID,
        NIVALO_DEV_WIFI_PASSWORD,
        NIVALO_DEV_DEVICE_ID,
        NIVALO_DEV_MQTT_HOST,
        NIVALO_DEV_MQTT_CLIENT_ID,
        NIVALO_DEV_MQTT_USERNAME,
        NIVALO_DEV_MQTT_PASSWORD,
        "",
        NIVALO_DEV_MQTT_PORT,
        ""};
    config.localDeveloperFixture = &localFixture;
#endif

    provisioning.begin(config);
}

void loop()
{
    provisioning.loop();
    if (provisioning.ready() && !deviceStartAttempted)
    {
        deviceStartAttempted = true;
        deviceStarted = startDevice(provisioning.credentials());
    }
    if (deviceStarted)
    {
        device.loop();
    }
    delay(2);
}
