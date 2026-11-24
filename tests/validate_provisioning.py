#!/usr/bin/env python3
"""Verify the signed device-claim proof vector and its tamper rejection."""

from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    vector = json.loads((ROOT / "tests/claim_proof_vector.json").read_text())
    assert hashlib.sha256(vector["mqttCredential"].encode()).hexdigest() == vector["credentialSha256"]
    manifest = (
        "NIVALO-DEVICE-CLAIM-V1\n"
        f'{vector["attemptId"]}\n{vector["claimCode"]}\n{vector["hardwareId"]}\n'
        f'{vector["nonce"]}\n{vector["credentialSha256"]}\n'
    ).encode()
    assert manifest.decode() == vector["manifestUtf8"]
    public = serialization.load_pem_public_key(vector["publicKeyPem"].encode())
    signature = base64.b64decode(vector["signatureBase64"])
    public.verify(signature, manifest, ec.ECDSA(hashes.SHA256()))
    for tampered in (
        manifest.replace(b"ABCD2345", b"ABCD2346"),
        manifest.replace(b"aabb", b"aabc"),
        manifest[:-2] + b"0\n",
    ):
        assert tampered != manifest
        try:
            public.verify(signature, tampered, ec.ECDSA(hashes.SHA256()))
        except InvalidSignature:
            continue
        raise AssertionError("tampered claim proof verified")
    print("validated signed claim proof vector and tamper rejection")


if __name__ == "__main__":
    main()
