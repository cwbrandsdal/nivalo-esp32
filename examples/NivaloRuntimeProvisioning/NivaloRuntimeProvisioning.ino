#include <Arduino.h>
#include <WiFi.h>

#include <NivaloDevice.h>
#include <NivaloProvisioning.h>

// Production API. Firmware updates additionally need NIVALO_PRODUCT_ID and
// NIVALO_SECURITY_VERSION as build flags and a signing key; without them every OTA job is
// rejected and the sketch still runs.
static const char ApiOrigin[] = "https://iot-api.nivalo.io";

NivaloDevice device;
NivaloProvisioning provisioning;

bool deviceStartAttempted = false;
bool deviceStarted = false;
unsigned long lastTelemetryAt = 0;
bool ledState = false;

// setLed {"state": true|false}
bool validSetLedArguments(JsonObjectConst arguments)
{
  return arguments.size() == 1U && arguments["state"].is<bool>();
}

NivaloFunctionOutcome setLed(JsonObjectConst arguments)
{
  ledState = arguments["state"].as<bool>();
#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
#endif
  return NIVALO_FUNCTION_SUCCEEDED;
}

void setup()
{
  Serial.begin(115200);

#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif

  NivaloFunctionMetadata setLedMetadata;
  setLedMetadata.displayName = "Set LED";
  setLedMetadata.argumentExample = "{\"state\":true}";
  setLedMetadata.returnType = "void";
  setLedMetadata.dangerLevel = "operational";
  setLedMetadata.validateArguments = validSetLedArguments;
  if (!device.function("setLed", setLed, setLedMetadata) ||
      !device.variable("ledState", &ledState)) {
    Serial.println("Nivalo SDK registration failed");
  }

  // Defaults use the WPA2 setup portal (per-unit setup code provisioned over USB) and the
  // certificate-validating claim flow. No Wi-Fi, device, MQTT, or firmware-signing secret is
  // compiled in. Production remains fail-closed when flash/NVS encryption is unavailable.
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
    config.mqtt.firmwareVersion = "0.3.0-example";
    config.mqtt.hardwareName = "esp32-arduino-example";
    config.mqtt.transport = NIVALO_MQTT_TRANSPORT_TLS;
    config.mqtt.caCertificate = runtime.mqttCaCertificate.length()
                                      ? runtime.mqttCaCertificate.c_str()
                                      : NULL;
    config.mqtt.apiOrigin = ApiOrigin;

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
  if (millis() - lastTelemetryAt >= 300000UL) {
    lastTelemetryAt = millis();
    device.publishVariables();
  }
}
