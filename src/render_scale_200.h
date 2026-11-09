#pragma once
// Experimental full-backbuffer 200% supersampling for the D3D9 WINDOWED path.
// 200% means twice the width AND height (4x pixels). This is NOT an engine
// scene-only render-scale implementation: HDR/offscreen targets and HUD may
// not follow the larger swap chain. The feature is OFF by default.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

static inline bool OC_RS200_Requested() {
    char exe[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,exe,MAX_PATH);
    if (!n || n>=MAX_PATH) return false;
    char* slash=strrchr(exe,'\\');
    if (!slash) return false;
    slash[1]='\0';
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",exe)<0)
        return false;
    // Deliberately reject arbitrary values. 200 is 2x each dimension,
    // 100 is the untouched original; no hidden 300% VRAM explosion.
    return GetPrivateProfileIntA("Graphics","RenderScalePercent",100,ini)==200;
}

static inline void OC_RS200_Log(const char* stage,
                                UINT logicalW,UINT logicalH,
                                UINT physicalW,UINT physicalH,HRESULT hr) {
    char exe[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,exe,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* slash=strrchr(exe,'\\');
    if (!slash) return;
    slash[1]='\0';
    char logPath[MAX_PATH]={0};
    if (_snprintf_s(logPath,sizeof(logPath),_TRUNCATE,
                    "%srender_scale_200.log",exe)<0) return;
    FILE* f=NULL;
    if (fopen_s(&f,logPath,"ab") || !f) return;
    fprintf(f,"tick=%lu event=%s logical=%ux%u physical=%ux%u hr=0x%08lX\n",
            (unsigned long)GetTickCount(),stage,
            logicalW,logicalH,physicalW,physicalH,
            (unsigned long)(DWORD)hr);
    fclose(f);
}

static inline bool OC_RS200_Prepare(const D3DPRESENT_PARAMETERS* original,
                                    HWND focusWindow,const D3DCAPS9& caps,
                                    D3DPRESENT_PARAMETERS& scaled,
                                    UINT& logicalW,UINT& logicalH) {
    logicalW=logicalH=0;
    if (!original || !original->Windowed) return false;
    if (original->MultiSampleType!=D3DMULTISAMPLE_NONE) return false;
    UINT w=original->BackBufferWidth;
    UINT h=original->BackBufferHeight;
    if (!w || !h) {
        HWND window=original->hDeviceWindow?
                    original->hDeviceWindow:focusWindow;
        RECT client={0,0,0,0};
        if (!window || !GetClientRect(window,&client)) return false;
        if (!w && client.right>client.left)
            w=(UINT)(client.right-client.left);
        if (!h && client.bottom>client.top)
            h=(UINT)(client.bottom-client.top);
    }
    // Avoid zero-sized/absurd buffers, 32-bit integer overflows and resource
    // creation above the driver's native 2D texture limits. No EXE patch.
    if (w<320 || h<200 || w>UINT_MAX/2 || h>UINT_MAX/2 ||
        w*2>caps.MaxTextureWidth || h*2>caps.MaxTextureHeight ||
        !caps.MaxTextureWidth || !caps.MaxTextureHeight) return false;
    scaled=*original;
    scaled.BackBufferWidth=w*2;
    scaled.BackBufferHeight=h*2;
    logicalW=w;logicalH=h;
    return true;
}

static inline bool OC_RS200_LogicalViewport(const D3DVIEWPORT9& in,
                                             UINT logicalW,UINT logicalH,
                                             D3DVIEWPORT9& out) {
    if (!logicalW || !logicalH || !in.Width || !in.Height ||
        in.X>logicalW || in.Y>logicalH ||
        in.Width>logicalW-in.X || in.Height>logicalH-in.Y)
        return false;
    out=in;
    out.X=in.X*2; out.Y=in.Y*2;
    out.Width=in.Width*2; out.Height=in.Height*2;
    return true;
}

static inline bool OC_RS200_LogicalRect(const RECT& in,
                                         UINT logicalW,UINT logicalH,
                                         RECT& out) {
    if (!logicalW || !logicalH ||
        in.left<0 || in.top<0 ||
        in.right<in.left || in.bottom<in.top ||
        (UINT)in.right>logicalW || (UINT)in.bottom>logicalH)
        return false;
    out=in;
    out.left*=2;out.top*=2;out.right*=2;out.bottom*=2;
    return true;
}

static inline bool OC_RS200_PhysicalViewport(const D3DVIEWPORT9& in,
                                              UINT logicalW,UINT logicalH,
                                              D3DVIEWPORT9& out) {
    if (!logicalW || !logicalH || in.X>logicalW*2 ||
        in.Y>logicalH*2 || in.Width>logicalW*2-in.X ||
        in.Height>logicalH*2-in.Y ||
        ((in.X|in.Y|in.Width|in.Height)&1)) return false;
    out=in;
    out.X/=2;out.Y/=2;out.Width/=2;out.Height/=2;
    return true;
}
