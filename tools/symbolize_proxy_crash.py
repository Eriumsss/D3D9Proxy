#!/usr/bin/env python3
"""Find nearest PUBLIC symbol for an RVA in the MAP from the SAME d3d9.dll."""
import argparse
import re
import sys
from pathlib import Path

BASE=re.compile(r"Preferred load address is\s+([0-9a-fA-F]+)",re.I)
LINE=re.compile(r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{8,16})\b")

def lookup(path, rva):
    if rva < 0: raise ValueError("negative RVA")
    image_base=None
    symbols=[]
    for line in path.read_text(encoding="utf-8-sig",errors="replace").splitlines():
        match=BASE.search(line)
        if match:
            image_base=int(match.group(1),16)
            continue
        match=LINE.match(line)
        if match and image_base is not None:
            segment,offset,name,address=match.groups()
            absolute=int(address,16)
            if absolute >= image_base and absolute-image_base <= 0xFFFFFFFF:
                symbols.append((absolute-image_base,name,segment+":"+offset))
    if image_base is None: raise ValueError("not an MSVC MAP (missing preferred base)")
    if not symbols: raise ValueError("no public symbols in MAP")
    before=max((s for s in symbols if s[0]<=rva),key=lambda s:s[0],default=None)
    after=min((s for s in symbols if s[0]>rva),key=lambda s:s[0],default=None)
    return image_base,before,after

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map",type=Path,required=True)
    parser.add_argument("--rva",type=lambda s:int(s,0))
    parser.add_argument("--eip",type=lambda s:int(s,0))
    parser.add_argument("--module-base",type=lambda s:int(s,0))
    a=parser.parse_args(argv)
    if a.rva is None and (a.eip is None or a.module_base is None):
        parser.error("use --rva OR --eip and --module-base")
    rva=a.rva if a.rva is not None else a.eip-a.module_base
    try:
        preferred,before,after=lookup(a.map,rva)
    except (ValueError,OSError) as e:
        print(f"Cannot resolve RVA: {e}",file=sys.stderr)
        return 2
    print(f"RVA=0x{rva:08X}; preferred image base=0x{preferred:08X}")
    if before:
        print(f"Nearest preceding PUBLIC: {before[1]} at 0x{before[0]:08X}, delta +0x{rva-before[0]:X}")
    else:
        print("No preceding PUBLIC")
    if after:
        print(f"Next PUBLIC: {after[1]} at 0x{after[0]:08X}")
    print("Nearest PUBLIC is NOT proof of the faulting function or exact instruction.")
    print("Load the matching PDB and crash dump in WinDbg for precise stack/source information.")
    return 0

if __name__=="__main__":
    sys.exit(main())
