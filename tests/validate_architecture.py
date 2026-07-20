#!/usr/bin/env python3
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main() -> None:
    device_path = ROOT / "src/NivaloDevice.cpp"
    device = device_path.read_text()
    header = (ROOT / "src/NivaloDevice.h").read_text()
    connection = (ROOT / "src/NivaloConnection.cpp").read_text()
    reconnect_policy = (ROOT / "src/NivaloReconnectPolicy.cpp").read_text()
    protocol = (ROOT / "src/NivaloProtocol.cpp").read_text()
    ota = (ROOT / "src/NivaloOta.cpp").read_text()
    lifecycle = (ROOT / "src/NivaloDeviceLifecycle.cpp").read_text()
    command = (ROOT / "src/NivaloDeviceCommand.cpp").read_text()
    device_ota = (ROOT / "src/NivaloDeviceOta.cpp").read_text()
    link = (ROOT / "src/NivaloDeviceLink.cpp").read_text()
    responsibility_files = [
        ROOT / "src/NivaloDeviceSdk.cpp",
        ROOT / "src/NivaloDeviceMessaging.cpp",
        ROOT / "src/NivaloDeviceReports.cpp",
        ROOT / "src/NivaloDeviceSerial.cpp",
        ROOT / "src/NivaloDeviceLifecycle.cpp",
        ROOT / "src/NivaloDeviceCommand.cpp",
        ROOT / "src/NivaloDeviceOta.cpp",
        ROOT / "src/NivaloDeviceLink.cpp",
        ROOT / "src/NivaloStatusPixel.cpp",
    ]
    assert all(path.exists() for path in responsibility_files)
    responsibilities = "\n".join(path.read_text() for path in responsibility_files)
    all_device_sources = device + "\n" + responsibilities
    assert len(device.splitlines()) <= 400
    assert "void NivaloDevice::mqttCallback" not in device and "void NivaloDevice::mqttCallback" in command
    assert "void NivaloDevice::handleFirmwareCommand" in device_ota
    assert "boolean NivaloDevice::beginMqtt" in lifecycle
    assert "void NivaloDevice::drainNivaloLink" in link
    assert "void NivaloDevice::handleNivaloLinkFrame" in link
    assert 'exchange(NIVALO_LINK_FRAME_HELLO' in link
    assert 'hello["protocol"] = "NivaloLink"' in link
    assert 'hello["transport"] = "spi-master"' in link
    assert "_link.helloPending = false" in link
    assert "InvalidFrameReportMs = 60000UL" in link
    assert 'diagnostic["error"] = _link.transport().lastError();' in link
    assert 'diagnostic["count"] = _link.invalidFrameCount;' in link
    assert 'diagnostic["dataReady"] = _link.transport().dataReady();' in link
    assert 'diagnostic["rxPrefix"] = rxPrefix;' in link
    assert 'queueEventReport("esp32.nivalolink.frame_invalid", diagnosticJson, "warning")' in link
    assert "forcePoll || (reportNow - _link.lastInvalidFrameReport)" not in link
    assert link.index(
        'queueEventReport("esp32.nivalolink.frame_invalid", diagnosticJson, "warning")'
    ) < link.index("_link.invalidFrameCount = 0;")
    assert "bool helloPending = true" in (ROOT / "src/NivaloLinkManager.h").read_text()
    assert "NivaloConnection _connection" in header
    assert "NivaloProtocol _protocol" in header
    assert "NivaloLinkManager _link" in header
    assert "NivaloOtaManager _ota" in header
    assert "NivaloStatusPixel _statusPixel" in header
    assert "boolean begin(const NivaloDeviceConfig &config)" in header
    assert "NivaloPinMap" in header and "telemetryBufferCapacity" in header
    assert "while (!_mqtt.connected())" not in connection
    assert "NivaloReconnectPolicy _retryPolicy" in (ROOT / "src/NivaloConnection.h").read_text()
    assert "_retryPolicy.isAttemptDue(nowMs)" in connection
    assert "_retryPolicy.onFailure(nowMs, esp_random())" in connection
    assert "_retryPolicy.onSuccess(nowMs)" in connection
    assert "Arduino" not in reconnect_policy and "esp_random" not in reconnect_policy
    assert (ROOT / "tests/host/test_reconnect_policy.cpp").exists()
    assert "_availabilityTopic.c_str(), 1, true" in connection
    assert "configTime(0, 0" in protocol and "1970-01-01" not in all_device_sources
    assert all_device_sources.count("if (!_protocol.timeValid())") >= 3
    assert "esp_task_wdt_reset" in device
    assert "NivaloOtaSession::~NivaloOtaSession" in ota
    assert "closeDownload();" in ota and "_link.setPaused(false);" in ota and "_ota.releasePins();" in ota
    assert "http.end()" not in device_ota
    assert "setPaused(false)" not in device_ota
    assert "_ota.releasePins()" not in device_ota
    assert device_ota.count("otaSession.closeDownload()") == 2
    assert "Adafruit_DAP_STM32 dap;" not in device
    assert "PubSubClient MqttClient" not in device
    assert "static NivaloLinkSpiTransport" not in device
    assert "static PendingCommandAck" not in device and "uint8_t buf[" not in device
    assert "return _connection.publish(_eventsTopic" in responsibilities
    assert "_telemetryBufferCapacity" in responsibilities
    for mutable_global in ("Adafruit_NeoPixel strip", "neoPixelBrightness", "neoPixelLoopCount", "isNeoPixelUpwords"):
        assert mutable_global not in all_device_sources
    assert not (ROOT / "src/bootloader.h").exists()
    assert not list(ROOT.glob("examples/**/patch_adafruit_dap_stm32.py"))
    assert 'Serial.println(doc["payload"]' not in all_device_sources
    assert "Serial.print((char)message[i])" not in all_device_sources
    assert 'else if (doc["payload"].is<const char *>())' in command
    assert 'const char *commandJson = reinterpret_cast<const char *>(message);' in command
    assert 'deserializeJson(doc, commandJson, length)' in command
    assert 'deserializeJson(doc, message, length)' not in command
    assert 'serializeJson(arguments, forwardedCommand)' in command
    assert 'forwardedCommand += ",\\\"payload\\\":"' in command
    assert 'forwardedCommand.length() > NIVALO_LINK_MAX_PAYLOAD' in command
    assert 'doc["payload"]["data"] = serialized(eventDataJson);' in responsibilities
    assert 'doc["payload"]["raw"] = data;' in responsibilities
    for bridge_path in [ROOT / "examples/Esp32Stm32Bridge/src/main.cpp"]:
        bridge = bridge_path.read_text()
        assert len(bridge.splitlines()) <= 140
        for abandoned in ("BLEDevice", "WebServer", "SPIFFS", "STATE_LOAD_SETTINGS", "setupSpiffsAndGetSettings"):
            assert abandoned not in bridge
    assert 'if (commandName == "function")' not in device
    metadata = json.loads((ROOT / "library.json").read_text())
    assert metadata["version"] == "0.2.0"
    assert {d["name"]: d["version"] for d in metadata["dependencies"]} == {
        "PubSubClient": "2.8",
        "ArduinoJson": "6.21.6",
        "Adafruit NeoPixel": "1.15.5",
    }
    assert all(d["name"] != "Adafruit DAP library" for d in metadata["dependencies"])
    for project in ("Esp32Only", "Esp32Stm32Bridge"):
        platformio = (ROOT / "examples" / project / "platformio.ini").read_text()
        assert "platform = espressif32@7.0.1" in platformio
        assert "knolleary/PubSubClient@2.8" in platformio
        assert "bblanchon/ArduinoJson@6.21.6" in platformio
        assert "adafruit/Adafruit NeoPixel@1.15.5" in platformio
    fork = json.loads((ROOT / "third_party/adafruit-dap-nivalo/fork-manifest.json").read_text())
    assert fork["upstreamRepository"] == "https://github.com/adafruit/Adafruit_DAP"
    assert fork["upstreamRelease"] == "1.8.3"
    assert fork["sourceStatus"] == "vendored-active"
    assert fork["publicationStatus"] == "not-published"
    assert fork["plannedPackage"]["version"].endswith("-nivalo.1")
    assert fork["localReplacementPaths"]["libraryExample"].startswith("symlink://")
    print("validated modular ownership, backoff/jitter, LWT, SNTP, watchdog, buffering, and dependency seams")

if __name__ == "__main__": main()
