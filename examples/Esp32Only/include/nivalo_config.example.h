#pragma once

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

#define NIVALO_IOT_FIRMWARE_VERSION "0.1.0"
#define NIVALO_IOT_HARDWARE_NAME "esp32-standalone"

// Firmware trust is fail-closed. Define the current public key and optionally
// the previous rotation key in ignored nivalo_config.h. Never provision a
// private signing key on the device.
// #define NIVALO_FIRMWARE_CURRENT_KEY_ID "firmware-current-key-id"
// #define NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM "-----BEGIN PUBLIC KEY-----\n...\n-----END PUBLIC KEY-----\n"
// #define NIVALO_FIRMWARE_PREVIOUS_KEY_ID "firmware-previous-key-id"
// #define NIVALO_FIRMWARE_PREVIOUS_PUBLIC_KEY_PEM "-----BEGIN PUBLIC KEY-----\n...\n-----END PUBLIC KEY-----\n"
