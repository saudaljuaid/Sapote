#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Require exact serial-log markers once and in the declared order."""

from __future__ import annotations

from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) < 4:
        raise SystemExit("usage: serial-marker-order.py LOG MARKER MARKER [MARKER ...]")
    lines = Path(sys.argv[1]).read_text(encoding="utf-8", errors="strict").splitlines()
    cursor = -1
    for marker in sys.argv[2:]:
        positions = [index for index, line in enumerate(lines) if line == marker]
        if len(positions) != 1:
            raise SystemExit(
                f"serial marker must appear exactly once: {marker!r} "
                f"(found {len(positions)})")
        if positions[0] <= cursor:
            raise SystemExit(f"serial marker is out of order: {marker!r}")
        cursor = positions[0]
    print(f"ordered serial lifecycle passed: {len(sys.argv) - 2} markers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
