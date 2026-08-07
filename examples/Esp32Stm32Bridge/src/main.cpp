#include <WiFi.h>
#include <NivaloDevice.h>
#include <NivaloProvisioning.h>

#if __has_include("nivalo_config.h")
#include "nivalo_config.h"
#else
#include "nivalo_config.example.h"
#endif

namespace
{
NivaloDevice device;
NivaloProvisioning provisioning;
bool deviceStarted = false;
String macAddress;

#if defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) && defined(NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM)
const NivaloFirmwareSigningKey firmwareSigningKeys[] = {
    {NIVALO_FIRMWARE_CURRENT_KEY_ID, NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM},
#if defined(NIVALO_FIRMWARE_PREVIOUS_KEY_ID) && defined(NIVALO_FIRMWARE_PREVIOUS_PUBLIC_KEY_PEM)
    {NIVALO_FIRMWARE_PREVIOUS_KEY_ID, NIVALO_FIRMWARE_PREVIOUS_PUBLIC_KEY_PEM},
#endif
};
#endif

String readMacAddress()
{
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char text[13];
    snprintf(text, sizeof(text), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(text);
}

bool startDevice(const NivaloRuntimeCredentials &runtime)
{
    macAddress = readMacAddress();

    NivaloDeviceConfig config;
    config.mqtt.deviceId = runtime.deviceId.c_str();
    config.mqtt.clientId = runtime.mqttClientId.c_str();
    config.mqtt.username = runtime.mqttUsername.c_str();
    config.mqtt.password = runtime.mqttPassword.c_str();
    config.mqtt.host = runtime.mqttHost.c_str();
    config.mqtt.port = runtime.mqttPort;
    config.mqtt.firmwareVersion = NIVALO_IOT_FIRMWARE_VERSION;
    config.mqtt.hardwareName = NIVALO_IOT_HARDWARE_NAME;
    config.mqtt.macAddress = macAddress;
    config.mqtt.transport = NIVALO_MQTT_TRANSPORT_TLS;
    config.mqtt.caCertificate = runtime.mqttCaCertificate.length() > 0U
                                    ? runtime.mqttCaCertificate.c_str()
                                    : NULL;

#if defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) && defined(NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM)
    config.mqtt.firmwareSigningKeys = firmwareSigningKeys;
    config.mqtt.firmwareSigningKeyCount = sizeof(firmwareSigningKeys) / sizeof(firmwareSigningKeys[0]);
#endif

#if defined(NIVALO_STM32_PRESERVE_KNOWN_GOOD) && defined(NIVALO_STM32_KNOWN_GOOD_IMAGE_SIZE_BYTES)
    config.mqtt.preserveKnownGoodStm32Image = NIVALO_STM32_PRESERVE_KNOWN_GOOD != 0;
    config.mqtt.knownGoodStm32ImageSizeBytes = NIVALO_STM32_KNOWN_GOOD_IMAGE_SIZE_BYTES;
#elif defined(NIVALO_STM32_GOLDEN_IMAGE_PATH) && defined(NIVALO_STM32_GOLDEN_IMAGE_SIZE_BYTES) && defined(NIVALO_STM32_GOLDEN_IMAGE_SHA256)
    config.mqtt.stm32GoldenImagePath = NIVALO_STM32_GOLDEN_IMAGE_PATH;
    config.mqtt.stm32GoldenImageSizeBytes = NIVALO_STM32_GOLDEN_IMAGE_SIZE_BYTES;
    config.mqtt.stm32GoldenImageSha256 = NIVALO_STM32_GOLDEN_IMAGE_SHA256;
#endif

#if defined(NIVALO_STM32_DESTRUCTIVE_RECOVERY_ACCEPTANCE_ONCE)
    config.mqtt.destructiveStm32RecoveryAcceptanceOnce = true;
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
    config.allowUnencryptedNvsForLocalDevelopment =
        NIVALO_ALLOW_UNENCRYPTED_NVS_FOR_LOCAL_DEVELOPMENT != 0;

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
    if (provisioning.ready() && !deviceStarted)
    {
        deviceStarted = startDevice(provisioning.credentials());
    }
    if (deviceStarted)
    {
        device.loop();
    }
    delay(2);
}
