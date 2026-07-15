from __future__ import annotations

import importlib.util
import io
import os
import tarfile
import tempfile
import unittest
from pathlib import Path


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


if __name__ == "__main__":
    unittest.main()
