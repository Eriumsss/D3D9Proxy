#!/usr/bin/env python3
"""Static integration checks. These do not substitute for a Win32/Direct3D9 runtime test."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT/"D3D9Proxy/src/retail_texture_vault.cpp").read_text(encoding="utf-8")
HDR = (ROOT/"D3D9Proxy/src/retail_texture_vault.h").read_text(encoding="utf-8")
DEVICE = (ROOT/"D3D9Proxy/src/d3d9device.cpp").read_text(encoding="utf-8")
BUILD = (ROOT/"build_all.bat").read_text(encoding="utf-8")
NPC = (ROOT/"DLL/NpcProxy/src/gfx.cpp").read_text(encoding="utf-8")

class RetailVaultContract(unittest.TestCase):
    def test_source_oracle_kept(self):
        # These are implementation-critical texture COM paths copied from NpcProxy.
        for marker in ("class VaultTexture : public IDirect3DTexture9",
                       "VaultTexture::Release()",
                       "CreateFileMappingA(INVALID_HANDLE_VALUE",
                       "MapViewOfFile(m_section",
                       "UnmapViewOfFile(view)",
                       "D3DPOOL_SYSTEMMEM",
                       "D3DPOOL_DEFAULT",
                       "D3DPOOL_MANAGED",
                       "void Cool()",
                       "int MakeStage()",
                       "void Restore()",
                       "int GoNative()",
                       "m_keepHot", "m_notUp", "g_stageVault"):
            with self.subTest(marker=marker):
                self.assertIn(marker, SRC)
                self.assertIn(marker, NPC)

    def test_opt_in_fails_closed(self):
        self.assertIn('"WorldResourceMemory","TextureVault",0', SRC)
        self.assertIn('"WorldResourceMemory","Monitor",0', SRC)
        self.assertIn("RetailSignatureMatches()", SRC)
        self.assertIn("0x0068D690", SRC)
        self.assertIn("0x0068D69A", SRC)
        self.assertIn("if ((uintptr_t)returnAddress!=kTextureReturnAddress)", SRC)
        self.assertIn("if (byteEstimate>kMaxTextureBytes)", SRC)
        self.assertIn("unsigned __int64 byteEstimate", SRC)

    def test_device_hook_coverage(self):
        for marker in ("OCV_OnDeviceCreated(this,pReal)",
                       "OCV_OnDeviceReleased(m_pReal)",
                       "OCV_BeforeReset(m_pReal)",
                       "OCV_AfterReset(m_pReal,hr)",
                       "OCV_OnPresent(m_pReal)",
                       "OCV_CreateTexture(m_pReal",
                       "OCV_SetTexture(m_pReal",
                       "OCV_GetTexture(m_pReal",
                       "OCV_UpdateTexture(m_pReal"):
            with self.subTest(marker=marker):
                self.assertIn(marker, DEVICE)

    def test_normal_d3d9_not_remix(self):
        self.assertIn("D3DPOOL_DEFAULT", SRC)
        self.assertIn("D3DPOOL_MANAGED", SRC)
        self.assertNotIn("d3d9_remix", SRC)
        self.assertNotIn("NpcGame_", SRC)
        self.assertNotIn("Gfx_On", SRC)
        self.assertNotIn("kAaPsCode", SRC)
        self.assertNotIn("LodApply", SRC)
        self.assertNotIn("VirtualProtect(", SRC)
        self.assertNotIn("WriteProcessMemory(", SRC)

    def test_monitor_and_build(self):
        self.assertIn("world_resource_memory.csv", SRC)
        self.assertIn("approx_gpu_available_mib", SRC)
        self.assertIn("InitOnceExecuteOnce(", SRC)
        self.assertIn("retail_texture_vault.cpp", BUILD)
        self.assertIn("retail_texture_vault.obj", BUILD)
        self.assertIn('retail_texture_vault.h', DEVICE)

    def test_header_exposes_all_methods(self):
        for method in ("OnDeviceCreated", "OnDeviceReleased", "BeforeReset",
                       "AfterReset", "OnPresent", "CreateTexture",
                       "SetTexture", "GetTexture", "UpdateTexture", "ReadStats"):
            self.assertIn("OCV_"+method, HDR)
            self.assertIn("OCV_"+method, SRC)

if __name__=="__main__":
    unittest.main()
