#!/usr/bin/env python3
"""
qblast tuner driver.

Orchestrates the full sweep: build N variants, push their skel .so files
to the device, fan out (cfg × shape) broadcasts, pull JSON results, print
a leaderboard.

Usage:
    qblast-env                                    # Hexagon SDK env
    export ANDROID_SERIAL=3C15AU002CL00000        # OnePlus 15 default
    scripts/tuner_driver.py \\
        --cfg-file scripts/database/cfgs/sample.json \\
        --shapes 4096_4096_64,4096_4096_128,11008_4096_64,11008_4096_128 \\
        --kernel gemv_w4a16

Per (cfg, shape) the driver waits up to --timeout seconds for the
result JSON to land; if multiple iters are slow, bump --timeout.

Cfg id N is paired with all shapes; the runtime check inside the kernel
falls back to scalar when the runtime q_block doesn't match the variant's
compile-time QBLAST_Q_BLOCK.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def run(cmd: list[str], check: bool = True, **kw) -> subprocess.CompletedProcess:
    print(f"[tuner] $ {' '.join(cmd)}")
    return subprocess.run(cmd, check=check, **kw)


def adb(serial: str, *args: str, **kw) -> subprocess.CompletedProcess:
    return run(["adb", "-s", serial, *args], **kw)


def parse_shape(shape: str) -> tuple[int, int, int]:
    parts = shape.split("_")
    if len(parts) != 3:
        sys.exit(f"[tuner] bad shape '{shape}', expected M_K_q")
    return int(parts[0]), int(parts[1]), int(parts[2])


def device_results_dir(package: str = "com.qblast.tuner") -> str:
    return f"/sdcard/Android/data/{package}/files/results"


def pull_json(serial: str, dst: Path) -> None:
    """Sync all device JSON into dst (flatten the auto-created subdir)."""
    src = device_results_dir()
    dst.mkdir(parents=True, exist_ok=True)
    tmp = dst.parent / "_pull_tmp"
    tmp.mkdir(exist_ok=True)
    try:
        run(["adb", "-s", serial, "pull", src, str(tmp)],
            check=False)
        sub = tmp / "results"
        if sub.is_dir():
            for f in sub.glob("*.json"):
                shutil.copy2(f, dst / f.name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def trigger(serial: str, cfg_id: int, shape: str,
            warmup: int, iters: int, seed: int) -> None:
    cmd = [
        "adb", "-s", serial, "shell", "am", "broadcast",
        "-a", "com.qblast.TUNE",
        "-n", "com.qblast.tuner/.TuneBroadcastReceiver",
        "--ei", "cfg_id", str(cfg_id),
        "--es", "shape", shape,
        "--ei", "seed", str(seed),
        "--ei", "warmup", str(warmup),
        "--ei", "iters", str(iters),
    ]
    run(cmd, stdout=subprocess.DEVNULL)


def wait_for_json(serial: str, cfg_id: int, M: int, K: int, q: int,
                  timeout_s: int = 120) -> dict | None:
    """Poll the device until <results_dir>/cfg{N}_M_K_q.json appears
    and stops growing. Returns parsed JSON or None on timeout."""
    target = f"{device_results_dir()}/cfg{cfg_id}_{M}_{K}_{q}.json"
    deadline = time.time() + timeout_s
    last_size = -1
    stable_for = 0
    while time.time() < deadline:
        # `adb shell stat` to check size and existence
        r = subprocess.run(
            ["adb", "-s", serial, "shell", f"stat -c %s {target} 2>/dev/null"],
            capture_output=True, text=True)
        size_str = r.stdout.strip()
        try:
            size = int(size_str)
        except ValueError:
            size = -1
        if size > 0:
            if size == last_size:
                stable_for += 1
                if stable_for >= 2:  # ~1 second stable
                    break
            else:
                stable_for = 0
                last_size = size
        time.sleep(0.5)
    else:
        return None

    # Pull it
    r = subprocess.run(
        ["adb", "-s", serial, "shell", f"cat {target}"],
        capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        return None
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError as e:
        print(f"[tuner] WARN: bad JSON for cfg{cfg_id} shape {M}_{K}_{q}: {e}")
        return None


def build_variants(repo_root: Path, kernel: str, cfg_file: Path,
                   variants_dir: Path) -> list[dict]:
    """Calls variant_builder.py + returns parsed manifest."""
    cmd = [
        sys.executable,
        str(repo_root / "scripts" / "variant_builder.py"),
        "--kernel", kernel,
        "--cfg-file", str(cfg_file),
        "--output-dir", str(variants_dir),
        "--repo-root", str(repo_root),
    ]
    run(cmd)
    manifest_path = variants_dir / "manifest.json"
    return json.loads(manifest_path.read_text())


def push_variants(serial: str, manifest_path: Path) -> None:
    repo_root = manifest_path.resolve().parent.parent.parent.parent
    run([str(repo_root / "scripts" / "push_skels.sh"), str(manifest_path)])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", default="gemv_w4a16")
    ap.add_argument("--cfg-file", required=True, type=Path)
    ap.add_argument("--shapes", required=True,
                    help="comma-separated list of M_K_q strings")
    ap.add_argument("--variants-dir", type=Path, default=None,
                    help="where variant_builder writes; default scripts/database/variants/")
    ap.add_argument("--results-dir", type=Path, default=None,
                    help="where to collect pulled JSONs; default scripts/database/json/")
    ap.add_argument("--device", default=os.environ.get("ANDROID_SERIAL")
                                    or os.environ.get("ANDROID_DEVICE")
                                    or "3C15AU002CL00000")
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--iters", type=int, default=5)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--timeout", type=int, default=180,
                    help="per-(cfg,shape) wait timeout in seconds")
    ap.add_argument("--skip-build", action="store_true",
                    help="reuse existing variants_dir, don't re-run variant_builder")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    variants_dir = (args.variants_dir or
                    repo_root / "scripts" / "database" / "variants").resolve()
    results_dir = (args.results_dir or
                   repo_root / "scripts" / "database" / "json").resolve()
    shapes = [s.strip() for s in args.shapes.split(",") if s.strip()]

    print(f"[tuner] kernel={args.kernel}")
    print(f"[tuner] cfgs from {args.cfg_file}")
    print(f"[tuner] shapes ({len(shapes)}): {shapes}")
    print(f"[tuner] device={args.device}")

    if not args.skip_build:
        manifest = build_variants(repo_root, args.kernel, args.cfg_file, variants_dir)
    else:
        manifest_path = variants_dir / "manifest.json"
        if not manifest_path.exists():
            sys.exit(f"[tuner] --skip-build but {manifest_path} missing")
        manifest = json.loads(manifest_path.read_text())
    push_variants(args.device, variants_dir / "manifest.json")

    # Make sure device results dir exists.
    adb(args.device, "shell", "mkdir", "-p", device_results_dir(),
        stdout=subprocess.DEVNULL)

    leaderboard: dict[str, list[dict]] = {s: [] for s in shapes}

    for cfg in manifest:
        cfg_id = cfg["cfg_id"]
        for shape in shapes:
            M, K, q = parse_shape(shape)
            print(f"[tuner] === cfg{cfg_id} shape={shape} ===")
            # Clear any stale json for this (cfg, shape) so the wait loop
            # doesn't return last run's data.
            target = f"{device_results_dir()}/cfg{cfg_id}_{M}_{K}_{q}.json"
            adb(args.device, "shell", f"rm -f {target}",
                stdout=subprocess.DEVNULL)

            trigger(args.device, cfg_id, shape, args.warmup, args.iters, args.seed)
            result = wait_for_json(args.device, cfg_id, M, K, q,
                                   timeout_s=args.timeout)
            if result is None:
                print(f"[tuner] !! timeout waiting on cfg{cfg_id} {shape}")
                continue
            print(f"[tuner]    cycles_med={result['dsp_cycles_med']:>12,d}"
                  f"  rel_err={result['max_rel_err']:.3e}"
                  f"  status={result['status']}")
            leaderboard[shape].append({
                "cfg_id": cfg_id,
                "defines": cfg.get("defines", {}),
                "cycles_med": result["dsp_cycles_med"],
                "compute_us_med": result["compute_us_med"],
                "rel_err": result["max_rel_err"],
                "status": result["status"],
            })

    pull_json(args.device, results_dir)

    print()
    print("=" * 80)
    print("LEADERBOARD")
    print("=" * 80)
    for shape, rows in leaderboard.items():
        print(f"\nshape {shape}:")
        # Sort by status=ok first, then ascending cycles
        rows_sorted = sorted(rows, key=lambda r: (r["status"] != "ok",
                                                   r["cycles_med"]))
        if not rows_sorted:
            print("  (no results)")
            continue
        winner = rows_sorted[0]
        for i, r in enumerate(rows_sorted):
            tag = " <- winner" if i == 0 and r["status"] == "ok" else ""
            print(f"  cfg{r['cfg_id']:>2}  defines={r['defines']}"
                  f"  cycles_med={r['cycles_med']:>12,d}"
                  f"  compute_us={r['compute_us_med']:>7,d}"
                  f"  rel_err={r['rel_err']:.3e}"
                  f"  {r['status']}{tag}")

    summary_path = results_dir / "leaderboard.json"
    summary_path.write_text(json.dumps(leaderboard, indent=2) + "\n")
    print(f"\n[tuner] leaderboard written to {summary_path.relative_to(repo_root)}")


if __name__ == "__main__":
    main()
