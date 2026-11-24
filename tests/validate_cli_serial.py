import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    fixture = json.loads((ROOT / "tests/cli_provision_request_v1.json").read_text())
    assert set(fixture) == {"schema", "requestId", "wifi", "mqtt"}
    assert set(fixture["wifi"]) == {"ssid", "password"}
    assert set(fixture["mqtt"]) == {
        "deviceId", "host", "port", "useTls", "clientId", "username", "password"
    }
    assert len(fixture["mqtt"]) == 7

    claim_fixture = json.loads((ROOT / "tests/cli_claim_request_v1.json").read_text())
    assert set(claim_fixture) == {"schema", "requestId", "expectedHardwareId", "wifi", "claim"}
    assert set(claim_fixture["wifi"]) == {"ssid", "password"}
    assert set(claim_fixture["claim"]) == {"code"}

    print("validated CLI provision and claim request fixture shapes")


if __name__ == "__main__":
    main()
