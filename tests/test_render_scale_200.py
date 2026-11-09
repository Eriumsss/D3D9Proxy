#!/usr/bin/env python3
"""D3D9 windowed backbuffer 200% SSAA contracts; not a retail gameplay test."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
HELP=(ROOT/"src/render_scale_200.h").read_text("utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text("utf-8")
DEVICE=(ROOT/"src/d3d9device.cpp").read_text("utf-8")
DEVICE_H=(ROOT/"src/d3d9device.h").read_text("utf-8")
CONFIG=(ROOT/"src/proxy_config.cpp").read_text("utf-8")
WF=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")
DOC=(ROOT/"RENDER_SCALE_200.md").read_text("utf-8")

class RenderScale200(unittest.TestCase):
    def test_opt_in_only_and_preserves_off_behavior(self):
        self.assertIn('"RenderScalePercent=100\\r\\n"',CONFIG)
        self.assertIn('"[Graphics]\\r\\n"',CONFIG)
        self.assertIn('GetPrivateProfileIntA("Graphics","RenderScalePercent",100,ini)==200',HELP)
        self.assertIn("OC_RS200_Requested()",PROXY)
        self.assertIn("OC_RS200_Requested()",DEVICE)
        self.assertIn("if (!rs200Active)",PROXY)
        self.assertIn("m_pReal->CreateDevice(Adapter,DeviceType,hFocusWindow,",PROXY)
        self.assertIn("BehaviorFlags,pParams,&pRealDevice)",PROXY)

    def test_double_each_axis_and_reject_unsafe_dimensions(self):
        for s in ("w*2", "h*2", "w>UINT_MAX/2", "h>UINT_MAX/2",
                  "caps.MaxTextureWidth", "caps.MaxTextureHeight",
                  "GetClientRect(window,&client)", "!original->Windowed",
                  "original->MultiSampleType!=D3DMULTISAMPLE_NONE"):
            self.assertIn(s,HELP)
        self.assertIn("scaled.BackBufferWidth=w*2;",HELP)
        self.assertIn("scaled.BackBufferHeight=h*2;",HELP)
        self.assertIn("scaled=*original;",HELP)
        self.assertIn("logicalW=w;logicalH=h;",HELP)

    def test_creation_uses_copy_and_falls_back(self):
        self.assertIn("#include \"render_scale_200.h\"",PROXY)
        self.assertIn("D3DPRESENT_PARAMETERS supersampled={};",PROXY)
        self.assertIn("OC_RS200_Prepare(pParams,hFocusWindow,caps,supersampled,",PROXY)
        self.assertIn("BehaviorFlags,&supersampled,&pRealDevice)",PROXY)
        self.assertIn('OC_RS200_Log("CREATE_ACTIVE"',PROXY)
        self.assertIn('OC_RS200_Log("CREATE_FALLBACK"',PROXY)
        self.assertIn("wrapped->ConfigureRenderScale200(rs200Active,logicalW,logicalH,hFocusWindow)",PROXY)
        self.assertIn("pParams->Windowed = TRUE",PROXY)
        self.assertIn("pParams->BackBufferFormat = D3DFMT_UNKNOWN",PROXY)

    def test_reset_revalidates_without_double_scaling(self):
        fn=DEVICE[DEVICE.index("HRESULT __stdcall ProxyIDirect3DDevice9::Reset("):
                  DEVICE.index("HRESULT __stdcall ProxyIDirect3DDevice9::Present(")]
        self.assertIn("OCV_BeforeReset(m_pReal);",fn)
        self.assertIn("OCV_AfterReset(m_pReal,hr);",fn)
        self.assertIn("OC_RS200_Prepare(pParams,m_renderWindow,caps,",fn)
        self.assertIn("pParams->BackBufferWidth==m_renderLogicalW*2",fn)
        self.assertIn("pParams->BackBufferHeight==m_renderLogicalH*2",fn)
        self.assertIn("hr=m_pReal->Reset(&scaled);",fn)
        self.assertIn("if (!active)",fn)
        self.assertIn("hr=m_pReal->Reset(pParams);",fn)
        self.assertIn("ConfigureRenderScale200(active,logicalW,logicalH,m_renderWindow)",fn)
        self.assertIn('OC_RS200_Log("RESET_FALLBACK"',fn)

    def test_does_not_scale_render_targets_shadows_hdr_or_blur(self):
        self.assertIn("bool ProxyIDirect3DDevice9::OnMainBackBuffer()",DEVICE)
        fn=DEVICE[DEVICE.index("bool ProxyIDirect3DDevice9::OnMainBackBuffer()"):
                  DEVICE.index("// IUnknown")]
        self.assertIn("GetRenderTarget(0,&current)",fn)
        self.assertIn("GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back)",fn)
        self.assertIn("current==back",fn)
        self.assertIn("current->Release()",fn)
        self.assertIn("back->Release()",fn)
        self.assertIn("bool OnMainBackBuffer();",DEVICE_H)
        self.assertIn("m_renderScale200",DEVICE_H)
        self.assertIn("m_renderLogicalW",DEVICE_H)

    def test_viewport_scissor_clear_and_present_bound_to_backbuffer(self):
        for snippet in (
            "OC_RS200_LogicalViewport(*pViewport",
            "OC_RS200_PhysicalViewport(*pViewport",
            "OC_RS200_LogicalRect(*pRect",
            "OC_RS200_LogicalRect(*pSrc",
            "return m_pReal->Present(src, pDst, hWnd, pDirtyRgn)",
            "return m_pReal->Clear(Count,transformed,Flags,Color,Z,Stencil)",
            "if (m_renderScale200 && pRects && Count>0 && Count<=16 && OnMainBackBuffer())",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet,DEVICE)
        self.assertIn("m_pReal->SetViewport(&physical)",DEVICE)
        self.assertIn("m_pReal->SetScissorRect(&physical)",DEVICE)
        self.assertIn("return m_pReal->SetRenderTarget(RTIndex, pRT)",DEVICE)

    def test_no_gpu_mode_switch_or_risky_renderer_shim(self):
        for snippet in ("ChangeDisplaySettings(", "SetDisplayConfig(",
                        "VirtualProtect(", "WriteProcessMemory(",
                        "D3DFMT_A32B32G32R32F", "D3DRESOURCETYPE"):
            self.assertNotIn(snippet,HELP)
        self.assertIn("render_scale_200.log",HELP)
        self.assertIn("D3D9",DOC)
        self.assertIn("NOT scene-only",DOC)
        self.assertIn("3840",DOC)
        self.assertIn("1920",DOC)

    def test_ci_includes_native_msbuild_and_python_regressions(self):
        self.assertEqual(WF.count("'tests/test_render_scale_200.py'"),2)
        self.assertIn("test_render_scale_200.py -v",WF)
        self.assertIn("src/d3d9device.cpp",WF)
        self.assertIn("d3d9proxy.cpp",WF)
        self.assertIn("RenderScalePercent=200",DOC)

if __name__=="__main__":
    unittest.main()
