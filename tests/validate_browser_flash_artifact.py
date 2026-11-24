#!/usr/bin/env python3
"""Inspect built browser and ordinary firmware outputs; fail closed."""

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


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def main() -> int:
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

    computed = inspect_artifact(MERGED_ARTIFACT.read_bytes())
    require(
        computed["partitionTable"]["inclusiveRange"] == "0x8000..0x8bff",
        "artifact inspection uses the wrong partition-table range",
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
    print("Browser-flash artifact valid; ordinary firmware remains unlabeled.")
    print(
        "Approved partition tables: "
        + json.dumps(computed["approvedPartitionTables"], separators=(",", ":"))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
