# D3D9Proxy

Standalone, public **x86 Direct3D 9 proxy** for *The Lord of the Rings: Conquest* (2009). This repository supersedes the original prototype and tracks the D3D9Proxy source formerly kept inside `Eriumsss/OpenConquest/D3D9Proxy/`.

**Development focus:** increase the retail game's **registry and memory pool ceilings** to support larger native levels, including a single Helm's Deep level containing static Isengard assets. The proxy is **not** a level importer, a second-world loader, or RTX Remix.

## Implemented experimental registry capacity patches

| Native registry | Retail cap | Opt-in patch |
| --- | ---: | ---: |
| ModelInfo | 1024 | 2048 |
| TextureInfo | 2048 | 4096 |
| EffectInfo | 512 | 1024 |

All three expansions are **OFF by default** and guarded by executable opcode fingerprints. The shared GFX block pool and PField capacities remain research targets, not implemented expansion patches. A successful DLL build or synthetic test does **not** prove an expanded map runs correctly in the game.

```ini
[StaticMapPools]
Monitor=1
ExpandModels2048=0
ExpandTextures4096=0
ExpandEffects1024=0
GeometryBudget=1
```

Other included modules: read-only pool census, static buffer budget telemetry, optional texture memory vault, native PAK/BIN preflight tools, and retail tracing components. The existing diagnostics and auxiliary modules have been retained from the working OpenConquest proxy rather than discarded during the split.

## Build

Requires **Windows**, the **MSVC x86 toolchain** and Windows SDK with D3D9 import libraries.

- Run `build.bat` from the repository root. It emits `out/d3d9.dll`.
- Or configure `cmake -S . -B out/cmake -A Win32` with a compatible Visual Studio generator, followed by `cmake --build out/cmake --config Release`.

On GitHub Actions the full proxy is compiled and linked with MSVC x86 and the read-only Python regression suite runs on Ubuntu. Follow the workflow results before deploying to a disposable game installation.

Do not replace the game's D3D9 DLL blindly if another graphics proxy is already installed. Experimental capacity patches are tied to an exact analyzed Conquest executable variant and will reject unsupported binaries.

## Offline validation

```sh
python -m unittest discover -s tests -p "test_*.py" -v
python tools/pak_pool_preflight.py --help
```

The PAK/BIN tooling is deliberately **read-only**. It checks reference integrity and capacities; it never imports a map or modifies a game asset.

### Repository migration

- Source snapshot: `Eriumsss/OpenConquest` commit `aa17f06a3539adc6fbd6d7d495884d25313cad38`, directory `D3D9Proxy/`.
- The previous standalone repository `main` is backed up at branch `backup/pre-openconquest-sync-20261108`, commit `2f7a067faae5189e795e2751ac01823251735e9b`.
- Old committed CMake build output and binaries were **not** copied into the new source tree.
- The original standalone repository's MIT `LICENSE` is retained.

`tools/build_retail_symbols.py` and `build_fn_trace.bat` may require an **external analysis corpus/ported engine tree** present in a local OpenConquest workspace. Those external source datasets are not vendored into this public standalone repository.

### Optional Magellan/NpcProxy oracle checks

Some static regression tests compare extracted x86 opcode contracts with the original Magellan C++ port or NpcProxy implementation. To keep this public project independent, those **external-oracle comparisons are skipped** when the private OpenConquest sources are absent; standalone source/patch and PAK tests still execute. To enable all original comparisons, set the `OPENCONQUEST_ROOT` environment variable to a local checkout of `Eriumsss/OpenConquest` (the directory containing `Vespucci/` and `DLL/`). No engine/oracle sources are bundled into this repository.
