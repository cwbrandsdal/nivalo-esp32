#!/usr/bin/env python3
"""Build and independently inspect the original-ESP32 browser-flash artifact.

This tool does not sign or publish artifacts. Server-side signing remains a
separate guarded release capability.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import tempfile
from typing import Any


DESCRIPTOR_PREFIX = b"NIVALO-BROWSER-FLASH-V1:"
DESCRIPTOR_MAXIMUM_BYTES = 1024
STORAGE_POLICY_MARKER = b"NIVALO-PROVISIONING-STORAGE-POLICY-V1:evaluation-unencrypted-nvs\0"
CLAIM_ENVIRONMENT_MARKER = b"NIVALO-PROVISIONING-CLAIM-ENVIRONMENT-V1:staging\0"
FLASH_SIZE_BYTES = 4 * 1024 * 1024
PARTITION_OFFSET = 0x8000
PARTITION_LENGTH = 0x0C00
PARTITION_END = PARTITION_OFFSET + PARTITION_LENGTH
BOOTLOADER_OFFSET = 0x1000
BOOT_APP_OFFSET = 0xE000
APPLICATION_OFFSET = 0x10000
APPLICATION_MAXIMUM_BYTES = 0x140000
BOOT_APP_LENGTH = 0x2000
EXPECTED_BOOT_APP_SHA256 = "f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0"
LAYOUT_ID = "nivalo-provisioning-4mb-v1"
EXPECTED_DESCRIPTOR_KEYS = (
    "schema",
    "chipFamily",
    "chipVariant",
    "boardId",
    "flashLayout",
    "flashSizeBytes",
    "partitionTableSha256",
)
EXPECTED_COMPATIBILITY = {
    "schema": "nivalo.browser-flash.v1",
    "chipFamily": "ESP32",
    "chipVariant": "ESP32-D0WDQ6",
    "boardId": "adafruit-feather-esp32",
    "flashLayout": LAYOUT_ID,
    "flashSizeBytes": FLASH_SIZE_BYTES,
}


class BrowserFlashArtifactError(ValueError):
    """Raised when an input cannot become an approved browser artifact."""


def sha256_hex(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def validate_esp32_image(
    image: bytes, name: str, maximum_bytes: int, *, allow_erased_trailing_bytes: bool = False
) -> int:
    if len(image) < 24 or len(image) > maximum_bytes:
        raise BrowserFlashArtifactError(f"{name} size is outside its reviewed flash region")
    if image[0] != 0xE9:
        raise BrowserFlashArtifactError(f"{name} has no Espressif image header")
    segment_count = image[1]
    if segment_count < 1 or segment_count > 16:
        raise BrowserFlashArtifactError(f"{name} segment count is invalid")
    if image[3] >> 4 != 2:
        raise BrowserFlashArtifactError(f"{name} does not declare exactly 4 MiB")
    entry_point = int.from_bytes(image[4:8], "little")
    if not 0x40000000 <= entry_point < 0x50000000:
        raise BrowserFlashArtifactError(f"{name} entry point is not executable")
    if image[8] != 0xEE:
        raise BrowserFlashArtifactError(f"{name} does not use the original-ESP32 WP-pin contract")
    if int.from_bytes(image[12:14], "little") != 0:
        raise BrowserFlashArtifactError(f"{name} does not target original ESP32")
    if image[23] != 1:
        raise BrowserFlashArtifactError(f"{name} must append its SHA-256 image digest")

    cursor = 24
    checksum = 0xEF
    for _ in range(segment_count):
        if cursor + 8 > len(image):
            raise BrowserFlashArtifactError(f"{name} segment header is truncated")
        address = int.from_bytes(image[cursor : cursor + 4], "little")
        size = int.from_bytes(image[cursor + 4 : cursor + 8], "little")
        cursor += 8
        if size == 0 or size % 4 != 0 or cursor + size > len(image):
            raise BrowserFlashArtifactError(f"{name} segment length is invalid")
        if not 0x3F000000 <= address < 0x50000000 or address + size > 0x50000000:
            raise BrowserFlashArtifactError(f"{name} segment address is invalid")
        for byte in image[cursor : cursor + size]:
            checksum ^= byte
        cursor += size

    checksum_offset = cursor + (15 - cursor % 16)
    digest_end = checksum_offset + 1 + 32
    if digest_end > len(image):
        raise BrowserFlashArtifactError(f"{name} image digest is truncated")
    if not allow_erased_trailing_bytes and digest_end != len(image):
        raise BrowserFlashArtifactError(f"{name} has trailing, missing, or misaligned image data")
    if allow_erased_trailing_bytes and any(byte != 0xFF for byte in image[digest_end:]):
        raise BrowserFlashArtifactError(f"{name} flash region has unexpected trailing data")
    if image[checksum_offset] != checksum:
        raise BrowserFlashArtifactError(f"{name} segment checksum is invalid")
    if image[checksum_offset + 1 : digest_end] != hashlib.sha256(
        image[: checksum_offset + 1]
    ).digest():
        raise BrowserFlashArtifactError(f"{name} appended image digest is invalid")
    return digest_end


def validate_ota_selector(image: bytes) -> None:
    if len(image) != BOOT_APP_LENGTH:
        raise BrowserFlashArtifactError("OTA selector initializer must be exactly 0x2000 bytes")
    if image[:4] != b"\x01\x00\x00\x00":
        raise BrowserFlashArtifactError("OTA selector initializer has an invalid first otadata entry")
    if sha256_hex(image) != EXPECTED_BOOT_APP_SHA256:
        raise BrowserFlashArtifactError("OTA selector initializer does not match the pinned framework input")


def read_descriptor(image: bytes) -> dict[str, Any]:
    matches: list[int] = []
    cursor = 0
    while True:
        match = image.find(DESCRIPTOR_PREFIX, cursor)
        if match < 0:
            break
        matches.append(match + len(DESCRIPTOR_PREFIX))
        cursor = match + 1
    if len(matches) != 1:
        raise BrowserFlashArtifactError(
            "artifact must contain exactly one NIVALO-BROWSER-FLASH-V1 descriptor"
        )

    start = matches[0]
    end = image.find(b"\0", start, start + DESCRIPTOR_MAXIMUM_BYTES + 1)
    if end < 0:
        raise BrowserFlashArtifactError("descriptor is not bounded and NUL-terminated")
    try:
        encoded = image[start:end].decode("utf-8", errors="strict")
        parsed = json.loads(encoded)
    except (UnicodeDecodeError, json.JSONDecodeError) as reason:
        raise BrowserFlashArtifactError("descriptor is not UTF-8 canonical JSON") from reason
    if not isinstance(parsed, dict) or tuple(parsed.keys()) != EXPECTED_DESCRIPTOR_KEYS:
        raise BrowserFlashArtifactError("descriptor has missing, reordered, or unknown fields")
    if json.dumps(parsed, separators=(",", ":"), ensure_ascii=False) != encoded:
        raise BrowserFlashArtifactError("descriptor JSON is not canonical")
    for key, expected in EXPECTED_COMPATIBILITY.items():
        if parsed.get(key) != expected:
            raise BrowserFlashArtifactError(f"descriptor {key} does not match {expected!r}")
    fingerprint = parsed.get("partitionTableSha256")
    if not isinstance(fingerprint, str) or len(fingerprint) != 64:
        raise BrowserFlashArtifactError("descriptor partition fingerprint is invalid")
    try:
        if bytes.fromhex(fingerprint).hex() != fingerprint:
            raise ValueError
    except ValueError as reason:
        raise BrowserFlashArtifactError(
            "descriptor partition fingerprint is not lowercase hexadecimal"
        ) from reason
    return parsed


def inspect_artifact(image: bytes) -> dict[str, Any]:
    if len(image) < PARTITION_END:
        raise BrowserFlashArtifactError("artifact is too small for the partition table")
    if len(image) > FLASH_SIZE_BYTES:
        raise BrowserFlashArtifactError("artifact exceeds the declared 4 MiB flash")
    if image[0] != 0xE9:
        raise BrowserFlashArtifactError("artifact has no original-ESP32 header at address 0")
    chip_id = int.from_bytes(image[12:14], "little")
    if chip_id != 0:
        raise BrowserFlashArtifactError(f"artifact chip ID is 0x{chip_id:04x}, not original ESP32")
    flash_size_code = image[3] >> 4
    if flash_size_code != 2:
        raise BrowserFlashArtifactError("artifact header does not declare exactly 4 MiB")
    if image[:16] != image[BOOTLOADER_OFFSET : BOOTLOADER_OFFSET + 16]:
        raise BrowserFlashArtifactError("address-0 compatibility header does not match the bootloader")
    if any(byte != 0xFF for byte in image[16:BOOTLOADER_OFFSET]):
        raise BrowserFlashArtifactError("unused first-sector bytes are not erased")
    validate_esp32_image(
        image[BOOTLOADER_OFFSET:PARTITION_OFFSET],
        "bootloader",
        PARTITION_OFFSET - BOOTLOADER_OFFSET,
        allow_erased_trailing_bytes=True,
    )
    if any(byte != 0xFF for byte in image[PARTITION_END:BOOT_APP_OFFSET]):
        raise BrowserFlashArtifactError(
            "partition-to-OTA gap, including fresh-board NVS, must remain erased"
        )
    validate_ota_selector(image[BOOT_APP_OFFSET:APPLICATION_OFFSET])
    validate_esp32_image(
        image[APPLICATION_OFFSET:], "provisioning application", APPLICATION_MAXIMUM_BYTES
    )

    descriptor = read_descriptor(image)
    if image.count(STORAGE_POLICY_MARKER) != 1:
        raise BrowserFlashArtifactError(
            "evaluation artifact must contain exactly one explicit unencrypted-NVS storage policy"
        )
    if image.count(CLAIM_ENVIRONMENT_MARKER) != 1:
        raise BrowserFlashArtifactError(
            "evaluation artifact must contain exactly one staging claim-environment marker"
        )
    partition = image[PARTITION_OFFSET:PARTITION_END]
    if partition[:2] != b"\xaaP":
        raise BrowserFlashArtifactError("artifact has no partition table at address 0x8000")
    partition_sha256 = sha256_hex(partition)
    if descriptor["partitionTableSha256"] != partition_sha256:
        raise BrowserFlashArtifactError(
            "descriptor fingerprint does not match the exact 0x8000..0x8bff bytes"
        )
    return {
        "schema": "nivalo.browser-flash-build-evidence.v1",
        "artifactSha256": sha256_hex(image),
        "artifactSizeBytes": len(image),
        "compatibility": descriptor,
        "credentialStoragePolicy": "evaluation-unencrypted-nvs",
        "claimEnvironment": "staging",
        "productionEligible": False,
        "partitionTable": {
            "offset": PARTITION_OFFSET,
            "length": PARTITION_LENGTH,
            "inclusiveRange": "0x8000..0x8bff",
            "sha256": partition_sha256,
        },
        "approvedPartitionTables": {LAYOUT_ID: partition_sha256},
    }


def _place(image: bytearray, occupied: list[tuple[int, int, str]], offset: int, data: bytes, name: str) -> None:
    if not data:
        raise BrowserFlashArtifactError(f"{name} is empty")
    end = offset + len(data)
    if offset < 0 or end > FLASH_SIZE_BYTES:
        raise BrowserFlashArtifactError(f"{name} does not fit the 4 MiB flash")
    for prior_start, prior_end, prior_name in occupied:
        if offset < prior_end and end > prior_start:
            raise BrowserFlashArtifactError(f"{name} overlaps {prior_name}")
    image[offset:end] = data
    occupied.append((offset, end, name))


def build_factory_bytes(bootloader: bytes, partition: bytes, boot_app: bytes, application: bytes) -> bytes:
    validate_esp32_image(bootloader, "bootloader", PARTITION_OFFSET - BOOTLOADER_OFFSET)
    validate_esp32_image(application, "provisioning application", APPLICATION_MAXIMUM_BYTES)
    validate_ota_selector(boot_app)
    if len(partition) != PARTITION_LENGTH:
        raise BrowserFlashArtifactError("partition table must be exactly 0x0c00 bytes")
    if partition[:2] != b"\xaaP":
        raise BrowserFlashArtifactError("partition input is not an ESP32 partition table")
    descriptor = read_descriptor(application)
    if application.count(STORAGE_POLICY_MARKER) != 1:
        raise BrowserFlashArtifactError(
            "compiled application lacks the exact evaluation storage-policy marker"
        )
    if application.count(CLAIM_ENVIRONMENT_MARKER) != 1:
        raise BrowserFlashArtifactError(
            "compiled application lacks the exact staging claim-environment marker"
        )
    partition_sha256 = sha256_hex(partition)
    if descriptor["partitionTableSha256"] != partition_sha256:
        raise BrowserFlashArtifactError(
            "compiled descriptor does not match the independently generated partition table"
        )

    artifact_size = APPLICATION_OFFSET + len(application)
    image = bytearray(b"\xff" * artifact_size)
    occupied: list[tuple[int, int, str]] = []
    # Original ESP32 boots the real second-stage bootloader at 0x1000. The
    # first sector is otherwise unused; retain the exact 16-byte bootloader
    # header there so the browser can attest family/flash before opening USB.
    _place(image, occupied, 0, bootloader[:16], "compatibility header")
    _place(image, occupied, BOOTLOADER_OFFSET, bootloader, "bootloader")
    _place(image, occupied, PARTITION_OFFSET, partition, "partition table")
    _place(image, occupied, BOOT_APP_OFFSET, boot_app, "OTA selector initializer")
    _place(image, occupied, APPLICATION_OFFSET, application, "provisioning application")
    result = bytes(image)
    inspect_artifact(result)
    return result


def _write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
        stream.flush()
    temporary.replace(path)


def build_factory_artifact(
    *,
    bootloader_path: Path,
    partition_path: Path,
    boot_app_path: Path,
    application_path: Path,
    artifact_path: Path,
    evidence_path: Path,
) -> dict[str, Any]:
    artifact = build_factory_bytes(
        bootloader_path.read_bytes(),
        partition_path.read_bytes(),
        boot_app_path.read_bytes(),
        application_path.read_bytes(),
    )
    evidence = inspect_artifact(artifact)
    _write_atomic(artifact_path, artifact)
    evidence_bytes = (json.dumps(evidence, indent=2, sort_keys=True) + "\n").encode("utf-8")
    _write_atomic(evidence_path, evidence_bytes)
    print(
        "Nivalo browser-flash artifact prepared without signing: "
        f"{artifact_path} ({len(artifact)} bytes, sha256={evidence['artifactSha256']})"
    )
    print(
        "Operator partition approval input: "
        + json.dumps(evidence["approvedPartitionTables"], separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    inspect_parser = subparsers.add_parser(
        "inspect", help="independently validate a merged artifact and print evidence"
    )
    inspect_parser.add_argument("artifact", type=Path)
    arguments = parser.parse_args()
    if arguments.command == "inspect":
        evidence = inspect_artifact(arguments.artifact.read_bytes())
        print(json.dumps(evidence, indent=2, sort_keys=True))
        return 0
    raise AssertionError("unreachable")


if __name__ == "__main__":
    raise SystemExit(main())
