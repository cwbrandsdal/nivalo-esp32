#include <Arduino.h>
#include <WiFi.h>

#include <NivaloDevice.h>
#include <NivaloProvisioning.h>

NivaloDevice device;
NivaloProvisioning provisioning;

bool deviceStartAttempted = false;
bool deviceStarted = false;
unsigned long lastTelemetryAt = 0;
int ledState = 0;

int setLed(String argument)
{
  argument.trim();
  argument.toLowerCase();

  if (argument == "on" || argument == "1" || argument == "true") {
    ledState = 1;
  } else if (argument == "off" || argument == "0" || argument == "false") {
    ledState = 0;
  } else {
    return -1;
  }

#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
#endif
  return ledState;
}

String deviceMacAddress()
{
  byte mac[6];
  char text[13];
  WiFi.macAddress(mac);
  snprintf(text, sizeof(text), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(text);
}

void setup()
{
  Serial.begin(115200);

#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif

  if (!device.function("setLed", setLed) ||
      !device.variable("ledState", &ledState)) {
    Serial.println("Nivalo SDK registration failed");
  }

  // Defaults use the captive setup portal and certificate-validating claim
  // flow. No Wi-Fi, device, MQTT, or firmware-signing secret is compiled in.
  // Production remains fail-closed when flash/NVS encryption is unavailable.
  NivaloProvisioningConfig config;
  if (!provisioning.begin(config)) {
    Serial.println("Nivalo provisioning could not start");
  }
}

void loop()
{
  provisioning.loop();

  if (provisioning.ready() && !deviceStartAttempted) {
    deviceStartAttempted = true;
    const NivaloRuntimeCredentials &runtime = provisioning.credentials();

    NivaloDeviceConfig config;
    config.mqtt.deviceId = runtime.deviceId.c_str();
    config.mqtt.clientId = runtime.mqttClientId.c_str();
    config.mqtt.username = runtime.mqttUsername.c_str();
    config.mqtt.password = runtime.mqttPassword.c_str();
    config.mqtt.host = runtime.mqttHost.c_str();
    config.mqtt.port = runtime.mqttPort;
    config.mqtt.firmwareVersion = "0.2.0-example";
    config.mqtt.hardwareName = "esp32-arduino-example";
    config.mqtt.macAddress = deviceMacAddress();
    config.mqtt.transport = NIVALO_MQTT_TRANSPORT_TLS;
    config.mqtt.caCertificate = runtime.mqttCaCertificate.length()
                                      ? runtime.mqttCaCertificate.c_str()
                                      : NULL;

    // An empty signing-key set intentionally leaves firmware updates disabled.
    deviceStarted = device.begin(config);
    if (!deviceStarted) {
      Serial.println("Nivalo device could not start");
    }
  }

  if (!deviceStarted) {
    delay(2);
    return;
  }

  device.loop();
  if (millis() - lastTelemetryAt >= 30000UL) {
    lastTelemetryAt = millis();
    device.publishRuntimeTelemetry();
    device.publishVariables();
  }
}
