#!/usr/bin/env python3
"""Reader for policy_io.bin (network input/output recording, see policy_io_recorder.h).

Usage: python3 read_policy_io.py policy_io.bin   (prints a short summary; needs numpy)
In code: d = read_policy_io(path); d["obs"], d["act"], d["frame"], d["t_ns"], d["clip"], d["seq"], d["dropped"].
A truncated last record (process crash) is dropped without an error.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np

CLIP_BYTES = 64


def read_policy_io(path: Path | str) -> dict:
    raw = Path(path).read_bytes()
    if raw[:4] != b"PIO1":
        raise ValueError("not a policy_io file (missing PIO1 magic)")
    version, obs_dim, act_dim, wall = struct.unpack_from("<IIId", raw, 4)
    if version != 1:
        raise ValueError(f"unknown format version {version}")
    (layout_len,) = struct.unpack_from("<I", raw, 24)
    layout = raw[28:28 + layout_len].decode("utf-8", "replace")
    start = 28 + layout_len
    dt = np.dtype([("seq", "<u8"), ("dropped", "<u8"), ("frame", "<i4"), ("t_ns", "<i8"),
                   ("clip", f"S{CLIP_BYTES}"), ("obs", "<f4", (obs_dim,)), ("act", "<f4", (act_dim,))])
    body = raw[start:]
    n = len(body) // dt.itemsize
    rec = np.frombuffer(body[: n * dt.itemsize], dtype=dt)
    return {
        "meta": {"version": version, "obs_dim": obs_dim, "act_dim": act_dim, "wall_start": wall,
                 "obs_layout": layout, "truncated_tail": len(body) % dt.itemsize != 0},
        "seq": rec["seq"].copy(), "dropped": rec["dropped"].copy(), "frame": rec["frame"].copy(),
        "t_ns": rec["t_ns"].copy(),
        "clip": np.array([c.decode("utf-8", "replace") for c in rec["clip"]], dtype=object),
        "obs": rec["obs"].copy(), "act": rec["act"].copy(),
    }


def summarize(d: dict) -> dict:
    seq = d["seq"]
    n = len(seq)
    gaps = int(np.sum(np.diff(seq) > 1)) if n > 1 else 0
    lost = int(seq[-1] + 1 - n) if n else 0
    dt = np.diff(d["t_ns"]) / 1e9 if n > 1 else np.array([])
    return {
        "records": n, "seq_gaps": gaps, "lost_by_seq": lost,
        "dropped_reported": int(d["dropped"][-1]) if n else 0,
        "rate_hz": float(1 / np.median(dt)) if len(dt) else 0.0,
        "longest_gap_s": float(dt.max()) if len(dt) else 0.0,
        "clips": sorted(set(d["clip"].tolist())),
        "frames": (int(d["frame"].min()), int(d["frame"].max())) if n else (0, 0),
        "truncated_tail": bool(d["meta"]["truncated_tail"]),
    }


def main() -> None:
    d = read_policy_io(sys.argv[1])
    s = summarize(d)
    print("input layout:", d["meta"]["obs_layout"], "| input dim", d["meta"]["obs_dim"], "| output dim", d["meta"]["act_dim"])
    print(f"records {s['records']}, rate {s['rate_hz']:.1f} Hz, longest gap {s['longest_gap_s'] * 1000:.0f} ms")
    print(f"lost by seq gaps: {s['lost_by_seq']} (gaps: {s['seq_gaps']}), reported drop counter: {s['dropped_reported']}")
    print("clips:", s["clips"], "| frames:", s["frames"][0], "-", s["frames"][1])
    if s["truncated_tail"]:
        print("last record is truncated (process crashed), it was dropped")
    print("OK" if s["records"] and s["lost_by_seq"] == 0 else "WARNING: empty or records were lost")


if __name__ == "__main__":
    main()
