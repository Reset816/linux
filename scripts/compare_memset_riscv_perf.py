#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Compare two test_memset_riscv perf logs.

Example:
  python3 scripts/compare_memset_riscv_perf.py --baseline off.log --optimized on.log
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path


PERF_RE = re.compile(
    r"""
    test_memset_riscv:\s+perf\s+
    len=\s*(?P<len>\d+)\s+
    val=\s*(?P<val>0x[0-9a-fA-F]+)\s+
    off=\s*(?P<off>\d+)\s+
    loops=\s*(?P<loops>\d+)\s+
    time=\s*(?P<time>\d+)ns\s+
    MiB/s=\s*(?P<mibps>\d+)\s+
    expect_zicboz=\s*(?P<expect>[01])
    """,
    re.VERBOSE,
)


@dataclass(frozen=True)
class CaseKey:
    length: int
    value: str
    offset: int


@dataclass
class CaseStats:
    mibps_samples: list[int]
    time_samples_ns: list[int]
    expect_samples: list[int]

    def __init__(self) -> None:
        self.mibps_samples = []
        self.time_samples_ns = []
        self.expect_samples = []

    @property
    def mibps_avg(self) -> float:
        return statistics.fmean(self.mibps_samples)

    @property
    def time_avg(self) -> float:
        return statistics.fmean(self.time_samples_ns)

    @property
    def expect_mode(self) -> int:
        return 1 if sum(self.expect_samples) * 2 >= len(self.expect_samples) else 0


def parse_log(path: Path) -> dict[CaseKey, CaseStats]:
    cases: dict[CaseKey, CaseStats] = {}
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line_no, line in enumerate(f, start=1):
            m = PERF_RE.search(line)
            if not m:
                continue
            key = CaseKey(
                length=int(m.group("len")),
                value=m.group("val").lower(),
                offset=int(m.group("off")),
            )
            stat = cases.setdefault(key, CaseStats())
            stat.mibps_samples.append(int(m.group("mibps")))
            stat.time_samples_ns.append(int(m.group("time")))
            stat.expect_samples.append(int(m.group("expect")))

    return cases


def format_delta_pct(delta: float) -> str:
    return f"{delta:+.2f}%"


def compare(
    base: dict[CaseKey, CaseStats],
    opt: dict[CaseKey, CaseStats],
    only_expect: int | None,
    sort_by: str,
) -> int:
    common = sorted(set(base.keys()) & set(opt.keys()), key=lambda k: (k.length, k.value, k.offset))
    only_base = sorted(set(base.keys()) - set(opt.keys()), key=lambda k: (k.length, k.value, k.offset))
    only_opt = sorted(set(opt.keys()) - set(base.keys()), key=lambda k: (k.length, k.value, k.offset))

    if only_expect is not None:
        common = [k for k in common if opt[k].expect_mode == only_expect]

    rows = []
    for k in common:
        b = base[k]
        o = opt[k]
        b_mibps = b.mibps_avg
        o_mibps = o.mibps_avg
        if b_mibps == 0:
            delta_pct = float("inf")
        else:
            delta_pct = (o_mibps - b_mibps) / b_mibps * 100.0
        rows.append((k, b, o, delta_pct))

    if sort_by == "delta_pct":
        rows.sort(key=lambda x: x[3], reverse=True)
    elif sort_by == "length":
        rows.sort(key=lambda x: (x[0].length, x[0].value, x[0].offset))

    print(
        "len   val    off  base_MiB/s  opt_MiB/s  delta_MiB/s  delta_pct  base_exp  opt_exp  samples(b/o)"
    )
    print(
        "----  -----  ---  ----------  ---------  -----------  ---------  --------  -------  -----------"
    )
    for k, b, o, delta_pct in rows:
        delta_abs = o.mibps_avg - b.mibps_avg
        print(
            f"{k.length:>4}  {k.value:>5}  {k.offset:>3}  "
            f"{b.mibps_avg:>10.2f}  {o.mibps_avg:>9.2f}  {delta_abs:>11.2f}  "
            f"{format_delta_pct(delta_pct):>9}  {b.expect_mode:>8}  {o.expect_mode:>7}  "
            f"{len(b.mibps_samples):>3}/{len(o.mibps_samples):<3}"
        )

    if rows:
        mean_delta = statistics.fmean([r[3] for r in rows if r[3] != float("inf")])
        improved = sum(1 for r in rows if r[3] > 0)
        degraded = sum(1 for r in rows if r[3] < 0)
        same = sum(1 for r in rows if r[3] == 0)
        print()
        print(f"Compared cases: {len(rows)}")
        print(f"Average delta:  {mean_delta:+.2f}%")
        print(f"Improved/Degraded/Same: {improved}/{degraded}/{same}")
    else:
        print()
        print("Compared cases: 0")

    if only_base:
        print()
        print(f"Cases only in baseline log: {len(only_base)}")
    if only_opt:
        print(f"Cases only in optimized log: {len(only_opt)}")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two test_memset_riscv perf logs.")
    parser.add_argument("--baseline", required=True, type=Path, help="log with optimization disabled")
    parser.add_argument("--optimized", required=True, type=Path, help="log with optimization enabled")
    parser.add_argument(
        "--only-expect",
        choices=("0", "1"),
        default=None,
        help="only compare cases whose optimized log has expect_zicboz as this value",
    )
    parser.add_argument(
        "--sort-by",
        choices=("length", "delta_pct"),
        default="length",
        help="output sort key",
    )
    args = parser.parse_args()

    if not args.baseline.is_file():
        print(f"baseline log not found: {args.baseline}", file=sys.stderr)
        return 2
    if not args.optimized.is_file():
        print(f"optimized log not found: {args.optimized}", file=sys.stderr)
        return 2

    base = parse_log(args.baseline)
    opt = parse_log(args.optimized)
    if not base:
        print(f"no perf lines found in baseline log: {args.baseline}", file=sys.stderr)
        return 2
    if not opt:
        print(f"no perf lines found in optimized log: {args.optimized}", file=sys.stderr)
        return 2

    only_expect = int(args.only_expect) if args.only_expect is not None else None
    return compare(base, opt, only_expect, args.sort_by)


if __name__ == "__main__":
    raise SystemExit(main())
