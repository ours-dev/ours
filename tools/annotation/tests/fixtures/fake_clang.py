#!/usr/bin/env python3

import json
import pathlib
import sys


fixture = pathlib.Path(__file__).with_name("ast_valid.json")
sys.stdout.write(fixture.read_text(encoding="utf-8"))
