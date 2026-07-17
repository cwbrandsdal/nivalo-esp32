#!/usr/bin/env python3
"""Attended, non-secret probe for the CLI serial hardware-mismatch guard."""

from __future__ import annotations

import argparse
import json
import time
import uuid
from typing import Any


MAXIMUM_LINE_BYTES = 16_384


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="attended serial port, for example COM6")
    parser.add_argument("--baud", type=int, default=115_200)
    parser.add_argument("--timeout-seconds", type=float, default=10.0)
    return parser.parse_args()


def mismatched_hardware_id(actual: str) -> str:
    if len(actual) != 18 or not actual.startswith("esp32-"):
        raise RuntimeError("The board returned an unsupported hardware identity format.")
    suffix = actual[6:]
    if any(character not in "0123456789abcdef" for character in suffix):
        raise RuntimeError("The board returned an unsupported hardware identity format.")
    replacement = "0" if suffix[-1] != "0" else "1"
    return f"esp32-{suffix[:-1]}{replacement}"


def write_request(connection: Any, payload: dict[str, Any]) -> None:
    encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    if len(encoded) > MAXIMUM_LINE_BYTES:
        raise RuntimeError("The serial request exceeded 16 KiB.")
    connection.write(encoded + b"\n")
    connection.flush()


def read_response(connection: Any, schema: str, request_id: str, deadline: float) -> dict[str, Any]:
    frame = bytearray()
    while time.monotonic() < deadline:
        value = connection.read(1)
        if not value:
            continue
        if value == b"\n":
            candidate = bytes(frame[:-1] if frame.endswith(b"\r") else frame)
            frame.clear()
            try:
                decoded = json.loads(candidate.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if (
                isinstance(decoded, dict)
                and decoded.get("schema") == schema
                and decoded.get("requestId") == request_id
            ):
                return decoded
            continue
        frame.extend(value)
        if len(frame) > MAXIMUM_LINE_BYTES:
            raise RuntimeError("The serial response exceeded 16 KiB.")
    raise RuntimeError(f"Timed out waiting for the {schema} response.")


def main() -> int:
    args = parse_args()
    if args.baud < 1 or args.timeout_seconds <= 0 or args.timeout_seconds > 30:
        raise SystemExit("baud must be positive and timeout must be greater than zero and at most 30 seconds")
    try:
        import serial
    except ImportError as error:
        raise SystemExit("pyserial is required; use the Python environment installed with PlatformIO") from error

    connection = serial.Serial()
    connection.port = args.port
    connection.baudrate = args.baud
    connection.timeout = 0.25
    connection.write_timeout = 2
    connection.dtr = False
    connection.rts = False
    connection.open()
    try:
        connection.reset_input_buffer()
        deadline = time.monotonic() + args.timeout_seconds
        identify_id = uuid.uuid4().hex
        write_request(connection, {
            "schema": "nivalo.cli.identify.v1",
            "requestId": identify_id,
        })
        identity = read_response(connection, "nivalo.cli.identify.v1", identify_id, deadline)
        if identity.get("ok") is not True or not isinstance(identity.get("hardwareId"), str):
            raise RuntimeError("The board did not return a valid CLI identity response.")

        claim_id = uuid.uuid4().hex
        write_request(connection, {
            "schema": "nivalo.cli.claim.v1",
            "requestId": claim_id,
            "expectedHardwareId": mismatched_hardware_id(identity["hardwareId"]),
            "wifi": {"ssid": "nivalo-mismatch-probe", "password": ""},
            "claim": {"code": "23456789"},
        })
        response = read_response(connection, "nivalo.cli.claim.v1", claim_id, deadline)
        if response.get("ok") is not False or set(response) != {"schema", "requestId", "ok"}:
            raise RuntimeError("The mismatch request was not rejected with the bounded generic response.")
    finally:
        connection.close()

    print(
        "OBSERVED: bounded generic rejection; classify mismatch PASS only after the same "
        "immutable image completes a positive claim; identities were not printed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
