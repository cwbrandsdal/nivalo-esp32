#pragma once

// Copy this file to include/nivalo_config.h and fill in local values.
// include/nivalo_config.h is ignored by git so MQTT passwords stay local.

#define NIVALO_WIFI_SSID "your-wifi-ssid"
#define NIVALO_WIFI_PASSWORD "your-wifi-password"

#define NIVALO_IOT_MQTT_HOST "mqtt.nivalo.io"
#define NIVALO_IOT_MQTT_PORT 1883

// Values returned by the Nivalo IoT Console credential rotation flow.
#define NIVALO_IOT_DEVICE_ID "00000000-0000-0000-0000-000000000000"
#define NIVALO_IOT_MQTT_CLIENT_ID "device-00000000000000000000000000000000"
#define NIVALO_IOT_MQTT_USERNAME "device-00000000000000000000000000000000"
#define NIVALO_IOT_MQTT_PASSWORD "paste-rotated-mqtt-password-here"

#define NIVALO_IOT_FIRMWARE_VERSION "0.2.5"
#define NIVALO_IOT_HARDWARE_NAME "nivalo-stm32-esp32"
