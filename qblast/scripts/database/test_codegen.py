#!/usr/bin/env python3
"""
Test scripts/database/database.py codegen.

Synthesizes a tiny leaderboard, runs the codegen, and asserts on the
emitted header content. Doesn't compile — purely string-level checks.
For compile + lookup correctness see src/database/test_database.cpp.

Run:
    scripts/database/test_codegen.py
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent.parent
    codegen = repo_root / "scripts" / "database" / "database.py"
    if not codegen.exists():
        sys.exit(f"missing {codegen}")

    failures = 0

    def check(cond: bool, msg: str) -> None:
        nonlocal failures
        if cond:
            print(f"  ok: {msg}")
        else:
            print(f"FAIL: {msg}")
            failures += 1

    # ---------- 1. happy path: 3 shapes, picks lowest cycles per shape ----------
    leaderboard = {
        "100_100_64": [
            {"cfg_id": 0, "defines": {"QBLAST_Q_BLOCK": 64,  "QBLAST_TILE_M": 1, "QBLAST_N_HW_THREADS": 1},
             "cycles_med": 1000, "compute_us_med": 10, "rel_err": 0.001, "status": "ok"},
            {"cfg_id": 1, "defines": {"QBLAST_Q_BLOCK": 128, "QBLAST_TILE_M": 1, "QBLAST_N_HW_THREADS": 1},
             "cycles_med":  900, "compute_us_med":  9, "rel_err": 0.001, "status": "ok"},
        ],
        "200_200_32": [
            {"cfg_id": 2, "defines": {"QBLAST_Q_BLOCK": 32,  "QBLAST_TILE_M": 1, "QBLAST_N_HW_THREADS": 1},
             "cycles_med": 5000, "compute_us_med": 50, "rel_err": 0.001, "status": "ok"},
        ],
        "300_300_64": [
            {"cfg_id": 0, "defines": {"QBLAST_Q_BLOCK": 64, "QBLAST_TILE_M": 1, "QBLAST_N_HW_THREADS": 1},
             "cycles_med": 7000, "compute_us_med": 70, "rel_err": 0.5, "status": "rel_err_exceeds_tolerance"},
            {"cfg_id": 1, "defines": {"QBLAST_Q_BLOCK": 128, "QBLAST_TILE_M": 1, "QBLAST_N_HW_THREADS": 1},
             "cycles_med": 8000, "compute_us_med": 80, "rel_err": 0.005, "status": "ok"},
        ],
    }

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        lb_path = tmp / "lb.json"
        out_path = tmp / "test.hpp"
        lb_path.write_text(json.dumps(leaderboard))

        proc = subprocess.run(
            [sys.executable, str(codegen),
             "--leaderboard", str(lb_path),
             "--kernel", "gemv_w4a16",
             "--soc-id", "sd8e_gen5",
             "--output", str(out_path)],
            capture_output=True, text=True)
        check(proc.returncode == 0, f"codegen exit 0 (stderr: {proc.stderr.strip()})")
        text = out_path.read_text()

        # Each ok-status shape should appear once.
        check('"100_100_64"' in text,    "100_100_64 entry present")
        check('"200_200_32"' in text,    "200_200_32 entry present")
        check('"300_300_64"' in text,    "300_300_64 entry present (only ok row picked)")

        # 100_100_64 should pick cfg_id=1 (cycles 900 < 1000) with Q=128.
        check(', 1, { 128, 1, 1 }, 900ULL' in text,
              "100_100_64 picks cfg_id=1 (Q=128) — lowest cycles")
        # 300_300_64 only has 1 ok row (cfg=1 Q=128); the 0.5 rel_err row is rejected.
        check(', 1, { 128, 1, 1 }, 8000ULL' in text,
              "300_300_64 picks cfg_id=1 (skipped the failing row)")

        # Default fallback should always be present.
        check('kDefault =' in text and '"default"' in text, "kDefault entry present")
        # Header guard + namespace correctly templated.
        check("QBLAST_DB_GEMV_W4A16_SD8E_GEN5_HPP" in text, "header guard correct")
        check("namespace qblast { namespace gemv_w4a16 { namespace sd8e_gen5" in text,
              "namespace correctly nested")
        check("constexpr DatabaseEntry kEntries[]" in text, "kEntries array declared")
        check("constexpr std::size_t kNumEntries" in text, "kNumEntries declared")

    # ---------- 2. all rows fail tolerance: codegen still produces valid file ----------
    bad_lb = {
        "9_9_9": [
            {"cfg_id": 0, "defines": {"QBLAST_Q_BLOCK": 64, "QBLAST_TILE_M": 1, "QBLAST_N_HW_THREADS": 1},
             "cycles_med": 1, "compute_us_med": 1, "rel_err": 99.0, "status": "rel_err_exceeds_tolerance"},
        ],
    }
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        lb_path = tmp / "lb.json"
        out_path = tmp / "test.hpp"
        lb_path.write_text(json.dumps(bad_lb))
        proc = subprocess.run(
            [sys.executable, str(codegen),
             "--leaderboard", str(lb_path),
             "--kernel", "gemv_w4a16",
             "--soc-id", "sd8e_gen5",
             "--output", str(out_path)],
            capture_output=True, text=True)
        check(proc.returncode == 0,
              "codegen survives a leaderboard with no ok rows")
        text = out_path.read_text()
        # No real entries — array contains a synthetic default-shaped row to keep
        # sizeof() valid.
        check('"default", 0, { 64, 1, 1 }' in text,
              "fallback synthetic entry inserted when zero ok rows")

    # ---------- 3. empty dict: bail nicely ----------
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        lb_path = tmp / "lb.json"
        out_path = tmp / "test.hpp"
        lb_path.write_text("{}")
        proc = subprocess.run(
            [sys.executable, str(codegen),
             "--leaderboard", str(lb_path),
             "--kernel", "gemv_w4a16",
             "--soc-id", "sd8e_gen5",
             "--output", str(out_path)],
            capture_output=True, text=True)
        check(proc.returncode == 0, "empty leaderboard produces empty header")
        check(out_path.exists(), "output file written even for empty input")

    print()
    if failures == 0:
        print("ALL CODEGEN TESTS PASSED")
        return 0
    print(f"{failures} FAILURES")
    return 1


if __name__ == "__main__":
    sys.exit(main())
