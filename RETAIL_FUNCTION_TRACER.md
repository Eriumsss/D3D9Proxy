# Retail Function Tracer

Traps catalogued retail functions with `INT3` and logs every entry by name.
The F11 frame tracer sees the game from the D3D side — every draw, none of the
code behind it. This is the other half: it sees the code.

**32,456 entry points** are catalogued. **1,680** carry the semantic name a 1:1
port earned, plus the `file:line` we ported them at. **6,398** are named
overall (ported + the symbols the binary kept). **18,800** are bucketed by
subsystem from the corpus foldering.

A trapped call logs as an indented call tree — depth reconstructed from ESP, so
nesting is real, not guessed:

```
f001234  12.345s t0e6c  Mg_006a32c7_RunPassF  [MgDrainPass] (MgDrainPass.cpp:118)  <- Mg_006a0d29_Exec+0x2A
f001234  12.345s t0e6c    Mg_0068ec82_SortBucket  [MgDrainPass]  <- Mg_006a32c7_RunPassF+0x91
f001234  12.346s t0e6c    Mg_00689578_Run  [MgTonemapRuntime]  <- Mg_006a32c7_RunPassF+0x1C4
f001234  12.346s t0e6c      Mg_0068928b_BrightPass  [MgTonemapRuntime]  <- Mg_00689578_Run+0x40
f001234  12.346s t0e6c      Mg_006893ef_LumAdapt  [MgTonemapRuntime]  <- Mg_00689578_Run+0x77
```

frame · uptime · thread · **nesting** · who ran · **the file we ported it in** ·
**who called it, by name**

---

## Reading it: `retail_story.txt`

The raw log is the truth in time order, which is not yet an explanation. The
story report re-cuts the same data three ways:

1. **EXECUTION ORDER** — every function that ran, in order of first appearance.
   This is the boot / level-load / first-frame narrative.
2. **BY LANE** — grouped by the file we ported it in (`MgTonemapRuntime`,
   `MgProgramRegistry`, `MgAtmosphere`, `MgDrainPass`, `MgFrameConduct` …).
3. **BY TOPIC** — grouped by story, cutting across files.

★ Every group also lists what **NEVER RAN**. That is half the result: it is the
difference between "this path was armed and the game did not take it" — a
finding — and "I don't know whether I was even watching".

**Topics** (11): `conduct` `frame` `drain` `hdr` `exposure` `shader` `sky`
`cloud` `shadow` `spray` `teardown`.

---

## Running it

The tracer needs no D3D, so it does **not** own `d3d9.dll` — that name belongs
to the RTX Remix FFP wrapper, and replacing it kills the whole RTX path. Three
ways in, pick one:

| how | catches | when to use |
|---|---|---|
| `trace_launch.exe ConquestLLC.exe` | **boot onward** | level load, engine init — the interesting window |
| `Injector.exe` (existing) | from attach onward | game already running |
| D3D9Proxy `d3d9.dll` | boot onward | only when our proxy owns d3d9.dll (no Remix) |

Files must sit **together**: `ConquestTrace.dll`, `retail_symbols.bin`,
`retail_trace.ini`. Logs land beside them, never in the working directory.
Already deployed to `ZeroEngine\RE\` and the game folder.

## Presets — `trace_presets\`

Copy one over `retail_trace.ini`. The deployed default is `everything.ini`.

| preset | arms | mode |
|---|---|---|
| **everything.ini** *(default)* | all **6,398** functions we know by name | count |
| everything_all.ini | all **30,584** catalogued entry points | coverage |
| ported_only.ini | the **1,680** we decoded ourselves | count |
| render.ini | as `everything`, with render phases anchored | count |
| shaders.ini | just the bin-read → bind path | detail |
| teardown.ini | just the shutdown/free/release paths | count |

**Why the default is the widest thing that still runs well.** `Topics`/`Lanes`
narrow what gets *armed*, and **1,317 of the 1,680** functions we decoded carry
no topic at all — so a topic filter would hide 78% of what we know. Capture
everything; narrow when *reading*, in `retail_story.txt`.

## Config — `retail_trace.ini`

Nothing arms while `Enabled=0`. A missing ini is **written** with the safe
defaults, not assumed.

**`Mode`**
- `coverage` *(default)* — one shot per function. Traps once, disarms itself,
  costs nothing after. This is the "what code does this level / menu / spawn
  actually touch" tool.
- `count` — re-arms through the single-step trap: call counts and full call
  order. Anything hotter than `MaxHitsPerSec` is auto-retired and marked `HOT`
  so the frame rate survives.
- `detail` — `count` plus `ECX` (`this`), the return address and stack args.

**`Set`**
- `ported` *(default)* — the 1,680 functions we decoded and named
- `named` — + the 4,724 that kept a real symbol in the binary
- `bucketed` — + everything the corpus filed under a subsystem (~23k)
- `all` — every catalogued entry point

Narrow further with `Subsystems=Animation,EntitySystem,LevelLoader`, or pin
exact addresses with `Include=0x0068e383,0x006a578e`. `Exclude=` always wins.

**Hotkeys** (hold CTRL+ALT): `T` arm/disarm · `D` write coverage now · `X` panic-disarm.

## Output

| file | what |
|---|---|
| `retail_calls.log` | every trapped call, in time order, indented into a call tree |
| `retail_story.txt` | the same data as a readable sequence — execution order, by lane, by topic, plus what never ran |
| `retail_coverage.csv` | per function: hits, first frame, first ms, state, our port site |
| `retail_trace_status.txt` | live counters — armed / ran / hot / dropped |

## Nothing is ever lost silently

A trace with an unannounced hole is worse than no trace: you read the gap as
"it never ran". Every way a call can go missing announces itself.

- **Ring overflow** → a `!!!!!!` banner naming how many records were lost and
  what to raise.
- **Hot governor** → `!! HOT <name> retired after N calls in one second. Its
  later calls are NOT in this log.`
- **Interlock refusal** → the function is named at arm time with its live vs
  expected byte.
- **Never ran** → listed explicitly, per lane and per topic, in the story.

The one real limit, stated plainly: a function called tens of thousands of times
a second cannot be traced call-by-call through CPU exceptions. Those get counted
and then retired **loudly**, by name.

## Why INT3 and not a detour

A 5-byte relative JMP must relocate what it overwrites and must not land inside
a branch target. Getting that wrong on any one of 32,456 entry points is an
instant crash and there is no way to audit it at that scale. One `0xCC` needs
no relocation, no trampoline, no length disassembler, and is an atomic byte
store on x86 — safe to plant into a running multithreaded game.

The handler does **no** allocation, CRT, file I/O or locking: a trap can land
inside `malloc` or inside the CRT's own locks, so anything else would deadlock
the game rather than trace it. It writes a fixed record to a lock-free ring;
the worker thread formats it.

## The safety interlocks

1. **Image identity** — base, `.text` bounds and PE sanity are checked before a
   byte is touched. Wrong exe ⇒ refuses to arm.
2. **Oracle detection** — reports whether it attached to `ConquestEA.exe` or the
   patched `ConquestLLC.exe` (`[0x00401aad]` = `6A` vs `90`). The two differ in
   86 bytes; a finding is worthless without knowing which produced it.
3. **Baked entry bytes** — every function's real first byte is baked in from
   both images. If live memory disagrees, **something else already hooked that
   function** (`memorypatch.cpp` detours ~20 of them) and the tracer refuses,
   names it in the log, and leaves the other hook intact.
4. **Suspect entry points** — 1 of 32,456 has a first byte that cannot open a
   function (a likely off-by-one split). Skipped unless `ArmSuspect=1`.
5. **Hot governor** — runaway functions retire themselves instead of taking the
   frame rate down.

Measured over the real image: **0** catalogued entry points collide with
existing `0xCC` padding, and the first-byte histogram is clean prologues
(`55` push ebp 29%, `8B` mov 16%, `56` push esi 12%, `53`/`51` pushes, `83` sub
esp, `8D` lea, `B8` mov imm).

## Rebuilding

```
build_fn_trace.bat            build only
build_fn_trace.bat deploy     ...and copy into RE\ + the game folder
```

Rebuild the symbol table whenever the port tree gains names — a stale table is
worse than none, it puts the wrong name on the right address:

```
python tools\build_retail_symbols.py
```

Sources it folds together: the port tree's `Mg_<va>_<Name>` (+ definition
site), `function_index.csv` (catalogue + surviving real symbols),
`implemented/<Subsystem>/` and `analyzed/<Subsystem>/`, and the entry bytes from
both oracle exes.

## Proving it works without the game

```
out\trace_selftest.exe          exit code = failed checks
```

Builds three real x86 functions in an RWX page, points the tracer's own globals
at a synthetic table describing them, and runs the **shipped** arm / trap /
restore / re-arm / drain code over them — `RtfArmImpl`, `RtfVeh`, `DrainRing`
and `Selected` are the same functions the game runs, not re-implementations.
28 checks, currently all passing. The load-bearing ones are not "did we count a
hit":

- **"did the function still return the right value afterwards"** — proves the
  byte went back and the instruction re-executed rather than being skipped
- **"was a pre-patched entry point refused"** — proves interlock 3
- **"did a real nested call enter below its caller"** — the call tree rests on
  exactly this claim, so it is measured with an actual `call` instruction
  rather than argued for (`outer esp=006FF7AC, inner=006FF7A8`)
