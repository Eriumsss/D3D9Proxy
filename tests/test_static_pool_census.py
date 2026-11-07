#!/usr/bin/env python3
"""Static-map load census contract. Source checks only, not retail x86 runtime."""
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[2]
C=(ROOT/"D3D9Proxy/src/static_pool_census.cpp").read_text(encoding="utf-8")
H=(ROOT/"D3D9Proxy/src/static_pool_census.h").read_text(encoding="utf-8")
P=(ROOT/"D3D9Proxy/src/d3d9proxy.cpp").read_text(encoding="utf-8")
B=(ROOT/"build_all.bat").read_text(encoding="utf-8")
LEVEL=(ROOT/"Vespucci/Engine/Magellan/MgLevelObject.cpp").read_text(encoding="utf-8")
MODEL=(ROOT/"Vespucci/Engine/Magellan/MgModelRegistry.cpp").read_text(encoding="utf-8")
TEX=(ROOT/"Vespucci/Engine/Magellan/MgTextureRegistry.cpp").read_text(encoding="utf-8")
FX=(ROOT/"Vespucci/Engine/Magellan/MgEffectRegistry.cpp").read_text(encoding="utf-8")
GFX=(ROOT/"Vespucci/Engine/Magellan/MgLevelConstruct.cpp").read_text(encoding="utf-8")
GEOM=(ROOT/"Vespucci/Engine/Magellan/MgGameBuffers.h").read_text(encoding="utf-8")

class StaticMapCensus(unittest.TestCase):
    def test_known_registry_offsets_and_caps(self):
        names={
            "texture_a":(0x24,0x800),"texture_b":(0x2c,0x800),
            "model_a":(0x8c,0x400),"model_b":(0x94,0x400),
            "model_c":(0x9c,0x400),"resource_other":(0xf4,0x800),
            "effect_a":(0xfc,0x200),"effect_b":(0x104,0x200),
            "effect_c":(0x10c,0x200),"gfx_block":(0x11c,0x40)
        }
        for name,(offset,cap) in names.items():
            with self.subTest(name=name):
                self.assertRegex(C,rf'\{{"{name}",0x{offset:x},0x{cap:x}\}}')
                self.assertRegex(LEVEL,rf'Mg_00ea3a30_Reserve8\(VEC\(ebx, 0x{offset:x}\), 0x{cap:x}\)')
        self.assertIn('"pfield",0x114,0 /* unverified */', C)
        self.assertIn("Mg_Reserve12(VEC(ebx, 0x114), 0x200)",LEVEL)

    def test_factory_caps_are_real(self):
        self.assertIn("> 0x400",MODEL)
        self.assertIn("> 0x800",TEX)
        self.assertIn("> 0x200",FX)
        self.assertIn("> 0x40",GFX)
        self.assertIn("ModelInfo",MODEL)

    def test_dynamic_pool_not_static_geometry(self):
        self.assertIn("0x00CD80D4",C)
        self.assertIn("0x17FD00",C)
        self.assertIn("0x17FD08",C)
        self.assertIn("0x17FD0C",C)
        self.assertIn("Neither touches static model buffers.",GEOM)

    def test_readonly_guards(self):
        for needle in ("VirtualQuery(", "PAGE_GUARD","__except(EXCEPTION_EXECUTE_HANDLER)",
                       "count>capacity", "count<0", "kMaxReasonableCount",
                       "OC_StaticPoolCensus_Init", "kPollMs = 250",
                       '"StaticMapPools","Monitor",0'):
            with self.subTest(needle=needle):
                self.assertIn(needle,C)
        for forbidden in ("VirtualProtect(", "WriteProcessMemory(", "memcpy(", "*(volatile DWORD*)",
                          "DllMain(", "HeapLock(", "VirtualAlloc("):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden,C)
        self.assertIn("static_map_pools.csv", C)
        self.assertIn("GetModuleHandleA(NULL)",C)

    def test_wiring(self):
        self.assertIn("#include \"static_pool_census.h\"",P)
        self.assertIn("OC_StaticPoolCensus_Init();",P)
        self.assertIn("static_pool_census.cpp",B)
        self.assertIn("static_pool_census.obj",B)
        self.assertIn("void OC_StaticPoolCensus_Init();",H)

if __name__=="__main__":
    unittest.main()
