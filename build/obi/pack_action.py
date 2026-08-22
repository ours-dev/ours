#!/usr/bin/env python3
"""GN trampoline — invokes `obit pack` with the given arguments.

Usage: pack_action.py --obit OBIT_PATH @RESPONSE.rsp --output OUT \
           [--bootable=ARCH] [--compressed=X]

Everything after --obit is forwarded unchanged to `obit`, prefixed with the
`pack` subcommand: obit pack @RESPONSE.rsp --output OUT ...
"""
import argparse
import subprocess
import sys

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--obit", required=True, help="path to the obit binary")
    args, remaining = parser.parse_known_args()

    cmd = [args.obit, "pack"] + remaining
    result = subprocess.run(cmd)
    return result.returncode

if __name__ == "__main__":
    raise SystemExit(main())
