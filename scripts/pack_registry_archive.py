#!/usr/bin/env python3
"""Create a byte-reproducible PlatformIO-compatible registry archive."""

from __future__ import annotations

import argparse
import gzip
import io
import json
import os
import tarfile
import tempfile
from pathlib import Path


FILE_MODE = 0o644
DIRECTORY_MODE = 0o755
REPRODUCIBLE_MTIME = 0
CANONICAL_TEXT_NAMES = {"LICENSE"}
CANONICAL_TEXT_SUFFIXES = {
    ".cpp",
    ".csv",
    ".h",
    ".ini",
    ".ino",
    ".json",
    ".md",
    ".properties",
    ".py",
    ".txt",
}


def archive_members(stage: Path) -> list[Path]:
    if not stage.is_dir():
        raise ValueError(f"package stage is not a directory: {stage}")
    manifest = stage / "library.json"
    if not manifest.is_file():
        raise ValueError("package stage is missing library.json")
    metadata = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(metadata.get("name"), str) or not isinstance(
        metadata.get("version"), str
    ):
        raise ValueError("package stage has an invalid library identity")

    members = sorted(
        stage.rglob("*"), key=lambda path: path.relative_to(stage).as_posix()
    )
    for path in members:
        resolved = path.resolve()
        if (
            path.is_symlink()
            or (resolved != stage and stage not in resolved.parents)
            or not (path.is_dir() or path.is_file())
        ):
            raise ValueError(f"unsafe package path: {path.relative_to(stage)}")
    return members


def canonical_file_bytes(path: Path) -> bytes:
    data = path.read_bytes()
    if (
        path.name in CANONICAL_TEXT_NAMES
        or path.suffix.lower() in CANONICAL_TEXT_SUFFIXES
    ):
        text = data.decode("utf-8")
        return text.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")
    return data


def tar_info(stage: Path, path: Path, size: int = 0) -> tarfile.TarInfo:
    relative = path.relative_to(stage).as_posix()
    if path.is_dir():
        relative += "/"
    info = tarfile.TarInfo(relative)
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = REPRODUCIBLE_MTIME
    if path.is_dir():
        info.type = tarfile.DIRTYPE
        info.mode = DIRECTORY_MODE
    else:
        info.type = tarfile.REGTYPE
        info.mode = FILE_MODE
        info.size = size
    return info


def pack(stage: Path, output: Path) -> Path:
    stage = stage.resolve()
    output = output.resolve()
    if output.suffixes[-2:] != [".tar", ".gz"]:
        raise ValueError("registry archive must end in .tar.gz")
    if output == stage or stage in output.parents:
        raise ValueError("registry archive cannot be written inside its package stage")
    if output.exists():
        raise FileExistsError(f"registry archive already exists: {output}")

    members = archive_members(stage)
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with temporary.open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                with tarfile.open(
                    fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT
                ) as archive:
                    for path in members:
                        if path.is_file():
                            data = canonical_file_bytes(path)
                            archive.addfile(
                                tar_info(stage, path, len(data)), io.BytesIO(data)
                            )
                        else:
                            archive.addfile(tar_info(stage, path))
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    return output


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("stage", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(pack(args.stage, args.output))


if __name__ == "__main__":
    main()
