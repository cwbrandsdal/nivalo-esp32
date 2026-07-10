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


def properties(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw and not raw.startswith("#") and "=" in raw:
            key, value = raw.split("=", 1)
            result[key] = value
    return result


def expected_tag(package: str, version: str) -> str:
    return f"v{version}" if package == "device" else f"dap-v{version}"


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


def ensure_safe_stage(stage: Path) -> None:
    forbidden_names = {".git", ".pio", ".env", "nivalo_config.h"}
    for path in stage.rglob("*"):
        if path.name in forbidden_names or path.is_symlink():
            raise ValueError(f"unsafe release path: {path.relative_to(stage)}")
        if path.is_file():
            data = path.read_bytes()
            if b"-----BEGIN PRIVATE KEY-----" in data or b"-----BEGIN EC PRIVATE KEY-----" in data:
                raise ValueError(f"private key material in release: {path.relative_to(stage)}")


def stage_package(package: str, tag: str, output: Path) -> Path:
    validate(package, tag)
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
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "dist/registry-stage")
    args = parser.parse_args()
    stage = stage_package(args.package, args.tag, args.output.resolve())
    print(stage)


if __name__ == "__main__":
    main()
