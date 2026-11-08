// D3D9Proxy: create a REAL proxy.ini the first time the game loads our DLL.
//
// Every registry plugin previously read "%EXE_DIR%\proxy.ini" with
// GetPrivateProfileIntA and silently accepted its baked-in defaults if the
// file didn't exist. That's not configuration, that's a disappearing
// instruction manual. Generate the fucking file once, safely, instead.
//
// This runs at first Direct3DCreate9/9Ex initialization, NEVER in DllMain.
// Write an adjacent temporary file and publish it using MoveFileEx WITHOUT
// MOVEFILE_REPLACE_EXISTING. Existing user files are NEVER truncated or
// rewritten, even if they omit newer keys. All x86 patch options start OFF;
// explicitly opt in only on the previously fingerprinted retail EXE.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "proxy_config.h"

namespace {
static const char kDefaultIni[] =
    "; OpenConquest D3D9Proxy: automatically created on first startup.\r\n"
    "; The file lives next to the game EXE, not beside a random working folder.\r\n"
    "; Native x86 registry/pool patches only run on a verified executable.\r\n"
    "; Set an individual Expand* option to 1 to attempt that patch.\r\n"
    "; Default OFF for experimental patches. Existing files are never edited.\r\n"
    "\r\n"
    "[StaticMapPools]\r\n"
    "; Read-only diagnostics\r\n"
    "Monitor=1\r\n"
    "GeometryBudget=1\r\n"
    "\r\n"
    "; Static native registries\r\n"
    "ExpandModels2048=0\r\n"
    "ExpandModels4096=0\r\n"
    "ExpandModels8192=0\r\n"
    "ExpandTextures4096=0\r\n"
    "ExpandTextures8192=0\r\n"
    "ExpandTextures16384=0\r\n"
    "ExpandEffects1024=0\r\n"
    "ExpandEffects2048=0\r\n"
    "ExpandEffects4096=0\r\n"
    "ExpandGfx256=0\r\n"
    "ExpandGfx512=0\r\n"
    "ExpandGfx1024=0\r\n"
    "; Animation clip and skeleton binding caches, not Havok\r\n"
    "ExpandAnimationBindingCache256=0\r\n"
    "ExpandAnimationBindingCache512=0\r\n"
    "ExpandResourceOther4096=0\r\n"
    "ExpandResourceOther8192=0\r\n"
    "ExpandResourceOther16384=0\r\n"
    "ExpandPFields8192=0\r\n"
    "ExpandPFields16384=0\r\n"
    "\r\n"
    "; Device-reset resource registration pools\r\n"
    "ExpandDeviceResetResources1024=0\r\n"
    "ExpandDeviceResetResources4096=0\r\n"
    "ExpandDynamicBufferReset1024=0\r\n"
    "ExpandDynamicBufferReset4096=0\r\n"
    "\r\n"
    "; Separate opt-in engine heap and world-resource facilities\r\n"
    "[RetailHeapExtender]\r\n"
    "Enabled=0\r\n"
    "\r\n"
    "[WorldResourceMemory]\r\n"
    "TextureVault=0\r\n"
    "Monitor=0\r\n";

static void Report(const char* status, DWORD code) {
    char message[192]={0};
    _snprintf_s(message,sizeof(message),_TRUNCATE,
                "D3D9Proxy proxy.ini: %s (Win32 %lu)\n",
                status,(unsigned long)code);
    OutputDebugStringA(message);
}
} // namespace

bool OC_ProxyConfig_Ensure() {
    char folder[MAX_PATH]={0};
    const DWORD length=GetModuleFileNameA(NULL,folder,MAX_PATH);
    if (!length || length>=MAX_PATH) {
        Report("game EXE path unavailable/truncated",GetLastError());
        return false;
    }
    char* slash=strrchr(folder,'\\');
    if (!slash) {
        Report("game EXE has no parent directory",ERROR_INVALID_NAME);
        return false;
    }
    slash[1]='\0';

    char finalPath[MAX_PATH]={0};
    if (_snprintf_s(finalPath,sizeof(finalPath),_TRUNCATE,
                    "%sproxy.ini",folder)<0) {
        Report("proxy.ini path would exceed MAX_PATH",ERROR_BUFFER_OVERFLOW);
        return false;
    }

    const DWORD existing=GetFileAttributesA(finalPath);
    if (existing!=INVALID_FILE_ATTRIBUTES) {
        // A directory called proxy.ini is invalid; otherwise preserve the
        // user's file byte-for-byte, including custom comments and choices.
        return (existing&FILE_ATTRIBUTE_DIRECTORY)==0;
    }

    // GetTempFileNameA creates a unique empty file IN THE SAME DIRECTORY.
    // Same-volume rename is atomic; nobody can read a half-written config.
    char temp[MAX_PATH]={0};
    if (!GetTempFileNameA(folder,"d9p",0,temp)) {
        const DWORD error=GetLastError();
        Report("cannot make a temporary config in game directory",error);
        return false;
    }
    HANDLE f=CreateFileA(temp,GENERIC_WRITE,0,NULL,TRUNCATE_EXISTING,
                         FILE_ATTRIBUTE_NORMAL,NULL);
    if (f==INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();
        DeleteFileA(temp);
        Report("cannot open the temporary config file",error);
        return false;
    }

    const DWORD expected=(DWORD)(sizeof(kDefaultIni)-1);
    DWORD written=0;
    const bool success=WriteFile(f,kDefaultIni,expected,&written,NULL)!=FALSE
                    && written==expected && FlushFileBuffers(f)!=FALSE;
    const DWORD writeError=success?ERROR_SUCCESS:GetLastError();
    CloseHandle(f);
    if (!success) {
        DeleteFileA(temp);
        Report("cannot write the default config",writeError);
        return false;
    }

    // No MOVEFILE_REPLACE_EXISTING: never overwrite a real user INI, even
    // if another process created it between our attribute check and rename.
    if (!MoveFileExA(temp,finalPath,MOVEFILE_WRITE_THROUGH)) {
        const DWORD error=GetLastError();
        DeleteFileA(temp);
        // Another process may have completed creation in the meantime.
        const DWORD now=GetFileAttributesA(finalPath);
        if (now!=INVALID_FILE_ATTRIBUTES &&
            !(now&FILE_ATTRIBUTE_DIRECTORY))
            return true;
        Report("cannot publish proxy.ini in game directory",error);
        return false;
    }
    Report("created next to game EXE (experimental switches OFF)",ERROR_SUCCESS);
    return true;
}
