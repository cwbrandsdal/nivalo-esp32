#!/usr/bin/env python3
"""Build clean PlatformIO consumers from the exact registry archives."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path


DEVICE_LOCAL_DEPENDENCY = "symlink://../.."


def extract_archive(archive: Path, destination: Path) -> None:
    try:
        destination.mkdir(parents=True)
    except FileExistsError as error:
        raise ValueError(f"archive destination already exists: {destination}") from error
    destination_root = destination.resolve()
    with tarfile.open(archive, "r:gz") as package:
        for member in package.getmembers():
            if member.issym() or member.islnk() or member.isdev():
                raise ValueError(f"unsafe archive member: {member.name}")
            target = (destination / member.name).resolve()
            if target != destination_root and destination_root not in target.parents:
                raise ValueError(f"archive path escapes destination: {member.name}")
        package.extractall(destination, filter="data")


def replace_once(path: Path, old: str, new: str) -> None:
    source = path.read_text(encoding="utf-8")
    if source.count(old) != 1:
        raise ValueError(f"expected exactly one {old!r} in {path}")
    path.write_text(source.replace(old, new), encoding="utf-8", newline="\n")


def run(*arguments: str) -> None:
    subprocess.run(arguments, check=True)


def platformio_file_uri(path: Path) -> str:
    # PlatformIO 6.1 on Windows treats RFC 8089 file:///C:/... as /C:/...
    # but accepts its documented file://C:/... package spelling.
    if os.name == "nt":
        return f"file://{path.as_posix()}"
    return path.as_uri()


def build_archives(device_archive: Path, dap_archive: Path) -> None:
    pio = shutil.which("pio") or shutil.which("platformio")
    if not pio:
        raise RuntimeError("PlatformIO CLI is not available")

    device_archive = device_archive.resolve()
    dap_archive = dap_archive.resolve()
    if not device_archive.is_file() or not dap_archive.is_file():
        raise FileNotFoundError("both device and DAP archives are required")

    with tempfile.TemporaryDirectory(prefix="nivalo-registry-consumer-") as temporary:
        root = Path(temporary)
        extracted = root / "device-package"
        extracted_dap = root / "dap-package"
        extract_archive(device_archive, extracted)
        extract_archive(dap_archive, extracted_dap)
        dap_manifest = json.loads(
            (extracted_dap / "library.json").read_text(encoding="utf-8")
        )
        dap_suffix = f"/{dap_manifest['name']}@{dap_manifest['version']}"

        browser_project = extracted / "examples/Esp32Only"
        run(
            pio,
            "run",
            "--project-dir",
            str(browser_project),
            "--environment",
            "featheresp32-browser-provisioning",
        )
        browser_build = browser_project / ".pio/build/featheresp32-browser-provisioning"
        for required in (
            "nivalo-provisioning.merged.bin",
            "nivalo-provisioning.merged.evidence.json",
        ):
            if not (browser_build / required).is_file():
                raise FileNotFoundError(f"packaged browser build did not create {required}")

        standalone = root / "standalone-consumer"
        shutil.copytree(extracted / "examples/Esp32Only", standalone)
        shutil.rmtree(standalone / ".pio", ignore_errors=True)
        replace_once(
            standalone / "platformio.ini",
            DEVICE_LOCAL_DEPENDENCY,
            platformio_file_uri(device_archive),
        )
        run(
            pio,
            "run",
            "--project-dir",
            str(standalone),
            "--environment",
            "featheresp32",
        )

        bridge = root / "bridge-consumer"
        shutil.copytree(extracted / "examples/Esp32Stm32Bridge", bridge)
        bridge_config = bridge / "platformio.ini"
        dap_dependencies = [
            line.strip()
            for line in bridge_config.read_text(encoding="utf-8").splitlines()
            if line.strip().endswith(dap_suffix)
        ]
        if len(dap_dependencies) != 1 or "/" not in dap_dependencies[0]:
            raise ValueError("bridge must select one owner-qualified exact DAP dependency")
        dap_dependency = dap_dependencies[0]
        replace_once(
            bridge_config,
            DEVICE_LOCAL_DEPENDENCY,
            platformio_file_uri(device_archive),
        )
        replace_once(
            bridge_config,
            dap_dependency,
            platformio_file_uri(dap_archive),
        )
        run(
            pio,
            "run",
            "--project-dir",
            str(bridge),
            "--environment",
            "featheresp32",
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", type=Path, required=True)
    parser.add_argument("--dap", type=Path, required=True)
    args = parser.parse_args()
    build_archives(args.device, args.dap)
    print("verified browser, standalone, and bridge builds from exact registry archives")


if __name__ == "__main__":
    main()
