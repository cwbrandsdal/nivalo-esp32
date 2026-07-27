#!/usr/bin/env python3
"""Deterministic dispatch/definition contract checks for the public ESP32 SDK."""

from __future__ import annotations

import json
import os
from pathlib import Path

from jsonschema import Draft202012Validator


ROOT = Path(__file__).resolve().parents[1]
PROTOCOL_ROOT = Path(os.environ.get("NIVALO_PROTOCOL_DIR", ROOT.parent / "nivalo-protocol"))


def dispatch(case: dict[str, object]) -> tuple[str, list[str]]:
    acknowledgements = ["accepted"]
    if case["command"] == "requestDefinitions" and not case["secondaryMcu"]:
        acknowledgements.append("succeeded")
        return "definitions", acknowledgements
    if case["functionResult"] is not None:
        acknowledgements.append("succeeded" if case["functionResult"] >= 0 else "failed")
        return "function", acknowledgements
    if case["genericResult"] != "unhandled":
        acknowledgements.append("succeeded" if case["genericResult"] == "succeeded" else "failed")
        return "generic", acknowledgements
    if case["secondaryMcu"]:
        acknowledgements.append("running")
        return "nivalolink", acknowledgements
    acknowledgements.append("failed")
    return "unsupported", acknowledgements


def main() -> None:
    cases = json.loads((ROOT / "tests/sdk_dispatch_cases.json").read_text())
    for case in cases:
        route, acknowledgements = dispatch(case)
        assert route == case["expectedRoute"], case["name"]
        assert acknowledgements == case["expectedAcks"], case["name"]

    definitions_schema = json.loads(
        (PROTOCOL_ROOT / "schemas/device-definitions.schema.json").read_text()
    )
    definitions = {
        "variables": [
            {"name": "ledState", "dataType": "number", "historyEnabled": True, "sortOrder": 0}
        ],
        "functions": [
            {
                "name": "setLed",
                "displayName": "Set LED",
                "description": "Turns the built-in LED on or off.",
                "argumentExample": "\"on\"",
                "returnType": "integer",
                "timeoutSeconds": 30,
                "dangerLevel": "operational",
                "sortOrder": 0,
            }
        ],
    }
    Draft202012Validator(definitions_schema).validate(definitions)

    header = (ROOT / "src/NivaloDevice.h").read_text()
    registry_header = (ROOT / "src/NivaloSdkRegistry.h").read_text()
    device = (ROOT / "src/NivaloDevice.cpp").read_text()
    command = (ROOT / "src/NivaloDeviceCommand.cpp").read_text()
    sdk = (ROOT / "src/NivaloDeviceSdk.cpp").read_text()
    assert "bool function(const char *name, NivaloFunctionHandler handler)" in header
    assert "const NivaloFunctionMetadata &metadata" in header
    assert "bool configureClock = true;" in header
    assert "bool variable(const char *name, int *reference" in header
    assert "void onCommand(NivaloCommandHandler handler)" in header
    assert "NIVALO_COMMAND_UNHANDLED" in registry_header
    assert "NIVALO_MAX_REGISTERED_FUNCTIONS" in registry_header
    assert "NIVALO_MAX_REGISTERED_VARIABLES" in registry_header
    assert "struct NivaloFunctionMetadata" in registry_header

    callback = command[command.index("void NivaloDevice::mqttCallback"):]
    assert callback.index('"accepted"') < callback.index("dispatchSdkCommand")
    assert callback.index("dispatchSdkCommand") < callback.index("_link.transport().exchange")
    dispatch_body = sdk[sdk.index("bool NivaloDevice::dispatchSdkCommand"):]
    assert 'result >= 0 ? "succeeded" : "failed"' in dispatch_body
    assert "NIVALO_COMMAND_UNHANDLED" in dispatch_body
    assert 'commandName == "requestDefinitions"' in dispatch_body
    assert "_sdkCommandResults.find(commandId)" in dispatch_body
    assert 'publishEvent("esp32.command.duplicate"' in dispatch_body
    assert "_sdkCommandResults.remember(" in dispatch_body
    lifecycle = (ROOT / "src/NivaloDeviceLifecycle.cpp").read_text()
    assert "if (config.configureClock)" in lifecycle
    link = (ROOT / "src/NivaloDeviceLink.cpp").read_text()
    assert (device + sdk + link).count("appendSdkDefinitions(") >= 4  # method plus local, SPI, and UART publication paths
    assert 'Serial.print((char)message[i])' not in callback

    example = ROOT / "examples/Esp32Only/src/main.cpp"
    source = example.read_text()
    assert 'device.function("setLed", setLed, setLedMetadata)' in source
    assert 'device.variable("ledState", &ledState)' in source
    assert "device.publishVariables()" in source

    print("validated SDK definitions, local ACK lifecycle, fallback order, and standalone registrations")


if __name__ == "__main__":
    main()
