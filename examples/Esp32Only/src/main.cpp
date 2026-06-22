#include <Arduino.h>
#include <WiFi.h>
#include <NivaloDevice.h>

#if __has_include("nivalo_config.h")
#include "nivalo_config.h"
#else
#include "nivalo_config.example.h"
#endif

NivaloDevice device;
String macAddress;
unsigned long lastTelemetryAt = 0;

static void connectWifi()
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(NIVALO_WIFI_SSID, NIVALO_WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
    }
}

void setup()
{
    Serial.begin(115200);
    connectWifi();

    byte mac[6];
    WiFi.macAddress(mac);
    char macText[13];
    snprintf(macText, sizeof(macText), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    macAddress = String(macText);

    device.begin();
    device.beginMqtt(
        NIVALO_IOT_DEVICE_ID,
        NIVALO_IOT_MQTT_CLIENT_ID,
        NIVALO_IOT_MQTT_USERNAME,
        NIVALO_IOT_MQTT_PASSWORD,
        NIVALO_IOT_MQTT_HOST,
        NIVALO_IOT_MQTT_PORT,
        NIVALO_IOT_FIRMWARE_VERSION,
        NIVALO_IOT_HARDWARE_NAME,
        macAddress);
}

void loop()
{
    device.loop();

    if (millis() - lastTelemetryAt > 30000UL)
    {
        lastTelemetryAt = millis();
        String uptime = String(millis());
        device.publishTelemetry("uptimeMs", uptime.c_str(), "ms");
    }
}
