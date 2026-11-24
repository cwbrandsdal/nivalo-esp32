#pragma once

// Copy this file to include/nivalo_config.h and fill in local values.
// include/nivalo_config.h is ignored by git so MQTT passwords stay local.

#define NIVALO_API_ORIGIN "https://iot-api.nivalo.io"
#define NIVALO_DEVICE_CLAIM_URL NIVALO_API_ORIGIN "/v1/device-claims/exchange"
#define NIVALO_SETUP_BUTTON_PIN 0
#define NIVALO_ALLOW_UNENCRYPTED_NVS_FOR_LOCAL_DEVELOPMENT 0
#define NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE 0

// Local development only: use NIVALO_MQTT_TRANSPORT_PLAINTEXT_LOCAL with a
// single-label, .local/.lan, loopback, link-local, or RFC1918 broker host.
// Plaintext is rejected for mqtt.nivalo.io and other public broker hosts.

#if NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE
#define NIVALO_DEV_WIFI_SSID "local-only-wifi"
#define NIVALO_DEV_WIFI_PASSWORD "local-only-password"
#define NIVALO_DEV_DEVICE_ID "00000000-0000-0000-0000-000000000000"
#define NIVALO_DEV_MQTT_HOST "mqtt.nivalo.io"
#define NIVALO_DEV_MQTT_PORT 8883
#define NIVALO_DEV_MQTT_CLIENT_ID "device-local-fixture"
#define NIVALO_DEV_MQTT_USERNAME "device-local-fixture"
#define NIVALO_DEV_MQTT_PASSWORD "replace-local-fixture-password"
#endif

#define NIVALO_IOT_FIRMWARE_VERSION "0.2.5"
#define NIVALO_IOT_HARDWARE_NAME "nivalo-stm32-esp32"

// Firmware public-key trust (private signing keys must never be provisioned).
// #define NIVALO_FIRMWARE_CURRENT_KEY_ID "firmware-current-key-id"
// #define NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM "-----BEGIN PUBLIC KEY-----\n...\n-----END PUBLIC KEY-----\n"
// #define NIVALO_FIRMWARE_PREVIOUS_KEY_ID "firmware-previous-key-id"
// #define NIVALO_FIRMWARE_PREVIOUS_PUBLIC_KEY_PEM "-----BEGIN PUBLIC KEY-----\n...\n-----END PUBLIC KEY-----\n"

// Protocol v1.1 does not advertise secondary-MCU execution or STM32 OTA yet; the bridge
// forwards the MCU's telemetry and events only. Without NIVALO_PRODUCT_ID in the build flags
// every ESP32 OTA job is rejected as well.
