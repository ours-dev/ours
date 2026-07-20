#!/usr/bin/env python3

import json
import sys


request = json.load(sys.stdin)
json.dump(
    {
        "schema_version": request["schema_version"],
        "generated_files": [
            {"path": "generated/action.txt", "contents": "annotation action ran\n"}
        ],
        "dependencies": [item["source_file"] for item in request["annotations"]],
        "diagnostics": [],
        "exit_status": 0,
    },
    sys.stdout,
    sort_keys=True,
)
