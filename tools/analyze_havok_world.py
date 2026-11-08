#!/usr/bin/env python3
"""Read-only triage for the retail Havok world lifecycle sample CSV.

The world pointer is NOT a count of hkpShapes, heap blocks, bodies, or bytes.
A one-second sample can miss a short world lifetime or a fast unload/reload.
Do not infer that Havok memory has been reclaimed because world_ptr is zero.
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

EXPECTED = (
    "time_ms", "process_id", "level_object", "level_valid", "manager_ptr",
    "manager_vtable", "thread_memory_ptr", "world_ptr", "loading_flag",
    "valid", "status", "transition", "observed_attaches",
    "observed_detaches", "observed_replacements",
)
TRANSITIONS = (
    "INITIAL_SAMPLE", "NONE", "UNKNOWN", "WORLD_APPEARED",
    "WORLD_DISAPPEARED", "WORLD_REPLACED",
)
STATUSES = (
    "NO_MANAGER", "NO_WORLD", "WORLD_PRESENT", "UNREADABLE_ROOT",
    "BAD_MANAGER_RANGE", "BAD_MANAGER_VTABLE",
    "UNREADABLE_MANAGER_FIELDS", "UNREADABLE_WORLD", "RACED",
)


def _decimal(row: dict[str, str], key: str) -> int:
    num = int(row[key].strip(), 10)
    if num < 0 or num > 0xFFFFFFFF:
        raise ValueError(f"{key} outside uint32 range")
    return num


def _hex32(row: dict[str, str], key: str) -> int:
    num = int(row[key].strip(), 16)
    if num < 0 or num > 0xFFFFFFFF:
        raise ValueError(f"{key} outside uint32 range")
    return num


def parse_row(row: dict[str, str]) -> dict[str, int | str]:
    if any(row.get(key) is None for key in EXPECTED):
        raise ValueError("missing or truncated CSV columns")
    result: dict[str, int | str] = {}
    for key in ("time_ms", "process_id", "level_valid", "loading_flag",
                "valid", "observed_attaches", "observed_detaches",
                "observed_replacements"):
        result[key] = _decimal(row, key)
    for key in ("level_object", "manager_ptr", "manager_vtable",
                "thread_memory_ptr", "world_ptr"):
        result[key] = _hex32(row, key)
    result["status"] = row["status"].strip()
    result["transition"] = row["transition"].strip()
    if result["status"] not in STATUSES or result["transition"] not in TRANSITIONS:
        raise ValueError("unsupported sample status/transition")
    if result["valid"] not in (0, 1) or result["level_valid"] not in (0, 1):
        raise ValueError("non-boolean sample validity")
    if result["loading_flag"] > 255:
        raise ValueError("loading flag outside byte range")
    if result["valid"] == 1:
        if result["transition"] == "UNKNOWN":
            raise ValueError("valid row cannot have unknown transition")
        if result["status"] == "NO_MANAGER":
            if result["manager_ptr"] or result["world_ptr"]:
                raise ValueError("NO_MANAGER with nonzero world/manager")
        elif result["status"] == "NO_WORLD":
            if result["manager_ptr"] == 0 or result["world_ptr"]:
                raise ValueError("NO_WORLD inconsistent pointers")
        elif result["status"] == "WORLD_PRESENT":
            if not result["manager_ptr"] or not result["world_ptr"]:
                raise ValueError("WORLD_PRESENT with null manager/world")
        else:
            raise ValueError("invalid status on valid row")
        if result["manager_ptr"] and result["manager_vtable"] != 0x009CAF94:
            raise ValueError("unverified physics manager vtable")
        if result["transition"] in ("WORLD_APPEARED", "WORLD_REPLACED") and not result["world_ptr"]:
            raise ValueError("world appearance requires nonzero world pointer")
        if result["transition"] == "WORLD_DISAPPEARED" and result["world_ptr"]:
            raise ValueError("world disappearance requires zero pointer")
    elif result["transition"] != "UNKNOWN":
        raise ValueError("invalid sample cannot claim lifecycle transition")
    return result


def summarize(directory: Path) -> dict[str, object]:
    path = directory / "havok_world_lifecycle.csv"
    if not path.is_file():
        raise FileNotFoundError(f"{path} not found; enable PhysicsWorldMonitor=1")
    valid: list[dict[str, int | str]] = []
    malformed = 0
    invalid_runtime = 0
    transition_counts = {k: 0 for k in TRANSITIONS}
    pids: set[int] = set()
    with path.open("r", encoding="utf-8-sig", newline="") as file:
        reader = csv.DictReader(file)
        if not reader.fieldnames or not set(EXPECTED).issubset(reader.fieldnames):
            raise ValueError("unsupported Havok world lifecycle CSV schema")
        for record in reader:
            if not any(record.values()):
                continue
            try:
                sample = parse_row(record)
            except (ValueError, KeyError, TypeError, AttributeError):
                malformed += 1
                continue
            pids.add(int(sample["process_id"]))
            if sample["valid"] != 1:
                invalid_runtime += 1
                continue
            valid.append(sample)
            transition_counts[str(sample["transition"])] += 1
    if not valid:
        raise ValueError("no valid world-pointer samples")
    last = valid[-1]
    present = sum(s["world_ptr"] != 0 for s in valid)
    level_unknown = sum(s["level_valid"] == 0 for s in valid)
    loading_world = sum(s["world_ptr"] != 0 and s["loading_flag"] != 0 for s in valid)
    notes = [
        "World pointer observations are not Havok allocator bytes, shape counts or an engine pool ceiling.",
        "One-second polling can miss fast creation, teardown and pointer reuse; counters are observed edges only.",
    ]
    if last["world_ptr"]:
        notes.append("A world was present at the last valid sample, NOT proof of a leak or failed unload.")
    if invalid_runtime or malformed:
        notes.append("Some samples were invalid; transitions around those samples cannot be reconstructed.")
    if level_unknown:
        notes.append("Some level_object reads failed; do not interpret their zero pointer as level absence.")
    return {
        "source": str(path),
        "valid_samples": len(valid),
        "invalid_runtime_samples": invalid_runtime,
        "malformed_samples": malformed,
        "distinct_process_ids": len(pids),
        "world_present_samples": present,
        "world_absent_samples": len(valid) - present,
        "loading_with_world_samples": loading_world,
        "unknown_level_samples": level_unknown,
        "observed_transitions": transition_counts,
        "last_valid_sample": last,
        "notes": notes,
    }


def format_report(result: dict[str, object]) -> str:
    last = result["last_valid_sample"]
    event = result["observed_transitions"]
    lines = [
        "D3D9Proxy: Havok world ownership (read-only sampled telemetry)",
        f"Valid samples: {result['valid_samples']}; invalid runtime: {result['invalid_runtime_samples']}; malformed: {result['malformed_samples']}",
        f"World pointer present / absent: {result['world_present_samples']} / {result['world_absent_samples']}",
        f"Sampled world attach/detach/replace edges: {event['WORLD_APPEARED']} / {event['WORLD_DISAPPEARED']} / {event['WORLD_REPLACED']}",
        f"Last valid: PID={last['process_id']} tick={last['time_ms']} manager=0x{last['manager_ptr']:08X} world=0x{last['world_ptr']:08X} loading={last['loading_flag']}",
    ]
    lines.extend("NOTE: " + note for note in result["notes"])
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = summarize(args.game_dir)
    except (OSError, ValueError, csv.Error) as exc:
        print(f"havok world analysis unavailable: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2) if args.json else format_report(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
