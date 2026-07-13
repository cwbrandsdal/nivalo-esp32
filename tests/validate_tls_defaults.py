from __future__ import annotations

import ipaddress
import json
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"{source}: missing {needle!r}"


def expected_local_policy(host: str) -> bool:
    normalized = host.lower().rstrip(".")
    if normalized == "localhost" or normalized.endswith((".local", ".lan", ".home.arpa")):
        return True
    if "." not in normalized and ":" not in normalized:
        return True
    try:
        address = ipaddress.ip_address(normalized)
    except ValueError:
        return False
    if isinstance(address, ipaddress.IPv4Address):
        octets = [int(value) for value in normalized.split(".")]
        return (
            octets[0] in (10, 127)
            or (octets[0] == 172 and 16 <= octets[1] <= 31)
            or octets[:2] == [192, 168]
            or octets[:2] == [169, 254]
        )
    first_hextet = int(address) >> 112
    return address == ipaddress.IPv6Address("::1") or (first_hextet & 0xFE00) == 0xFC00 or (first_hextet & 0xFFC0) == 0xFE80


def main() -> None:
    header = REPO / "src" / "NivaloDevice.h"
    source = REPO / "src" / "NivaloDevice.cpp"
    lifecycle = REPO / "src" / "NivaloDeviceLifecycle.cpp"
    connection = REPO / "src" / "NivaloConnection.cpp"
    connection_header = REPO / "src" / "NivaloConnection.h"
    header_text = header.read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    lifecycle_text = lifecycle.read_text(encoding="utf-8")

    require(header_text, "boolean beginMqtt(const NivaloMqttConfig &config);", header)
    require(header_text, "uint16_t port = 8883;", header)
    require(header_text, "NIVALO_MQTT_TRANSPORT_TLS", header)
    require(header_text, "NIVALO_MQTT_TRANSPORT_PLAINTEXT_LOCAL", header)
    connection_text = connection.read_text(encoding="utf-8")
    require(connection_header.read_text(encoding="utf-8"), "WiFiClientSecure _secureClient", connection_header)
    require(connection_text, "_secureClient.setCACert", connection)
    require(connection_text, "_mqtt.setClient(_secureClient)", connection)
    require(connection_text, "_mqtt.setClient(_plainClient)", connection)
    require(lifecycle_text, "isProductionMqttHost(config.host) || !isLocalBrokerHost(config.host)", lifecycle)
    require(lifecycle_text, "parseIpv4Address(normalized.c_str(), octets)", lifecycle)
    require(lifecycle_text, "isLocalIpv6Address(normalized.c_str())", lifecycle)
    assert "NivaloConnection::defaultCaCertificate()" in lifecycle_text
    assert "setInsecure(" not in (source_text + lifecycle_text + connection_text), "insecure TLS bypass must not be used"
    for unsafe_prefix_check in (
        'normalized.startsWith("10.")',
        'normalized.startsWith("192.168.")',
        'normalized.startsWith("fc")',
        'normalized.startsWith("fd")',
    ):
        assert unsafe_prefix_check not in lifecycle_text, f"{lifecycle}: unsafe address prefix check {unsafe_prefix_check}"

    policy_cases = json.loads((REPO / "tests" / "mqtt_host_policy_cases.json").read_text(encoding="utf-8"))
    for host in policy_cases["acceptedLocalHosts"]:
        assert expected_local_policy(host), f"fixture should be accepted as local: {host}"
    for host in policy_cases["rejectedPublicHosts"]:
        assert not expected_local_policy(host), f"fixture should be rejected as public/non-local: {host}"

    configs = [
        REPO / "examples" / "Esp32Only" / "include" / "nivalo_config.example.h",
        REPO / "examples" / "Esp32Stm32Bridge" / "include" / "nivalo_config.example.h",
    ]
    for config in configs:
        text = config.read_text(encoding="utf-8")
        require(text, '#define NIVALO_DEVICE_CLAIM_URL "https://', config)
        require(text, '#define NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE 0', config)
        require(text, '#define NIVALO_DEV_MQTT_PORT 8883', config)
        assert '#define NIVALO_IOT_MQTT_PORT 1883' not in text

    print("validated secure MQTT defaults and explicit local-plaintext policy")


if __name__ == "__main__":
    main()
