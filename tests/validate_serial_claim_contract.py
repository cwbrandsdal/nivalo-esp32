#!/usr/bin/env python3
"""Verify the ESP32 serial-claim fixture against an immutable platform pin."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
PIN_PATH = ROOT / "tests" / "serial_claim_contract_pin.json"
PIN_SCHEMA = "nivalo.cross-repository-contract-pin.v1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def load_json_bytes(payload: bytes, description: str) -> Any:
    try:
        return json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"{description} is not valid UTF-8 JSON: {error}") from error


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def read_pinned_platform_fixture(repository: Path, ref: str, path: str) -> bytes:
    try:
        result = subprocess.run(
            ["git", "-C", str(repository), "show", f"{ref}:{path}"],
            check=True,
            capture_output=True,
        )
    except FileNotFoundError as error:
        raise SystemExit("git is required when --platform-repository is used") from error
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode("utf-8", errors="replace").strip()
        raise SystemExit(
            f"could not read pinned platform fixture {ref}:{path}: {detail}"
        ) from error
    return result.stdout


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--platform-repository",
        type=Path,
        help=(
            "optional local nivalo-platform Git repository; verifies the fixture "
            "directly from the pinned commit without changing its checkout"
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    pin = load_json_bytes(PIN_PATH.read_bytes(), "contract pin")
    require(
        set(pin) == {"schema", "source", "localPath", "canonicalJsonSha256"},
        "contract pin must contain only schema, source, localPath, and canonicalJsonSha256",
    )
    require(pin["schema"] == PIN_SCHEMA, "unsupported contract pin schema")

    source = pin["source"]
    require(
        isinstance(source, dict) and set(source) == {"repository", "ref", "path"},
        "contract pin source must contain only repository, ref, and path",
    )
    require(
        source["repository"] == "cwbrandsdal/nivalo-platform",
        "contract pin must identify the canonical platform repository",
    )
    require(
        isinstance(source["ref"], str)
        and re.fullmatch(r"[0-9a-f]{40}", source["ref"]) is not None,
        "contract pin ref must be an immutable lowercase commit SHA",
    )
    expected_platform_path = (
        "tools/nivalo-cli/tests/Nivalo.Cli.Tests/Fixtures/cli_claim_request_v1.json"
    )
    require(
        source["path"] == expected_platform_path,
        "contract pin points at an unexpected platform fixture",
    )

    local_path = PurePosixPath(pin["localPath"])
    require(
        not local_path.is_absolute() and ".." not in local_path.parts,
        "contract pin localPath must stay inside the repository",
    )
    require(
        local_path.as_posix() == "tests/cli_claim_request_v1.json",
        "contract pin points at an unexpected ESP32 fixture",
    )
    expected_digest = pin["canonicalJsonSha256"]
    require(
        isinstance(expected_digest, str)
        and re.fullmatch(r"[0-9a-f]{64}", expected_digest) is not None,
        "contract pin digest must be a lowercase SHA-256 value",
    )

    local_fixture = load_json_bytes(
        (ROOT / Path(*local_path.parts)).read_bytes(), "ESP32 claim fixture"
    )
    local_canonical = canonical_json(local_fixture)
    local_digest = hashlib.sha256(local_canonical).hexdigest()
    require(
        local_digest == expected_digest,
        "ESP32 serial-claim fixture drifted from the pinned platform contract",
    )

    if args.platform_repository is not None:
        require(
            (args.platform_repository / ".git").exists(),
            "--platform-repository must point at a Git repository",
        )
        platform_fixture = load_json_bytes(
            read_pinned_platform_fixture(
                args.platform_repository, source["ref"], source["path"]
            ),
            "pinned platform claim fixture",
        )
        require(
            canonical_json(platform_fixture) == local_canonical,
            "pinned platform and ESP32 serial-claim fixtures differ",
        )

    print(
        "validated ESP32 serial-claim fixture against "
        f"{source['repository']}@{source['ref']}:{source['path']} "
        f"({expected_digest})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
