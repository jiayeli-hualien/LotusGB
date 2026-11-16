#!/usr/bin/env python3
"""Summarize guest-clock progress during stream-v1 serial transfer waits."""

from __future__ import annotations

import argparse
import math
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional


DEFAULT_DMG_CLOCK_HZ = 4_194_304
STATE_RE = re.compile(r"Serial state \(([^)]+)\):")
PAIR_RE = re.compile(r"Serial byte pair:")
FIELD_RE = re.compile(r"([A-Za-z_]+)=([0-9]+)")


@dataclass(frozen=True)
class TransferSample:
    sequence: int
    wall_wait_us: int
    guest_cycles: int
    logged_wait_us: Optional[int]

    def guest_wall_ratio(self, clock_hz: int) -> float | None:
        if self.wall_wait_us <= 0 or clock_hz <= 0:
            return None
        return self.guest_cycles * 1_000_000 / (clock_hz * self.wall_wait_us)


@dataclass
class LogAnalysis:
    label: str
    starts: dict[int, dict[str, int]]
    completions: dict[int, dict[str, int]]
    pair_waits: dict[int, int]
    timeouts: int

    def samples(self) -> list[TransferSample]:
        result = []
        for sequence, start in sorted(self.starts.items()):
            end = self.completions.get(sequence)
            if end is None:
                continue
            wall_wait = end["session_us"] - start["session_us"]
            guest_cycles = end["cpu_tcycles"] - start["cpu_tcycles"]
            if wall_wait < 0 or guest_cycles < 0:
                continue
            result.append(TransferSample(
                sequence=sequence,
                wall_wait_us=wall_wait,
                guest_cycles=guest_cycles,
                logged_wait_us=self.pair_waits.get(sequence),
            ))
        return result

    @property
    def incomplete(self) -> list[int]:
        return sorted(set(self.starts) - set(self.completions))


def parse_log_lines(lines: Iterable[str], label: str) -> LogAnalysis:
    starts: dict[int, dict[str, int]] = {}
    completions: dict[int, dict[str, int]] = {}
    pair_waits: dict[int, int] = {}
    timeouts = 0

    for line in lines:
        state_match = STATE_RE.search(line)
        if state_match:
            event = state_match.group(1)
            fields = {key: int(value) for key, value in FIELD_RE.findall(line)}
            if event == "peer-byte-timeout":
                timeouts += 1
            if event not in ("transfer-start", "after-byte-pair"):
                continue
            sequence = fields.get("sequence")
            session_us = fields.get("session_us")
            cpu_tcycles = fields.get("cpu_tcycles")
            if sequence is None or session_us is None or cpu_tcycles is None:
                continue
            target = starts if event == "transfer-start" else completions
            target[sequence] = {
                "session_us": session_us,
                "cpu_tcycles": cpu_tcycles,
            }
            continue

        if PAIR_RE.search(line):
            fields = {key: int(value) for key, value in FIELD_RE.findall(line)}
            sequence = fields.get("local_sequence")
            wait_us = fields.get("wait_us")
            if sequence is not None and wait_us is not None:
                pair_waits[sequence] = wait_us

    return LogAnalysis(label, starts, completions, pair_waits, timeouts)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def format_ms(microseconds: float) -> str:
    return f"{microseconds / 1000:.2f} ms"


def print_analysis(analysis: LogAnalysis, clock_hz: int) -> bool:
    samples = analysis.samples()
    if not analysis.starts:
        print(f"{analysis.label}: no transfer-start traces found")
        return False
    if not samples:
        print(f"{analysis.label}: no matched transfer-start/after-byte-pair traces")
        return False

    waits = [sample.logged_wait_us if sample.logged_wait_us is not None
             else sample.wall_wait_us for sample in samples]
    guest_ms = [sample.guest_cycles * 1000 / clock_hz for sample in samples]
    ratios = [ratio for sample in samples
              if (ratio := sample.guest_wall_ratio(clock_hz)) is not None]

    print(f"{analysis.label}: {len(samples)}/{len(analysis.starts)} transfers paired; "
          f"timeouts={analysis.timeouts}; incomplete={len(analysis.incomplete)}")
    print(f"  local transfer wait: median={format_ms(statistics.median(waits))}, "
          f"p95={format_ms(percentile(waits, 0.95))}, "
          f"max={format_ms(max(waits))}")
    print(f"  guest time advanced while waiting: "
          f"median={statistics.median(guest_ms):.2f} ms, "
          f"p95={percentile(guest_ms, 0.95):.2f} ms")
    if ratios:
        print(f"  guest-time / wall-wait ratio: "
              f"median={statistics.median(ratios):.2f}x, "
              f"p95={percentile(ratios, 0.95):.2f}x")
    if analysis.incomplete:
        preview = ", ".join(str(sequence) for sequence in analysis.incomplete[:8])
        suffix = "…" if len(analysis.incomplete) > 8 else ""
        print(f"  incomplete transfer sequences: {preview}{suffix}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description=("Compare guest T-cycle progress during local stream-v1 "
                     "serial waits in host and client logs."))
    parser.add_argument("host_log", type=Path)
    parser.add_argument("client_log", type=Path)
    parser.add_argument("--clock-hz", type=int, default=DEFAULT_DMG_CLOCK_HZ,
                        help="CPU T-cycle frequency (default: DMG 4194304 Hz)")
    args = parser.parse_args()
    if args.clock_hz <= 0:
        parser.error("--clock-hz must be positive")

    analyses = []
    for label, path in (("Host", args.host_log), ("Client", args.client_log)):
        try:
            with path.open(encoding="utf-8", errors="replace") as stream:
                analyses.append(parse_log_lines(stream, label))
        except OSError as error:
            print(f"{label}: cannot read {path}: {error}", file=sys.stderr)
            return 2

    success = all(print_analysis(analysis, args.clock_hz)
                  for analysis in analyses)
    print("Note: this compares each endpoint's guest-time advance during its "
          "own transfer wait; it is not an absolute inter-machine phase/skew "
          "measurement.")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
