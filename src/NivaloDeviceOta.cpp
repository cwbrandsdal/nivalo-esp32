#include "NivaloDevice.h"
#include "NivaloFirmwareSecurity.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>

#if NIVALO_HAS_SECONDARY_MCU
static constexpr uint32_t FlashStartAddress = 0x08000000UL;
#endif

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

static bool computeFileSha256(fs::FS &fs, const char *path, size_t expectedSize, char output[65])
{
    File file = fs.open(path, FILE_READ);
    if (!file || file.size() != expectedSize)
    {
        if (file)
        {
            file.close();
        }
        return false;
    }

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts_ret(&context, 0) != 0)
    {
        mbedtls_sha256_free(&context);
        file.close();
        return false;
    }

    uint8_t hashBuffer[1024];
    size_t totalRead = 0U;
    bool ok = true;
    while (totalRead < expectedSize)
    {
        size_t requested = min(sizeof(hashBuffer), expectedSize - totalRead);
        size_t read = file.read(hashBuffer, requested);
        if (read == 0U || mbedtls_sha256_update_ret(&context, hashBuffer, read) != 0)
        {
            ok = false;
            break;
        }
        totalRead += read;
    }

    uint8_t digest[32];
    if (!ok || totalRead != expectedSize || mbedtls_sha256_finish_ret(&context, digest) != 0)
    {
        ok = false;
    }
    mbedtls_sha256_free(&context);
    file.close();
    if (ok)
    {
        bytesToHex(digest, sizeof(digest), output, 65U);
    }
    return ok;
}

#if NIVALO_HAS_SECONDARY_MCU
static bool copyStm32FlashToFile(NivaloStm32Dap &programmer, const char *path, size_t imageSize)
{
    SPIFFS.remove(path);
    File output = SPIFFS.open(path, FILE_WRITE);
    if (!output)
    {
        return false;
    }

    uint8_t transfer[4096];
    size_t offset = 0U;
    bool ok = true;
    while (offset < imageSize)
    {
        size_t count = min(sizeof(transfer), imageSize - offset);
        if (!programmer.dap_read_block(FlashStartAddress + (uint32_t)offset, transfer, (int)count) ||
            output.write(transfer, count) != count)
        {
            ok = false;
            break;
        }
        offset += count;
        yield();
    }
    output.flush();
    output.close();
    if (!ok || offset != imageSize)
    {
        SPIFFS.remove(path);
        return false;
    }
    return true;
}

static bool programStm32FromFile(NivaloStm32Dap &programmer, const char *path, size_t imageSize)
{
    File input = SPIFFS.open(path, FILE_READ);
    if (!input || input.size() != imageSize)
    {
        if (input)
        {
            input.close();
        }
        return false;
    }

    programmer.programPrepare(FlashStartAddress, imageSize);
    uint8_t transfer[4096] __attribute__((aligned(4)));
    size_t offset = 0U;
    bool ok = true;
    while (offset < imageSize)
    {
        size_t count = min(sizeof(transfer), imageSize - offset);
        memset(transfer, 0xFF, sizeof(transfer));
        if (input.read(transfer, count) != count ||
            !programmer.programFlash(FlashStartAddress + (uint32_t)offset, transfer, count, false))
        {
            ok = false;
            break;
        }
        offset += count;
        yield();
    }
    input.close();
    return ok && offset == imageSize;
}

static bool verifyStm32AgainstFile(NivaloStm32Dap &programmer, const char *path, size_t imageSize)
{
    File input = SPIFFS.open(path, FILE_READ);
    if (!input || input.size() != imageSize)
    {
        if (input)
        {
            input.close();
        }
        return false;
    }

    uint8_t expected[4096];
    uint8_t actual[4096];
    size_t offset = 0U;
    bool ok = true;
    while (offset < imageSize)
    {
        size_t count = min(sizeof(expected), imageSize - offset);
        if (input.read(expected, count) != count ||
            !programmer.dap_read_block(FlashStartAddress + (uint32_t)offset, actual, (int)count) ||
            memcmp(expected, actual, count) != 0)
        {
            ok = false;
            break;
        }
        offset += count;
        yield();
    }
    input.close();
    return ok && offset == imageSize;
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

// Function called when there's an SWD error
void error(const char *text)
{
    Serial.println(text);
    while (1)
        ;
}
void NivaloDevice::handleFirmwareCommand(const String &commandId, JsonObject arguments)
{
        Serial.println("[HTTP] Starting up...");

        const char *target = arguments["target"] | "";
        const char *firmwareUrl = arguments["url"] | "";
        const char *expectedSha256 = arguments["sha256"] | "";
        const char *signatureAlgorithm = arguments["signature"]["algorithm"] | "";
        const char *signatureKeyId = arguments["signature"]["keyId"] | "";
        const char *signatureValue = arguments["signature"]["value"] | "";
        uint32_t expectedSizeBytes = arguments["sizeBytes"] | 0U;

        auto rejectFlash = [&](const char *reason)
        {
            publishEvent("esp32.flash.rejected", reason, "warning");
            if (commandId.length() > 0)
            {
                publishCommandAck(commandId.c_str(), "failed", reason);
            }
        };

        if (firmwareUrl[0] == '\0')
        {
            rejectFlash("Missing firmware URL");
            return;
        }
        String firmwareDownloadUrl = firmwareUrl;

        bool targetIsEsp32 = strcmp(target, "esp32") == 0;
        bool targetIsStm32 = strcmp(target, "stm32") == 0;

        if (!targetIsEsp32 && !targetIsStm32)
        {
            rejectFlash("Unsupported or non-canonical firmware target");
            return;
        }

        if (!arguments["sizeBytes"].is<uint32_t>() || expectedSizeBytes == 0U)
        {
            rejectFlash("Firmware sizeBytes is missing or outside the supported unsigned 32-bit range");
            return;
        }

#if !NIVALO_HAS_SECONDARY_MCU
        if (targetIsStm32)
        {
            rejectFlash("Secondary MCU flashing is not enabled for this build");
            return;
        }
#endif

        char signedManifest[160];
        size_t signedManifestLength = 0U;
        if (!nivaloBuildFirmwareManifest(
                target,
                expectedSizeBytes,
                expectedSha256,
                signedManifest,
                sizeof(signedManifest),
                &signedManifestLength))
        {
            rejectFlash("Firmware manifest target, sizeBytes, or sha256 is missing or malformed");
            return;
        }

        char signatureFailure[120] = {0};
        if (!nivaloVerifyFirmwareSignature(
                _firmwareSigningKeys,
                _firmwareSigningKeyCount,
                signatureAlgorithm,
                signatureKeyId,
                signatureValue,
                signedManifest,
                signedManifestLength,
                signatureFailure,
                sizeof(signatureFailure)))
        {
            rejectFlash(signatureFailure);
            return;
        }

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
        _connection.client().loop();

        bool flashSucceeded = false;

        HTTPClient http;
        NivaloOtaSession otaSession(_link.transport(), _ota, http);
        http.setConnectTimeout(15000);
        http.setTimeout(15000);

        // Your Domain name with URL path or IP address with path
        if (!http.begin(firmwareDownloadUrl, NivaloConnection::defaultCaCertificate()))
        {
            flashFailure = "Firmware download URL rejected";
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
                _statusPixel.rainbow(1);

                // get length of document (is -1 when Server sends no Content-Length header)
                int len = http.getSize();
                Serial.printf("[HTTP] GET... size: %d\n", len);

                if (len >= 0 && (uint32_t)len != expectedSizeBytes)
                {
                    flashFailure = "Firmware Content-Length does not match signed sizeBytes";
                    publishEvent("esp32.flash.verification-failed", flashFailure.c_str(), "warning");
                    if (commandId.length() > 0)
                    {
                        publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                    }
                    return;
                }

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

                // _ota.dap().programFlash(addr, fwbinfile, sizeof(fwbinfile), true);
                //  uint8_t firmware[len];

                Serial.println("mounting FS...");

                if (SPIFFS.begin(true))
                {
                    listDir(SPIFFS, "/", 0);
                    Serial.println("mounted file system");

                    SPIFFS.remove("/firmware.bin");
                    size_t availableStorage = SPIFFS.totalBytes() - SPIFFS.usedBytes();
                    if (availableStorage < expectedSizeBytes)
                    {
                        flashFailure = "Insufficient durable storage for verified firmware staging";
                        publishEvent("esp32.flash.rejected", flashFailure.c_str(), "warning");
                        if (commandId.length() > 0)
                        {
                            publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                        }
                        return;
                    }
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
                        bool downloadWriteOk = true;

                        while (http.connected() && (len > 0 || len == -1))
                        {
                            // Firmware downloads can take longer than the configured loop-task
                            // watchdog window on constrained links. This handler runs synchronously
                            // from loop(), so service the same watchdog that NivaloDevice::loop()
                            // normally resets until control returns to the application.
                            if (_watchdogEnabled)
                            {
                                esp_task_wdt_reset();
                            }

                            // get available data size
                            size_t size = stream->available();

                            if (size)
                            {
                                // read up to 128 byte
                                int c = stream->readBytes(_ota.buffer(), ((size > _ota.bufferSize()) ? _ota.bufferSize() : size));

                                // write it to Serial
                                // Serial.write(buf, c);

                                Serial.printf("[Writing to staging] chunk=%u bytes=%d remaining=%d\n", (unsigned int)_ota.bufferSize(), c, len);
                                // programDap(c);

                                // _ota.dap().programFlash(addr, buf, c, false);
                                if (c <= 0 || downloadedBytes + (size_t)c > expectedSizeBytes ||
                                    firmwareFile.write(_ota.buffer(), (size_t)c) != (size_t)c)
                                {
                                    flashFailure = downloadedBytes + (size_t)max(c, 0) > expectedSizeBytes
                                                       ? "Firmware download exceeds signed sizeBytes"
                                                       : "Durable firmware staging write failed";
                                    downloadWriteOk = false;
                                    break;
                                }
                                (void)mbedtls_sha256_update_ret(&shaContext, _ota.buffer(), (size_t)c);
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
                        firmwareFile.flush();
                        firmwareFile.close();

                        if (!downloadWriteOk || downloadedBytes != expectedSizeBytes)
                        {
                            deleteFile(SPIFFS, "/firmware.bin");
                            if (downloadWriteOk)
                            {
                                flashFailure = "Firmware size verification failed";
                            }
                            publishEvent("esp32.flash.verification-failed", flashFailure.c_str(), "warning");
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }

                        char stagedSha256Text[65] = {0};
                        if (strcmp(expectedSha256, actualSha256Text) != 0 ||
                            !computeFileSha256(SPIFFS, "/firmware.bin", expectedSizeBytes, stagedSha256Text) ||
                            strcmp(expectedSha256, stagedSha256Text) != 0)
                        {
                            deleteFile(SPIFFS, "/firmware.bin");
                            flashFailure = "Firmware SHA-256 verification failed";
                            publishEvent("esp32.flash.verification-failed", flashFailure.c_str(), "warning");
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
                            // End the network phase before opening the updater.
                            otaSession.closeDownload();

                            File esp32FirmwareFile = SPIFFS.open("/firmware.bin", FILE_READ);
                            if (!esp32FirmwareFile)
                            {
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
                                publishEvent("esp32.flash.failed", updateError.c_str(), "error");
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

                            ESP.restart();
                            return;
                        }

#if NIVALO_HAS_SECONDARY_MCU
                        _ota.dap().begin(_pins.ota.swclk, _pins.ota.swdio, _pins.ota.swreset, &error);
                        Serial.println("Connecting to DAP...");
                        if (!_ota.dap().targetConnect())
                        {
                            flashFailure = String("DAP target connect failed: ") + _ota.dap().error_message;
                            firmwareFile.close();
                            deleteFile(SPIFFS, "/firmware.bin");
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }

                        char debuggername[100];
                        _ota.dap().dap_get_debugger_info(debuggername);
                        Serial.println(debuggername);

                        uint32_t dsu_did;
                        if (!_ota.dap().select(&dsu_did))
                        {
                            Serial.println("Unknown device found 0x");
                            Serial.println(dsu_did, HEX);
                            flashFailure = "Unknown STM32 target";
                            firmwareFile.close();
                            deleteFile(SPIFFS, "/firmware.bin");
                            _ota.dap().dap_disconnect();
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }





                        if (expectedSizeBytes > _ota.dap().target_device.flash_size)
                        {
                            flashFailure = "Signed STM32 image exceeds target flash capacity";
                            deleteFile(SPIFFS, "/firmware.bin");
                            _ota.dap().deselect();
                            _ota.dap().dap_disconnect();
                            publishEvent("esp32.flash.rejected", flashFailure.c_str(), "warning");
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }

                        const char *recoveryPath = NULL;
                        size_t recoverySize = 0U;
                        bool recoveryIsSnapshot = false;
                        char goldenDigest[65] = {0};
                        if (_stm32GoldenImagePath.length() > 0U)
                        {
                            recoveryPath = _stm32GoldenImagePath.c_str();
                            recoverySize = _stm32GoldenImageSizeBytes;
                            if (_stm32GoldenImagePath == "/firmware.bin" ||
                                _stm32GoldenImagePath == "/stm32-prior-known-good.bin" ||
                                recoverySize == 0U || recoverySize > _ota.dap().target_device.flash_size ||
                                _stm32GoldenImageSha256.length() != 64U ||
                                !computeFileSha256(SPIFFS, recoveryPath, recoverySize, goldenDigest) ||
                                strcmp(goldenDigest, _stm32GoldenImageSha256.c_str()) != 0)
                            {
                                flashFailure = "Configured STM32 golden image is missing, malformed, or corrupt";
                            }
                        }
                        else if (_preserveKnownGoodStm32Image)
                        {
                            recoveryPath = "/stm32-prior-known-good.bin";
                            recoverySize = _knownGoodStm32ImageSizeBytes;
                            recoveryIsSnapshot = true;
                            SPIFFS.remove(recoveryPath);
                            size_t freeBytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
                            if (recoverySize == 0U || recoverySize > _ota.dap().target_device.flash_size)
                            {
                                flashFailure = "Known-good STM32 image size is not configured or exceeds target flash";
                            }
                            else if (freeBytes < recoverySize)
                            {
                                flashFailure = "Insufficient durable storage for STM32 recovery snapshot";
                            }
                            else if (!copyStm32FlashToFile(_ota.dap(), recoveryPath, recoverySize) ||
                                     !verifyStm32AgainstFile(_ota.dap(), recoveryPath, recoverySize))
                            {
                                flashFailure = "STM32 known-good recovery snapshot could not be preserved and verified";
                            }
                            else
                            {
                                publishEvent("esp32.flash.stm32-recovery-prepared", "Known-good STM32 image preserved", "info");
                            }
                        }
                        else
                        {
                            flashFailure = "STM32 recovery is not configured; refusing to program";
                        }

                        if (recoveryPath == NULL || recoverySize == 0U || flashFailure != "Flash command failed")
                        {
                            deleteFile(SPIFFS, "/firmware.bin");
                            _ota.dap().deselect();
                            _ota.dap().dap_disconnect();
                            publishEvent("esp32.flash.rejected", flashFailure.c_str(), "warning");
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }

                        // End the network phase before taking exclusive SWD ownership.
                        otaSession.closeDownload();
                        publish("flashing", "[DAP] Programming and full read-back verification...");
                        if (commandId.length() > 0)
                        {
                            publishCommandAck(commandId.c_str(), "running", "Programming STM32 flash", 50);
                        }

                        bool stm32ProgrammingOk = programStm32FromFile(_ota.dap(), "/firmware.bin", expectedSizeBytes);
                        if (stm32ProgrammingOk && commandId.length() > 0)
                        {
                            publishCommandAck(commandId.c_str(), "running", "Read-back verifying STM32 flash", 85);
                        }
                        bool stm32Verified = stm32ProgrammingOk &&
                                             verifyStm32AgainstFile(_ota.dap(), "/firmware.bin", expectedSizeBytes);

                        if (!stm32Verified)
                        {
                            flashFailure = stm32ProgrammingOk
                                               ? "STM32 full read-back verification failed"
                                               : "STM32 programming failed";
                            publishEvent("esp32.flash.stm32-recovery-started", flashFailure.c_str(), "warning");
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "running", "STM32 update failed; restoring known-good image", 90);
                            }

                            bool recoveryProgrammed = programStm32FromFile(_ota.dap(), recoveryPath, recoverySize);
                            bool recoveryVerified = recoveryProgrammed &&
                                                    verifyStm32AgainstFile(_ota.dap(), recoveryPath, recoverySize);
                            if (recoveryVerified)
                            {
                                flashFailure += "; known-good image restored and verified";
                                publishEvent("esp32.flash.stm32-recovery-succeeded", flashFailure.c_str(), "warning");
                                if (recoveryIsSnapshot)
                                {
                                    SPIFFS.remove(recoveryPath);
                                }
                            }
                            else
                            {
                                flashFailure += "; automatic recovery failed (recovery image retained)";
                                publishEvent("esp32.flash.stm32-recovery-failed", flashFailure.c_str(), "error");
                            }

                            _ota.dap().deselect();
                            _ota.dap().dap_disconnect();
                            if (commandId.length() > 0)
                            {
                                publishCommandAck(commandId.c_str(), "failed", flashFailure.c_str());
                            }
                            return;
                        }

                        publishEvent("esp32.flash.stm32-verified", "STM32 flash matched the signed staged image", "info");
                        flashSucceeded = true;
                        if (recoveryIsSnapshot)
                        {
                            SPIFFS.remove(recoveryPath);
                        }
                        _ota.dap().deselect();
                        _ota.dap().dap_disconnect();
#endif
                    }
                    else
                    {
                        flashFailure = "Durable firmware staging file could not be opened";
                    }
                    listDir(SPIFFS, "/", 0);
                    deleteFile(SPIFFS, "/firmware.bin");
                    listDir(SPIFFS, "/", 0);
                }
                else
                {
                    Serial.println("failed to mount FS");
                    flashFailure = "Durable firmware staging filesystem could not be mounted";
                }

#ifdef LED_BUILTIN
                digitalWrite(LED_BUILTIN, LOW);
#endif

                if (flashSucceeded)
                {
                    mqttReconnect();
                    publish("flashing", "[DAP] Firmware flashed and verified successfully!");
                    nivalo_status_t err = init();
                    (void)err;
                }
            }
        }
        else
        {
            Serial.print("Error code: ");
            Serial.println(httpResponseCode);
            flashFailure = String("Firmware download failed: ") + http.errorToString(httpResponseCode);
        }
        if (!flashSucceeded)
        {
            publishEvent("esp32.flash.failed", flashFailure.c_str(), "error");
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

        ESP.restart();
    }
