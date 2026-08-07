#pragma once

// Copy this file to include/nivalo_config.h and fill in local values.
// include/nivalo_config.h is ignored by git so MQTT passwords stay local.

#define NIVALO_DEVICE_CLAIM_URL "https://iot-api.nivalo.io/v1/device-claims/exchange"
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

// STM32 writes are refused unless one recovery option is explicitly enabled.
// Snapshot mode asserts the currently running image is known-good and requires
// enough SPIFFS capacity for both the signed update and this exact image size.
// #define NIVALO_STM32_PRESERVE_KNOWN_GOOD 1
// #define NIVALO_STM32_KNOWN_GOOD_IMAGE_SIZE_BYTES 524288U
// Or pre-provision and hash a golden image in durable SPIFFS storage:
// #define NIVALO_STM32_GOLDEN_IMAGE_PATH "/stm32-golden.bin"
// #define NIVALO_STM32_GOLDEN_IMAGE_SIZE_BYTES 524288U
// #define NIVALO_STM32_GOLDEN_IMAGE_SHA256 "64-lowercase-hex-characters"

// DESTRUCTIVE NON-PRODUCTION ACCEPTANCE ONLY. When defined locally, the next
// successfully programmed STM32 candidate is deliberately changed before
// read-back so automatic recovery can be proven. Reflash normal bridge
// firmware immediately after the attended drill. Never enable in production.
// #define NIVALO_STM32_DESTRUCTIVE_RECOVERY_ACCEPTANCE_ONCE 1
