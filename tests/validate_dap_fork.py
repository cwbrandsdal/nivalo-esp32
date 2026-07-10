#!/usr/bin/env python3
"""Validate the repository-local Adafruit DAP fork provenance and delta."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FORK = ROOT / "third_party/adafruit-dap-nivalo"
SOURCE = FORK / "source"


def normalized_bytes(path: Path) -> bytes:
    lines = path.read_bytes().replace(b"\r\n", b"\n").split(b"\n")
    while lines and lines[-1] == b"":
        lines.pop()
    return b"\n".join(lines) + b"\n"


def digest(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def main() -> None:
    provenance = json.loads((FORK / "upstream-provenance.json").read_text(encoding="utf-8"))
    manifest = json.loads((FORK / "fork-manifest.json").read_text(encoding="utf-8"))

    assert provenance["repository"] == "https://github.com/adafruit/Adafruit_DAP"
    assert provenance["tag"] == "1.8.3"
    assert provenance["commit"] == "8ca356d92e73d0d1005534030849e7ca37324805"
    assert provenance["licenseFile"] == "license.txt"
    assert set(provenance["modifiedFiles"]) == {"Adafruit_DAP_STM32.cpp", "library.properties"}
    assert {path.name for path in SOURCE.iterdir() if path.is_file()} == set(provenance["filesSha256"])

    for name, expected in provenance["filesSha256"].items():
        if name not in provenance["modifiedFiles"]:
            assert digest(normalized_bytes(SOURCE / name)) == expected, name

    stm32 = normalized_bytes(SOURCE / "Adafruit_DAP_STM32.cpp")
    patch_line = b'    {0x421, "STM32F446 and STM32F469/479"},\n'
    assert stm32.count(patch_line) == 1
    assert digest(stm32.replace(patch_line, b"")) == provenance["filesSha256"]["Adafruit_DAP_STM32.cpp"]

    properties = normalized_bytes(SOURCE / "library.properties")
    assert b"name=Nivalo Adafruit DAP\n" in properties
    assert b"version=1.8.3-nivalo.1\n" in properties
    upstream_properties = properties
    replacements = {
        b"name=Nivalo Adafruit DAP": b"name=Adafruit DAP library",
        b"version=1.8.3-nivalo.1": b"version=1.8.3",
        b"maintainer=Nivalo firmware maintainers": b"maintainer=Adafruit <info@adafruit.com>",
        b"sentence=Maintained Nivalo fork of Adafruit DAP for verified ARM Cortex programming": b"sentence=Arduino library for DAP programming on ARM cortex microcontroller",
        b"paragraph=Adafruit DAP 1.8.3 plus the reviewed STM32 compatibility required by Nivalo secondary-MCU OTA": b"paragraph=Arduino library for DAP programming on ARM cortex microcontroller",
        b"depends=Adafruit SPIFlash (=5.1.1), SdFat - Adafruit Fork (=2.3.103), Adafruit TinyUSB Library (=3.7.7), SD": b"depends=Adafruit SPIFlash, SdFat - Adafruit Fork, Adafruit TinyUSB Library, SD",
    }
    for current, upstream in replacements.items():
        assert upstream_properties.count(current) == 1
        upstream_properties = upstream_properties.replace(current, upstream)
    assert digest(upstream_properties) == provenance["filesSha256"]["library.properties"]

    assert manifest["sourceStatus"] == "vendored-active"
    assert manifest["publicationStatus"] == "not-published"
    assert manifest["plannedPackage"]["version"] == "1.8.3-nivalo.1"

    internal = (ROOT / "examples/Esp32Stm32Bridge/platformio.ini").read_text(encoding="utf-8")
    assert manifest["localReplacementPaths"]["libraryExample"] in internal
    assert "-I../../third_party/adafruit-dap-nivalo/source" in internal
    assert "adafruit/Adafruit DAP library" not in internal
    for dependency in (
        "adafruit/Adafruit SPIFlash@5.1.1",
        "adafruit/Adafruit TinyUSB Library@3.7.7",
        "adafruit/SdFat - Adafruit Fork@2.3.103",
        "fortyseveneffects/MIDI Library@5.0.2",
    ):
        assert dependency in internal

    library = json.loads((ROOT / "library.json").read_text(encoding="utf-8"))
    assert all(dependency["name"] != "Adafruit DAP library" for dependency in library["dependencies"])
    assert "Adafruit DAP library" not in (ROOT / "library.properties").read_text(encoding="utf-8")

    ota_header = (ROOT / "src/NivaloOta.h").read_text(encoding="utf-8")
    ota_source = (ROOT / "src/NivaloOta.cpp").read_text(encoding="utf-8")
    assert "using NivaloStm32Dap = Adafruit_DAP_STM32" in ota_header
    assert "NivaloStm32Dap::select" not in ota_source

    print("validated active Adafruit DAP fork provenance, minimal delta, and local dependency paths")


if __name__ == "__main__":
    main()
