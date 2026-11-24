// OTA install of specs/cloud-mqtt-v1.1.md section 11.4 on Arduino-ESP32 2.0.17 (ESP-IDF 4.4).
//
// The image streams over certificate-validating HTTPS, without redirects, straight into the
// inactive application slot while its SHA-256 and embedded identity are computed. esp_ota_end()
// validates the image; the boot partition is only selected later, by the executor's single
// activation call, after the pending record is durable and a fresh clock reading allows it.
// (Update.end() would do both at once, which leaves no room for that ordering.)

#include "NivaloDevice.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include <new>

namespace
{
static constexpr size_t ChunkBytes = 4096U;
static constexpr unsigned long IdleTimeoutMs = 15000UL;
static constexpr unsigned long ServiceIntervalMs = 250UL;
} // namespace

NivaloDownloadOutcome NivaloDevice::downloadFirmware(const NivaloOperation &operation)
{
    _otaTarget = NULL;
    const NivaloOtaManifest &manifest = operation.manifest;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL || target->size < manifest.sizeBytes)
    {
        return NIVALO_DOWNLOAD_REJECTED;
    }

    WiFiClientSecure tls;
    tls.setCACert(_apiCaCertificate != NULL ? _apiCaCertificate : NivaloConnection::defaultCaCertificate());
    HTTPClient http;
    // Pauses the NivaloLink transport in bridge builds and restores it on every return path.
    NivaloOtaSession session(_link.transport(), _otaPins, http);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    if (!http.begin(tls, operation.url))
    {
        return NIVALO_DOWNLOAD_REJECTED;
    }
    const int status = http.GET();
    const int contentLength = http.getSize();
    // An exact Content-Length is required: a chunked body would be read with its framing.
    if (status != HTTP_CODE_OK || contentLength < 0 || (uint32_t)contentLength != manifest.sizeBytes)
    {
        Serial.printf("Nivalo OTA: download refused (HTTP %d)\n", status);
        return NIVALO_DOWNLOAD_REJECTED;
    }

    uint8_t *chunk = new (std::nothrow) uint8_t[ChunkBytes];
    if (chunk == NULL)
    {
        return NIVALO_DOWNLOAD_REJECTED;
    }
    if (_watchdogEnabled)
    {
        esp_task_wdt_reset();
    }
    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(target, manifest.sizeBytes, &handle) != ESP_OK)
    {
        delete[] chunk;
        return NIVALO_DOWNLOAD_REJECTED;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts_ret(&sha, 0) == 0;
    NivaloImageDescriptorScanner scanner;
    WiFiClient *stream = http.getStreamPtr();
    uint32_t received = 0U;
    unsigned long lastData = millis();
    unsigned long lastService = 0U;
    while (ok && received < manifest.sizeBytes)
    {
        if (millis() - lastService >= ServiceIntervalMs)
        {
            // Keep the session, presence and duplicate answers alive during the download.
            serviceWhileBusy();
            lastService = millis();
        }
        const size_t available = stream == NULL ? 0U : (size_t)stream->available();
        if (available == 0U)
        {
            if (!http.connected() || millis() - lastData > IdleTimeoutMs)
            {
                ok = false;
                break;
            }
            delay(5);
            continue;
        }
        const size_t wanted = min(min(available, ChunkBytes), (size_t)(manifest.sizeBytes - received));
        const int read = stream->readBytes(chunk, wanted);
        if (read <= 0)
        {
            ok = false;
            break;
        }
        ok = mbedtls_sha256_update_ret(&sha, chunk, (size_t)read) == 0 &&
             esp_ota_write(handle, chunk, (size_t)read) == ESP_OK;
        scanner.update(chunk, (size_t)read);
        received += (uint32_t)read;
        lastData = millis();
    }
    // Exactly sizeBytes: anything after them is ignored only if it never arrives.
    if (ok && stream != NULL && stream->available() > 0)
    {
        ok = false;
    }
    uint8_t digest[32];
    ok = ok && received == manifest.sizeBytes && mbedtls_sha256_finish_ret(&sha, digest) == 0;
    mbedtls_sha256_free(&sha);
    delete[] chunk;
    session.closeDownload();

    char digestHex[65] = {0};
    if (ok)
    {
        nivaloHexEncode(digest, sizeof(digest), digestHex);
    }
    NivaloImageIdentity embedded;
    ok = ok && strcmp(digestHex, manifest.sha256) == 0 && scanner.identity(&embedded) &&
         strcmp(embedded.productId, manifest.productId) == 0 && embedded.securityVersion == manifest.securityVersion;
    if (!ok)
    {
        esp_ota_abort(handle);
        Serial.println("Nivalo OTA: image rejected (size, digest or embedded identity)");
        return NIVALO_DOWNLOAD_REJECTED;
    }
    // Validates the image in the slot; does not select it.
    if (esp_ota_end(handle) != ESP_OK)
    {
        Serial.println("Nivalo OTA: image validation failed");
        return NIVALO_DOWNLOAD_REJECTED;
    }
    _otaTarget = target;
    return NIVALO_DOWNLOAD_VERIFIED;
}

NivaloDownloadOutcome NivaloDevice::Platform::downloadImage(const NivaloOperation &operation)
{
    return _device.downloadFirmware(operation);
}

NivaloActivationOutcome NivaloDevice::Platform::activateImage()
{
    const esp_partition_t *target = _device._otaTarget;
    _device._otaTarget = NULL;
    if (target == NULL)
    {
        return NIVALO_ACTIVATION_REJECTED;
    }
    const esp_err_t result = esp_ota_set_boot_partition(target);
    if (result == ESP_OK)
    {
        return NIVALO_ACTIVATION_SELECTED;
    }
    // Validation and argument errors happen before otadata is touched: definitely not selected.
    // Anything else (a flash erase or write error in otadata) may have selected it.
    if (result == ESP_ERR_OTA_VALIDATE_FAILED || result == ESP_ERR_INVALID_ARG || result == ESP_ERR_NOT_FOUND)
    {
        return NIVALO_ACTIVATION_REJECTED;
    }
    return NIVALO_ACTIVATION_UNCERTAIN;
}

bool NivaloDevice::Platform::hashRunningSlot(uint32_t sizeBytes, char sha256Hex[65])
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL || sizeBytes == 0U || sizeBytes > running->size)
    {
        return false;
    }
    uint8_t *chunk = new (std::nothrow) uint8_t[ChunkBytes];
    if (chunk == NULL)
    {
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts_ret(&sha, 0) == 0;
    // Only the signed sizeBytes; the rest of the slot is padding.
    for (uint32_t offset = 0U; ok && offset < sizeBytes;)
    {
        const size_t count = min((size_t)ChunkBytes, (size_t)(sizeBytes - offset));
        ok = esp_partition_read(running, offset, chunk, count) == ESP_OK &&
             mbedtls_sha256_update_ret(&sha, chunk, count) == 0;
        offset += (uint32_t)count;
        if (_device._watchdogEnabled)
        {
            esp_task_wdt_reset();
        }
    }
    uint8_t digest[32];
    ok = ok && mbedtls_sha256_finish_ret(&sha, digest) == 0;
    mbedtls_sha256_free(&sha);
    delete[] chunk;
    if (ok)
    {
        nivaloHexEncode(digest, sizeof(digest), sha256Hex);
    }
    return ok;
}

void NivaloDevice::Platform::restart()
{
    _device.shutdown();
    delay(200);
    ESP.restart();
}
