#!/usr/bin/env python3
"""Static source/ABI contracts for read-only x86 D3D9 geometry telemetry.

Actual retail create callsites are confirmed against recovered ConquestEA.exe.
This test cannot prove a live GPU allocation or level load.
"""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]
CPP=(ROOT/"D3D9Proxy/src/geometry_budget_telemetry.cpp").read_text("utf-8")
H=(ROOT/"D3D9Proxy/src/geometry_budget_telemetry.h").read_text("utf-8")
DEV=(ROOT/"D3D9Proxy/src/d3d9device.cpp").read_text("utf-8")
PROXY=(ROOT/"D3D9Proxy/src/d3d9proxy.cpp").read_text("utf-8")
BUILD=(ROOT/"build_all.bat").read_text("utf-8")
MAGELLAN=(ROOT/"Vespucci/Engine/Magellan/MgGameBuffers.cpp").read_text("utf-8")

class StaticGeometryBudget(unittest.TestCase):
    def test_actual_retail_vtable_callsites_are_explicit(self):
        self.assertIn("kVBStaticCall=0x0068E0E9",CPP)
        self.assertIn("kIBStaticCall=0x0068DD74",CPP)
        self.assertIn("kVBReturn=0x0068E0EC",CPP)
        self.assertIn("kIBReturn=0x0068DD77",CPP)
        self.assertIn("0xFF,0x51,0x68",CPP)
        self.assertIn("0xFF,0x51,0x6C",CPP)
        self.assertIn("0068e0e9",MAGELLAN)
        self.assertIn("0068dd74",MAGELLAN)

    def test_passthrough_d3d9_retains_original_result_and_com_object(self):
        self.assertIn("HRESULT hr=m_pReal->CreateVertexBuffer(Len,Usage,FVF,Pool,ppVB,pHandle);",DEV)
        self.assertIn("HRESULT hr=m_pReal->CreateIndexBuffer(Len,Usage,Fmt,Pool,ppIB,pHandle);",DEV)
        self.assertIn("OC_GeometryBudget_VB(Len,Usage,Pool,hr,ppVB&&*ppVB!=nullptr,_ReturnAddress());",DEV)
        self.assertIn("OC_GeometryBudget_IB(Len,Usage,Pool,hr,ppIB&&*ppIB!=nullptr,_ReturnAddress());",DEV)
        self.assertNotIn("class ProxyIDirect3DVertexBuffer9",CPP)
        self.assertNotIn("class ProxyIDirect3DIndexBuffer9",CPP)
        self.assertNotIn("SetStreamSource(",CPP)
        self.assertNotIn("SetIndices(",CPP)

    def test_never_claims_live_residency(self):
        self.assertIn("Cumulative allocation attempts since game start",CPP)
        self.assertIn("NOT live GPU residency",H)
        self.assertIn("bytesOk",CPP)
        self.assertIn("bytesFailed",CPP)
        self.assertIn("managedBytes",CPP)
        self.assertIn("defaultBytes",CPP)
        self.assertIn("systemBytes",CPP)
        self.assertIn("staticBytesOk",CPP)
        self.assertIn("staticCallsFailed",CPP)

    def test_fail_closed_and_renderless_observation(self):
        self.assertIn('GetPrivateProfileIntA("StaticMapPools","GeometryBudget",0',CPP)
        self.assertIn("SafeMatch(kVBStaticCall",CPP)
        self.assertIn("SafeMatch(kIBStaticCall",CPP)
        self.assertIn("Sleep(1000)",CPP)
        self.assertIn("GlobalMemoryStatusEx(&st)",CPP)
        self.assertIn("static_geometry_budget.csv",CPP)
        self.assertIn("static_callsite_signature_ok",CPP)
        self.assertIn("InitOnceExecuteOnce(",CPP)
        self.assertIn("GET_MODULE_HANDLE_EX_FLAG_PIN",CPP)

    def test_does_not_mutate_pools_or_assets(self):
        for forbidden in ("VirtualProtect(", "WriteProcessMemory(",
                          "FlushInstructionCache(", "FUN_00876",
                          "NpcGame_", "PAK_WRITE", "CreateFileMapping"):
            self.assertNotIn(forbidden,CPP)
        self.assertIn("Record(g_vb",CPP)
        self.assertIn("Record(g_ib",CPP)

    def test_x86_compile_and_startup_links(self):
        self.assertIn("void OC_GeometryBudget_Init();",H)
        self.assertIn("OC_GeometryBudget_Init();",PROXY)
        self.assertIn('geometry_budget_telemetry.cpp',BUILD)
        self.assertIn('geometry_budget_telemetry.obj',BUILD)
        self.assertIn('"geometry_budget_telemetry.h"',DEV)
        self.assertIn('"geometry_budget_telemetry.h"',PROXY)

if __name__=="__main__":
    unittest.main()
