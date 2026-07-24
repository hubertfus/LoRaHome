#!/usr/bin/env python3
"""Independently verifies the shared CRC16 vector file.

Written straight from the CRC-16/CCITT-FALSE parameter definition (poly 0x1021,
init 0xFFFF, no reflection, no final XOR) rather than from the TypeScript or C
sources, and bit-by-bit rather than table-driven. The point is to be a genuinely
independent third opinion: if this agrees with the committed vectors, then the
nibble table used by the shipped implementations is not merely self-consistent.

Usage:
    python tools/verify-crc-vectors.py
"""

from __future__ import annotations

import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
VECTOR_FILE = REPO_ROOT / "packages" / "protocol" / "test" / "fixtures" / "crc16-vectors.json"

POLY = 0x1021
INIT = 0xFFFF
CHECK_INPUT = b"123456789"
CHECK_VALUE = 0x29B1


def crc16_ccitt_false(data: bytes) -> int:
    """Bit-by-bit CRC-16/CCITT-FALSE, direct from the parameter definition."""
    crc = INIT
    for byte in data:
        for bit in range(7, -1, -1):
            msb = (crc >> 15) & 1
            crc = (crc << 1) & 0xFFFF
            if msb ^ ((byte >> bit) & 1):
                crc ^= POLY
    return crc


def main() -> int:
    anchor = crc16_ccitt_false(CHECK_INPUT)
    if anchor != CHECK_VALUE:
        print(f"FAIL  check value: got 0x{anchor:04X}, expected 0x{CHECK_VALUE:04X}")
        return 1
    print(f"OK    check value CRC('123456789') = 0x{anchor:04X}")

    if not VECTOR_FILE.exists():
        print(f"FAIL  vector file missing: {VECTOR_FILE}")
        return 1

    document = json.loads(VECTOR_FILE.read_text(encoding="utf-8"))

    algorithm = document["algorithm"]
    declared = (
        int(algorithm["poly"], 16),
        int(algorithm["init"], 16),
        algorithm["reflect_in"],
        algorithm["reflect_out"],
        int(algorithm["xor_out"], 16),
    )
    if declared != (POLY, INIT, False, False, 0x0000):
        print(f"FAIL  vector file declares different parameters: {algorithm}")
        return 1
    print(f"OK    parameters match: poly=0x{POLY:04X} init=0x{INIT:04X} no-reflect no-xorout")

    mismatches = 0
    for index, vector in enumerate(document["vectors"]):
        data = bytes.fromhex(vector["data_hex"])
        if len(data) != vector["len"]:
            print(f"FAIL  vector {index}: len {vector['len']} but {len(data)} bytes of data")
            mismatches += 1
            continue
        expected = int(vector["crc"], 16)
        actual = crc16_ccitt_false(data)
        if actual != expected:
            print(f"FAIL  vector {index} (len {len(data)}): got 0x{actual:04X}, file says 0x{expected:04X}")
            mismatches += 1

    total = len(document["vectors"])
    if mismatches:
        print(f"\nFAIL  {mismatches}/{total} vectors disagree")
        return 1

    print(f"OK    {total}/{total} vectors verified against an independent implementation")
    print(f"LH_METRIC test.cross_lang_vectors_py value={total} unit=vectors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
