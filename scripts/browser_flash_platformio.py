"""PlatformIO post action for the explicit browser-provisioning environment."""

from pathlib import Path
import sys

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.

scripts_dir = Path(env.subst("$PROJECT_DIR")).resolve().parents[1] / "scripts"
sys.path.insert(0, str(scripts_dir))

from browser_flash_artifact import build_factory_artifact  # noqa: E402


def create_browser_artifact(source, target, env):  # noqa: ANN001
    build_dir = Path(env.subst("$BUILD_DIR")).resolve()
    framework_dir = Path(
        env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    ).resolve()
    build_factory_artifact(
        bootloader_path=build_dir / "bootloader.bin",
        partition_path=build_dir / "partitions.bin",
        boot_app_path=framework_dir / "tools" / "partitions" / "boot_app0.bin",
        application_path=build_dir / "firmware.bin",
        artifact_path=build_dir / "nivalo-provisioning.merged.bin",
        evidence_path=build_dir / "nivalo-provisioning.merged.evidence.json",
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", create_browser_artifact)
