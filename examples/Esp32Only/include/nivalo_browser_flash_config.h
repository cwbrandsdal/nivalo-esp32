#pragma once

#include "nivalo_browser_flash_descriptor.h"

// Checked-in evaluation contract. The browser build never includes the ignored
// nivalo_config.h, so local Wi-Fi/MQTT credentials cannot alter this artifact.
#define NIVALO_DEVICE_CLAIM_URL "https://iot-api-staging.nivalo.io/v1/device-claims/exchange"
#define NIVALO_SETUP_BUTTON_PIN 0
#define NIVALO_ALLOW_UNENCRYPTED_NVS_FOR_LOCAL_DEVELOPMENT 0
#define NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE 0
#define NIVALO_IOT_FIRMWARE_VERSION "0.1.0-browser-evaluation"
#define NIVALO_IOT_HARDWARE_NAME "adafruit-feather-esp32-browser-evaluation"
