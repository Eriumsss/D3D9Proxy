#!/usr/bin/env python3
# =============================================================================
# build_retail_symbols.py  —  bake everything we know about ConquestLLC.exe
#                             into one file the in-game tracer can mmap.
#
# WHY THIS EXISTS. We have 32,468 catalogued retail functions and, for 1,900+
# of them, a semantic name earned by a 1:1 port. That knowledge lives in five
# different places and none of it is reachable from inside the running game.
# This script folds all of it into `retail_symbols.bin`, which the D3D9Proxy
# tracer loads at startup so every trapped call prints as
#     Mg_0068e383_RingFrameRotate  [GameBuffers]  (MgGameBuffers.cpp:2228)
# instead of a bare address.
#
# SOURCES, in priority order (later sources never overwrite an earlier name):
#   1. PORT TREE      Scene3D/**/*.{cpp,h}     ->  Mg_<va>_<SemanticName>
#                     plus the file:line where we defined it = "what it does"
#   2. REAL SYMBOLS   function_index.csv       ->  mangled C++ names that
#                                                  survived in the binary
#                                                  (Scaleform GFx, CRT)
#   3. CATALOGUE      function_index.csv       ->  FUN_00xxxxxx + Ghidra category
#   4. SUBSYSTEM TAG  implemented/<Sub>/*.asm   ->  19,226 functions bucketed
#                     analyzed/<Sub>/*.asm      ->   1,134 functions bucketed
#
# OUTPUT  out/retail_symbols.bin   (see the struct comments below; the C++ side
#                                   is retail_fn_trace.cpp / RtfHeader)
# =============================================================================
import csv, os, re, struct, sys, collections

HERE     = os.path.dirname(os.path.abspath(__file__))
PROXY    = os.path.dirname(HERE)                       # .../DLL/D3D9Proxy
ENGINE   = os.path.dirname(os.path.dirname(PROXY))     # .../ZeroEngine/Engine
ZEROENG  = os.path.dirname(ENGINE)                     # .../ZeroEngine
CORPUS   = os.path.join(ZEROENG, "RE", "analysis", "disassembly_organized_v13c")
PORTTREE = os.path.join(ZEROENG, "Scene3D")
OUTDIR   = os.path.join(PROXY, "out")

TEXT_LO, TEXT_HI = 0x00401000, 0x00970000              # .text of the 2009 image
IMAGE_BASE       = 0x00400000

F_PORTED, F_ANALYZED, F_IMPL, F_REALNAME, F_THUNK = 1, 2, 4, 8, 16
F_SUSPECT = 32          # first byte does not look like a function prologue
F_UNSAFE  = 64          # ⛔ NEVER arm: part of exception dispatch itself


# ---------------------------------------------------------------------------
# ⛔ THE DENY LIST — functions the tracer must never instrument.
#
# Learned the hard way on 2026-08-13: the first live run armed __SEH_prolog4
# and __SEH_epilog4 and trapped them 2,043 times before the game died.
#
# THE INVARIANT: a breakpoint tracer runs INSIDE exception dispatch. Planting a
# breakpoint in the code that BUILDS and TEARS DOWN exception frames means a
# trap can fire while the thread is halfway through registering the handler
# that would service it — the exception nests inside its own delivery path.
# Windows tears the process down rather than unwind that.
#
# This is not a tuning knob and there is no ini key to override it.
# ---------------------------------------------------------------------------
NEVER_ARM = re.compile(
    r"^(__SEH_(prolog|epilog)|_+except_handler|__CxxFrameHandler|"
    r"_+local_unwind|_+global_unwind|_+NLG_|_+unwind|"
    r"_abnormal_termination|RtlUnwind|__CxxThrowException|"
    r"_CxxThrowException|__FrameUnwind|_+chkstk|_+alloca_probe)",
    re.I)


# ---------------------------------------------------------------------------
# LANES AND TOPICS — how you ask the tracer for a STORY instead of a firehose.
#
# The LANE is free and needs no judgement: it is the port file the function
# lives in. MgTonemapRuntime.cpp holds the tonemap chain, MgProgramRegistry.cpp
# holds the shader-bin reader, MgAtmosphere.cpp holds the dome and the clouds.
# Asking for `Lanes=MgTonemapRuntime` is asking for that subsystem exactly, with
# no address list to maintain.
#
# The TOPIC cuts across lanes, because a story does not respect file boundaries:
# the exposure chain starts in MgTonemapRuntime, reads state seeded by
# MgAtmosphere, and is driven from MgFrameConduct. Topics are matched on the
# function NAME and the lane, so they follow the meaning rather than the file.
# ---------------------------------------------------------------------------
T_TEARDOWN, T_HDR, T_EXPOSURE, T_DRAIN = 0x001, 0x002, 0x004, 0x008
T_SHADER, T_SKY, T_CLOUD, T_FRAME      = 0x010, 0x020, 0x040, 0x080
T_SPRAY, T_SHADOW, T_CONDUCT           = 0x100, 0x200, 0x400

TOPIC_NAMES = [
    ("teardown", T_TEARDOWN), ("hdr",    T_HDR),    ("exposure", T_EXPOSURE),
    ("drain",    T_DRAIN),    ("shader", T_SHADER), ("sky",      T_SKY),
    ("cloud",    T_CLOUD),    ("frame",  T_FRAME),  ("spray",    T_SPRAY),
    ("shadow",   T_SHADOW),   ("conduct", T_CONDUCT),
]

# (topic, name regex, lane regex) — either pattern hitting sets the bit.
TOPIC_RULES = [
    (T_TEARDOWN, r"teardown|shutdown|destroy|dtor|release|free|close|reset|"
                 r"unload|dispose|purge", r""),
    (T_HDR,      r"tonemap|hdr|bloom|brightpass|blur|adapt", r"MgTonemapRuntime"),
    (T_EXPOSURE, r"lum|adapt|expos|meter|bright|eye", r""),
    (T_DRAIN,    r"drain|bucket|sort|scratch|runpass", r"MgDrainPass"),
    (T_SHADER,   r"shader|bytecode|programregistry|vsby|psby|findps|findvs|"
                 r"findset|fillfrombin|createshaders|registry|material.*current|"
                 r"getprogram",
                 r"MgProgramRegistry|MgGameShaderCache"),
    (T_SKY,      r"sky|dome|atmo|scatter|haze|betamie|betaray", r"MgAtmosphere"),
    (T_CLOUD,    r"cloud", r""),
    (T_FRAME,    r"framebegin|frameseed|frameconduct|frameend|present|flip|"
                 r"framerotate|framebank|frameclock",
                 r"MgFrameSeed|MgFrameConduct|MgFrameClock"),
    (T_SPRAY,    r"spray", r"MgSprayRuntime"),
    # CONDUCT is retail's frame PROGRAM — the step list that decides what runs
    # in what order every frame. It is the spine the other stories hang off, so
    # it gets its own topic rather than being folded into `frame`.
    (T_CONDUCT,  r"conduct|step\d|lane\d|passslot|exec|seed|frameprogram",
                 r"MgFrameConduct|MgFrameSeed|MgExecColor"),
    # ⚠ NOT a bare `z` or `zero`: MgRenderScene's ZeroRow4 and MgPhysics'
    # Zero60 are memset helpers, nothing to do with the depth pass.
    (T_SHADOW,   r"shadow|cascade|zprepass|atlas", r"MgZPrepass"),
]
TOPIC_RULES = [(bit,
                re.compile(n, re.I) if n else None,
                re.compile(l, re.I) if l else None)
               for bit, n, l in TOPIC_RULES]


def classify_topics(name, lane):
    bits = 0
    for bit, npat, lpat in TOPIC_RULES:
        if npat and npat.search(name):
            bits |= bit
        elif lpat and lane and lpat.search(lane):
            bits |= bit
    return bits


def lane_of(note):
    """The port file a function was implemented in, e.g. 'MgTonemapRuntime'."""
    if not note:
        return ""
    n = note.lstrip("~").split(" | ")[0].split(":")[0]
    for ext in (".cpp", ".h", ".hpp", ".inl"):
        if n.endswith(ext):
            return n[:-len(ext)]
    return n


# ---------------------------------------------------------------------------
# ENTRY BYTES — the tracer's safety interlock.
#
# Two different things can make planting 0xCC at a catalogued address fatal:
#   1. Ghidra split a function in the wrong place, so the "entry point" is the
#      second byte of a real instruction.
#   2. SOMETHING ELSE ALREADY PATCHED IT. memorypatch.cpp detours ~20 retail
#      functions with 5-byte JMPs, and the community exes carry their own.
#
# Baking the expected first byte from the image on disk defeats (2) outright —
# the tracer compares before it writes and refuses on a mismatch. An opcode
# plausibility check flags (1) so those entries can be excluded by default.
# ---------------------------------------------------------------------------
# Every byte that legitimately opens a function in this binary, measured from
# the catalogue rather than assumed: prologue pushes, register moves, stack
# adjustment, immediate loads, SSE prefixes, jumps for tail-call thunks.
#
# ⚠ THE LIMIT OF THIS CHECK. It rejects bytes that cannot plausibly start a
# function, not addresses that are wrong. A mis-split whose second byte happens
# to be a valid opcode passes it. 0x00 (`add r/m8,r8`) is excluded precisely
# because it is a valid instruction that never opens a real function here — it
# is the signature of landing one byte late. The interlock that actually has
# teeth is the baked entry byte compared against live memory.
PLAUSIBLE_ENTRY = set([
    0x05, 0x09, 0x0A, 0x0B, 0x0D, 0x0F, 0x1B, 0x21, 0x23, 0x25, 0x2A,
    0x2B, 0x2D, 0x30, 0x31, 0x32, 0x33, 0x35, 0x38, 0x39, 0x3A, 0x3B, 0x3D,
    0x40, 0x48, 0x49, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5B, 0x5D, 0x5E, 0x5F, 0x64, 0x66, 0x68, 0x69, 0x6A, 0x6B, 0x72,
    0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7C, 0x7D, 0x7E, 0x7F, 0x80,
    0x81, 0x83, 0x84, 0x85, 0x88, 0x89, 0x8A, 0x8B, 0x8D, 0x8F, 0x90, 0x9B,
    0xA0, 0xA1, 0xA2, 0xA3, 0xA9, 0xB0, 0xB2, 0xB4, 0xB8, 0xB9, 0xBA, 0xBB,
    0xBD, 0xBE, 0xBF, 0xC1, 0xC2, 0xC3, 0xC6, 0xC7, 0xD1, 0xD8, 0xD9, 0xDB,
    0xDD, 0xE8, 0xE9, 0xEB, 0xF2, 0xF3, 0xF6, 0xF7, 0xF9, 0xFF,
])


class Image(object):
    """Just enough PE to read a byte at a virtual address."""

    def __init__(self, path):
        self.ok = os.path.isfile(path)
        if not self.ok:
            return
        with open(path, "rb") as fh:
            self.d = fh.read()
        e = struct.unpack_from("<I", self.d, 0x3C)[0]
        nsec = struct.unpack_from("<H", self.d, e + 6)[0]
        optsz = struct.unpack_from("<H", self.d, e + 20)[0]
        self.base = struct.unpack_from("<I", self.d, e + 24 + 28)[0]
        self.secs = []
        for i in range(nsec):
            o = e + 24 + optsz + i * 40
            vsz, va, rsz, ro = struct.unpack_from("<IIII", self.d, o + 8)
            self.secs.append((va, max(vsz, rsz), ro))

    def byte(self, va):
        if not self.ok:
            return None
        rva = va - self.base
        for sva, size, ro in self.secs:
            if sva <= rva < sva + size:
                off = ro + (rva - sva)
                if off < len(self.d):
                    return self.d[off]
        return None


# ---------------------------------------------------------------------------
# 1. THE PORT TREE — our own semantic names, and where we implemented them.
# ---------------------------------------------------------------------------
NAME_RE = re.compile(r"\bMg_(00[0-9a-fA-F]{6})_([A-Za-z0-9_]+)")
SRC_EXT = (".cpp", ".h", ".hpp", ".cc", ".inl")
SKIP_DIR = {".git", "obj", "out", "build", "imgui_temp", "__pycache__",
            "shadercache", "texdump", "deps", "NeuralTest"}


TRAIL_COMMENT = re.compile(r"\s*//.*$")


def is_definition_head(line, name_start, name_end):
    """True when this line opens a function BODY, not a call or a prototype.

    The distinction matters: `Mg_0068e383_RingFrameRotate(mgr);` inside
    MgFrameSeed is a CALL, and citing it as the implementation site sends the
    reader to the wrong file. A body head in this codebase is always at
    column 0, carries a return type before the name, and ends the line at the
    closing paren (or an opening brace) rather than a semicolon.
    """
    if line[:1] in ("", " ", "\t"):
        return False                       # bodies start at column 0
    before = line[:name_start].strip()
    if not before or before.endswith(("=", ",", "(", "&&", "||", "return")):
        return False                       # need a return type, not an operand
    tail = TRAIL_COMMENT.sub("", line[name_end:]).rstrip()
    if not tail.startswith("("):
        return False
    return tail.endswith(")") or tail.endswith("{")


def scan_port_tree(root):
    """VA -> (name, 'file.cpp:line')  preferring the definition site."""
    votes = collections.defaultdict(collections.Counter)   # va -> name -> hits
    defsite = collections.defaultdict(list)                # (va,name) -> [where]
    refsite = {}                                           # (va,name) -> where
    files = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIR]
        for fn in filenames:
            if not fn.endswith(SRC_EXT):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    lines = fh.readlines()
            except OSError:
                continue
            files += 1
            for lineno, line in enumerate(lines, 1):
                if "Mg_00" not in line:
                    continue
                stripped = line.lstrip()
                comment = stripped.startswith("//") or stripped.startswith("*")
                for m in NAME_RE.finditer(line):
                    va = int(m.group(1), 16)
                    if not (TEXT_LO <= va < TEXT_HI):
                        continue
                    name = "Mg_%s_%s" % (m.group(1).lower(), m.group(2))
                    votes[va][name] += 1
                    if comment:
                        continue
                    refsite.setdefault((va, name), "~%s:%d" % (fn, lineno))
                    if is_definition_head(line, m.start(), m.end()):
                        sites = defsite[(va, name)]
                        where = "%s:%d" % (fn, lineno)
                        if where not in sites:
                            sites.append(where)

    out = {}
    for va, counter in votes.items():
        # Prefer a name we actually defined; then the most-referenced; then the
        # longest (most specific) so `..._Release` loses to `..._ReleaseTeamMgr`.
        cand = sorted(counter.items(),
                      key=lambda kv: (bool(defsite.get((va, kv[0]))),
                                      kv[1], len(kv[0])),
                      reverse=True)
        name = cand[0][0]
        # Two definition sites for one VA is a DUPLICATE PORT TWIN — surface
        # both, the tracer prints them side by side.
        sites = defsite.get((va, name), [])
        # No body in the tree (header-only, or we only ever call it): fall back
        # to the first use, marked `~` so nobody mistakes it for the port site.
        note = " | ".join(sites[:2]) if sites else refsite.get((va, name), "")
        out[va] = (name, note)
    return out, files


# ---------------------------------------------------------------------------
# 2+3. THE GHIDRA CATALOGUE — every function the corpus knows about.
# ---------------------------------------------------------------------------
def scan_index(path):
    """VA -> (name, category, is_real_name, is_thunk)"""
    out = {}
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        for row in csv.DictReader(fh):
            try:
                va = int(row["EntryPoint"], 16)
            except (ValueError, KeyError, TypeError):
                continue
            if not (TEXT_LO <= va < TEXT_HI):
                continue
            name = (row.get("FunctionName") or "").strip()
            if not name:
                continue
            thunk = name.startswith("thunk_")
            real = not name.startswith("FUN_") and not thunk
            cat = (row.get("Category") or "").strip()
            out[va] = (name, cat if cat and cat != "Unknown" else "", real, thunk)
    return out


# ---------------------------------------------------------------------------
# 4. SUBSYSTEM BUCKETS — the corpus foldering is a free classification.
# ---------------------------------------------------------------------------
ASM_VA_RE = re.compile(r"_?(?:FUN_)?([0-9a-fA-F]{8})(?:_[0-9A-Fa-f]{8})?\.asm$")


def scan_buckets(root, flag, out):
    """Walk <root>/<Subsystem>/**.asm and tag each VA with its folder."""
    if not os.path.isdir(root):
        return 0
    n = 0
    for sub in sorted(os.listdir(root)):
        subdir = os.path.join(root, sub)
        if not os.path.isdir(subdir):
            continue
        for dirpath, _dirs, files in os.walk(subdir):
            for fn in files:
                if not fn.endswith(".asm"):
                    continue
                m = ASM_VA_RE.search(fn)
                if not m:
                    continue
                va = int(m.group(1), 16)
                if not (TEXT_LO <= va < TEXT_HI):
                    continue
                prev = out.get(va)
                # "Misc" is a dumping ground — never let it beat a real bucket.
                if prev and prev[0] != "Misc":
                    continue
                out[va] = (sub, flag)
                n += 1
    return n


# ---------------------------------------------------------------------------
# BAKE
# ---------------------------------------------------------------------------
def main():
    idx_path = os.path.join(CORPUS, "function_index.csv")
    if not os.path.isfile(idx_path):
        sys.exit("function_index.csv not found at %s" % idx_path)

    print("[1/6] port tree   ", PORTTREE)
    ported, nfiles = scan_port_tree(PORTTREE)
    print("      %d semantic names over %d source files "
          "(%d with a definition site)"
          % (len(ported), nfiles, sum(1 for v in ported.values() if v[1])))

    print("[2/6] catalogue   ", idx_path)
    catalogue = scan_index(idx_path)
    reals = sum(1 for v in catalogue.values() if v[2])
    print("      %d functions in .text (%d carry a real symbol name)"
          % (len(catalogue), reals))

    print("[3/6] subsystems  ", CORPUS)
    buckets = {}
    n_impl = scan_buckets(os.path.join(CORPUS, "implemented"), F_IMPL, buckets)
    n_anal = scan_buckets(os.path.join(CORPUS, "analyzed"), F_ANALYZED, buckets)
    print("      implemented/=%d  analyzed/=%d  -> %d tagged"
          % (n_impl, n_anal, len(buckets)))

    print("[4/6] entry bytes")
    ea  = Image(os.path.join(ZEROENG, "RE", "ConquestEA.exe"))
    llc = Image(os.path.join(ZEROENG, "RE", "ConquestLLC.exe"))
    if not ea.ok and not llc.ok:
        sys.exit("neither ConquestEA.exe nor ConquestLLC.exe found in RE\\ — "
                 "the tracer's safety interlock needs at least one")
    print("      EA=%s  LLC=%s" % ("ok" if ea.ok else "MISSING",
                                   "ok" if llc.ok else "MISSING"))

    # ---- merge -------------------------------------------------------------
    print("[5/6] merge")
    all_vas = set(catalogue) | set(ported) | set(buckets)
    subsys_ids, subsys_list = {"": 0}, [""]
    blob, blob_off = bytearray(b"\x00"), {"": 0}

    def intern(s):
        if s in blob_off:
            return blob_off[s]
        off = len(blob)
        blob.extend(s.encode("utf-8", "replace"))
        blob.append(0)
        blob_off[s] = off
        return off

    entries = []
    for va in sorted(all_vas):
        flags = 0
        name = note = ""

        cat = catalogue.get(va)
        if cat:
            name = cat[0]
            if cat[2]:
                flags |= F_REALNAME
            if cat[3]:
                flags |= F_THUNK

        p = ported.get(va)
        if p:                       # our own name always wins
            name, note = p[0], p[1]
            flags |= F_PORTED

        if not name:
            name = "FUN_%08x" % va

        b = buckets.get(va)
        if b:
            sub, bflag = b
            flags |= bflag
        elif cat and cat[1]:
            sub = cat[1]
        else:
            sub = ""
        if sub not in subsys_ids:
            subsys_ids[sub] = len(subsys_list)
            subsys_list.append(sub)

        bEA  = ea.byte(va)
        bLLC = llc.byte(va)
        # Where only one image is on hand, use it for both — the tracer will
        # still refuse to arm on a mismatch, it just cannot tell the two builds
        # apart at that address.
        if bEA is None:
            bEA = bLLC
        if bLLC is None:
            bLLC = bEA
        if bEA is None:
            continue                    # address is not in any section: drop it
        if bEA not in PLAUSIBLE_ENTRY or bLLC not in PLAUSIBLE_ENTRY:
            flags |= F_SUSPECT

        if NEVER_ARM.search(name):
            flags |= F_UNSAFE

        lane = lane_of(note)
        if lane not in subsys_ids:
            subsys_ids[lane] = len(subsys_list)
            subsys_list.append(lane)
        topics = classify_topics(name, lane)

        entries.append((va, intern(name),
                        intern(note) if note else 0,
                        subsys_ids[sub], subsys_ids[lane], flags, topics,
                        bEA, bLLC))

    # ---- write -------------------------------------------------------------
    os.makedirs(OUTDIR, exist_ok=True)
    out_path = os.path.join(OUTDIR, "retail_symbols.bin")

    subtab = bytearray()
    subtab += struct.pack("<I", len(subsys_list))
    for s in subsys_list:
        subtab += s.encode("utf-8", "replace") + b"\x00"

    HDR = 48
    entry_off = HDR
    entry_sz = len(entries) * 22
    subsys_off = entry_off + entry_sz
    blob_start = subsys_off + len(subtab)

    hdr = struct.pack("<4sIIIIIIIIII",
                      b"RTSY", 5, len(entries), IMAGE_BASE, TEXT_LO, TEXT_HI,
                      len(subsys_list), subsys_off, blob_start, len(blob),
                      entry_off)
    hdr += b"\x00" * (HDR - len(hdr))

    with open(out_path, "wb") as fh:
        fh.write(hdr)
        for va, noff, note, sub, lane, flags, topics, bEA, bLLC in entries:
            fh.write(struct.pack("<IIIHHHBBBB", va, noff, note, sub, lane,
                                 topics, flags, bEA, bLLC, 0))
        fh.write(subtab)
        fh.write(blob)

    print("[6/6] wrote", out_path)
    print("      %d entries | %d subsystems | name blob %d KB | file %d KB"
          % (len(entries), len(subsys_list), len(blob) // 1024,
             os.path.getsize(out_path) // 1024))
    unsafe = [e for e in entries if e[5] & F_UNSAFE]
    print("      NEVER-ARM (exception dispatch): %d" % len(unsafe))
    seen = set()
    for e in unsafe:
        nm = blob[e[1]:blob.index(b"\x00", e[1])].decode("utf-8", "replace")
        base = re.sub(r"\d+$", "", nm)
        if base not in seen:
            seen.add(base)
            print("        %s" % nm)
    print("      ported=%d  realname=%d  thunk=%d  bucketed=%d  suspect=%d"
          % (sum(1 for e in entries if e[5] & F_PORTED),
             sum(1 for e in entries if e[5] & F_REALNAME),
             sum(1 for e in entries if e[5] & F_THUNK),
             sum(1 for e in entries if e[5] & (F_IMPL | F_ANALYZED)),
             sum(1 for e in entries if e[5] & F_SUSPECT)))
    diff = sum(1 for e in entries if e[7] != e[8])
    print("      %d entry points differ between ConquestEA and ConquestLLC"
          % diff)
    print("      topics: " + "  ".join(
        "%s=%d" % (nm, sum(1 for e in entries if e[6] & bit))
        for nm, bit in TOPIC_NAMES))
    lanes = collections.Counter(subsys_list[e[4]] for e in entries
                                if subsys_list[e[4]])
    print("      %d lanes, biggest: %s" % (len(lanes), ", ".join(
        "%s(%d)" % (n, c) for n, c in lanes.most_common(6))))

    # A human-readable twin, so the table can be grepped outside the game.
    txt_path = os.path.join(OUTDIR, "retail_symbols.txt")
    with open(txt_path, "w", encoding="utf-8") as fh:
        fh.write("# va\tflags\ttopics\tsubsystem\tlane\tname\tported-at\n")
        for va, noff, note, sub, lane, flags, topics, bEA, bLLC in entries:
            nm = blob[noff:blob.index(b"\x00", noff)].decode("utf-8", "replace")
            nt = (blob[note:blob.index(b"\x00", note)].decode("utf-8", "replace")
                  if note else "")
            tn = "|".join(n for n, b in TOPIC_NAMES if topics & b)
            fh.write("0x%08x\t%d\t%s\t%s\t%s\t%s\t%s\n"
                     % (va, flags, tn, subsys_list[sub], subsys_list[lane],
                        nm, nt))
    print("      wrote", txt_path)


if __name__ == "__main__":
    main()
