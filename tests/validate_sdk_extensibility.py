#!/usr/bin/env python3
"""Validate SDK definition payloads against the pinned protocol schema."""

from __future__ import annotations

import json
import os
from pathlib import Path

from jsonschema import Draft202012Validator


ROOT = Path(__file__).resolve().parents[1]
PROTOCOL_ROOT = Path(os.environ.get("NIVALO_PROTOCOL_DIR", ROOT.parent / "nivalo-protocol"))


def main() -> None:
    definitions_schema = json.loads(
        (PROTOCOL_ROOT / "schemas/device-definitions.schema.json").read_text()
    )
    definitions = {
        "variables": [
            {"name": "ledState", "dataType": "number", "historyEnabled": True, "sortOrder": 0}
        ],
        "functions": [
            {
                "name": "setLed",
                "displayName": "Set LED",
                "description": "Turns the built-in LED on or off.",
                "argumentExample": "\"on\"",
                "returnType": "integer",
                "timeoutSeconds": 30,
                "dangerLevel": "operational",
                "sortOrder": 0,
            }
        ],
    }
    Draft202012Validator(definitions_schema).validate(definitions)

    print("validated SDK definitions against the pinned protocol schema")


if __name__ == "__main__":
    main()
