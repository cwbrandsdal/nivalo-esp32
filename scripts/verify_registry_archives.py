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
import time
from pathlib import Path


DEVICE_LOCAL_DEPENDENCY = "symlink://../.."
PINNED_PLATFORM = "espressif32@7.0.1"


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


def run(*arguments: str, environment: dict[str, str] | None = None) -> None:
    subprocess.run(arguments, check=True, env=environment)


def platformio_file_uri(path: Path) -> str:
    # PlatformIO 6.1 on Windows treats RFC 8089 file:///C:/... as /C:/...
    # but accepts its documented file://C:/... package spelling.
    if os.name == "nt":
        return f"file://{path.as_posix()}"
    return path.as_uri()


def fresh_platformio_environment(core_dir: Path | None) -> dict[str, str] | None:
    if core_dir is None:
        return None
    core_dir = core_dir.resolve()
    if core_dir.exists() and any(core_dir.iterdir()):
        raise ValueError(f"PlatformIO core directory is not empty: {core_dir}")
    core_dir.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["PLATFORMIO_CORE_DIR"] = str(core_dir)
    return environment


def preflight_windows_toolchain(
    core_dir: Path | None,
    environment: dict[str, str] | None,
    attempts: int = 3,
    windows: bool | None = None,
) -> None:
    if windows is None:
        windows = os.name == "nt"
    if not windows or core_dir is None or environment is None:
        return
    compiler = (
        core_dir.resolve()
        / "packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-g++.exe"
    )
    if not compiler.is_file():
        raise FileNotFoundError(
            f"fresh PlatformIO toolchain is missing {compiler.name}"
        )
    last_error = ""
    with tempfile.TemporaryDirectory(prefix="nivalo-toolchain-preflight-") as temporary:
        source = Path(temporary) / "preflight.cpp"
        output = Path(temporary) / "preflight.o"
        source.write_text("int nivalo_registry_preflight = 1;\n", encoding="ascii")
        for attempt in range(attempts):
            completed = subprocess.run(
                [str(compiler), "-c", str(source), "-o", str(output)],
                check=False,
                capture_output=True,
                text=True,
                env=environment,
            )
            if completed.returncode == 0 and output.is_file():
                return
            last_error = completed.stderr.strip()
            time.sleep(0.25 * (attempt + 1))
    raise RuntimeError(
        "fresh PlatformIO compiler preflight failed after "
        f"{attempts} attempts: {last_error}"
    )


def build_archives(
    device_archive: Path, dap_archive: Path, fresh_core_dir: Path | None = None
) -> None:
    pio = shutil.which("pio") or shutil.which("platformio")
    if not pio:
        raise RuntimeError("PlatformIO CLI is not available")

    device_archive = device_archive.resolve()
    dap_archive = dap_archive.resolve()
    if not device_archive.is_file() or not dap_archive.is_file():
        raise FileNotFoundError("both device and DAP archives are required")

    environment = fresh_platformio_environment(fresh_core_dir)
    if environment is not None:
        # Separate installation from compilation and prove the newly extracted
        # Windows compiler subprocess chain is ready before a package build.
        run(
            pio,
            "pkg",
            "install",
            "--global",
            "--platform",
            PINNED_PLATFORM,
            environment=environment,
        )
        preflight_windows_toolchain(fresh_core_dir, environment)
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
            "--jobs",
            "1",
            environment=environment,
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
            "--jobs",
            "1",
            environment=environment,
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
            "--jobs",
            "1",
            environment=environment,
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", type=Path, required=True)
    parser.add_argument("--dap", type=Path, required=True)
    parser.add_argument(
        "--fresh-core-dir",
        type=Path,
        help="require and use an empty PlatformIO core/cache directory",
    )
    args = parser.parse_args()
    build_archives(args.device, args.dap, fresh_core_dir=args.fresh_core_dir)
    print("verified browser, standalone, and bridge builds from exact registry archives")


if __name__ == "__main__":
    main()
