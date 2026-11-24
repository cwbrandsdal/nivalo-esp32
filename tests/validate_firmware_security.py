#!/usr/bin/env python3
"""Deterministic public-only firmware signing vectors."""

from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec


ROOT = Path(__file__).resolve().parents[1]


def manifest(target: str, size: int, sha256: str) -> bytes:
    if target not in {"esp32", "stm32"} or size <= 0:
        raise ValueError("non-canonical target or size")
    if len(sha256) != 64 or any(c not in "0123456789abcdef" for c in sha256):
        raise ValueError("non-canonical digest")
    return f"NIVALO-FIRMWARE-SIGNATURE-V1\n{target}\n{size}\n{sha256}\n".encode()


def verifies(key: dict[str, str], signed: bytes, signature: str | None) -> bool:
    if not signature:
        return False
    public_key = serialization.load_pem_public_key(key["publicKeyPem"].encode())
    assert isinstance(public_key, ec.EllipticCurvePublicKey)
    assert isinstance(public_key.curve, ec.SECP256R1)
    try:
        public_key.verify(base64.b64decode(signature, validate=True), signed, ec.ECDSA(hashes.SHA256()))
        return True
    except (InvalidSignature, ValueError):
        return False


def verifies_with_trust(keys: list[dict[str, str]], key_id: str, signed: bytes, signature: str | None) -> bool:
    selected = next((key for key in keys if key["keyId"] == key_id), None)
    return selected is not None and verifies(selected, signed, signature)


def main() -> None:
    vectors = json.loads((ROOT / "tests/firmware_signature_vectors.json").read_text())
    image = base64.b64decode(vectors["imageBase64"], validate=True)
    assert len(image) == vectors["sizeBytes"]
    assert hashlib.sha256(image).hexdigest() == vectors["sha256"]
    canonical = manifest(vectors["target"], vectors["sizeBytes"], vectors["sha256"])
    assert canonical.decode() == vectors["manifestUtf8"]

    current, previous = vectors["keys"]
    assert verifies_with_trust(vectors["keys"], current["keyId"], canonical, current["signatureBase64"])
    assert verifies_with_trust(vectors["keys"], previous["keyId"], canonical, previous["signatureBase64"])
    assert not verifies_with_trust(vectors["keys"], current["keyId"], canonical, None)  # unsigned
    assert not verifies_with_trust(vectors["keys"], "fixture-unknown", canonical, current["signatureBase64"])
    assert not verifies_with_trust(vectors["keys"], current["keyId"], canonical, "not-base64!")
    assert not verifies(current, manifest("stm32", vectors["sizeBytes"], vectors["sha256"]), current["signatureBase64"])
    assert not verifies(current, manifest("esp32", vectors["sizeBytes"] + 1, vectors["sha256"]), current["signatureBase64"])
    changed_hash = "0" * 64
    assert not verifies(current, manifest("esp32", vectors["sizeBytes"], changed_hash), current["signatureBase64"])
    assert hashlib.sha256(image + b"tampered").hexdigest() != vectors["sha256"]

    print("validated firmware signature vectors, key rotation, and tamper rejection")


if __name__ == "__main__":
    main()
