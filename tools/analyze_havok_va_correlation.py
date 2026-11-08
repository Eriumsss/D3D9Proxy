#!/usr/bin/env python3
"""Correlate opt-in retail Havok world pointer samples with process-wide x86 VA.

One CSV row contains both observations, so no unsafe joining of old logs by
GetTickCount is required. This DOES NOT measure hkPoolMemory allocations,
Havok shape count, physics bodies, or allocator leak/refcount correctness.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path

MIB = 1024 * 1024
COLUMNS = (
    "session_id", "process_id", "time_ms", "level_object", "manager_ptr",
    "world_ptr", "loading_flag", "world_valid", "world_status",
    "transition", "va_valid", "committed_bytes", "reserved_bytes",
    "free_bytes", "largest_free_bytes", "free_regions",
    "reserved_regions", "committed_regions",
)
TRIGGERS = {"WORLD_APPEARED", "WORLD_DISAPPEARED", "WORLD_REPLACED"}
WORLD_STATUSES = {"NO_MANAGER", "NO_WORLD", "WORLD_PRESENT"}
INVALID_STATUSES = {
    "UNREADABLE_ROOT", "BAD_MANAGER_RANGE", "BAD_MANAGER_VTABLE",
    "UNREADABLE_MANAGER_FIELDS", "UNREADABLE_WORLD", "RACED",
    "RACED_DURING_VA_SCAN",
}
TRANSITIONS = {"INITIAL_SAMPLE", "NONE", "UNKNOWN"} | TRIGGERS


def decimal(row: dict[str, str], key: str, maximum: int = 0xFFFFFFFF) -> int:
    val = int(row[key].strip(), 10)
    if not (0 <= val <= maximum):
        raise ValueError(f"invalid {key}")
    return val


def hex32(row: dict[str, str], key: str) -> int:
    raw = row[key].strip()
    if not re.fullmatch(r"[0-9A-Fa-f]{8}", raw):
        raise ValueError(f"invalid {key}")
    return int(raw, 16)


def parse_row(row: dict[str, str]) -> dict[str, object]:
    if any(row.get(key) is None for key in COLUMNS):
        raise ValueError("missing or truncated columns")
    session_id = row["session_id"].strip().upper()
    if not re.fullmatch(r"[0-9A-F]{16}", session_id):
        raise ValueError("invalid session_id")
    result: dict[str, object] = {
        "session_id": session_id,
        "process_id": decimal(row, "process_id"),
        "time_ms": decimal(row, "time_ms"),
        "level_object": hex32(row, "level_object"),
        "manager_ptr": hex32(row, "manager_ptr"),
        "world_ptr": hex32(row, "world_ptr"),
        "loading_flag": decimal(row, "loading_flag", 255),
        "world_valid": decimal(row, "world_valid", 1),
        "va_valid": decimal(row, "va_valid", 1),
        "world_status": row["world_status"].strip(),
        "transition": row["transition"].strip(),
    }
    for field in ("committed_bytes", "reserved_bytes", "free_bytes",
                  "largest_free_bytes"):
        result[field] = decimal(row, field, 0xFFFFFFFFFFFFFFFF)
    for field in ("free_regions", "reserved_regions", "committed_regions"):
        result[field] = decimal(row, field)
    if result["world_status"] not in WORLD_STATUSES | INVALID_STATUSES:
        raise ValueError("unsupported world_status")
    if result["transition"] not in TRANSITIONS:
        raise ValueError("unsupported transition")
    if result["world_valid"]:
        if result["world_status"] not in WORLD_STATUSES:
            raise ValueError("valid world pointer with invalid status")
        if result["transition"] == "UNKNOWN":
            raise ValueError("valid sample with unknown transition")
        if result["world_status"] == "WORLD_PRESENT" and not result["world_ptr"]:
            raise ValueError("WORLD_PRESENT with zero world pointer")
        if result["world_status"] in ("NO_MANAGER", "NO_WORLD") and result["world_ptr"]:
            raise ValueError("world missing but world_ptr nonzero")
        if result["world_status"] == "NO_MANAGER" and result["manager_ptr"]:
            raise ValueError("NO_MANAGER with nonzero manager_ptr")
        if result["world_status"] == "NO_WORLD" and not result["manager_ptr"]:
            raise ValueError("NO_WORLD with zero manager_ptr")
        if result["transition"] in ("WORLD_APPEARED", "WORLD_REPLACED") and not result["world_ptr"]:
            raise ValueError("new world event with null world")
        if result["transition"] == "WORLD_DISAPPEARED" and result["world_ptr"]:
            raise ValueError("world disappeared with nonnull world")
    elif result["transition"] != "UNKNOWN":
        raise ValueError("invalid world pointer cannot establish a transition")
    if result["va_valid"] and result["largest_free_bytes"] > result["free_bytes"]:
        raise ValueError("largest free > total free bytes")
    return result


def summarize(directory: Path, max_pair_gap_ms: int = 3500) -> dict[str, object]:
    if max_pair_gap_ms < 1 or max_pair_gap_ms > 60_000:
        raise ValueError("--max-pair-gap-ms must be between 1 and 60000")
    source = directory / "havok_world_va_pressure.csv"
    if not source.is_file():
        raise FileNotFoundError(f"{source} not found; opt in to PhysicsWorldMonitor=1 and PhysicsVaCorrelation=1")

    groups: dict[tuple[str, int], list[dict[str, object]]] = {}
    malformed = 0
    with source.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or not set(COLUMNS).issubset(reader.fieldnames):
            raise ValueError("unrecognized Havok+VA CSV columns")
        for row in reader:
            if not row or not any(row.values()):
                continue
            try:
                sample = parse_row(row)
            except (KeyError, AttributeError, TypeError, ValueError):
                malformed += 1
                continue
            key = (str(sample["session_id"]), int(sample["process_id"]))
            groups.setdefault(key, []).append(sample)
    runs = []
    for (session_id, pid), samples in groups.items():
        good: list[dict[str, object]] = []
        invalid_va = invalid_world = 0
        gaps = 0
        prev: dict[str, object] | None = None
        events = []
        baseline: dict[str, object] | None = None
        most_recent_unload: dict[str, object] | None = None
        seen_world = False
        for sample in samples:
            if not sample["va_valid"] or not sample["world_valid"]:
                invalid_va += int(not sample["va_valid"])
                invalid_world += int(not sample["world_valid"])
                prev = None
                gaps += 1
                continue
            if good and int(sample["time_ms"]) <= int(good[-1]["time_ms"]):
                # GetTickCount wrapped, file was reordered, or another process
                # reused this exact session key. Do not fabricate a delta.
                prev = None
                gaps += 1
            good.append(sample)
            if not sample["world_ptr"] and not seen_world and baseline is None:
                baseline = sample
            if sample["world_ptr"]:
                seen_world = True
            if sample["transition"] == "WORLD_DISAPPEARED":
                most_recent_unload = sample
            if sample["transition"] in TRIGGERS:
                pair_ok = bool(prev) and (
                    0 < int(sample["time_ms"]) - int(prev["time_ms"]) <= max_pair_gap_ms
                )
                events.append({
                    "time_ms": sample["time_ms"],
                    "type": sample["transition"],
                    "pair_valid": pair_ok,
                    "previous_time_ms": prev["time_ms"] if pair_ok else None,
                    "process_committed_delta_bytes": (
                        int(sample["committed_bytes"]) - int(prev["committed_bytes"])
                        if pair_ok else None
                    ),
                    "largest_free_delta_bytes": (
                        int(sample["largest_free_bytes"]) - int(prev["largest_free_bytes"])
                        if pair_ok else None
                    ),
                })
            prev = sample
        if not good:
            runs.append({"session_id": session_id, "process_id": pid,
                         "valid_joint_samples": 0, "invalid_world_samples": invalid_world,
                         "invalid_va_samples": invalid_va, "continuity_gaps": gaps,
                         "events": []})
            continue
        peak = max(good, key=lambda s: int(s["committed_bytes"]))
        worst = min(good, key=lambda s: int(s["largest_free_bytes"]))
        world_samples = sum(bool(s["world_ptr"]) for s in good)
        runs.append({
            "session_id": session_id,
            "process_id": pid,
            "valid_joint_samples": len(good),
            "invalid_world_samples": invalid_world,
            "invalid_va_samples": invalid_va,
            "continuity_gaps": gaps,
            "first_time_ms": good[0]["time_ms"],
            "last_time_ms": good[-1]["time_ms"],
            "world_present_samples": world_samples,
            "world_absent_samples": len(good)-world_samples,
            "loading_with_world_samples": sum(bool(s["world_ptr"] and s["loading_flag"]) for s in good),
            "peak_process_committed_bytes": peak["committed_bytes"],
            "peak_committed_time_ms": peak["time_ms"],
            "smallest_largest_free_bytes": worst["largest_free_bytes"],
            "smallest_largest_free_time_ms": worst["time_ms"],
            "pre_world_baseline_committed_bytes": baseline["committed_bytes"] if baseline else None,
            "last_observed_unload_committed_bytes": most_recent_unload["committed_bytes"] if most_recent_unload else None,
            "post_unload_vs_baseline_bytes": (
                int(most_recent_unload["committed_bytes"]) - int(baseline["committed_bytes"])
                if baseline and most_recent_unload else None
            ),
            "events": events,
        })
    if not any(r["valid_joint_samples"] for r in runs):
        raise ValueError("no jointly valid world/VA samples; no memory comparison possible")
    return {
        "source": str(source),
        "session_count": len(runs),
        "malformed_rows": malformed,
        "runs": runs,
        "notes": [
            "All memory metrics are VirtualQuery process-WIDE committed/reserved/free VA, NOT Havok heap allocation bytes.",
            "World transitions and memory values are sampled; slow scans can miss edges and cannot establish causation.",
            "Residual committed memory after unload is NOT sufficient evidence of a leak: caches and non-Havok subsystems also retain memory.",
            "Cross-launch comparisons MUST use session_id + PID; GetTickCount alone is not a valid join key.",
        ],
    }


def format_report(result: dict[str, object]) -> str:
    mb = lambda x: f"{x / MIB:,.1f} MiB"
    lines = [f"Havok world / x86 process VA joint samples: sessions={result['session_count']}; malformed={result['malformed_rows']}"]
    for run in result["runs"]:
        lines.append(
            f"Session {run['session_id']} PID={run['process_id']}: valid={run['valid_joint_samples']} "
            f"invalid world={run['invalid_world_samples']} invalid VA={run['invalid_va_samples']} "
            f"continuity breaks={run['continuity_gaps']}"
        )
        if not run["valid_joint_samples"]:
            continue
        lines.append(f"  World-present samples: {run['world_present_samples']}, absent: {run['world_absent_samples']}")
        lines.append(
            f"  Peak process committed: {mb(run['peak_process_committed_bytes'])}; "
            f"worst largest contiguous free: {mb(run['smallest_largest_free_bytes'])}"
        )
        if run["post_unload_vs_baseline_bytes"] is not None:
            lines.append(
                f"  Process VA committed after sampled unload vs initial no-world baseline: "
                f"{mb(run['post_unload_vs_baseline_bytes'])} (NOT a Havok leak estimate)"
            )
        for event in run["events"]:
            if event["pair_valid"]:
                lines.append(
                    f"  {event['type']} tick={event['time_ms']}: process committed "
                    f"delta={mb(event['process_committed_delta_bytes'])}, "
                    f"largest-free delta={mb(event['largest_free_delta_bytes'])}"
                )
            else:
                lines.append(f"  {event['type']} tick={event['time_ms']}: no adjacent trustworthy pair")
    lines.extend("NOTE: "+s for s in result["notes"])
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--max-pair-gap-ms", type=int, default=3500,
                        help="maximum adjacent sample interval to calculate a process VA delta")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = summarize(args.game_dir, args.max_pair_gap_ms)
    except (OSError, csv.Error, ValueError) as exc:
        print(f"Havok+VA correlation unavailable: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2) if args.json else format_report(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
