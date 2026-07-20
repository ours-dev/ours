#!/usr/bin/env python3

import pathlib
import sys


sys.stdout.write(
    pathlib.Path(__file__).with_name("ast_fixture.json").read_text(encoding="utf-8")
)
