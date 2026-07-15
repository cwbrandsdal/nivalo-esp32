#!/usr/bin/env python3
"""Validate release metadata and create a minimal registry staging tree.

This script performs no network operations. Publishing is deliberately left to
the guarded GitHub Actions workflow.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ("device", "dap")
DEVICE_RELEASE_SCRIPTS = (
    "browser_flash_artifact.py",
    "browser_flash_platformio.py",
)
DAP_LOCAL_SOURCE = "../../third_party/adafruit-dap-nivalo/source"
LOCAL_PORT_KEYS = {"upload_port", "monitor_port"}


def properties(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw and not raw.startswith("#") and "=" in raw:
            key, value = raw.split("=", 1)
            result[key] = value
    return result


def expected_tag(package: str, version: str) -> str:
    return f"v{version}" if package == "device" else f"dap-v{version}"


def dap_registry_dependency(owner: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", owner):
        raise ValueError("DAP PlatformIO owner is required and has an invalid format")
    source = ROOT / "third_party/adafruit-dap-nivalo/source"
    manifest = json.loads((source / "library.json").read_text(encoding="utf-8"))
    fork = json.loads((source.parent / "fork-manifest.json").read_text(encoding="utf-8"))
    name = manifest.get("name")
    version = manifest.get("version")
    planned = fork.get("plannedPackage", {})
    if (
        name != "Nivalo Adafruit DAP"
        or not isinstance(version, str)
        or planned.get("platformioName") != name
        or planned.get("version") != version
        or planned.get("releaseTag") != expected_tag("dap", version)
    ):
        raise ValueError("DAP registry identity does not match the reviewed fork plan")
    return f"{owner}/{name}@{version}"


def validate(package: str, tag: str) -> str:
    if package not in PACKAGES:
        raise ValueError(f"unsupported package: {package}")

    if package == "device":
        manifest_path = ROOT / "library.json"
        properties_path = ROOT / "library.properties"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        metadata = properties(properties_path)
        if manifest["name"] != "NivaloDevice" or metadata["name"] != "NivaloDevice":
            raise ValueError("device package name mismatch")
        if manifest["version"] != metadata["version"]:
            raise ValueError("device version mismatch")
        if manifest["license"] != "MIT" or not (ROOT / "LICENSE").is_file():
            raise ValueError("device MIT license metadata/file mismatch")
        version = manifest["version"]
    else:
        source = ROOT / "third_party/adafruit-dap-nivalo/source"
        manifest = json.loads((source / "library.json").read_text(encoding="utf-8"))
        metadata = properties(source / "library.properties")
        fork = json.loads((source.parent / "fork-manifest.json").read_text(encoding="utf-8"))
        provenance = json.loads((source.parent / "upstream-provenance.json").read_text(encoding="utf-8"))
        if manifest["name"] != "Nivalo Adafruit DAP" or metadata["name"] != manifest["name"]:
            raise ValueError("DAP package name mismatch")
        if manifest["version"] != metadata["version"] or manifest["version"] != fork["plannedPackage"]["version"]:
            raise ValueError("DAP version mismatch")
        if manifest["license"] != "BSD-3-Clause":
            raise ValueError("unexpected DAP license metadata")
        expected_dependencies = {
            "Adafruit SPIFlash": "5.1.1",
            "SdFat - Adafruit Fork": "2.3.103",
            "Adafruit TinyUSB Library": "3.7.7",
            "MIDI Library": "5.0.2",
        }
        if {item["name"]: item["version"] for item in manifest["dependencies"]} != expected_dependencies:
            raise ValueError("unexpected DAP dependency graph")
        if provenance["commit"] != "8ca356d92e73d0d1005534030849e7ca37324805":
            raise ValueError("unexpected DAP upstream commit")
        if not (source / provenance["licenseFile"]).is_file():
            raise ValueError("DAP upstream license is missing")
        version = manifest["version"]

    wanted = expected_tag(package, version)
    if tag != wanted:
        raise ValueError(f"tag {tag!r} does not match metadata; expected {wanted!r}")
    if not re.fullmatch(r"(?:dap-)?v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", tag):
        raise ValueError("release tag is not version-shaped")
    return version


def release_identity(package: str) -> dict[str, str]:
    if package == "device":
        manifest_path = ROOT / "library.json"
    elif package == "dap":
        manifest_path = ROOT / "third_party/adafruit-dap-nivalo/source/library.json"
    else:
        raise ValueError(f"unsupported package: {package}")
    version = json.loads(manifest_path.read_text(encoding="utf-8"))["version"]
    tag = expected_tag(package, version)
    validate(package, tag)
    return {
        "version": version,
        "tag": tag,
        "archive": f"{package}-{version}.tar.gz",
    }


def ensure_safe_stage(stage: Path) -> None:
    forbidden_names = {".git", ".pio", ".env", "nivalo_config.h"}
    for path in stage.rglob("*"):
        if path.name in forbidden_names or path.is_symlink():
            raise ValueError(f"unsafe release path: {path.relative_to(stage)}")
        if path.is_file():
            data = path.read_bytes()
            if b"-----BEGIN PRIVATE KEY-----" in data or b"-----BEGIN EC PRIVATE KEY-----" in data:
                raise ValueError(f"private key material in release: {path.relative_to(stage)}")


def normalize_registry_bridge_config(path: Path, dap_owner: str) -> None:
    """Remove workstation-only settings and select the exact registry DAP package."""

    dependency = dap_registry_dependency(dap_owner)
    source = path.read_text(encoding="utf-8")
    normalized_lines: list[str] = []
    for line in source.splitlines():
        stripped = line.strip()
        key = stripped.partition("=")[0].strip().lower()
        if key in LOCAL_PORT_KEYS:
            continue
        if stripped == f"-I{DAP_LOCAL_SOURCE}":
            continue
        if stripped == f"symlink://{DAP_LOCAL_SOURCE}":
            indentation = line[: len(line) - len(line.lstrip())]
            normalized_lines.append(f"{indentation}{dependency}")
            continue
        normalized_lines.append(line)

    normalized = "\n".join(normalized_lines) + "\n"
    path.write_text(normalized, encoding="utf-8", newline="\n")


def ensure_device_stage(stage: Path, dap_owner: str) -> None:
    browser_project = stage / "examples/Esp32Only"
    browser_config = browser_project / "platformio.ini"
    bridge_config = stage / "examples/Esp32Stm32Bridge/platformio.ini"
    expected_script = stage / "scripts/browser_flash_platformio.py"
    expected_helper = stage / "scripts/browser_flash_artifact.py"
    for required in (browser_config, bridge_config, expected_script, expected_helper):
        if not required.is_file():
            raise ValueError(f"device stage is missing {required.relative_to(stage)}")

    matches = re.findall(
        r"^\s*extra_scripts\s*=\s*post:([^\s]+)\s*$",
        browser_config.read_text(encoding="utf-8"),
        flags=re.MULTILINE,
    )
    if len(matches) != 1:
        raise ValueError("browser example must select exactly one post-build script")
    selected_script = (browser_project / matches[0]).resolve()
    if selected_script != expected_script.resolve():
        raise ValueError("browser example post-build script does not resolve inside the package")

    bridge = bridge_config.read_text(encoding="utf-8")
    normalized_local_path = bridge.replace("\\", "/")
    if "third_party/adafruit-dap-nivalo" in normalized_local_path:
        raise ValueError("registry bridge example retains a repository-local DAP path")
    for line in bridge.splitlines():
        key = line.strip().partition("=")[0].strip().lower()
        if key in LOCAL_PORT_KEYS:
            raise ValueError("registry bridge example retains a workstation serial port")
    dependency = dap_registry_dependency(dap_owner)
    if sum(line.strip() == dependency for line in bridge.splitlines()) != 1:
        raise ValueError("registry bridge example must select the exact DAP package once")


def stage_package(
    package: str, tag: str, output: Path, dap_owner: str | None = None
) -> Path:
    validate(package, tag)
    if package == "device" and dap_owner is None:
        raise ValueError("--dap-owner is required when staging the device package")
    destination = output / package
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)

    if package == "device":
        for name in ("library.json", "library.properties", "README.md", "LICENSE"):
            shutil.copy2(ROOT / name, destination / name)
        shutil.copytree(ROOT / "src", destination / "src")
        shutil.copytree(
            ROOT / "examples",
            destination / "examples",
            ignore=shutil.ignore_patterns(".pio", "nivalo_config.h"),
        )
        scripts = destination / "scripts"
        scripts.mkdir()
        for name in DEVICE_RELEASE_SCRIPTS:
            shutil.copy2(ROOT / "scripts" / name, scripts / name)
        normalize_registry_bridge_config(
            destination / "examples/Esp32Stm32Bridge/platformio.ini", dap_owner
        )
        ensure_device_stage(destination, dap_owner)
    else:
        source = ROOT / "third_party/adafruit-dap-nivalo/source"
        for path in source.iterdir():
            target = destination / path.name
            shutil.copytree(path, target) if path.is_dir() else shutil.copy2(path, target)

    ensure_safe_stage(destination)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", choices=PACKAGES, required=True)
    parser.add_argument("--tag")
    parser.add_argument("--dap-owner")
    parser.add_argument("--identity-field", choices=("version", "tag", "archive"))
    parser.add_argument("--output", type=Path, default=ROOT / "dist/registry-stage")
    args = parser.parse_args()
    if args.identity_field:
        if args.tag:
            parser.error("--tag and --identity-field cannot be used together")
        print(release_identity(args.package)[args.identity_field])
        return
    if not args.tag:
        parser.error("--tag is required when staging a package")
    stage = stage_package(
        args.package, args.tag, args.output.resolve(), dap_owner=args.dap_owner
    )
    print(stage)


if __name__ == "__main__":
    main()
