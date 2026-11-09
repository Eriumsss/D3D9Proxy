#!/usr/bin/env python3
"""Regression contracts for automatic proxy.ini creation and safe startup.

Linux Python verifies source/config invariants; Windows CI compiles/links the
real implementation with MSVC x86. Runtime startup still needs the game.
"""
import ast
import configparser
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[1]
CPP=(ROOT/"src/proxy_config.cpp").read_text(encoding="utf-8")
HDR=(ROOT/"src/proxy_config.h").read_text(encoding="utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text(encoding="utf-8")
BUILD=(ROOT/"build.bat").read_text(encoding="utf-8")
CMAKE=(ROOT/"CMakeLists.txt").read_text(encoding="utf-8")
WORKFLOW=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text(encoding="utf-8")
README=(ROOT/"README.md").read_text(encoding="utf-8")

def default_ini():
    match=re.search(r"static const char kDefaultIni\[\]\s*=\s*(.*?)\n\nstatic void Report",CPP,re.S)
    if not match:
        raise AssertionError("missing embedded default proxy.ini")
    tokens=re.findall(r'"(?:\\.|[^"\\])*"',match.group(1))
    if not tokens:
        raise AssertionError("default proxy.ini must not be empty")
    return "".join(ast.literal_eval(x) for x in tokens)


class ProxyIniAutocreate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ini=default_ini()
        cls.config=configparser.ConfigParser(interpolation=None,strict=True)
        cls.config.read_string(cls.ini)

    def test_generated_config_is_valid_crlf_ini(self):
        self.assertIn("\r\n[StaticMapPools]\r\n",self.ini)
        self.assertNotIn("\n",self.ini.replace("\r\n",""))
        self.assertTrue(self.ini.endswith("\r\n"))
        self.assertIn("StaticMapPools",self.config)

    def test_every_current_registry_and_device_reset_tier_is_present(self):
        keys=[
            "ExpandModels2048","ExpandModels4096","ExpandModels8192",
            "ExpandTextures4096","ExpandTextures8192","ExpandTextures16384",
            "ExpandEffects1024","ExpandEffects2048","ExpandEffects4096",
            "ExpandGfx256","ExpandGfx512","ExpandGfx1024",
            "ExpandResourceOther4096","ExpandResourceOther8192","ExpandResourceOther16384",
            "ExpandPFields8192","ExpandPFields16384",
            "ExpandDeviceResetResources1024","ExpandDeviceResetResources4096",
            "ExpandDynamicBufferReset1024","ExpandDynamicBufferReset4096",
            "ExpandAnimationEventPool4096"
        ]
        values=self.config["StaticMapPools"]
        for name in keys:
            with self.subTest(name=name):
                self.assertIn(name.lower(),values)
                self.assertEqual(values.getint(name),0)
        self.assertEqual(len(set(name.lower() for name in keys)),len(keys))

    def test_only_read_only_diagnostics_default_enabled(self):
        pool=self.config["StaticMapPools"]
        self.assertEqual(pool.getint("Monitor"),1)
        self.assertEqual(pool.getint("GeometryBudget"),1)
        self.assertEqual(pool.getint("BlendMonitor"),0)
        self.assertEqual(pool.getint("EventPoolMonitor"),0)
        self.assertEqual(pool.getint("PhysicsWorldMonitor"),0)
        self.assertEqual(pool.getint("PhysicsVaCorrelation"),0)
        self.assertEqual(self.config["Graphics"].getint("RenderScalePercent"),100)
        self.assertEqual(self.config["RetailHeapExtender"].getint("Enabled"),0)
        self.assertEqual(self.config["WorldResourceMemory"].getint("TextureVault"),0)
        self.assertEqual(self.config["WorldResourceMemory"].getint("Monitor"),0)
        self.assertNotIn("[Remix]",self.ini)  # don't change legacy chain-load preference

    def test_game_exe_directory_not_current_working_directory(self):
        self.assertIn("GetModuleFileNameA(NULL,folder,MAX_PATH)",CPP)
        self.assertIn("char* slash=strrchr(folder,'\\\\')",CPP)
        self.assertIn('"%sproxy.ini",folder',CPP)
        self.assertNotIn("GetCurrentDirectory",CPP)

    def test_no_rewrites_of_existing_user_configuration(self):
        self.assertIn("GetFileAttributesA(finalPath)",CPP)
        self.assertIn("if (existing!=INVALID_FILE_ATTRIBUTES)",CPP)
        self.assertIn("(existing&FILE_ATTRIBUTE_DIRECTORY)==0",CPP)
        self.assertNotIn("WritePrivateProfileStringA(",CPP)
        self.assertNotIn("CREATE_ALWAYS",CPP)
        self.assertNotIn("MOVEFILE_REPLACE_EXISTING",CPP[CPP.index("if (!MoveFileExA(temp,finalPath"):])

    def test_atomic_same_folder_creation_and_rename(self):
        self.assertIn('GetTempFileNameA(folder,"d9p",0,temp)',CPP)
        self.assertIn("TRUNCATE_EXISTING",CPP)
        self.assertIn("WriteFile(f,kDefaultIni,expected,&written,NULL)",CPP)
        self.assertIn("written==expected",CPP)
        self.assertIn("FlushFileBuffers(f)",CPP)
        self.assertIn("MoveFileExA(temp,finalPath,MOVEFILE_WRITE_THROUGH)",CPP)
        self.assertIn("DeleteFileA(temp)",CPP)

    def test_concurrent_creation_keeps_winning_user_ini(self):
        # If a different process creates the real path while temp is being
        # written, MoveFileEx fails and code returns success iff it is a file.
        self.assertIn("const DWORD now=GetFileAttributesA(finalPath)",CPP)
        self.assertIn("now!=INVALID_FILE_ATTRIBUTES",CPP)
        self.assertIn("!(now&FILE_ATTRIBUTE_DIRECTORY)",CPP)

    def test_create_runs_before_all_pool_config_reads(self):
        self.assertIn("bool OC_ProxyConfig_Ensure();",HDR)
        self.assertIn('#include "proxy_config.h"',PROXY)
        callback=PROXY[PROXY.index("static BOOL CALLBACK ProxyApplyPatchesOnce"):PROXY.index("static void EnsureProxyPatches()")]
        self.assertIn("OC_ProxyConfig_Ensure();",callback)
        self.assertLess(callback.index("OC_ProxyConfig_Ensure();"),
                        callback.index("OC_HeapExt_Init();"))
        self.assertLess(callback.index("OC_ProxyConfig_Ensure();"),
                        callback.index("OC_ModelRegistry2048_Init();"))
        self.assertLess(callback.index("OC_ProxyConfig_Ensure();"),
                        callback.index("OC_DeviceResetPool4096_Init();"))

    def test_config_creation_is_not_in_dllmain(self):
        start=PROXY.index("BOOL APIENTRY DllMain(")
        self.assertNotIn("OC_ProxyConfig_Ensure()",PROXY[start:])
        self.assertIn("EnsureProxyPatches();",PROXY[PROXY.index('extern "C" IDirect3D9* WINAPI Direct3DCreate9'):])
        self.assertIn("InitOnceExecuteOnce(&g_PatchesOnce",PROXY)

    def test_cmake_msbuild_and_ci_compile_file(self):
        self.assertIn("src/proxy_config.cpp",CMAKE)
        self.assertIn("proxy_config.cpp",BUILD)
        self.assertIn("proxy_config.obj",BUILD)
        self.assertIn("cl /nologo /W3 /EHsc /c /Fo:proxy_config.obj src\\proxy_config.cpp",WORKFLOW)
        self.assertIn("proxy_config.cpp static_pool_census.cpp",WORKFLOW)

    def test_ci_triggers_on_new_files(self):
        for path in ("src/proxy_config.cpp",
                     "src/proxy_config.h",
                     "tests/test_proxy_ini_autocreate.py"):
            self.assertEqual(WORKFLOW.count("'"+path+"'"),2)
        self.assertIn("test_proxy_ini_autocreate.py -v",WORKFLOW)

    def test_existing_user_settings_are_documented_as_immutable(self):
        self.assertIn("Existing",README)
        self.assertIn("never overwritten",README)
        self.assertIn("automatically",README)
        self.assertIn("EXE folder",README)
        self.assertIn("ExpandModels4096=1",README)


if __name__=="__main__":
    unittest.main()
