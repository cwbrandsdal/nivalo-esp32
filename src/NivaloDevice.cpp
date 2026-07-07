#include <Arduino.h>
#include "NivaloDevice.h"
#include <FS.h> // this needs to be first, or it all crashes and burns...
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#if NIVALO_HAS_SECONDARY_MCU
#include <Adafruit_DAP.h>
#endif
#include <Update.h>
#include "bootloader.h"
#include <SPIFFS.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <stdlib.h>
#include <time.h>

#ifndef NIVALO_STATUS_NEOPIXEL_ENABLED
#define NIVALO_STATUS_NEOPIXEL_ENABLED 1
#endif

#ifndef NIVALO_STATUS_NEOPIXEL_PIN
#define NIVALO_STATUS_NEOPIXEL_PIN 27
#endif

#if NIVALO_STATUS_NEOPIXEL_ENABLED
#include <Adafruit_NeoPixel.h>
#endif

WiFiClient EspClient;
PubSubClient MqttClient(EspClient);

const char *root_ca =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";

#define BUFSIZE (16 * 1024)
uint8_t buf[BUFSIZE] __attribute__((aligned(4)));

static unsigned char bufFile[BUFSIZE];

#if NIVALO_HAS_SECONDARY_MCU
unsigned long t = 0;  // timer
uint32_t addr = 0;    // current addr
uint32_t bufferd = 0; // currently bufferd

// STM32 auto map 0x00 to 0x08000000, use 0 for simplicity
#define FLASH_START_ADDR 0x08000000

// create a DAP for programming Atmel SAM devices
Adafruit_DAP_STM32 dap;

#define SWDIO 12
#define SWCLK 14
#define SWRST 4
#endif

static constexpr size_t MQTT_PACKET_BUFFER_SIZE = 4096;
static constexpr size_t STM32_UART_RX_BUFFER_SIZE = 2048;
static constexpr size_t UART_MESSAGE_JSON_CAPACITY = 2048;
static constexpr size_t DEFINITIONS_JSON_CAPACITY = 3072;
static constexpr size_t PENDING_REPORT_COUNT = 8;
static constexpr unsigned long NIVALO_LINK_PERIODIC_POLL_MS = 1000UL;
static constexpr unsigned long NIVALO_LINK_HEARTBEAT_MS = 30000UL;
static constexpr unsigned long NIVALO_LINK_INVALID_FRAME_REPORT_MS = 10000UL;
static constexpr unsigned long NIVALO_LINK_INVALID_FRAME_BACKOFF_MS = 250UL;
static constexpr size_t NIVALO_LINK_DRAIN_LIMIT = 8U;

static NivaloLinkSpiTransport NivaloLink;
static unsigned long lastNivaloLinkPoll = 0;
static unsigned long lastNivaloLinkHeartbeat = 0;
static unsigned long lastNivaloLinkInvalidFrameReport = 0;
static unsigned long nivaloLinkBackoffUntil = 0;
static uint32_t nivaloLinkInvalidFrameCount = 0;

struct PendingCommandAck
{
    bool queued;
    char commandId[40];
    char status[16];
    char message[96];
    float progress;
};

struct PendingEventReport
{
    bool queued;
    char name[64];
    char data[512];
    char severity[16];
};

static PendingCommandAck pendingCommandAcks[PENDING_REPORT_COUNT];
static PendingEventReport pendingEventReports[PENDING_REPORT_COUNT];

static void addFirmwareTargets(JsonVariant firmware)
{
    JsonArray targets = firmware["targets"].to<JsonArray>();
    targets.add("esp32");
#if NIVALO_HAS_SECONDARY_MCU
    targets.add("stm32");
#endif
}

static const char *wifiStatusName(int status)
{
    switch (status)
    {
    case WL_IDLE_STATUS:
        return "idle";
    case WL_NO_SSID_AVAIL:
        return "no_ssid";
    case WL_SCAN_COMPLETED:
        return "scan_completed";
    case WL_CONNECTED:
        return "connected";
    case WL_CONNECT_FAILED:
        return "connect_failed";
    case WL_CONNECTION_LOST:
        return "connection_lost";
    case WL_DISCONNECTED:
        return "disconnected";
    default:
        return "unknown";
    }
}

static void copyText(char *destination, size_t destinationSize, const char *source)
{
    if (destination == NULL || destinationSize == 0)
    {
        return;
    }

    if (source == NULL)
    {
        source = "";
    }

    strncpy(destination, source, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
}

static void releaseDapPins()
{
#if NIVALO_HAS_SECONDARY_MCU
    pinMode(SWDIO, INPUT);
    pinMode(SWCLK, INPUT);
    pinMode(SWRST, INPUT);
#endif
}

static void bytesToHex(const uint8_t *bytes, size_t length, char *output, size_t outputLength)
{
    static const char hex[] = "0123456789abcdef";
    if (bytes == NULL || output == NULL || outputLength < (length * 2U + 1U))
    {
        return;
    }

    for (size_t i = 0; i < length; i++)
    {
        output[i * 2U] = hex[(bytes[i] >> 4U) & 0x0FU];
        output[i * 2U + 1U] = hex[bytes[i] & 0x0FU];
    }
    output[length * 2U] = '\0';
}

static bool caseInsensitiveEquals(const char *left, const char *right)
{
    if (left == NULL || right == NULL)
    {
        return false;
    }

    while (*left != '\0' && *right != '\0')
    {
        char a = *left;
        char b = *right;
        if (a >= 'A' && a <= 'Z')
        {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z')
        {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b)
        {
            return false;
        }
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

static bool queueCommandAckReport(const char *commandId, const char *status, const char *message, float progress = -1)
{
    if (commandId == NULL || strlen(commandId) == 0)
    {
        return false;
    }

    for (size_t i = 0; i < PENDING_REPORT_COUNT; i++)
    {
        if (!pendingCommandAcks[i].queued)
        {
            copyText(pendingCommandAcks[i].commandId, sizeof(pendingCommandAcks[i].commandId), commandId);
            copyText(pendingCommandAcks[i].status, sizeof(pendingCommandAcks[i].status), status);
            copyText(pendingCommandAcks[i].message, sizeof(pendingCommandAcks[i].message), message);
            pendingCommandAcks[i].progress = progress;
            pendingCommandAcks[i].queued = true;
            return true;
        }
    }

    return false;
}

static bool queueEventReport(const char *name, const char *data, const char *severity = "info")
{
    for (size_t i = 0; i < PENDING_REPORT_COUNT; i++)
    {
        if (!pendingEventReports[i].queued)
        {
            copyText(pendingEventReports[i].name, sizeof(pendingEventReports[i].name), name);
            copyText(pendingEventReports[i].data, sizeof(pendingEventReports[i].data), data);
            copyText(pendingEventReports[i].severity, sizeof(pendingEventReports[i].severity), severity);
            pendingEventReports[i].queued = true;
            return true;
        }
    }

    return false;
}

#if NIVALO_STATUS_NEOPIXEL_ENABLED
static const uint8_t PIN_NEOPIXEL = NIVALO_STATUS_NEOPIXEL_PIN;
uint8_t neoPixelBrightness = 10;
uint8_t neoPixelMaxBrightness = 15;
volatile uint8_t neoPixelLoopCount = 0;
static const uint8_t neoPixelLoopCountSteps = 30;
bool isNeoPixelUpwords = true;
Adafruit_NeoPixel strip = Adafruit_NeoPixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
#endif

long lastHeartbeat = 0;

#if NIVALO_HAS_SECONDARY_MCU
// dumping stm32 memory for verification
void print_memory(uint32_t addr, uint8_t *buffer, uint32_t bufsize)
{
    memset(buffer, 0xff, bufsize);
    dap.dap_read_block(addr, buffer, bufsize);

    for (uint32_t i = 0; i < bufsize; i++)
    {
        if (i % 16 == 0)
        {
            if (i != 0)
                Serial.println();
            // print offset
            if (i < 0x100)
                Serial.print("0");
            if (i < 0x10)
                Serial.print("0");
            Serial.print(i, HEX);
            Serial.print(": ");
        }

        if (buffer[i] < 0x10)
            Serial.print("0");
        Serial.print(buffer[i], HEX);
        Serial.print(" ");
    }
    Serial.println();
}
#endif

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
#if NIVALO_STATUS_NEOPIXEL_ENABLED
static void beginStatusPixel()
{
    strip.begin();
    strip.setBrightness(neoPixelBrightness);
    strip.setPixelColor(0, strip.Color(0, 0, 255));
    strip.show();
}

static void setStatusPixel(uint8_t red, uint8_t green, uint8_t blue)
{
    strip.setBrightness(neoPixelBrightness);
    strip.setPixelColor(0, strip.Color(red, green, blue));
    strip.show();
}

static void pulseStatusPixel()
{
    if (neoPixelLoopCount == neoPixelLoopCountSteps)
    {
        if (neoPixelBrightness < neoPixelMaxBrightness && isNeoPixelUpwords)
        {
            neoPixelBrightness++;
        }
        else if (neoPixelBrightness == neoPixelMaxBrightness)
        {
            neoPixelBrightness--;
            isNeoPixelUpwords = false;
        }
        else if (neoPixelBrightness > 1 && !isNeoPixelUpwords)
        {
            neoPixelBrightness--;
        }
        else if (neoPixelBrightness == 1)
        {
            neoPixelBrightness++;
            isNeoPixelUpwords = true;
        }

        setStatusPixel(52, 204, 235);
        neoPixelLoopCount = 0;
    }
    else
    {
        neoPixelLoopCount++;
    }
}

uint32_t Wheel(byte WheelPos)
{
    WheelPos = 255 - WheelPos;
    if (WheelPos < 85)
    {
        return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
    }
    if (WheelPos < 170)
    {
        WheelPos -= 85;
        return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
    }
    WheelPos -= 170;
    return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}
#else
static void beginStatusPixel()
{
}

static void setStatusPixel(uint8_t red, uint8_t green, uint8_t blue)
{
    (void)red;
    (void)green;
    (void)blue;
}

static void pulseStatusPixel()
{
}
#endif

void listDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
    Serial.printf("Listing directory: %s\r\n", dirname);

    File root = fs.open(dirname);
    if (!root)
    {
        Serial.println("- failed to open directory");
        return;
    }
    if (!root.isDirectory())
    {
        Serial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while (file)
    {
        if (file.isDirectory())
        {
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if (levels)
            {
                listDir(fs, file.name(), levels - 1);
            }
        }
        else
        {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}

void deleteFile(fs::FS &fs, const char *path)
{
    Serial.printf("Deleting file: %s\r\n", path);
    if (fs.remove(path))
    {
        Serial.println("- file deleted");
    }
    else
    {
        Serial.println("- delete failed");
    }
}

void rainbowCycle(uint8_t wait)
{
#if NIVALO_STATUS_NEOPIXEL_ENABLED
    uint16_t i, j;

    for (j = 0; j < 256 * 5; j++)
    { // 5 cycles of all colors on wheel
        for (i = 0; i < strip.numPixels(); i++)
        {
            strip.setPixelColor(i, Wheel(((i * 256 / strip.numPixels()) + j) & 255));
        }
        strip.show();
        delay(wait);
    }
#else
    (void)wait;
#endif
}

NivaloDevice::NivaloDevice()
{
    _hardSerial = NULL;
    _baud = 0;
    _mqttPort = 1883;
}

#if NIVALO_HAS_SECONDARY_MCU
void programDap(int c)
{
    dap.programBlock(addr, buf, c);
    addr += c;

    // reset buffer for the next round
    memset(buf, BUFSIZE, c);
    bufferd = 0;
}
#endif

// Function called when there's an SWD error
void error(const char *text)
{
    Serial.println(text);
    while (1)
        ;
}
boolean NivaloDevice::begin(HardwareSerial &hardSerial, unsigned long baud)
{
    _hardSerial = &hardSerial;
    boolean ok = begin(baud);
    _hardSerial = &hardSerial;
    (void)init(baud);
    return ok;
}

boolean NivaloDevice::begin(unsigned long baud)
{
    beginStatusPixel();

    releaseDapPins();
    _baud = baud;

#if NIVALO_HAS_SECONDARY_MCU
    return NivaloLink.begin();
#else
    return true;
#endif
}

boolean NivaloDevice::beginMqtt(
    const char *deviceId,
    const char *clientId,
    const char *username,
    const char *password,
    const char *host,
    uint16_t port,
    const char *firmwareVersion,
    const char *hardwareName,
    String macAddress)
{
    setStatusPixel(255, 0, 0);

    _macAddr = macAddress;
    _deviceId = deviceId;
    _clientId = clientId;
    _username = username;
    _password = password;
    _mqttHost = host;
    _mqttPort = port;
    _firmwareVersion = firmwareVersion;
    _hardwareName = hardwareName;

    snprintf(_telemetryTopic, sizeof(_telemetryTopic), "nivalo/v1/devices/%s/telemetry", _deviceId.c_str());
    snprintf(_stateTopic, sizeof(_stateTopic), "nivalo/v1/devices/%s/state", _deviceId.c_str());
    snprintf(_eventsTopic, sizeof(_eventsTopic), "nivalo/v1/devices/%s/events", _deviceId.c_str());
    snprintf(_definitionsTopic, sizeof(_definitionsTopic), "nivalo/v1/devices/%s/definitions", _deviceId.c_str());
    snprintf(_availabilityTopic, sizeof(_availabilityTopic), "nivalo/v1/devices/%s/availability", _deviceId.c_str());
    snprintf(_commandsTopic, sizeof(_commandsTopic), "nivalo/v1/devices/%s/commands/+", _deviceId.c_str());
    snprintf(_ackTopicPrefix, sizeof(_ackTopicPrefix), "nivalo/v1/devices/%s/acks/", _deviceId.c_str());

    MqttClient.setServer(_mqttHost.c_str(), _mqttPort);
    MqttClient.setBufferSize(MQTT_PACKET_BUFFER_SIZE);
    MqttClient.setCallback([this](char *topic, byte *payload, unsigned int length)
                           { this->mqttCallback(topic, payload, length); });

    return true;
}

void NivaloDevice::setColor(NivaloStatusColor color)
{
    switch (color)
    {
    case NIVALO_STATUS_RED:
        setStatusPixel(255, 0, 0);
        break;
    case NIVALO_STATUS_GREEN:
        setStatusPixel(0, 255, 0);
        break;
    case NIVALO_STATUS_YELLOW:
        setStatusPixel(255, 255, 0);
        break;
    case NIVALO_STATUS_PULSE_BLUE:
        setStatusPixel(52, 204, 235);
        break;
    default:
        break;
    }
}

void NivaloDevice::mqttCallback(char *topic, byte *message, unsigned int length)
{
    Serial.print("Message arrived on topic: ");
    Serial.print(topic);
    Serial.print(". Message: ");
    for (unsigned int i = 0; i < length; i++)
    {
        Serial.print((char)message[i]);
    }

    Serial.println("");
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, message, length);
    if (err)
    {
        Serial.print("MQTT command JSON parse failed: ");
        Serial.println(err.c_str());
        queueEventReport("esp32.command.parse_error", err.c_str(), "warning");
        return;
    }

    String commandId = getCommandIdFromTopic(topic);
    String commandName;
    if (doc["payload"]["command"].is<const char *>())
    {
        commandName = doc["payload"]["command"].as<String>();
    }
    else if (doc["command"].is<const char *>())
    {
        commandName = doc["command"].as<String>();
    }
    String payload;
    JsonVariant arguments = doc["payload"]["arguments"];
    if (arguments.is<const char *>())
    {
        payload = arguments.as<String>();
    }
    else if (arguments["url"].is<const char *>())
    {
        payload = arguments["url"].as<String>();
    }
    else if (arguments["signedUrl"].is<const char *>())
    {
        payload = arguments["signedUrl"].as<String>();
    }
    else if (arguments["downloadUrl"].is<const char *>())
    {
        payload = arguments["downloadUrl"].as<String>();
    }
    else if (!arguments.isNull())
    {
        serializeJson(arguments, payload);
    }
    else if (doc["payload"].is<const char *>())
    {
        payload = doc["payload"].as<String>();
    }

    Serial.println("-----------------------------");
    Serial.println("MQTT command received");
    Serial.println(commandName);
    Serial.println(payload);
    Serial.println("-----------------------------");
    queueEventReport("esp32.command.received", commandName.c_str());

    if (commandId.length() > 0)
    {
        queueCommandAckReport(commandId.c_str(), "accepted", "Command received by ESP32");
    }
    drainQueuedReports();
    MqttClient.loop();

    if (commandName == "flash" || commandName == "firmware.flash")
    {
        Serial.println("[HTTP] Starting up...");

        const char *target = arguments["target"] | "stm32";
        if (payload.length() == 0)
        {
            queueEventReport("esp32.flash.rejected", "missing firmware URL", "warning");
            if (commandId.length() > 0)
            {
                queueCommandAckReport(commandId.c_str(), "failed", "Missing firmware URL");
            }
            return;
        }

        bool targetIsEsp32 = caseInsensitiveEquals(target, "esp32") ||
                             caseInsensitiveEquals(target, "device") ||
                             caseInsensitiveEquals(target, "bridge");
        bool targetIsStm32 = caseInsensitiveEquals(target, "stm32") ||
                             caseInsensitiveEquals(target, "mcu") ||
                             caseInsensitiveEquals(target, "applicationMcu");

        if (!targetIsEsp32 && !targetIsStm32)
        {
            queueEventReport("esp32.flash.rejected", "unsupported firmware target", "warning");
            if (commandId.length() > 0)
            {
                queueCommandAckReport(commandId.c_str(), "failed", "Unsupported firmware target");
            }
            return;
        }

#if !NIVALO_HAS_SECONDARY_MCU
        if (targetIsStm32)
        {
            queueEventReport("esp32.flash.rejected", "secondary MCU support is disabled", "warning");
            if (commandId.length() > 0)
            {
                queueCommandAckReport(commandId.c_str(), "failed", "Secondary MCU flashing is not enabled for this build");
            }
            return;
        }
#endif

        size_t expectedSizeBytes = arguments["sizeBytes"] | 0U;
        const char *expectedSha256 = arguments["sha256"] | "";
        String flashFailure = "Flash command failed";

        if (commandId.length() > 0)
        {
            publishCommandAck(
                commandId.c_str(),
                "running",
                targetIsEsp32 ? "Downloading ESP32 firmware" : "Downloading STM32 firmware",
                5);
        }
        publishEvent("esp32.flash.started", target, "info");
        MqttClient.loop();

        NivaloLink.setPaused(true);
        releaseDapPins();
        bool flashSucceeded = false;

        HTTPClient http;
        http.setConnectTimeout(15000);
        http.setTimeout(15000);

        // Your Domain name with URL path or IP address with path
        if (!http.begin(payload, root_ca))
        {
            flashFailure = "Firmware download URL rejected";
            NivaloLink.setPaused(false);
            releaseDapPins();
            if (commandId.length() > 0)
            {
                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
            }
            return;
        }

        // Send HTTP POST request
        int httpResponseCode = http.GET();

        String payload = "";

        if (httpResponseCode > 0)
        {
            // HTTP header has been send and Server response header has been handled
            Serial.printf("[HTTP] GET... code: %d\n", httpResponseCode);

            // file found at server
            if (httpResponseCode == HTTP_CODE_OK)
            {
                rainbowCycle(1);

                // get length of document (is -1 when Server sends no Content-Length header)
                int len = http.getSize();
                Serial.printf("[HTTP] GET... size: %d\n", len);

                char bufSizeText[30];
                sprintf(bufSizeText, "[HTTP] Size of firmware: %d\n", len);

                publish("flashing", bufSizeText);
                // create buffer for read
                // uint8_t buff[128] = {0};

                Serial.println();
                Serial.println("[HTTP] connection closed or file end.\n");

                Serial.println("Processing uploaded file\n");

                // get tcp stream
                WiFiClient *stream = http.getStreamPtr();

                // dap.programFlash(addr, fwbinfile, sizeof(fwbinfile), true);
                //  uint8_t firmware[len];

                Serial.println("mounting FS...");

                if (SPIFFS.begin(true))
                {
                    listDir(SPIFFS, "/", 0);
                    Serial.println("mounted file system");

                    // file exists, reading and loading
                    Serial.println("reading config file");
                    File firmwareFile = SPIFFS.open("/firmware.bin", FILE_WRITE);
                    if (firmwareFile)
                    {
                        Serial.println("opened firmware file");

                        size_t downloadedBytes = 0U;
                        mbedtls_sha256_context shaContext;
                        mbedtls_sha256_init(&shaContext);
                        (void)mbedtls_sha256_starts_ret(&shaContext, 0);
                        static constexpr unsigned long DOWNLOAD_IDLE_TIMEOUT_MS = 15000UL;
                        static constexpr unsigned long DOWNLOAD_PROGRESS_INTERVAL_MS = 1500UL;
                        static constexpr size_t DOWNLOAD_PROGRESS_CHUNK_BYTES = 16384U;
                        unsigned long lastDownloadProgressMs = millis();
                        unsigned long lastDownloadDataMs = millis();

                        while (http.connected() && (len > 0 || len == -1))
                        {
                            // get available data size
                            size_t size = stream->available();

                            if (size)
                            {
                                // read up to 128 byte
                                int c = stream->readBytes(buf, ((size > sizeof(buf)) ? sizeof(buf) : size));

                                // write it to Serial
                                // Serial.write(buf, c);

                                Serial.printf("[Writing to flash] bufsize: %d c: %d size: %d len: %d\n", sizeof(buf), c, size, len);
                                // programDap(c);

                                // dap.programFlash(addr, buf, c, false);
                                firmwareFile.write(buf, c);
                                (void)mbedtls_sha256_update_ret(&shaContext, buf, (size_t)c);
                                downloadedBytes += (size_t)c;
                                lastDownloadDataMs = millis();
                                // addr += c;

                                if (len > 0)
                                {
                                    len -= c;
                                }

                                if (commandId.length() > 0 &&
                                    (millis() - lastDownloadProgressMs >= DOWNLOAD_PROGRESS_INTERVAL_MS ||
                                     downloadedBytes % DOWNLOAD_PROGRESS_CHUNK_BYTES == 0U))
                                {
                                    float progress = expectedSizeBytes > 0U
                                                         ? 5.0f + ((float)downloadedBytes / (float)expectedSizeBytes) * 30.0f
                                                         : 15.0f;
                                    publishCommandAck(commandId.c_str(), "running", "Downloading firmware", progress);
                                    lastDownloadProgressMs = millis();
                                }
                            }
                            else if (millis() - lastDownloadDataMs > DOWNLOAD_IDLE_TIMEOUT_MS)
                            {
                                flashFailure = "Firmware download timed out";
                                break;
                            }

                            delay(100);
                        }

                        uint8_t actualSha256[32];
                        char actualSha256Text[65];
                        (void)mbedtls_sha256_finish_ret(&shaContext, actualSha256);
                        mbedtls_sha256_free(&shaContext);
                        bytesToHex(actualSha256, sizeof(actualSha256), actualSha256Text, sizeof(actualSha256Text));

                        if (expectedSizeBytes > 0U && downloadedBytes != expectedSizeBytes)
                        {
                            firmwareFile.close();
                            deleteFile(SPIFFS, "/firmware.bin");
                            http.end();
                            NivaloLink.setPaused(false);
                            releaseDapPins();
                            flashFailure = "Firmware size verification failed";
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }

                        if (expectedSha256 != NULL && strlen(expectedSha256) > 0U && !caseInsensitiveEquals(expectedSha256, actualSha256Text))
                        {
                            firmwareFile.close();
                            deleteFile(SPIFFS, "/firmware.bin");
                            http.end();
                            NivaloLink.setPaused(false);
                            releaseDapPins();
                            flashFailure = "Firmware SHA-256 verification failed";
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }

                        if (commandId.length() > 0)
                        {
                            publishCommandAck(commandId.c_str(), "running", "Firmware verified", targetIsEsp32 ? 45 : 35);
                        }

                        if (targetIsEsp32)
                        {
                            firmwareFile.close();
                            http.end();

                            File esp32FirmwareFile = SPIFFS.open("/firmware.bin", FILE_READ);
                            if (!esp32FirmwareFile)
                            {
                                NivaloLink.setPaused(false);
                                releaseDapPins();
                                if (commandId.length() > 0)
                                {
                                    publishCommandAck(commandId.c_str(), "failed", "Verified ESP32 firmware file could not be reopened");
                                }
                                return;
                            }

                            size_t firmwareSize = esp32FirmwareFile.size();
                            bool updateOk = Update.begin(firmwareSize);
                            if (updateOk)
                            {
                                size_t writtenBytes = Update.writeStream(esp32FirmwareFile);
                                updateOk = (writtenBytes == firmwareSize) && Update.end(true);
                            }

                            esp32FirmwareFile.close();
                            deleteFile(SPIFFS, "/firmware.bin");

                            if (!updateOk || Update.hasError())
                            {
                                String updateError = Update.errorString();
                                NivaloLink.setPaused(false);
                                releaseDapPins();
                                if (commandId.length() > 0)
                                {
                                    publishCommandAck(commandId.c_str(), "failed", updateError.c_str());
                                }
                                return;
                            }

                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "succeeded", "ESP32 firmware updated; restarting");
                            }

                            NivaloLink.setPaused(false);
                            releaseDapPins();
                            ESP.restart();
                            return;
                        }

#if NIVALO_HAS_SECONDARY_MCU
                        releaseDapPins();
                        dap.begin(SWCLK, SWDIO, SWRST, &error);
                        Serial.println("Connecting to DAP...");
                        if (!dap.targetConnect())
                        {
                            flashFailure = String("DAP target connect failed: ") + dap.error_message;
                            firmwareFile.close();
                            deleteFile(SPIFFS, "/firmware.bin");
                            http.end();
                            NivaloLink.setPaused(false);
                            releaseDapPins();
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }

                        char debuggername[100];
                        dap.dap_get_debugger_info(debuggername);
                        Serial.println(debuggername);

                        uint32_t dsu_did;
                        if (!dap.select(&dsu_did))
                        {
                            Serial.println("Unknown device found 0x");
                            Serial.println(dsu_did, HEX);
                            flashFailure = "Unknown STM32 target";
                            firmwareFile.close();
                            deleteFile(SPIFFS, "/firmware.bin");
                            http.end();
                            dap.dap_disconnect();
                            NivaloLink.setPaused(false);
                            releaseDapPins();
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }





                        char buftargetText[40];
                        sprintf(buftargetText, "[DAP] Start flashing to target\t: %s\n", dap.target_device.name);
                        publish("flashing", buftargetText);

                        Serial.print("\nFound Target\t: ");
                        Serial.println(dap.target_device.name);
                        Serial.print("Flash size\t: ");
                        Serial.println(dap.target_device.flash_size);

                        publish("flashing", "[DAP] Programming...");
                        if (commandId.length() > 0)
                        {
                            publishCommandAck(commandId.c_str(), "running", "Programming STM32 flash", 50);
                        }

                        Serial.print("Programming... ");
                        t = millis();

                        firmwareFile.seek(0, SeekSet);
                        firmwareFile.close();

                        delay(500);
                        File myfile = SPIFFS.open("/firmware.bin", FILE_READ);
                        if (!myfile)
                        {
                            flashFailure = "Verified firmware file could not be reopened";
                            http.end();
                            dap.deselect();
                            dap.dap_disconnect();
                            NivaloLink.setPaused(false);
                            releaseDapPins();
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }

                        len = myfile.size();

                        uint32_t start_ms, duaration;

                        //------------- Preparing sectors -------------//
                        Serial.print("Preparing ... ");

                        start_ms = millis();

                        // preparing flash sector with address = 0, size = Binary size
                        dap.programPrepare(FLASH_START_ADDR, len);
                        duaration = millis() - start_ms;

                        Serial.print(" done in ");
                        Serial.print(duaration);
                        Serial.println(" ms");

                        char bufSizeText123[70];
                        sprintf(bufSizeText123, "[DAP] Preparing done in %d\n ms", duaration);

                        publish("flashing", bufSizeText123);

                        size_t flen = len;

                        Serial.println("");
                        Serial.printf("- %u bytes read\r\n", flen);

                        //------------- Programming -------------//
                        Serial.print("Programming & Verifying ");
                        Serial.print(sizeof(buf) / 1024);
                        Serial.print(" KBs ...");


                        start_ms = millis();
               

  // Program the flash in chunks
    Serial.println("Start programming in chunks...");
    uint32_t address = FLASH_START_ADDR; // Start address for the STM32 flash
    int originalLen = len;
    bool stm32ProgrammingOk = true;
    while (len > 0)
    {
        int toRead = (len < BUFSIZE) ? len : BUFSIZE;
        
        memset(buf, 0xFF, BUFSIZE); // empty it out
        size_t c = myfile.read(buf, toRead);

        if (c > 0)
        {
            bool success = dap.programFlash(address, buf, c, false);
            if (!success)
            {
                flashFailure = "STM32 programming failed";
                stm32ProgrammingOk = false;
                break;
            }
            address += c;
            len -= c;
            if (commandId.length() > 0 && originalLen > 0)
            {
                float progress = 50.0f + ((float)(originalLen - len) / (float)originalLen) * 45.0f;
                publishCommandAck(commandId.c_str(), "running", "Programming STM32 flash", progress);
            }
            Serial.print("Flashed ");
            Serial.print(c);
            Serial.println(" bytes");
        }
        else
        {
            // If no bytes were read, break the loop
            flashFailure = "Firmware file read failed during STM32 programming";
            stm32ProgrammingOk = false;
            break;
        }
    }

                        myfile.close();

                        if (!stm32ProgrammingOk)
                        {
                            http.end();
                            dap.deselect();
                            dap.dap_disconnect();
                            NivaloLink.setPaused(false);
                            releaseDapPins();
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }




                        duaration = millis() - start_ms;
                        Serial.print(" done in ");
                        Serial.print(duaration);
                        Serial.print(" ms, ");

                        Serial.print("Speed ");
                        Serial.print((double)sizeof(buf) / (duaration * 1.024));
                        Serial.println(" KBs/s");

                        // Serial.print("Verified: ");
                        // if (verified)
                        // {
                        //     Serial.println("matched");
                        // }
                        // else
                        // {
                        //     Serial.print("mis-matched");
                        // }

                        dap.deselect();
                        dap.dap_disconnect();
                        releaseDapPins();
#endif
                    }
                    listDir(SPIFFS, "/", 0);
                    deleteFile(SPIFFS, "/firmware.bin");
                    listDir(SPIFFS, "/", 0);
                }
                else
                {
                    Serial.println("failed to mount FS");
                }

#ifdef LED_BUILTIN
                digitalWrite(LED_BUILTIN, LOW);
#endif

                mqttReconnect();

                publish("flashing", "[DAP] Firmware flashed successfully!");
                flashSucceeded = true;

                nivalo_status_t err = init();
                (void)err;
            }
        }
        else
        {
            Serial.print("Error code: ");
            Serial.println(httpResponseCode);
            flashFailure = String("Firmware download failed: ") + http.errorToString(httpResponseCode);
        }
        // Free resources
        http.end();

        if (!flashSucceeded)
        {
            NivaloLink.setPaused(false);
            releaseDapPins();
            if (commandId.length() > 0)
            {
                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
            }
            return;
        }

        if (commandId.length() > 0)
        {
            publishCommandAck(commandId.c_str(), "succeeded", "Flash command completed; restarting ESP32");
        }

        NivaloLink.setPaused(false);
        releaseDapPins();
        ESP.restart();
    }
    else if (commandName.length() > 0)
    {
#if NIVALO_HAS_SECONDARY_MCU
        StaticJsonDocument<1024> stmCommand;
        stmCommand["commandId"] = commandId;
        stmCommand["command"] = commandName;
        if (!arguments.isNull())
        {
            stmCommand["payload"] = arguments;
        }
        else
        {
            stmCommand["payload"] = payload;
        }

        String forwardedCommand;
        serializeJson(stmCommand, forwardedCommand);
        NivaloLinkReceivedFrame immediateFrame;
        bool sent = NivaloLink.exchange(NIVALO_LINK_FRAME_COMMAND, forwardedCommand.c_str(), &immediateFrame);

        if (sent)
        {
            queueEventReport("mcu.command.forwarded", forwardedCommand.c_str());
            if (commandId.length() > 0)
            {
                queueCommandAckReport(commandId.c_str(), "running", "Forwarded to secondary MCU over NivaloLink");
            }
            if (immediateFrame.valid)
            {
                handleNivaloLinkFrame(immediateFrame);
            }
            drainNivaloLink(true);
        }
        else
        {
            queueEventReport("mcu.command.forward_failed", NivaloLink.lastError(), "warning");
            if (commandId.length() > 0)
            {
                queueCommandAckReport(commandId.c_str(), "failed", NivaloLink.lastError());
            }
        }
#else
        queueEventReport("esp32.command.unsupported", commandName.c_str(), "warning");
        if (commandId.length() > 0)
        {
            queueCommandAckReport(commandId.c_str(), "failed", "No secondary MCU bridge is enabled");
        }
#endif
    }

    Serial.println();
}

void NivaloDevice::mqttReconnect()
{
    // Loop until we're reconnected
    while (!MqttClient.connected())
    {
        Serial.print("Attempting MQTT connection...");
        // Attempt to connect
        if (MqttClient.connect(_clientId.c_str(), _username.c_str(), _password.c_str()))
        {
            Serial.println("connected");
            setStatusPixel(52, 204, 235);
            // digitalWrite(BUILTIN_LED, HIGH);
            // Subscribe
            delay(1000);

            MqttClient.subscribe(_commandsTopic);
            publishAvailability("online", "mqtt-connected");
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(MqttClient.state());
            Serial.println(" try again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(1000);
        }
    }
}

size_t NivaloDevice::publish(const char *eventName, const char *eventData)
{
    return publishEvent(eventName, eventData);
}

size_t NivaloDevice::publishTelemetry(const char *name, const char *value, const char *unit)
{
    String output;
    StaticJsonDocument<768> doc;
    doc["schema"] = "nivalo.iot.v1";
    doc["messageId"] = createMessageId();
    doc["deviceId"] = _deviceId;
    doc["sentAt"] = createTimestamp();
    doc["firmware"]["version"] = _firmwareVersion;
    doc["firmware"]["hardware"] = _hardwareName;
    doc["firmware"]["mac"] = _macAddr;
    addFirmwareTargets(doc["firmware"]);
    doc["payload"]["name"] = name;

    if (value == NULL)
    {
        doc["payload"]["raw"] = "";
    }
    else
    {
        char *end = NULL;
        double numericValue = strtod(value, &end);
        if (end != value && *end == '\0')
        {
            doc["payload"]["value"] = numericValue;
        }
        else
        {
            doc["payload"]["raw"] = value;
        }
    }

    if (unit != NULL)
    {
        doc["payload"]["unit"] = unit;
    }

    serializeJson(doc, output);

    MqttClient.publish(_telemetryTopic, output.c_str());

    return (size_t)1;
}

size_t NivaloDevice::publishRuntimeTelemetry()
{
    size_t published = 0;
    published += publishUptimeTelemetry();
    published += publishWifiSignalTelemetry();
    published += publishHeapTelemetry();
    return published;
}

size_t NivaloDevice::publishUptimeTelemetry()
{
    String uptime = String(millis());
    return publishTelemetry("uptime_ms", uptime.c_str(), "ms");
}

size_t NivaloDevice::publishWifiSignalTelemetry()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return 0;
    }

    String rssi = String(WiFi.RSSI());
    return publishTelemetry("wifi_rssi", rssi.c_str(), "dBm");
}

size_t NivaloDevice::publishHeapTelemetry()
{
    uint32_t heapTotal = ESP.getHeapSize();
    uint32_t heapFree = ESP.getFreeHeap();
    uint32_t heapUsed = heapTotal > heapFree ? heapTotal - heapFree : 0;

    String heapUsedText = String(heapUsed);
    String heapFreeText = String(heapFree);
    String heapTotalText = String(heapTotal);

    size_t published = 0;
    published += publishTelemetry("heap_used", heapUsedText.c_str(), "B");
    published += publishTelemetry("heap_free", heapFreeText.c_str(), "B");
    published += publishTelemetry("heap_total", heapTotalText.c_str(), "B");
    return published;
}

size_t NivaloDevice::publishEvent(const char *name, const char *data, const char *severity)
{
    String output;
    StaticJsonDocument<1536> doc;
    doc["schema"] = "nivalo.iot.v1";
    doc["messageId"] = createMessageId();
    doc["deviceId"] = _deviceId;
    doc["sentAt"] = createTimestamp();
    doc["firmware"]["version"] = _firmwareVersion;
    doc["firmware"]["hardware"] = _hardwareName;
    addFirmwareTargets(doc["firmware"]);
    doc["payload"]["name"] = name;
    doc["payload"]["severity"] = severity;
    doc["payload"]["raw"] = data;
    serializeJson(doc, output);

    MqttClient.publish(_eventsTopic, output.c_str());
    return (size_t)1;
}

size_t NivaloDevice::publishAvailability(const char *status, const char *reason)
{
    String output;
    StaticJsonDocument<512> doc;
    doc["schema"] = "nivalo.iot.v1";
    doc["messageId"] = createMessageId();
    doc["deviceId"] = _deviceId;
    doc["sentAt"] = createTimestamp();
    doc["payload"]["status"] = status;
    if (reason != NULL)
    {
        doc["payload"]["reason"] = reason;
    }
    serializeJson(doc, output);

    MqttClient.publish(_availabilityTopic, output.c_str());
    return (size_t)1;
}

void NivaloDevice::publishNivaloLinkHeartbeat(const char *payload)
{
    DynamicJsonDocument doc(1536);
    bool parsedPayload = false;

    if (payload != NULL && strlen(payload) > 0U)
    {
        DeserializationError err = deserializeJson(doc, payload);
        parsedPayload = (err == DeserializationError::Ok && doc.is<JsonObject>());
    }

    if (!parsedPayload)
    {
        doc.clear();
        doc.to<JsonObject>();
    }

    JsonObject esp32 = doc.createNestedObject("esp32");
    esp32["uptimeMs"] = millis();
    esp32["firmwareVersion"] = _firmwareVersion;
    esp32["hardware"] = _hardwareName;
    esp32["mac"] = _macAddr;
    esp32["sdkVersion"] = ESP.getSdkVersion();
    esp32["chipModel"] = ESP.getChipModel();
    esp32["chipRevision"] = ESP.getChipRevision();
    esp32["cpuFreqMHz"] = ESP.getCpuFreqMHz();

    JsonObject wifi = esp32.createNestedObject("wifi");
    int wifiStatus = WiFi.status();
    wifi["status"] = wifiStatusName(wifiStatus);
    wifi["statusCode"] = wifiStatus;
    wifi["connected"] = (wifiStatus == WL_CONNECTED);
    if (wifiStatus == WL_CONNECTED)
    {
        wifi["rssiDbm"] = WiFi.RSSI();
        wifi["channel"] = WiFi.channel();
        wifi["ip"] = WiFi.localIP().toString();
    }

    JsonObject heap = esp32.createNestedObject("heap");
    heap["totalBytes"] = ESP.getHeapSize();
    heap["freeBytes"] = ESP.getFreeHeap();
    heap["minFreeBytes"] = ESP.getMinFreeHeap();
    heap["maxAllocBytes"] = ESP.getMaxAllocHeap();

    JsonObject sketch = esp32.createNestedObject("sketch");
    sketch["sizeBytes"] = ESP.getSketchSize();
    sketch["freeBytes"] = ESP.getFreeSketchSpace();

    String enrichedPayload;
    if (!doc.overflowed() && serializeJson(doc, enrichedPayload) > 0U)
    {
        publishEvent("mcu.nivalolink.heartbeat", enrichedPayload.c_str(), "info");
        return;
    }

    publishEvent("mcu.nivalolink.heartbeat", payload != NULL ? payload : "{}", "info");
}

void NivaloDevice::handleNivaloLinkFrame(const NivaloLinkReceivedFrame &frame)
{
    if (!frame.valid || frame.type == NIVALO_LINK_FRAME_IDLE || frame.type == NIVALO_LINK_FRAME_POLL)
    {
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_HELLO)
    {
        publishEvent("mcu.nivalolink.hello", frame.payloadLength > 0U ? frame.payload : "{}", "info");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_HEARTBEAT)
    {
        publishNivaloLinkHeartbeat(frame.payloadLength > 0U ? frame.payload : "{}");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_ERROR)
    {
        publishEvent("mcu.nivalolink.error", frame.payloadLength > 0U ? frame.payload : "{}", "warning");
        return;
    }

    DynamicJsonDocument doc(UART_MESSAGE_JSON_CAPACITY);
    DeserializationError err = deserializeJson(doc, frame.payload);
    if (err)
    {
        char parseMessage[96];
        snprintf(
            parseMessage,
            sizeof(parseMessage),
            "%s type=0x%02x bytes=%u",
            err.c_str(),
            frame.type,
            (unsigned int)frame.payloadLength);
        publishEvent("esp32.nivalolink.parse_error", parseMessage, "warning");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_TELEMETRY)
    {
        String value;
        JsonVariant payloadValue = doc["value"];
        const char *name = doc["name"] | "";
        const char *unit = doc["unit"] | "";
        if (strlen(unit) == 0)
        {
            unit = NULL;
        }

        if (strlen(name) == 0 || payloadValue.isNull())
        {
            publishEvent("mcu.telemetry.rejected", frame.payload, "warning");
            return;
        }

        if (payloadValue.is<const char *>())
        {
            value = payloadValue.as<const char *>();
        }
        else
        {
            serializeJson(payloadValue, value);
        }

        publishTelemetry(name, value.c_str(), unit);
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_EVENT)
    {
        const char *eventName = doc["eventName"].is<const char *>() ? doc["eventName"].as<const char *>() : NULL;
        if (eventName == NULL)
        {
            eventName = doc["name"] | "mcu.event";
        }
        const char *eventData = doc["eventData"].is<const char *>() ? doc["eventData"].as<const char *>() : NULL;
        if (eventData == NULL)
        {
            eventData = doc["data"].is<const char *>() ? doc["data"].as<const char *>() : NULL;
        }
        if (eventData == NULL)
        {
            eventData = doc["raw"] | frame.payload;
        }
        const char *severity = doc["severity"] | "info";
        publishEvent(eventName, eventData, severity);
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_DEFINITION)
    {
        JsonVariant payload = doc.as<JsonVariant>();
        if (!doc["payload"].isNull())
        {
            payload = doc["payload"];
        }

        JsonArray variables = payload["variables"].as<JsonArray>();
        JsonArray functions = payload["functions"].as<JsonArray>();
        size_t variableCount = variables.isNull() ? 0 : variables.size();
        size_t functionCount = functions.isNull() ? 0 : functions.size();

        DynamicJsonDocument definitions(DEFINITIONS_JSON_CAPACITY);
        definitions["schema"] = "nivalo.iot.v1";
        definitions["messageId"] = createMessageId();
        definitions["deviceId"] = _deviceId;
        definitions["sentAt"] = createTimestamp();
        definitions["firmware"]["version"] = _firmwareVersion;
        definitions["firmware"]["hardware"] = _hardwareName;
        addFirmwareTargets(definitions["firmware"]);
        if (!variables.isNull())
        {
            definitions["payload"]["variables"] = variables;
        }
        if (!functions.isNull())
        {
            definitions["payload"]["functions"] = functions;
        }

        String definitionsOutput;
        size_t definitionsBytes = serializeJson(definitions, definitionsOutput);
        bool publishOk = false;
        if (!definitions.overflowed())
        {
            publishOk = MqttClient.publish(_definitionsTopic, definitionsOutput.c_str());
        }

        char publishMessage[160];
        snprintf(
            publishMessage,
            sizeof(publishMessage),
            "variables=%u functions=%u bytes=%u overflow=%s mqtt=%s",
            (unsigned int)variableCount,
            (unsigned int)functionCount,
            (unsigned int)definitionsBytes,
            definitions.overflowed() ? "true" : "false",
            publishOk ? "published" : "failed");
        publishEvent(
            publishOk ? "esp32.definitions.publish.ok" : "esp32.definitions.publish.failed",
            publishMessage,
            publishOk ? "info" : "warning");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_DEFINITIONS_BEGIN)
    {
        publishEvent("mcu.definitions.begin", frame.payloadLength > 0U ? frame.payload : "{}", "info");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_DEFINITIONS_END)
    {
        publishEvent("mcu.definitions.end", frame.payloadLength > 0U ? frame.payload : "{}", "info");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_COMMAND_STATUS)
    {
        const char *commandId = doc["commandId"] | "";
        const char *status = doc["status"] | "failed";
        const char *message = doc["message"].is<const char *>() ? doc["message"].as<const char *>() : NULL;
        float progress = -1;
        if (!doc["progress"].isNull())
        {
            progress = doc["progress"].as<float>();
        }

        publishCommandAck(commandId, status, message, progress);
        return;
    }

    char eventName[48];
    snprintf(eventName, sizeof(eventName), "mcu.nivalolink.type_0x%02x", frame.type);
    publishEvent(eventName, frame.payloadLength > 0U ? frame.payload : "{}", "info");
}

void NivaloDevice::drainNivaloLink(bool forcePoll)
{
    if (NivaloLink.isPaused())
    {
        return;
    }

    unsigned long now = millis();
    bool heartbeatDue = (now - lastNivaloLinkHeartbeat) >= NIVALO_LINK_HEARTBEAT_MS;
    bool pollDue = (now - lastNivaloLinkPoll) >= NIVALO_LINK_PERIODIC_POLL_MS;
    if (!forcePoll && !heartbeatDue && now < nivaloLinkBackoffUntil)
    {
        return;
    }

    bool shouldDrain = forcePoll || NivaloLink.dataReady() || heartbeatDue || pollDue;
    if (!shouldDrain)
    {
        return;
    }

    size_t drained = 0U;
    do
    {
        NivaloLinkReceivedFrame frame;
        bool sent;
        if (heartbeatDue)
        {
            StaticJsonDocument<128> heartbeat;
            heartbeat["uptimeMs"] = millis();
            String payload;
            serializeJson(heartbeat, payload);
            sent = NivaloLink.exchange(NIVALO_LINK_FRAME_HEARTBEAT, payload.c_str(), &frame);
            lastNivaloLinkHeartbeat = now;
            heartbeatDue = false;
        }
        else
        {
            sent = NivaloLink.poll(&frame);
        }

        lastNivaloLinkPoll = now;
        if (!sent)
        {
            queueEventReport("esp32.nivalolink.poll_failed", NivaloLink.lastError(), "warning");
            return;
        }

        if (frame.valid)
        {
            handleNivaloLinkFrame(frame);
            if (frame.type == NIVALO_LINK_FRAME_IDLE && !NivaloLink.dataReady())
            {
                break;
            }
        }
        else
        {
            nivaloLinkInvalidFrameCount++;
            nivaloLinkBackoffUntil = millis() + NIVALO_LINK_INVALID_FRAME_BACKOFF_MS;

            unsigned long reportNow = millis();
            if (forcePoll || (reportNow - lastNivaloLinkInvalidFrameReport) >= NIVALO_LINK_INVALID_FRAME_REPORT_MS)
            {
                char rxPrefix[25];
                char message[160];
                NivaloLink.formatLastRxPrefix(rxPrefix, sizeof(rxPrefix), 12U);
                snprintf(
                    message,
                    sizeof(message),
                    "%s count=%lu dataReady=%s rx=%s",
                    NivaloLink.lastError(),
                    (unsigned long)nivaloLinkInvalidFrameCount,
                    NivaloLink.dataReady() ? "true" : "false",
                    rxPrefix);
                queueEventReport("esp32.nivalolink.frame_invalid", message, "warning");
                lastNivaloLinkInvalidFrameReport = reportNow;
                nivaloLinkInvalidFrameCount = 0;
            }
            break;
        }

        drained++;
    } while ((forcePoll || NivaloLink.dataReady()) && drained < NIVALO_LINK_DRAIN_LIMIT);
}

void NivaloDevice::loop()
{
    pulseStatusPixel();

    MqttClient.loop();

    if (!MqttClient.connected())
    {
        setStatusPixel(255, 0, 0);
        // digitalWrite(BUILTIN_LED, LOW);
        mqttReconnect();
    }

    drainQueuedReports();
#if NIVALO_HAS_SECONDARY_MCU
    drainNivaloLink();
#endif

    if (millis() - lastHeartbeat > 60000)
    {
        lastHeartbeat = millis();

        publishAvailability("online", "heartbeat");
        publishRuntimeTelemetry();
    }

    while (hwAvailable() > 0)
    {
        String uartLine = _hardSerial->readStringUntil('\n');
        uartLine.trim();
        if (uartLine.length() == 0)
        {
            continue;
        }

        Serial.println("GETTING DATA!");
        // Allocate the JSON document
        // This one must be bigger than for the sender because it must store the strings
        DynamicJsonDocument doc(UART_MESSAGE_JSON_CAPACITY);

        // Read one complete newline-delimited JSON document from the STM32 link.
        DeserializationError err = deserializeJson(doc, uartLine);

        if (err == DeserializationError::Ok)
        {
            String commandName = doc["command"].as<String>();
            Serial.println("COMMAND:");
            Serial.println(commandName);
            const char *payload = doc["payload"];

            if (commandName == "function")
            {
            }
            else if (commandName == "publish")
            {
                String output;
                serializeJson(doc, output);

                const char *eventName = doc["payload"]["eventName"] | "stm32.publish";
                const char *eventData = doc["payload"]["eventData"] | output.c_str();
                publishEvent(eventName, eventData);
            }
            else if (commandName == "variable" || commandName == "telemetry")
            {
                String value;
                JsonVariant payload = doc["payload"];
                JsonVariant payloadValue = payload["value"];
                const char *name = payload["name"] | "";
                if (strlen(name) == 0)
                {
                    name = payload["variable"] | "";
                }
                const char *unit = payload["unit"] | "";
                if (strlen(unit) == 0)
                {
                    unit = NULL;
                }

                if (strlen(name) == 0 || payloadValue.isNull())
                {
                    String output;
                    serializeJson(doc, output);
                    publishEvent("stm32.variable.rejected", output.c_str(), "warning");
                }
                else
                {
                    if (payloadValue.is<const char *>())
                    {
                        value = payloadValue.as<const char *>();
                    }
                    else
                    {
                        serializeJson(payloadValue, value);
                    }

                    publishTelemetry(name, value.c_str(), unit);
                }
            }
            else if (commandName == "register")
            {
                String output;
                serializeJson(doc, output);

                JsonArray variables = doc["payload"]["variables"].as<JsonArray>();
                JsonArray functions = doc["payload"]["functions"].as<JsonArray>();
                size_t variableCount = variables.isNull() ? 0 : variables.size();
                size_t functionCount = functions.isNull() ? 0 : functions.size();
                char parsedMessage[120];
                snprintf(
                    parsedMessage,
                    sizeof(parsedMessage),
                    "variables=%u functions=%u bytes=%u",
                    (unsigned int)variableCount,
                    (unsigned int)functionCount,
                    (unsigned int)output.length());
                publishEvent("esp32.register.parsed", parsedMessage);

                DynamicJsonDocument definitions(DEFINITIONS_JSON_CAPACITY);
                definitions["schema"] = "nivalo.iot.v1";
                definitions["messageId"] = createMessageId();
                definitions["deviceId"] = _deviceId;
                definitions["sentAt"] = createTimestamp();
                definitions["firmware"]["version"] = _firmwareVersion;
                definitions["firmware"]["hardware"] = _hardwareName;
                addFirmwareTargets(definitions["firmware"]);
                if (!doc["payload"]["variables"].isNull())
                {
                    definitions["payload"]["variables"] = doc["payload"]["variables"];
                }
                if (!doc["payload"]["functions"].isNull())
                {
                    definitions["payload"]["functions"] = doc["payload"]["functions"];
                }

                String definitionsOutput;
                size_t definitionsBytes = serializeJson(definitions, definitionsOutput);
                bool publishOk = false;
                if (!definitions.overflowed())
                {
                    publishOk = MqttClient.publish(_definitionsTopic, definitionsOutput.c_str());
                }

                char publishMessage[160];
                snprintf(
                    publishMessage,
                    sizeof(publishMessage),
                    "variables=%u functions=%u bytes=%u overflow=%s mqtt=%s",
                    (unsigned int)variableCount,
                    (unsigned int)functionCount,
                    (unsigned int)definitionsBytes,
                    definitions.overflowed() ? "true" : "false",
                    publishOk ? "published" : "failed");
                publishEvent(
                    publishOk ? "esp32.definitions.publish.ok" : "esp32.definitions.publish.failed",
                    publishMessage,
                    publishOk ? "info" : "warning");

                publishEvent("stm32.register", output.c_str());
            }

            else
            {
                // Print the values
                // (we must use as<T>() to resolve the ambiguity)
                Serial.print("command = ");
                Serial.println(doc["command"].as<String>());
                Serial.print("payload = ");
                Serial.println(doc["payload"].as<String>());
            }
        }
        else
        {
            // Print error to the "debug" serial port
            Serial.print("FROM ESP: deserializeJson() returned ");
            Serial.println(err.c_str());
            char parseMessage[96];
            snprintf(
                parseMessage,
                sizeof(parseMessage),
                "%s bytes=%u",
                err.c_str(),
                (unsigned int)uartLine.length());
            publishEvent("esp32.uart.parse_error", parseMessage, "warning");
        }
    }
}

/////////////
// Private //
/////////////

nivalo_status_t NivaloDevice::init(unsigned long baud)
{
    nivalo_status_t err;

    beginSerial(baud); // Begin serial

    _baud = baud;

    return NIVALO_STATUS_SUCCESS;
}

size_t NivaloDevice::hwPrint(const char *s)
{
    if (_hardSerial != NULL)
    {
        return _hardSerial->print(s);
    }

    return (size_t)0;
}

size_t NivaloDevice::hwWrite(const char c)
{
    if (_hardSerial != NULL)
    {
        return _hardSerial->write(c);
    }

    return (size_t)0;
}

int NivaloDevice::readAvailable(char *inString)
{
    int len = 0;

    if (_hardSerial != NULL)
    {
        while (_hardSerial->available())
        {
            char c = (char)_hardSerial->read();
            if (inString != NULL)
            {
                inString[len++] = c;
            }
        }
        if (inString != NULL)
        {
            inString[len] = 0;
        }
    }

    return len;
}

char NivaloDevice::readChar(void)
{
    char ret;

    if (_hardSerial != NULL)
    {
        ret = (char)_hardSerial->read();
    }

    return ret;
}

byte NivaloDevice::readByte(void)
{
    byte ret;

    if (_hardSerial != NULL)
    {
        ret = (byte)_hardSerial->read();
    }

    return ret;
}

int NivaloDevice::hwAvailable(void)
{
    if (_hardSerial != NULL)
    {
        return _hardSerial->available();
    }

    return -1;
}

void NivaloDevice::beginSerial(unsigned long baud)
{
    if (_hardSerial != NULL)
    {
#if defined(ARDUINO_ARCH_ESP32)
        _hardSerial->setRxBufferSize(STM32_UART_RX_BUFFER_SIZE);
        _hardSerial->begin(baud, SERIAL_8N1, RX, TX);
#else
        _hardSerial->begin(baud);
#endif
        _hardSerial->setTimeout(250);
    }

    delay(100);
}

void NivaloDevice::setTimeout(unsigned long timeout)
{
    if (_hardSerial != NULL)
    {
        _hardSerial->setTimeout(timeout);
    }
}

bool NivaloDevice::find(char *target)
{
    bool found = false;
    if (_hardSerial != NULL)
    {
        found = _hardSerial->find(target);
    }

    return found;
}

void NivaloDevice::drainQueuedReports()
{
    for (size_t i = 0; i < PENDING_REPORT_COUNT; i++)
    {
        if (pendingCommandAcks[i].queued)
        {
            PendingCommandAck ack = pendingCommandAcks[i];
            pendingCommandAcks[i].queued = false;
            publishCommandAck(ack.commandId, ack.status, ack.message, ack.progress);
        }
    }

    for (size_t i = 0; i < PENDING_REPORT_COUNT; i++)
    {
        if (pendingEventReports[i].queued)
        {
            PendingEventReport event = pendingEventReports[i];
            pendingEventReports[i].queued = false;
            publishEvent(event.name, event.data, event.severity);
        }
    }
}

void NivaloDevice::publishCommandAck(const char *commandId, const char *status, const char *message, float progress)
{
    if (commandId == NULL || strlen(commandId) == 0)
    {
        return;
    }

    char topic[190];
    snprintf(topic, sizeof(topic), "%s%s", _ackTopicPrefix, commandId);

    String output;
    StaticJsonDocument<512> doc;
    doc["schema"] = "nivalo.iot.v1";
    doc["messageId"] = createMessageId();
    doc["correlationId"] = commandId;
    doc["deviceId"] = _deviceId;
    doc["sentAt"] = createTimestamp();
    doc["payload"]["status"] = status;
    if (message != NULL)
    {
        doc["payload"]["message"] = message;
    }
    if (progress >= 0)
    {
        doc["payload"]["progress"] = progress;
    }

    serializeJson(doc, output);
    if (!MqttClient.connected())
    {
        mqttReconnect();
    }

    bool published = MqttClient.publish(topic, output.c_str());
    if (!published)
    {
        Serial.print("Command ACK publish failed: ");
        Serial.println(commandId);
    }
    else
    {
        MqttClient.loop();
    }
}

String NivaloDevice::getCommandIdFromTopic(const char *topic)
{
    String value = String(topic);
    int index = value.lastIndexOf('/');
    if (index < 0 || index + 1 >= value.length())
    {
        return "";
    }

    return value.substring(index + 1);
}

String NivaloDevice::createMessageId()
{
    char value[37];
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    uint32_t c = esp_random();
    uint32_t d = esp_random();
    snprintf(
        value,
        sizeof(value),
        "%08x-%04x-%04x-%04x-%04x%08x",
        a,
        (uint16_t)(b >> 16),
        (uint16_t)((b & 0x0FFF) | 0x4000),
        (uint16_t)((c & 0x3FFF) | 0x8000),
        (uint16_t)(c >> 16),
        d);
    return String(value);
}

String NivaloDevice::createTimestamp()
{
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 50))
    {
        char value[25];
        strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
        return String(value);
    }

    return "1970-01-01T00:00:00Z";
}
