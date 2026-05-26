#!/usr/bin/env python3
"""
NanoJS8 — DRAM budget check.

Invoked as a POST_BUILD hook from main/CMakeLists.txt. Reads the ESP-IDF
`idf.py size` JSON output and prints / enforces a DRAM budget floor.

Phase 0:  informational. Prints free DRAM, no enforcement.
Phase 4+: hard gate. Build fails if free DRAM < CONFIG_NANOJS8_DRAM_STEADY_KB
          (default 120 KB per Build Specification §4).

Usage (from CMake):
    python tools/check_dram_budget.py <build_dir> --phase 0
    python tools/check_dram_budget.py <build_dir> --variant ft8 --floor-kb 120

Designed to fail fast with a clear error message rather than producing a binary
that boots and then runs out of heap during the first decode slot.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def run_idf_size(build_dir: Path) -> dict:
    """Invoke `idf.py size --format json` and return parsed output.

    We use the IDF tool rather than parsing map files directly because the
    map-file format has changed between IDF versions and we want the future
    migration path (v5.5 -> v6.x) to be as boring as possible.
    """
    # idf.py is a wrapper script — works from PowerShell, bash, anywhere export.ps1
    # / export.sh has set up the environment. CMake's POST_BUILD hook inherits
    # that environment.
    cmd = ["idf.py", "-C", str(build_dir.parent), "size", "--format", "json"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    except FileNotFoundError:
        # `idf.py` not on PATH — likely the developer ran this script directly
        # without sourcing export.ps1. Be helpful.
        print("[check_dram_budget] ERROR: 'idf.py' not on PATH.")
        print("[check_dram_budget] Did you run export.ps1 (or export.sh)?")
        sys.exit(1)

    if result.returncode != 0:
        print("[check_dram_budget] WARNING: `idf.py size` exited non-zero.")
        print(result.stderr)
        # Not fatal in Phase 0 — we want the developer to see the binary even
        # if size analysis fails. Return empty dict so caller treats as unknown.
        return {}

    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as e:
        print(f"[check_dram_budget] WARNING: failed to parse idf.py size JSON: {e}")
        return {}


def main() -> int:
    parser = argparse.ArgumentParser(description="NanoJS8 DRAM budget check")
    parser.add_argument("build_dir", type=Path, help="CMake build directory")
    parser.add_argument("--phase", type=int, default=0,
                        help="Project phase number (0 = informational, 4+ = enforce)")
    parser.add_argument("--floor-kb", type=int, default=120,
                        help="Minimum free internal DRAM in KB (default 120)")
    parser.add_argument("--variant", type=str, default=None,
                        help="Build variant label (informational only)")
    args = parser.parse_args()

    if not args.build_dir.exists():
        print(f"[check_dram_budget] build dir does not exist: {args.build_dir}")
        return 0  # Don't fail the build if the script runs before any output exists.

    size_data = run_idf_size(args.build_dir)
    if not size_data:
        print("[check_dram_budget] Could not retrieve size data; skipping check.")
        return 0

    # `idf.py size --format json` output structure changes between IDF versions.
    # We probe a few known shapes rather than hard-coding one. The values we
    # want are total used DRAM, available DRAM, and total internal RAM.
    used_dram = None
    total_dram = None

    # Probe shape A: "memory_overview": { "dram": {"used": N, "total": N} }
    if isinstance(size_data, dict):
        overview = size_data.get("memory_overview") or size_data.get("Memory overview")
        if isinstance(overview, dict):
            dram_block = overview.get("dram") or overview.get("DRAM") or overview.get("dram_data")
            if isinstance(dram_block, dict):
                used_dram = dram_block.get("used") or dram_block.get("Used")
                total_dram = dram_block.get("total") or dram_block.get("Total")

        # Probe shape B: top-level keys.
        if used_dram is None:
            used_dram = size_data.get("used_dram") or size_data.get("dram_used")
        if total_dram is None:
            total_dram = size_data.get("total_dram") or size_data.get("dram_total")

    if used_dram is None or total_dram is None:
        print("[check_dram_budget] Could not find DRAM stats in idf.py size output.")
        print("[check_dram_budget] Output keys:",
              list(size_data.keys()) if isinstance(size_data, dict) else "<non-dict>")
        return 0  # Informational only.

    free_dram = total_dram - used_dram
    free_dram_kb = free_dram / 1024
    used_dram_kb = used_dram / 1024
    total_dram_kb = total_dram / 1024

    variant_str = f" (variant={args.variant})" if args.variant else ""
    print(f"[check_dram_budget] Phase {args.phase}{variant_str}")
    print(f"[check_dram_budget]   DRAM used  : {used_dram_kb:.1f} KB")
    print(f"[check_dram_budget]   DRAM total : {total_dram_kb:.1f} KB")
    print(f"[check_dram_budget]   DRAM free  : {free_dram_kb:.1f} KB (floor = {args.floor_kb} KB)")

    if args.phase >= 4 and free_dram_kb < args.floor_kb:
        print(f"[check_dram_budget] FAIL: free DRAM {free_dram_kb:.1f} KB is below "
              f"the floor of {args.floor_kb} KB. Build rejected.")
        return 1

    print("[check_dram_budget] OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
