from __future__ import annotations

import importlib.util
import io
import os
import tarfile
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/verify_registry_archives.py"
SPEC = importlib.util.spec_from_file_location("verify_registry_archives", SCRIPT)
assert SPEC and SPEC.loader
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)


class RegistryArchiveTests(unittest.TestCase):
    def test_extract_rejects_existing_destination_directory_without_modifying_it(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "package.tar.gz"
            with tarfile.open(archive, "w:gz") as package:
                member = tarfile.TarInfo("payload")
                member.size = 1
                package.addfile(member, io.BytesIO(b"x"))
            destination = root / "extract"
            destination.mkdir()
            sentinel = destination / "sentinel"
            sentinel.write_bytes(b"keep")

            with self.assertRaisesRegex(ValueError, "archive destination already exists"):
                verify.extract_archive(archive, destination)

            self.assertEqual(sentinel.read_bytes(), b"keep")
            self.assertFalse((destination / "payload").exists())

    def test_extract_rejects_existing_destination_file_without_modifying_it(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "package.tar.gz"
            with tarfile.open(archive, "w:gz") as package:
                member = tarfile.TarInfo("payload")
                member.size = 1
                package.addfile(member, io.BytesIO(b"x"))
            destination = root / "extract"
            destination.write_bytes(b"keep")

            with self.assertRaisesRegex(ValueError, "archive destination already exists"):
                verify.extract_archive(archive, destination)

            self.assertEqual(destination.read_bytes(), b"keep")

    def test_extract_rejects_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "malicious.tar.gz"
            with tarfile.open(archive, "w:gz") as package:
                member = tarfile.TarInfo("../escape")
                member.size = 1
                package.addfile(member, io.BytesIO(b"x"))

            with self.assertRaises(ValueError):
                verify.extract_archive(archive, root / "extract")
            self.assertFalse((root / "escape").exists())

    def test_extract_rejects_links(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "link.tar.gz"
            with tarfile.open(archive, "w:gz") as package:
                member = tarfile.TarInfo("link")
                member.type = tarfile.SYMTYPE
                member.linkname = "target"
                package.addfile(member)

            with self.assertRaises(ValueError):
                verify.extract_archive(archive, root / "extract")

    def test_replace_once_is_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "platformio.ini"
            path.write_text("before\n", encoding="utf-8")
            verify.replace_once(path, "before", "after")
            self.assertEqual(path.read_text(encoding="utf-8"), "after\n")
            with self.assertRaises(ValueError):
                verify.replace_once(path, "missing", "replacement")

    def test_platformio_file_uri_matches_platform(self) -> None:
        path = Path("C:/archive.tar.gz") if os.name == "nt" else Path("/archive.tar.gz")
        uri = verify.platformio_file_uri(path.resolve())
        self.assertTrue(uri.startswith("file://"))
        if os.name == "nt":
            self.assertFalse(uri.startswith("file:///"))

    def test_fresh_platformio_environment_requires_an_empty_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            core = root / "core"
            environment = verify.fresh_platformio_environment(core)
            self.assertEqual(
                environment["PLATFORMIO_CORE_DIR"], str(core.resolve())
            )
            (core / "existing").write_bytes(b"x")
            with self.assertRaisesRegex(ValueError, "not empty"):
                verify.fresh_platformio_environment(core)

    def test_windows_toolchain_preflight_is_bounded_and_retries_first_use(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            core = Path(temporary) / "core"
            compiler = (
                core
                / "packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-g++.exe"
            )
            compiler.parent.mkdir(parents=True)
            compiler.write_bytes(b"fixture")
            calls = 0

            def compile_fixture(arguments, **_kwargs):
                nonlocal calls
                calls += 1
                if calls == 2:
                    Path(arguments[-1]).write_bytes(b"object")
                    return SimpleNamespace(returncode=0, stderr="")
                return SimpleNamespace(returncode=1, stderr="not ready")

            with mock.patch.object(verify.subprocess, "run", side_effect=compile_fixture):
                with mock.patch.object(verify.time, "sleep"):
                    verify.preflight_windows_toolchain(
                        core, {"PLATFORMIO_CORE_DIR": str(core)}, windows=True
                    )
            self.assertEqual(calls, 2)


if __name__ == "__main__":
    unittest.main()
