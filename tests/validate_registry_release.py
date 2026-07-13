#!/usr/bin/env python3
"""Exercise registry release validation and staging without network access."""

from __future__ import annotations

import importlib.util
import json
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/prepare_registry_release.py"
SPEC = importlib.util.spec_from_file_location("prepare_registry_release", SCRIPT)
assert SPEC and SPEC.loader
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


def expect_rejected(package: str, tag: str) -> None:
    try:
        release.validate(package, tag)
    except ValueError:
        return
    raise AssertionError(f"release unexpectedly accepted {package} {tag}")


def main() -> None:
    assert release.validate("device", "v0.2.0") == "0.2.0"
    assert release.validate("dap", "dap-v1.8.3-nivalo.1") == "1.8.3-nivalo.1"
    expect_rejected("device", "v0.2.1")
    expect_rejected("dap", "v1.8.3-nivalo.1")

    with tempfile.TemporaryDirectory() as temporary:
        output = Path(temporary)
        device = release.stage_package("device", "v0.2.0", output)
        dap = release.stage_package("dap", "dap-v1.8.3-nivalo.1", output)

        assert (device / "LICENSE").is_file()
        assert (device / "src/NivaloDevice.h").is_file()
        assert not (device / "tests").exists()
        assert not (device / "third_party").exists()

        dap_manifest = json.loads((dap / "library.json").read_text(encoding="utf-8"))
        assert dap_manifest["name"] == "Nivalo Adafruit DAP"
        assert (dap / "license.txt").is_file()
        assert not any(path.is_symlink() for path in output.rglob("*"))

    workflow = (ROOT / ".github/workflows/registry-release.yml").read_text(encoding="utf-8")
    for required in (
        "NIVALO_DEVICE_PLATFORMIO_AUTH_TOKEN",
        "NIVALO_DAP_PLATFORMIO_AUTH_TOKEN",
        "registry-publication",
        "--no-interactive",
    ):
        assert required in workflow
    assert "release create" not in workflow and "git push" not in workflow
    print("validated fail-closed registry metadata, tags, staging, and guarded publication workflow")


if __name__ == "__main__":
    main()
