from __future__ import annotations

import gzip
import hashlib
import importlib.util
import json
import os
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/pack_registry_archive.py"
SPEC = importlib.util.spec_from_file_location("pack_registry_archive", SCRIPT)
assert SPEC and SPEC.loader
registry = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(registry)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ReproducibleRegistryArchiveTests(unittest.TestCase):
    def make_stage(self, root: Path) -> Path:
        stage = root / "stage"
        (stage / "src").mkdir(parents=True)
        (stage / "library.json").write_text(
            json.dumps({"name": "Fixture", "version": "1.2.3"}), encoding="utf-8"
        )
        (stage / "src/example.cpp").write_bytes(b"int fixture = 1;\n")
        return stage

    def test_archive_is_byte_reproducible_across_source_metadata_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = self.make_stage(root)
            first = registry.pack(stage, root / "first.tar.gz")

            for index, path in enumerate([stage, stage / "src", *stage.rglob("*")]):
                os.utime(path, (1_800_000_000 + index, 1_800_000_000 + index))
            (stage / "src/example.cpp").write_bytes(b"int fixture = 1;\r\n")
            second = registry.pack(stage, root / "second.tar.gz")

            self.assertEqual(digest(first), digest(second))
            self.assertEqual(first.read_bytes()[4:8], b"\0\0\0\0")
            with tarfile.open(first, "r:gz") as archive:
                members = archive.getmembers()
                self.assertEqual(
                    [member.name for member in members],
                    ["library.json", "src", "src/example.cpp"],
                )
                self.assertTrue(all(member.mtime == 0 for member in members))
                source = archive.extractfile("src/example.cpp")
                self.assertIsNotNone(source)
                self.assertEqual(source.read(), b"int fixture = 1;\n")

    def test_archive_uses_a_stable_gzip_stream(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = registry.pack(self.make_stage(root), root / "fixture.tar.gz")
            self.assertTrue(gzip.decompress(archive.read_bytes()))

    def test_rejects_existing_output_and_output_inside_stage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = self.make_stage(root)
            output = root / "fixture.tar.gz"
            output.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                registry.pack(stage, output)
            self.assertEqual(output.read_bytes(), b"keep")
            with self.assertRaisesRegex(ValueError, "inside"):
                registry.pack(stage, stage / "fixture.tar.gz")

    def test_rejects_missing_or_invalid_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / "stage"
            stage.mkdir()
            with self.assertRaisesRegex(ValueError, "library.json"):
                registry.pack(stage, root / "missing.tar.gz")
            (stage / "library.json").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "identity"):
                registry.pack(stage, root / "invalid.tar.gz")


if __name__ == "__main__":
    unittest.main()
