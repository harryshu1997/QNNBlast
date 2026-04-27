#!/usr/bin/env python3
"""
qblast database codegen.

Reads a tuner_driver leaderboard.json and emits a per-(kernel, soc) C++
header populating `qblast::DatabaseEntry kEntries[]`. The generated header
is meant to be #included by src/database/database.hpp and ultimately
linked into libqblast.so.

Usage:
    scripts/database/database.py \\
        --leaderboard scripts/database/json/leaderboard.json \\
        --kernel gemv_w4a16 \\
        --soc-id sd8e_gen5 \\
        --output src/database/kernels/gemv_w4a16/sd8e_gen5.hpp

The leaderboard JSON shape:
    {
      "<shape_key>": [
        {"cfg_id": 0, "defines": {...}, "cycles_med": ...,
         "compute_us_med": ..., "rel_err": ..., "status": "ok"},
        ...
      ],
      ...
    }

For each shape, picks the row with smallest cycles_med among status=="ok"
entries. shape_key is used verbatim as the lookup key (e.g. "4096_4096_64").
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


def pick_winner(rows: list[dict]) -> dict | None:
    ok_rows = [r for r in rows if r.get("status") == "ok"]
    if not ok_rows:
        return None
    return min(ok_rows, key=lambda r: r["cycles_med"])


def make_entry_line(shape_key: str, winner: dict) -> str:
    defines = winner.get("defines", {})
    q_block = defines.get("QBLAST_Q_BLOCK", 64)
    tile_m = defines.get("QBLAST_TILE_M", 1)
    n_hw = defines.get("QBLAST_N_HW_THREADS", 1)
    return (f'    {{ "{shape_key}", {winner["cfg_id"]}, '
            f'{{ {q_block}, {tile_m}, {n_hw} }}, '
            f'{int(winner["cycles_med"])}ULL }},')


def render_header(kernel: str, soc_id: str, leaderboard: dict[str, list[dict]],
                  source_path: Path) -> str:
    namespace = f"qblast::{kernel}::{soc_id}"
    guard = f"QBLAST_DB_{kernel.upper()}_{soc_id.upper()}_HPP"

    lines: list[str] = []
    lines.append(f"// AUTO-GENERATED from {source_path.name} — do not edit.")
    lines.append(f"// Regenerate with: scripts/database/database.py")
    lines.append("")
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append('#include "../../database_structure.hpp"')
    lines.append("")
    lines.append(f"namespace qblast {{ namespace {kernel} {{ namespace {soc_id} {{")
    lines.append("")
    lines.append("constexpr DatabaseEntry kEntries[] = {")

    skipped: list[str] = []
    entries: list[str] = []
    for shape_key in sorted(leaderboard.keys()):
        rows = leaderboard[shape_key]
        winner = pick_winner(rows)
        if winner is None:
            skipped.append(shape_key)
            continue
        entries.append(make_entry_line(shape_key, winner))

    if not entries:
        # Make the array non-empty so sizeof works; default 0-cfg row.
        entries.append('    { "default", 0, { 64, 1, 1 }, 0ULL },')

    lines.extend(entries)
    lines.append("};")
    lines.append("")
    lines.append("constexpr std::size_t kNumEntries = sizeof(kEntries) / sizeof(kEntries[0]);")
    lines.append("")
    lines.append("// Fallback cfg used when a (M, K, q_block) shape isn't in kEntries.")
    lines.append("// Plan §289 says default = baseline Q=64 / TILE_M=1 / single-thread.")
    lines.append('constexpr DatabaseEntry kDefault = { "default", 0, { 64, 1, 1 }, 0ULL };')
    lines.append("")
    lines.append(f"}} }} }}  // namespace {namespace}")
    lines.append("")
    lines.append(f"#endif  // {guard}")
    lines.append("")

    if skipped:
        sys.stderr.write(f"[database] WARN: skipped {len(skipped)} shapes "
                         f"with no ok entry: {skipped}\n")

    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--leaderboard", required=True, type=Path)
    ap.add_argument("--kernel", required=True,
                    help='kernel name, e.g. "gemv_w4a16"')
    ap.add_argument("--soc-id", required=True,
                    help='SoC id, e.g. "sd8e_gen5" (Snapdragon 8 Elite Gen 5)')
    ap.add_argument("--output", required=True, type=Path,
                    help="path to write the generated .hpp")
    args = ap.parse_args()

    leaderboard: Any = json.loads(args.leaderboard.read_text())
    if not isinstance(leaderboard, dict):
        sys.exit(f"[database] leaderboard.json must be a dict, got {type(leaderboard).__name__}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    text = render_header(args.kernel, args.soc_id, leaderboard, args.leaderboard)
    args.output.write_text(text)

    n_entries = sum(1 for v in leaderboard.values()
                    if any(r.get("status") == "ok" for r in v))
    print(f"[database] wrote {args.output} with {n_entries} entries "
          f"for {args.kernel} on {args.soc_id}")


if __name__ == "__main__":
    main()
