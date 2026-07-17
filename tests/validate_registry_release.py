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


def expect_stage_rejected(stage: Path) -> None:
    try:
        release.ensure_device_stage(stage, "nivalo-ci")
    except ValueError:
        return
    raise AssertionError("unsafe device stage unexpectedly passed structural validation")


def main() -> None:
    attributes = (ROOT / ".gitattributes").read_text(encoding="utf-8")
    assert "* text=auto eol=lf" in attributes.splitlines()
    device_identity = release.release_identity("device")
    dap_identity = release.release_identity("dap")
    assert release.validate("device", device_identity["tag"]) == device_identity["version"]
    assert release.validate("dap", dap_identity["tag"]) == dap_identity["version"]
    expect_rejected("device", f"{device_identity['tag']}.unexpected")
    expect_rejected("dap", f"{dap_identity['tag']}.unexpected")

    with tempfile.TemporaryDirectory() as temporary:
        output = Path(temporary)
        device = release.stage_package(
            "device", device_identity["tag"], output, dap_owner="nivalo-ci"
        )
        dap = release.stage_package("dap", dap_identity["tag"], output)

        assert (device / "LICENSE").is_file()
        assert (device / "src/NivaloDevice.h").is_file()
        assert (device / "scripts/browser_flash_platformio.py").is_file()
        assert (device / "scripts/browser_flash_artifact.py").is_file()
        assert not (device / "tests").exists()
        assert not (device / "third_party").exists()

        browser_project = device / "examples/Esp32Only"
        browser_config = (browser_project / "platformio.ini").read_text(encoding="utf-8")
        browser_script = browser_project / "../../scripts/browser_flash_platformio.py"
        assert "extra_scripts = post:../../scripts/browser_flash_platformio.py" in browser_config
        assert browser_script.resolve() == (device / "scripts/browser_flash_platformio.py").resolve()

        bridge_path = device / "examples/Esp32Stm32Bridge/platformio.ini"
        bridge = bridge_path.read_text(encoding="utf-8")
        dependency = release.dap_registry_dependency("nivalo-ci")
        assert dependency == f"nivalo-ci/Nivalo Adafruit DAP@{dap_identity['version']}"
        assert sum(line.strip() == dependency for line in bridge.splitlines()) == 1
        assert "third_party/adafruit-dap-nivalo" not in bridge.replace("\\", "/")
        assert "upload_port" not in bridge
        assert "monitor_port" not in bridge

        bridge_path.write_text(
            bridge.replace(dependency, "nivalo-ci/Nivalo Adafruit DAP@1.8.3-nivalo.2"),
            encoding="utf-8",
        )
        expect_stage_rejected(device)
        bridge_path.write_text(bridge, encoding="utf-8")

        (device / "scripts/browser_flash_platformio.py").unlink()
        expect_stage_rejected(device)

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
        "verify_registry_archives.py",
        "pack_registry_archive.py",
        "--fresh-core-dir",
        "NivaloRuntimeProvisioning.ino",
        "arduino-lint_1.3.0_Linux_64bit.tar.gz",
        "181671ca174988f2601e1cdf2b40a552682db8eaa047de33117e67e70338b63e",
        "github.event.repository.private == false",
        "pio pkg show --type library",
        "publish=false",
    ):
        assert required in workflow
    assert "release create" not in workflow and "git push" not in workflow
    assert "git describe" not in workflow
    assert "DEVICE_TOKEN" not in workflow and "DAP_TOKEN" not in workflow
    assert "&& secrets." not in workflow and "&& vars." not in workflow
    print("validated fail-closed registry metadata, tags, staging, and guarded publication workflow")


if __name__ == "__main__":
    main()
