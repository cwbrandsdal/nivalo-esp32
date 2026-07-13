from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from browser_flash_artifact import (  # noqa: E402
    APPLICATION_OFFSET,
    BrowserFlashArtifactError,
    CLAIM_ENVIRONMENT_MARKER,
    DESCRIPTOR_PREFIX,
    PARTITION_LENGTH,
    STORAGE_POLICY_MARKER,
    build_factory_bytes,
    inspect_artifact,
    sha256_hex,
)


def make_esp32_image(payload: bytes, entry_point: int = 0x40080000) -> bytes:
    padding = (-len(payload)) % 4
    payload += b"\0" * padding
    header = bytearray(b"\0" * 24)
    header[0] = 0xE9
    header[1] = 1
    header[2] = 2
    header[3] = 0x20
    header[4:8] = entry_point.to_bytes(4, "little")
    header[8] = 0xEE
    header[12:14] = b"\0\0"
    header[23] = 1
    image = bytes(header) + (0x3FFB0000).to_bytes(4, "little") + len(payload).to_bytes(4, "little") + payload
    checksum = 0xEF
    for byte in payload:
        checksum ^= byte
    image += b"\0" * (15 - len(image) % 16) + bytes([checksum])
    return image + __import__("hashlib").sha256(image).digest()


def make_inputs() -> tuple[bytes, bytes, bytes, bytes]:
    bootloader = make_esp32_image(b"bootloader")
    partition = bytearray(b"\xff" * PARTITION_LENGTH)
    partition[:2] = b"\xaaP"
    partition[16:28] = b"nivalo-test!"
    partition_sha256 = sha256_hex(partition)
    descriptor = {
        "schema": "nivalo.browser-flash.v1",
        "chipFamily": "ESP32",
        "chipVariant": "ESP32-D0WDQ6",
        "boardId": "adafruit-feather-esp32",
        "flashLayout": "nivalo-provisioning-4mb-v1",
        "flashSizeBytes": 4194304,
        "partitionTableSha256": partition_sha256,
    }
    application = make_esp32_image(
        DESCRIPTOR_PREFIX
        + json.dumps(descriptor, separators=(",", ":")).encode("utf-8")
        + b"\0payload"
        + STORAGE_POLICY_MARKER
        + CLAIM_ENVIRONMENT_MARKER
    )
    boot_app = bytearray(b"\xff" * 0x2000)
    boot_app[:4] = b"\x01\x00\x00\x00"
    return bootloader, bytes(partition), bytes(boot_app), application


def build_test_factory(inputs: tuple[bytes, bytes, bytes, bytes]) -> bytes:
    with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(inputs[2])):
        return build_factory_bytes(*inputs)


def inspect_test_artifact(image: bytes) -> dict[str, object]:
    boot_app_sha256 = sha256_hex(make_inputs()[2])
    with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", boot_app_sha256):
        return inspect_artifact(image)


class BrowserFlashArtifactTests(unittest.TestCase):
    def test_browser_build_excludes_ignored_local_configuration(self) -> None:
        source = (ROOT / "examples" / "Esp32Only" / "src" / "main.cpp").read_text(
            encoding="utf-8"
        )
        browser_branch = source.split("#else", 1)[0]
        self.assertIn('#include "nivalo_browser_flash_config.h"', browser_branch)
        self.assertNotIn("nivalo_config.h", browser_branch)
        browser_config = (
            ROOT / "examples" / "Esp32Only" / "include" / "nivalo_browser_flash_config.h"
        ).read_text(encoding="utf-8")
        self.assertIn("#define NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE 0", browser_config)
        self.assertNotIn("NIVALO_DEV_WIFI_", browser_config)
        self.assertNotIn("NIVALO_DEV_MQTT_", browser_config)

    def test_builds_complete_factory_image_and_independent_evidence(self) -> None:
        image = build_test_factory(make_inputs())
        evidence = inspect_test_artifact(image)
        self.assertEqual(image[0], 0xE9)
        self.assertEqual(image[0x1000], 0xE9)
        self.assertEqual(image[APPLICATION_OFFSET], 0xE9)
        self.assertEqual(
            evidence["partitionTable"]["sha256"],
            evidence["approvedPartitionTables"]["nivalo-provisioning-4mb-v1"],
        )
        self.assertEqual(evidence["credentialStoragePolicy"], "evaluation-unencrypted-nvs")
        self.assertFalse(evidence["productionEligible"])

    def test_rejects_partition_tampering_independently_of_descriptor(self) -> None:
        image = bytearray(build_test_factory(make_inputs()))
        image[0x8010] ^= 0x01
        with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(make_inputs()[2])):
            with self.assertRaisesRegex(BrowserFlashArtifactError, "descriptor fingerprint"):
                inspect_artifact(bytes(image))

    def test_rejects_preloaded_unencrypted_nvs_bytes(self) -> None:
        image = bytearray(build_test_factory(make_inputs()))
        image[0x9000] = 0x00
        with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(make_inputs()[2])):
            with self.assertRaisesRegex(BrowserFlashArtifactError, "fresh-board NVS"):
                inspect_artifact(bytes(image))

    def test_rejects_duplicate_descriptor(self) -> None:
        bootloader, partition, boot_app, application = make_inputs()
        descriptor_at = application.index(DESCRIPTOR_PREFIX)
        application = make_esp32_image(
            application[32 : -33] + application[descriptor_at : application.index(b"\0", descriptor_at) + 1]
        )
        with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(boot_app)):
            with self.assertRaisesRegex(BrowserFlashArtifactError, "exactly one"):
                build_factory_bytes(bootloader, partition, boot_app, application)

    def test_rejects_missing_evaluation_storage_policy(self) -> None:
        bootloader, partition, boot_app, application = make_inputs()
        application = make_esp32_image(application[32:-33].replace(STORAGE_POLICY_MARKER, b""))
        with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(boot_app)):
            with self.assertRaisesRegex(BrowserFlashArtifactError, "storage-policy marker"):
                build_factory_bytes(bootloader, partition, boot_app, application)

    def test_rejects_nonbootable_application(self) -> None:
        bootloader, partition, boot_app, application = make_inputs()
        application = b"not-an-esp32-image" + application
        with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(boot_app)):
            with self.assertRaisesRegex(BrowserFlashArtifactError, "Espressif image header"):
                build_factory_bytes(bootloader, partition, boot_app, application)

    def test_rejects_nonexecutable_application_entry_point(self) -> None:
        bootloader, partition, boot_app, application = make_inputs()
        application = bytearray(application)
        application[4:8] = b"\0\0\0\0"
        with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(boot_app)):
            with self.assertRaisesRegex(BrowserFlashArtifactError, "entry point"):
                build_factory_bytes(bootloader, partition, boot_app, bytes(application))

    def test_rejects_application_segment_checksum_tampering(self) -> None:
        bootloader, partition, boot_app, application = make_inputs()
        application = bytearray(application)
        application[40] ^= 0x01
        with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(boot_app)):
            with self.assertRaisesRegex(BrowserFlashArtifactError, "segment checksum"):
                build_factory_bytes(bootloader, partition, boot_app, bytes(application))

    def test_rejects_wrong_ota_selector(self) -> None:
        bootloader, partition, boot_app, application = make_inputs()
        boot_app = boot_app[:-1]
        with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(boot_app)):
            with self.assertRaisesRegex(BrowserFlashArtifactError, "exactly 0x2000"):
                build_factory_bytes(bootloader, partition, boot_app, application)

    def test_rejects_unpinned_ota_selector(self) -> None:
        bootloader, partition, boot_app, application = make_inputs()
        with self.assertRaisesRegex(BrowserFlashArtifactError, "pinned framework input"):
            build_factory_bytes(bootloader, partition, boot_app, application)

    def test_rejects_bootloader_that_overlaps_partition_table(self) -> None:
        bootloader, partition, boot_app, application = make_inputs()
        bootloader = make_esp32_image(b"\xff" * 0x7000)
        with patch("browser_flash_artifact.EXPECTED_BOOT_APP_SHA256", sha256_hex(boot_app)):
            with self.assertRaisesRegex(BrowserFlashArtifactError, "size is outside"):
                build_factory_bytes(bootloader, partition, boot_app, application)


if __name__ == "__main__":
    unittest.main()
