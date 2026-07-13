#!/usr/bin/env python3
"""Validate built browser and ordinary firmware outputs fail closed."""

from __future__ import annotations

import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from browser_flash_artifact import DESCRIPTOR_PREFIX, inspect_artifact  # noqa: E402


BUILD_ROOT = ROOT / "examples" / "Esp32Only" / ".pio" / "build"
NORMAL_APPLICATION = BUILD_ROOT / "featheresp32" / "firmware.bin"
BROWSER_BUILD = BUILD_ROOT / "featheresp32-browser-provisioning"
BROWSER_APPLICATION = BROWSER_BUILD / "firmware.bin"
MERGED_ARTIFACT = BROWSER_BUILD / "nivalo-provisioning.merged.bin"
EVIDENCE = BROWSER_BUILD / "nivalo-provisioning.merged.evidence.json"
BROWSER_MAIN = ROOT / "examples" / "Esp32Only" / "src" / "main.cpp"
BROWSER_CONFIG = ROOT / "examples" / "Esp32Only" / "include" / "nivalo_browser_flash_config.h"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def main() -> int:
    source = BROWSER_MAIN.read_text(encoding="utf-8")
    browser_branch = source.split("#else", 1)[0]
    require(
        '#include "nivalo_browser_flash_config.h"' in browser_branch
        and "nivalo_config.h" not in browser_branch,
        "browser build must use only its checked-in config before the local-config branch",
    )
    config = BROWSER_CONFIG.read_text(encoding="utf-8")
    require(
        "#define NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE 0" in config
        and "NIVALO_DEV_WIFI_" not in config
        and "NIVALO_DEV_MQTT_" not in config,
        "browser config must reject developer fixtures and contain no fixture credentials",
    )
    normal = NORMAL_APPLICATION.read_bytes()
    browser = BROWSER_APPLICATION.read_bytes()
    require(
        normal.count(DESCRIPTOR_PREFIX) == 0,
        "ordinary featheresp32 firmware is incorrectly labeled for browser flashing",
    )
    require(
        browser.count(DESCRIPTOR_PREFIX) == 1,
        "explicit browser provisioning application must contain exactly one descriptor",
    )

    artifact = MERGED_ARTIFACT.read_bytes()
    computed = inspect_artifact(artifact)
    recorded = json.loads(EVIDENCE.read_text(encoding="utf-8"))
    require(recorded == computed, "recorded build evidence does not match independent inspection")
    require(
        computed["partitionTable"]["inclusiveRange"] == "0x8000..0x8bff",
        "operator evidence uses the wrong partition-table range",
    )
    require(
        computed["compatibility"]["partitionTableSha256"]
        == computed["partitionTable"]["sha256"],
        "descriptor and independently computed partition fingerprints differ",
    )
    require(
        computed["credentialStoragePolicy"] == "evaluation-unencrypted-nvs"
        and computed["productionEligible"] is False
        and computed["claimEnvironment"] == "staging",
        "current artifact must remain explicitly evaluation-only",
    )
    print("Browser-flash artifact contract valid; ordinary firmware remains unlabeled.")
    print(
        "Operator partition approval input: "
        + json.dumps(computed["approvedPartitionTables"], separators=(",", ":"))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
