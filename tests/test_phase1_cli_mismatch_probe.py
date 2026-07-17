import json
from pathlib import Path
import sys
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import phase1_cli_mismatch_probe as probe


class FakeConnection:
    def __init__(self, incoming: bytes = b"") -> None:
        self.incoming = bytearray(incoming)
        self.written = bytearray()

    def read(self, size: int) -> bytes:
        if not self.incoming:
            return b""
        value = bytes(self.incoming[:size])
        del self.incoming[:size]
        return value

    def write(self, value: bytes) -> None:
        self.written.extend(value)

    def flush(self) -> None:
        pass


class Phase1CliMismatchProbeTests(unittest.TestCase):
    def test_mismatch_changes_one_hex_digit_and_preserves_policy_shape(self) -> None:
        actual = "esp32-0123456789ab"
        mismatch = probe.mismatched_hardware_id(actual)

        self.assertEqual("esp32-0123456789a0", mismatch)
        self.assertNotEqual(actual, mismatch)

    def test_mismatch_rejects_unexpected_identity_without_echoing_it(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "unsupported hardware identity format"):
            probe.mismatched_hardware_id("unexpected-private-value")

    def test_reader_discards_noise_and_returns_only_matching_response(self) -> None:
        expected = {"schema": "nivalo.cli.claim.v1", "requestId": "wanted", "ok": False}
        wire = b"boot noise\r\n" + json.dumps({
            "schema": "nivalo.cli.claim.v1", "requestId": "other", "ok": True
        }).encode() + b"\n" + json.dumps(expected).encode() + b"\r\n"

        result = probe.read_response(
            FakeConnection(wire), "nivalo.cli.claim.v1", "wanted", time.monotonic() + 1
        )

        self.assertEqual(expected, result)

    def test_writer_emits_one_compact_ndjson_line(self) -> None:
        connection = FakeConnection()
        probe.write_request(connection, {"schema": "test", "requestId": "one"})

        self.assertEqual(b'{"schema":"test","requestId":"one"}\n', connection.written)


if __name__ == "__main__":
    unittest.main()
