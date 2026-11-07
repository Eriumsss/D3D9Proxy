// conquest_hooks.cpp — shared hook module usable from any injection DLL.
// Compatible with: d3d9.dll (D3D9Proxy), version.dll (ConquestDebugger), ConquestConsole.
// Just #include and call InstallFileHooks() from your DllMain or Init function.
//
// Hooks installed (all via IAT patching on the game exe):
//   CreateFileA/W, ReadFile, SetFilePointer, CreateFileMappingA/W, CloseHandle
//   CreateMutexA/W (bypass single-instance check)
//
// Outputs conquest_fileio.log — answers:
//   "Does the game load everything into RAM at startup?"
//   "Can we swap assets at runtime?"
//   "When does each file get read?"

#include <windows.h>
#include <cstdio>
#include <cstring>

// Real function pointers (set by IAT hook)
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL   (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL   (WINAPI *CloseHandle_t)(HANDLE);
typedef DWORD  (WINAPI *SetFilePointer_t)(HANDLE, LONG, PLONG, DWORD);
typedef HANDLE (WINAPI *CreateFileMappingA_t)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
typedef HANDLE (WINAPI *CreateFileMappingW_t)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
typedef LPVOID (WINAPI *MapViewOfFile_t)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
typedef HANDLE (WINAPI *CreateMutexA_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
typedef HANDLE (WINAPI *CreateMutexW_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);

static CreateFileA_t        g_realCreateFileA        = NULL;
static CreateFileW_t        g_realCreateFileW        = NULL;
static ReadFile_t           g_realReadFile           = NULL;
static CloseHandle_t        g_realCloseHandle        = NULL;
static SetFilePointer_t     g_realSetFilePointer     = NULL;
static CreateFileMappingA_t g_realCreateFileMappingA = NULL;
static CreateFileMappingW_t g_realCreateFileMappingW = NULL;
static MapViewOfFile_t      g_realMapViewOfFile      = NULL;
static CreateMutexA_t       g_realCreateMutexA       = NULL;
static CreateMutexW_t       g_realCreateMutexW       = NULL;

static FILE* g_fileLog = NULL;
static CRITICAL_SECTION g_fileLogCS;
static bool g_fileLogInit = false;

// Track which handles belong to "interesting" files we want to log reads for
#include <map>
#include <string>
static std::map<HANDLE, std::string> g_trackedHandles;

static void EnsureLog() {
    if (!g_fileLogInit) {
        InitializeCriticalSection(&g_fileLogCS);
        g_fileLogInit = true;
    }
    if (!g_fileLog) {
        g_fileLog = fopen("conquest_fileio.log", "w");
        if (g_fileLog) {
            fprintf(g_fileLog, "=== Conquest File I/O Log ===\n");
            fprintf(g_fileLog, "Format: [TICK_MS] ACTION path size\n\n");
            fflush(g_fileLog);
        }
    }
}

static bool IsInteresting(const char* path) {
    // Log only game assets — skip system DLLs, registry, pipes
    if (!path) return false;
    // Skip Windows system paths
    if (strstr(path, "\\Windows\\") || strstr(path, "\\SYSTEM32\\") ||
        strstr(path, "\\system32\\") || strstr(path, "\\WinSxS\\")) return false;
    // Skip pipes and devices
    if (strncmp(path, "\\\\.\\", 4) == 0) return false;
    if (strncmp(path, "\\\\?\\", 4) == 0 && !strstr(path, ":\\")) return false;
    return true;
}

static const char* FileExt(const char* path) {
    const char* dot = strrchr(path, '.');
    return dot ? dot : "";
}

// Get the exe directory (cached, ends with a backslash)
static const char* GetExeDir() {
    static char dir[MAX_PATH] = {0};
    static bool cached = false;
    if (!cached) {
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        char* lastSlash = strrchr(dir, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        else dir[0] = '\0';
        cached = true;
    }
    return dir;
}

// Check if a modded version of this file exists — if yes, return its path.
// Layout: if game opens "X/levels/BlackGates.bin", we check
// "<EXE_DIR>/mods/levels/BlackGates.bin" and redirect there.
// Returns true if redirect happened; fills redirectPath (absolute).
static bool CheckModRedirect(const char* origPath, char* redirectPath, size_t maxLen) {
    if (!origPath || !redirectPath) return false;

    // Only redirect files we care about: .bin, .dat, .pak, .dds
    const char* ext = strrchr(origPath, '.');
    if (!ext) return false;
    bool isModdable = (_stricmp(ext, ".bin") == 0) || (_stricmp(ext, ".dat") == 0) ||
                      (_stricmp(ext, ".pak") == 0) || (_stricmp(ext, ".dds") == 0);
    if (!isModdable) return false;

    const char* exeDir = GetExeDir();
    if (!exeDir[0]) return false;

    // Extract filename
    const char* slash = strrchr(origPath, '\\');
    const char* fname = slash ? slash + 1 : origPath;

    // Extract parent folder name (e.g. "levels", "flash", "audio")
    char subpath[64] = "";
    if (slash) {
        const char* parentStart = slash;
        while (parentStart > origPath && *(parentStart-1) != '\\') parentStart--;
        size_t parentLen = slash - parentStart;
        if (parentLen > 0 && parentLen < sizeof(subpath)) {
            memcpy(subpath, parentStart, parentLen);
            subpath[parentLen] = '\0';
        }
    }

    // Try absolute: "<exeDir>mods\<subpath>\<fname>"
    if (subpath[0])
        _snprintf(redirectPath, maxLen, "%smods\\%s\\%s", exeDir, subpath, fname);
    else
        _snprintf(redirectPath, maxLen, "%smods\\%s", exeDir, fname);
    redirectPath[maxLen-1] = '\0';

    DWORD attr = GetFileAttributesA(redirectPath);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

    // Fallback: flat "<exeDir>mods\<fname>"
    _snprintf(redirectPath, maxLen, "%smods\\%s", exeDir, fname);
    redirectPath[maxLen-1] = '\0';
    attr = GetFileAttributesA(redirectPath);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

    return false;
}

// Hook function: CreateFileA
// Checks whether a path ends with `levels\.pak` or `levels\.bin` — i.e. the
// game asked to open a level with an empty filename. Returns the shell.pak
// equivalent that we should open instead.
static bool RewriteEmptyLevelPath(const char* in, char* out, size_t outSize) {
    if (!in || !out) return false;
    size_t len = strlen(in);
    const char* needlePak = "levels\\.pak";
    const char* needleBin = "levels\\.bin";
    size_t nlen = 11;   // same for both needles
    if (len < nlen) return false;
    const char* tail = in + (len - nlen);
    if (_stricmp(tail, needlePak) == 0) {
        size_t headLen = len - nlen;
        if (headLen + strlen("levels\\shell.pak") + 1 > outSize) return false;
        memcpy(out, in, headLen);
        strcpy_s(out + headLen, outSize - headLen, "levels\\shell.pak");
        return true;
    }
    if (_stricmp(tail, needleBin) == 0) {
        size_t headLen = len - nlen;
        if (headLen + strlen("levels\\shell.bin") + 1 > outSize) return false;
        memcpy(out, in, headLen);
        strcpy_s(out + headLen, outSize - headLen, "levels\\shell.bin");
        return true;
    }
    return false;
}

static HANDLE WINAPI Hooked_CreateFileA(
    LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSA, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplate)
{
    // Check for mod redirect (read access only)
    const char* actualPath = lpFileName;
    char redirectPath[MAX_PATH];
    bool redirected = false;
    if ((dwDesiredAccess & GENERIC_READ) && !(dwDesiredAccess & GENERIC_WRITE)) {
        // First: if the game is asking for an empty-named level (SP Stronghold
        // end-of-region with no more queued levels), redirect to shell.
        if (RewriteEmptyLevelPath(lpFileName, redirectPath, sizeof(redirectPath))) {
            actualPath = redirectPath;
            redirected = true;
        } else if (CheckModRedirect(lpFileName, redirectPath, sizeof(redirectPath))) {
            actualPath = redirectPath;
            redirected = true;
        }
    }

    HANDLE h = g_realCreateFileA(actualPath, dwDesiredAccess, dwShareMode, lpSA,
                                  dwCreationDisposition, dwFlagsAndAttributes, hTemplate);
    if (IsInteresting(lpFileName) && g_fileLog) {
        EnterCriticalSection(&g_fileLogCS);
        if (redirected) {
            fprintf(g_fileLog, "[%u] REDIRECT %s -> %s handle=%p result=%s\n",
                    GetTickCount(), lpFileName, redirectPath, h,
                    (h == INVALID_HANDLE_VALUE) ? "FAIL" : "OK");
        } else {
            fprintf(g_fileLog, "[%u] OPEN_A %s handle=%p access=0x%08X result=%s\n",
                    GetTickCount(), lpFileName, h, dwDesiredAccess,
                    (h == INVALID_HANDLE_VALUE) ? "FAIL" : "OK");
        }
        fflush(g_fileLog);
        if (h != INVALID_HANDLE_VALUE) g_trackedHandles[h] = lpFileName;
        LeaveCriticalSection(&g_fileLogCS);
    }
    return h;
}

// Hook function: CreateFileW
static HANDLE WINAPI Hooked_CreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSA, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplate)
{
    // Convert to ANSI and check for mod redirect
    char pathA[MAX_PATH] = {0};
    if (lpFileName)
        WideCharToMultiByte(CP_UTF8, 0, lpFileName, -1, pathA, MAX_PATH, NULL, NULL);

    char redirectPath[MAX_PATH];
    bool redirected = false;
    LPCWSTR actualPath = lpFileName;
    wchar_t redirectW[MAX_PATH];
    if ((dwDesiredAccess & GENERIC_READ) && !(dwDesiredAccess & GENERIC_WRITE) && pathA[0]) {
        if (CheckModRedirect(pathA, redirectPath, sizeof(redirectPath))) {
            MultiByteToWideChar(CP_UTF8, 0, redirectPath, -1, redirectW, MAX_PATH);
            actualPath = redirectW;
            redirected = true;
        }
    }

    HANDLE h = g_realCreateFileW(actualPath, dwDesiredAccess, dwShareMode, lpSA,
                                  dwCreationDisposition, dwFlagsAndAttributes, hTemplate);
    if (g_fileLog && pathA[0] && IsInteresting(pathA)) {
        EnterCriticalSection(&g_fileLogCS);
        if (redirected) {
            fprintf(g_fileLog, "[%u] REDIRECT_W %s -> %s handle=%p result=%s\n",
                    GetTickCount(), pathA, redirectPath, h,
                    (h == INVALID_HANDLE_VALUE) ? "FAIL" : "OK");
        } else {
            fprintf(g_fileLog, "[%u] OPEN_W %s handle=%p access=0x%08X result=%s\n",
                    GetTickCount(), pathA, h, dwDesiredAccess,
                    (h == INVALID_HANDLE_VALUE) ? "FAIL" : "OK");
        }
        fflush(g_fileLog);
        if (h != INVALID_HANDLE_VALUE) g_trackedHandles[h] = pathA;
        LeaveCriticalSection(&g_fileLogCS);
    }
    return h;
}

// Hook function: ReadFile
static BOOL WINAPI Hooked_ReadFile(
    HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    BOOL r = g_realReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    if (g_fileLog) {
        EnterCriticalSection(&g_fileLogCS);
        std::map<HANDLE, std::string>::iterator it = g_trackedHandles.find(hFile);
        if (it != g_trackedHandles.end()) {
            DWORD bytes = lpNumberOfBytesRead ? *lpNumberOfBytesRead : 0;
            fprintf(g_fileLog, "[%u] READ  %s bytes=%u/%u\n",
                    GetTickCount(), it->second.c_str(), bytes, nNumberOfBytesToRead);
            fflush(g_fileLog);
        }
        LeaveCriticalSection(&g_fileLogCS);
    }
    return r;
}

// Hook: SetFilePointer (tracks seeks — PAK format needs seeks into table)
static DWORD WINAPI Hooked_SetFilePointer(HANDLE h, LONG lDist, PLONG lpDistHigh, DWORD dwMethod) {
    DWORD r = g_realSetFilePointer(h, lDist, lpDistHigh, dwMethod);
    if (g_fileLog) {
        EnterCriticalSection(&g_fileLogCS);
        std::map<HANDLE, std::string>::iterator it = g_trackedHandles.find(h);
        if (it != g_trackedHandles.end()) {
            const char* method = dwMethod == FILE_BEGIN ? "BEGIN" :
                                 dwMethod == FILE_CURRENT ? "CURRENT" :
                                 dwMethod == FILE_END ? "END" : "?";
            fprintf(g_fileLog, "[%u] SEEK  %s offset=%d(%s) newPos=%u\n",
                    GetTickCount(), it->second.c_str(), lDist, method, r);
            fflush(g_fileLog);
        }
        LeaveCriticalSection(&g_fileLogCS);
    }
    return r;
}

// Hook: CreateFileMappingA — detects memory-mapped files (the "load into RAM" pattern)
static HANDLE WINAPI Hooked_CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES sa, DWORD prot,
                                                DWORD sizeHi, DWORD sizeLo, LPCSTR name) {
    HANDLE m = g_realCreateFileMappingA(hFile, sa, prot, sizeHi, sizeLo, name);
    if (g_fileLog) {
        EnterCriticalSection(&g_fileLogCS);
        std::map<HANDLE, std::string>::iterator it = g_trackedHandles.find(hFile);
        if (it != g_trackedHandles.end()) {
            fprintf(g_fileLog, "[%u] MMAP_A %s size=%u mapping=%p\n",
                    GetTickCount(), it->second.c_str(), sizeLo, m);
            fflush(g_fileLog);
        }
        LeaveCriticalSection(&g_fileLogCS);
    }
    return m;
}

static HANDLE WINAPI Hooked_CreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES sa, DWORD prot,
                                                DWORD sizeHi, DWORD sizeLo, LPCWSTR name) {
    HANDLE m = g_realCreateFileMappingW(hFile, sa, prot, sizeHi, sizeLo, name);
    if (g_fileLog) {
        EnterCriticalSection(&g_fileLogCS);
        std::map<HANDLE, std::string>::iterator it = g_trackedHandles.find(hFile);
        if (it != g_trackedHandles.end()) {
            fprintf(g_fileLog, "[%u] MMAP_W %s size=%u mapping=%p\n",
                    GetTickCount(), it->second.c_str(), sizeLo, m);
            fflush(g_fileLog);
        }
        LeaveCriticalSection(&g_fileLogCS);
    }
    return m;
}

// Hook function: CloseHandle
static BOOL WINAPI Hooked_CloseHandle(HANDLE h) {
    if (g_fileLog) {
        EnterCriticalSection(&g_fileLogCS);
        std::map<HANDLE, std::string>::iterator it = g_trackedHandles.find(h);
        if (it != g_trackedHandles.end()) {
            fprintf(g_fileLog, "[%u] CLOSE %s\n", GetTickCount(), it->second.c_str());
            fflush(g_fileLog);
            g_trackedHandles.erase(it);
        }
        LeaveCriticalSection(&g_fileLogCS);
    }
    return g_realCloseHandle(h);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Mutex bypass — game uses a named mutex to prevent multiple instances.
//  These hooks log the mutex name and force SetLastError(0) so the game's
//  "already running" check always thinks it's the first instance.
// ═══════════════════════════════════════════════════════════════════════════
static HANDLE WINAPI Hooked_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL bInitialOwner, LPCSTR name) {
    HANDLE h = g_realCreateMutexA(sa, bInitialOwner, name);
    DWORD err = GetLastError();
    if (g_fileLog && name) {
        EnterCriticalSection(&g_fileLogCS);
        fprintf(g_fileLog, "[%u] MUTEX_A name=\"%s\" result=%p err=0x%08X%s\n",
                GetTickCount(), name, h, err,
                err == ERROR_ALREADY_EXISTS ? " [BYPASSED]" : "");
        fflush(g_fileLog);
        LeaveCriticalSection(&g_fileLogCS);
    }
    // Force "I'm the first instance" so the game proceeds
    if (err == ERROR_ALREADY_EXISTS) SetLastError(0);
    return h;
}

static HANDLE WINAPI Hooked_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL bInitialOwner, LPCWSTR name) {
    HANDLE h = g_realCreateMutexW(sa, bInitialOwner, name);
    DWORD err = GetLastError();
    if (g_fileLog && name) {
        char nameA[256];
        WideCharToMultiByte(CP_UTF8, 0, name, -1, nameA, sizeof(nameA), NULL, NULL);
        EnterCriticalSection(&g_fileLogCS);
        fprintf(g_fileLog, "[%u] MUTEX_W name=\"%s\" result=%p err=0x%08X%s\n",
                GetTickCount(), nameA, h, err,
                err == ERROR_ALREADY_EXISTS ? " [BYPASSED]" : "");
        fflush(g_fileLog);
        LeaveCriticalSection(&g_fileLogCS);
    }
    if (err == ERROR_ALREADY_EXISTS) SetLastError(0);
    return h;
}

// IAT patcher: find the import entry for (dllName, funcName) and replace the pointer
static bool PatchIAT(HMODULE hModule, const char* dllName, const char* funcName, void* newFunc, void** pOriginal) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hModule;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)hModule + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.Size || !imp.VirtualAddress) return false;

    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)hModule + imp.VirtualAddress);
    while (desc->Name) {
        const char* importedDll = (const char*)((BYTE*)hModule + desc->Name);
        if (_stricmp(importedDll, dllName) == 0) {
            IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)((BYTE*)hModule + desc->OriginalFirstThunk);
            IMAGE_THUNK_DATA* iatThunk = (IMAGE_THUNK_DATA*)((BYTE*)hModule + desc->FirstThunk);
            while (origThunk->u1.AddressOfData) {
                if (!(origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    IMAGE_IMPORT_BY_NAME* byName = (IMAGE_IMPORT_BY_NAME*)((BYTE*)hModule + origThunk->u1.AddressOfData);
                    if (strcmp((const char*)byName->Name, funcName) == 0) {
                        DWORD oldProtect;
                        if (VirtualProtect(&iatThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                            if (pOriginal) *pOriginal = (void*)iatThunk->u1.Function;
                            iatThunk->u1.Function = (ULONG_PTR)newFunc;
                            VirtualProtect(&iatThunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                            return true;
                        }
                    }
                }
                ++origThunk; ++iatThunk;
            }
        }
        ++desc;
    }
    return false;
}

// Patch a function across ALL imported DLLs — not just kernel32.
// Games may import from kernelbase.dll, api-ms-win-core-*, etc.
static int PatchIAT_AllDlls(HMODULE hModule, const char* funcName, void* newFunc) {
    int patched = 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hModule;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)hModule + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.Size || !imp.VirtualAddress) return 0;

    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)hModule + imp.VirtualAddress);
    while (desc->Name) {
        const char* dllName = (const char*)((BYTE*)hModule + desc->Name);
        IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)((BYTE*)hModule + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iatThunk = (IMAGE_THUNK_DATA*)((BYTE*)hModule + desc->FirstThunk);
        while (origThunk->u1.AddressOfData) {
            if (!(origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                IMAGE_IMPORT_BY_NAME* byName = (IMAGE_IMPORT_BY_NAME*)((BYTE*)hModule + origThunk->u1.AddressOfData);
                if (strcmp((const char*)byName->Name, funcName) == 0) {
                    DWORD oldProtect;
                    if (VirtualProtect(&iatThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                        iatThunk->u1.Function = (ULONG_PTR)newFunc;
                        VirtualProtect(&iatThunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                        patched++;
                        if (g_fileLog) fprintf(g_fileLog, "[IAT] patched %s in %s\n", funcName, dllName);
                    }
                }
            }
            ++origThunk; ++iatThunk;
        }
        ++desc;
    }
    return patched;
}

// Install hooks on the game exe
extern "C" void InstallFileHooks() {
    static bool installed = false;
    if (installed) return;  // idempotent — safe to call multiple times
    installed = true;

    EnsureLog();
    HMODULE hExe = GetModuleHandleA(NULL); // game's exe
    if (!hExe) return;

    // Get real function pointers from kernel32 first
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    g_realCreateFileA        = (CreateFileA_t)        GetProcAddress(kernel32, "CreateFileA");
    g_realCreateFileW        = (CreateFileW_t)        GetProcAddress(kernel32, "CreateFileW");
    g_realReadFile           = (ReadFile_t)           GetProcAddress(kernel32, "ReadFile");
    g_realCloseHandle        = (CloseHandle_t)        GetProcAddress(kernel32, "CloseHandle");
    g_realSetFilePointer     = (SetFilePointer_t)     GetProcAddress(kernel32, "SetFilePointer");
    g_realCreateFileMappingA = (CreateFileMappingA_t) GetProcAddress(kernel32, "CreateFileMappingA");
    g_realCreateFileMappingW = (CreateFileMappingW_t) GetProcAddress(kernel32, "CreateFileMappingW");
    g_realMapViewOfFile      = (MapViewOfFile_t)      GetProcAddress(kernel32, "MapViewOfFile");
    g_realCreateMutexA       = (CreateMutexA_t)       GetProcAddress(kernel32, "CreateMutexA");
    g_realCreateMutexW       = (CreateMutexW_t)       GetProcAddress(kernel32, "CreateMutexW");

    // Patch EVERY DLL in the exe's import table — not just kernel32.
    // Games on modern Windows may import from kernelbase.dll or api-ms-win-*.dll
    PatchIAT_AllDlls(hExe, "CreateFileA",        (void*)Hooked_CreateFileA);
    PatchIAT_AllDlls(hExe, "CreateFileW",        (void*)Hooked_CreateFileW);
    PatchIAT_AllDlls(hExe, "ReadFile",           (void*)Hooked_ReadFile);
    PatchIAT_AllDlls(hExe, "CloseHandle",        (void*)Hooked_CloseHandle);
    PatchIAT_AllDlls(hExe, "SetFilePointer",     (void*)Hooked_SetFilePointer);
    PatchIAT_AllDlls(hExe, "CreateFileMappingA", (void*)Hooked_CreateFileMappingA);
    PatchIAT_AllDlls(hExe, "CreateFileMappingW", (void*)Hooked_CreateFileMappingW);
    int mutexA = PatchIAT_AllDlls(hExe, "CreateMutexA", (void*)Hooked_CreateMutexA);
    int mutexW = PatchIAT_AllDlls(hExe, "CreateMutexW", (void*)Hooked_CreateMutexW);
    if (g_fileLog) fprintf(g_fileLog, "[IAT] CreateMutex patches: A=%d W=%d\n", mutexA, mutexW);

    if (g_fileLog) {
        fprintf(g_fileLog, "[%u] HOOKS INSTALLED on exe\n", GetTickCount());
        fflush(g_fileLog);
    }
}
