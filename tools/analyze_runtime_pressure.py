#!/usr/bin/env python3
"""Summarize in-game native VA/registry pressure CSVs without modifying files.

The debugger-facing data is sampled and version-specific. A patch setting
being ON is NOT evidence the patch installed, and a healthy address-space
summary does NOT prove a merged native PAK will load or unload.
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

MIB = 1024 * 1024

def rows(path: Path):
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        for line in reader:
            if line and any(value for value in line.values()):
                yield line

def number(row, key, radix=10):
    value = row.get(key)
    if value is None or value.strip() == "":
        raise ValueError(f"missing {key}")
    parsed = int(value.strip(), radix)
    if parsed < 0:
        raise ValueError(f"negative {key}")
    return parsed

def vm_sample(row):
    fields = ("time_ms", "free_bytes", "reserved_bytes",
              "committed_bytes", "largest_free_bytes",
              "free_regions", "reserved_regions", "committed_regions")
    obj = {key: number(row,key) for key in fields}
    obj["level_object"] = number(row,"level_object",16)
    obj["valid"] = number(row,"valid")
    obj["minimum_address"] = number(row,"minimum_address",16)
    obj["maximum_address"] = number(row,"maximum_address",16)
    if obj["valid"] != 1:
        raise ValueError("invalid partial virtual memory scan")
    if obj["largest_free_bytes"] > obj["free_bytes"]:
        raise ValueError("largest free region exceeds sum of free regions")
    if obj["maximum_address"] < obj["minimum_address"]:
        raise ValueError("inverted process address range")
    return obj

def summarize(directory: Path, threshold_mib: int = 128):
    if threshold_mib <= 0:
        raise ValueError("--warn-largest-free-mib must be positive")
    path = directory / "virtual_address_pressure.csv"
    if not path.is_file():
        raise FileNotFoundError(f"{path} does not exist; run matching x86 proxy with Monitor=1")

    valid = []
    bad = 0
    for row in rows(path):
        try:
            valid.append(vm_sample(row))
        except (ValueError, TypeError):
            bad += 1
    if not valid:
        raise ValueError("no valid virtual-address samples in " + str(path))

    latest = valid[-1]
    worst = min(valid,key=lambda x:x["largest_free_bytes"])
    peak_committed = max(x["committed_bytes"] for x in valid)
    pre_level = sum(1 for x in valid if x["level_object"] == 0)

    notes = []
    if latest["largest_free_bytes"] < threshold_mib * MIB:
        notes.append("Current largest contiguous free region is below the configured warning threshold")
    if worst["largest_free_bytes"] < threshold_mib * MIB:
        notes.append("At least one sample had a small largest contiguous free region; inspect the worst timestamp")
    if bad:
        notes.append("Some VM samples were partial or malformed and were ignored")
    if pre_level:
        notes.append("Pre-level VM samples exist; useful when an infinite load never constructs a level")
    result = {
        "source":str(path),
        "valid_samples":len(valid),
        "invalid_samples":bad,
        "pre_level_samples":pre_level,
        "threshold_mib":threshold_mib,
        "latest":latest,
        "worst_largest_free":worst,
        "peak_committed_bytes":peak_committed,
        "registry_peaks":{},
        "reset_list_peaks":{},
        "binding_peaks":{},
        "blend_pool":{},
        "event_pool":{},
        "notes":notes,
    }

    registry_file = directory / "static_map_pools.csv"
    if registry_file.is_file():
        for row in rows(registry_file):
            try:
                if number(row,"valid") != 1:
                    continue
                name = row.get("registry","").strip()
                if not name:
                    continue
                current = number(row,"entry_count")
                cap = number(row,"capacity")
                peak = number(row,"peak_count")
                if cap == 0 or current > cap or peak > cap:
                    continue
                previous = result["registry_peaks"].get(name)
                if previous is None or peak > previous["peak_count"]:
                    result["registry_peaks"][name] = {
                        "peak_count":peak,"observed_capacity":cap,
                        "utilization":round(peak/cap,4),
                    }
            except (ValueError,TypeError):
                continue
        for name, info in result["registry_peaks"].items():
            if info["utilization"] >= 0.9:
                result["notes"].append(
                    f"Registry {name} reached {info['utilization']:.0%} of its OBSERVED capacity"
                )
    pressure_file=directory / "runtime_pool_pressure.csv"
    if pressure_file.is_file():
        for row in rows(pressure_file):
            for name in ("device_reset","dynamic_vb","dynamic_ib"):
                try:
                    if number(row,name+"_valid") != 1:
                        continue
                    peak = number(row,name+"_peak")
                    result["reset_list_peaks"][name] = max(
                        peak,result["reset_list_peaks"].get(name,0)
                    )
                except (ValueError,TypeError):
                    continue
            try:
                if number(row,"bind_valid") == 1:
                    for metric in ("clip_peak","inner_pairs_peak"):
                        result["binding_peaks"][metric] = max(
                            number(row,metric),result["binding_peaks"].get(metric,0)
                        )
            except (ValueError,TypeError):
                continue
    blend_file=directory / "animation_blend_pool.csv"
    if blend_file.is_file():
        blend={"valid_samples":0,"invalid_samples":0,
               "peak_next_free_index":0,"retail_capacity":1024,
               "at_capacity_samples":0}
        for row in rows(blend_file):
            try:
                valid_sample=number(row,"valid")
                cap=number(row,"retail_capacity")
                index=number(row,"next_free_index")
                peak=number(row,"peak_next_free_index")
                if (valid_sample!=1 or cap!=1024 or index>cap
                        or peak>cap or peak<index):
                    blend["invalid_samples"]+=1
                    continue
                blend["valid_samples"]+=1
                blend["peak_next_free_index"]=max(blend["peak_next_free_index"],peak)
                if index==cap:
                    blend["at_capacity_samples"]+=1
            except (ValueError,TypeError):
                blend["invalid_samples"]+=1
        result["blend_pool"]=blend
        if blend["at_capacity_samples"]:
            notes.append("Fixed animation blend allocator reached its stock 1024-slot boundary in sampled data")
        elif blend["peak_next_free_index"]>=1008:
            notes.append("Fixed animation blend next-free index sampled near 1024 (NOT a live-object count)")
        if blend["invalid_samples"]:
            notes.append("Some blend samples were invalid or used an unverified retail guard")
    event_file=directory / "animation_event_pool.csv"
    if event_file.is_file():
        event={"valid_samples":0,"invalid_samples":0,
               "peak_used_count":0,"retail_capacity":1024,
               "at_stock_cap_samples":0,"unverified_guard_samples":0,
               "other_invalid_samples":0}
        for record in rows(event_file):
            try:
                status=record.get("status","").strip()
                valid=number(record,"valid")
                cap=number(record,"stock_capacity")
                used=number(record,"used_count")
                peak=number(record,"peak_used_count")
                if valid!=1 or cap!=1024 or used>cap or peak>cap or peak<used:
                    event["invalid_samples"]+=1
                    if status=="UNVERIFIED_RETAIL_GUARD":
                        event["unverified_guard_samples"]+=1
                    else:
                        event["other_invalid_samples"]+=1
                    continue
                if status not in ("OK","NEAR_STOCK_CAP","AT_STOCK_CAP","NO_POOL"):
                    event["invalid_samples"]+=1
                    event["other_invalid_samples"]+=1
                    continue
                if status=="NO_POOL" and used!=0:
                    event["invalid_samples"]+=1
                    event["other_invalid_samples"]+=1
                    continue
                if status=="AT_STOCK_CAP" and used!=cap:
                    event["invalid_samples"]+=1
                    event["other_invalid_samples"]+=1
                    continue
                event["valid_samples"]+=1
                event["peak_used_count"]=max(event["peak_used_count"],peak)
                if status=="AT_STOCK_CAP":
                    event["at_stock_cap_samples"]+=1
            except (ValueError,TypeError):
                event["invalid_samples"]+=1
                event["other_invalid_samples"]+=1
        result["event_pool"]=event
        if event["at_stock_cap_samples"]:
            notes.append("Native animation EVENT pool reached 1024 records; free-list reuse and allocation failures still require runtime evidence")
        elif event["peak_used_count"]>=1008:
            notes.append("Native animation EVENT pool observed near 1024 stock records; NOT the separate 0xF8 blend-slot pool")
        if event["unverified_guard_samples"]:
            notes.append("Native animation event guard did not match the stock retail opcode; capacity was NOT inferred")
    return result

def format_report(result):
    latest=result["latest"]
    worst=result["worst_largest_free"]
    mb=lambda x: f"{x/MIB:,.1f} MiB"
    lines=[
        "D3D9Proxy x86 native pool / virtual-address pressure",
        f"Valid VA samples: {result['valid_samples']}; invalid: {result['invalid_samples']}; pre-level: {result['pre_level_samples']}",
        f"Latest: committed={mb(latest['committed_bytes'])}, reserved={mb(latest['reserved_bytes'])}, free={mb(latest['free_bytes'])}",
        f"Latest largest contiguous free={mb(latest['largest_free_bytes'])}",
        f"Worst observed largest free={mb(worst['largest_free_bytes'])} at tick={worst['time_ms']}",
        f"Peak committed={mb(result['peak_committed_bytes'])}",
    ]
    for name, stats in sorted(result["registry_peaks"].items()):
        lines.append(f"Registry {name}: peak={stats['peak_count']} / observed cap={stats['observed_capacity']} ({stats['utilization']:.1%})")
    for name, count in sorted(result["reset_list_peaks"].items()):
        lines.append(f"Reset list {name}: sampled peak={count} (actual allocated cap unknown)")
    for name, count in sorted(result["binding_peaks"].items()):
        lines.append(f"Animation {name}: sampled peak={count}")
    if result["event_pool"]:
        ep=result["event_pool"]
        lines.append(f"Animation EVENT pool: stock=1024, sampled peak={ep['peak_used_count']}; "
                     f"at-cap samples={ep['at_stock_cap_samples']}, "
                     f"valid={ep['valid_samples']}, invalid={ep['invalid_samples']}")
    if result["blend_pool"]:
        p=result["blend_pool"]
        lines.append(f"Fixed animation blend: sampled next-free peak={p['peak_next_free_index']} / stock cap={p['retail_capacity']}; valid={p['valid_samples']}; invalid={p['invalid_samples']}")
    for note in result["notes"]:
        lines.append("NOTE: "+note)
    lines.append("Counts are samples, not proof of patch activation, valid imported assets or in-game stability.")
    return "\n".join(lines)

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir",type=Path,required=True,
                        help="game EXE directory containing native proxy CSV logs")
    parser.add_argument("--warn-largest-free-mib",type=int,default=128,
                        help="comparison threshold in MiB, NOT a validated engine hard limit")
    parser.add_argument("--json",action="store_true",help="print machine-readable summary")
    args=parser.parse_args(argv)
    try:
        result=summarize(args.game_dir,args.warn_largest_free_mib)
    except (OSError,ValueError) as exc:
        print(f"pressure analysis unavailable: {exc}",file=sys.stderr)
        return 2
    print(json.dumps(result,indent=2) if args.json else format_report(result))
    return 0

if __name__=="__main__":
    sys.exit(main())
