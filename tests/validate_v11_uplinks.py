#!/usr/bin/env python3
"""Replays the uplinks built by the firmware's host test through nivalo-protocol's reference model.

tests/host/test_v11_uplink.cpp writes every message it builds (availability, ACKs, telemetry
batches, events, state, capability packets) to a samples file. Each one must pass
validate_message() of nivalo-protocol tests/protocol_v1_1.py exactly as the gateway would
receive it: topic, size, encoding, complexity, envelope and payload schemas and route rules.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--protocol", required=True, type=Path, help="nivalo-protocol checkout")
    parser.add_argument("--samples", required=True, type=Path, help="samples written by the host test")
    args = parser.parse_args()

    sys.path.insert(0, str(args.protocol / "tests"))
    import protocol_v1_1 as protocol  # noqa: E402  (the pinned reference model)

    samples = json.loads(args.samples.read_text(encoding="utf-8"))
    routes: dict[str, int] = {}
    failures = 0
    for sample in samples:
        topic = sample["topic"]
        data = sample["payload"].encode("utf-8")
        try:
            envelope = protocol.validate_message(topic, data, protocol.parse_utc(sample["receivedAt"]))
        except protocol.Rejected as rejected:
            failures += 1
            print(f"rejected ({rejected.rule}): {topic}\n  {sample['payload'][:300]}", file=sys.stderr)
            continue
        route = protocol.parse_topic(topic)["route"]
        routes[route] = routes.get(route, 0) + 1
        if "firmware" in envelope and envelope["firmware"]["targets"] != ["esp32"]:
            failures += 1
            print(f"birth advertises {envelope['firmware']['targets']}: only esp32 is executable in v1.1", file=sys.stderr)

    expected = {"availability", "commands/ack", "ota/ack", "telemetry", "events", "state", "definitions"}
    missing = expected - routes.keys()
    if missing:
        failures += 1
        print(f"no samples for {sorted(missing)}", file=sys.stderr)
    if failures:
        print(f"{failures} of {len(samples)} uplinks failed the reference model", file=sys.stderr)
        return 1
    print(f"{len(samples)} uplinks accepted by the reference model: {dict(sorted(routes.items()))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
