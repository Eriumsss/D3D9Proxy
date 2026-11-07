// ═══════════════════════════════════════════════════════════════════════════
// trace_launch.exe — start the game with ConquestTrace.dll already inside it.
//
// The existing Injector.exe attaches to a RUNNING process, which means boot,
// engine init and the first level load are over before the tracer sees a
// single instruction — and those are exactly the windows worth tracing. This
// starts the exe SUSPENDED, injects, then resumes, so the very first traced
// call is whatever the game does first.
//
//   trace_launch.exe <game.exe> [ConquestTrace.dll] [-- game args...]
//
// The DLL defaults to ConquestTrace.dll beside the game exe. The tracer stays
// disarmed unless retail_trace.ini (written next to the DLL on first run) says
// Enabled=1, so launching through this is harmless on its own.
// ═══════════════════════════════════════════════════════════════════════════
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

static bool Inject(HANDLE proc, const char* dllPath) {
    size_t len = strlen(dllPath) + 1;
    void* remote = VirtualAllocEx(proc, 0, len, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) { printf("VirtualAllocEx failed (%lu)\n", GetLastError()); return false; }
    if (!WriteProcessMemory(proc, remote, dllPath, len, 0)) {
        printf("WriteProcessMemory failed (%lu)\n", GetLastError());
        return false;
    }
    // kernel32 sits at the same address in every process on a given boot, so
    // the local LoadLibraryA pointer is valid in the target.
    FARPROC loadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(proc, 0, 0, (LPTHREAD_START_ROUTINE)loadLib,
                                   remote, 0, 0);
    if (!th) { printf("CreateRemoteThread failed (%lu)\n", GetLastError()); return false; }
    WaitForSingleObject(th, 15000);
    DWORD loaded = 0;
    GetExitCodeThread(th, &loaded);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (!loaded) {
        printf("LoadLibraryA returned NULL in the target — is %s an x86 DLL "
               "and does it exist?\n", dllPath);
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("usage: trace_launch.exe <game.exe> [tracer.dll] [-- game args]\n");
        return 1;
    }
    char exePath[MAX_PATH];
    GetFullPathNameA(argv[1], MAX_PATH, exePath, 0);

    // Default the DLL to sit beside the game exe, which is also where the
    // tracer will look for retail_symbols.bin and drop its logs.
    char dllPath[MAX_PATH];
    if (argc > 2 && strcmp(argv[2], "--") != 0) {
        GetFullPathNameA(argv[2], MAX_PATH, dllPath, 0);
    } else {
        strcpy_s(dllPath, exePath);
        char* sl = strrchr(dllPath, '\\');
        if (sl) strcpy_s(sl + 1, MAX_PATH - (sl + 1 - dllPath), "ConquestTrace.dll");
    }
    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        printf("tracer DLL not found: %s\n", dllPath);
        return 1;
    }

    // The game resolves its data relative to the working directory, so run it
    // from its own folder no matter where this launcher was invoked.
    char workDir[MAX_PATH];
    strcpy_s(workDir, exePath);
    { char* sl = strrchr(workDir, '\\'); if (sl) *sl = 0; }

    char cmdline[4096];
    _snprintf_s(cmdline, sizeof(cmdline), _TRUNCATE, "\"%s\"", exePath);
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            for (int j = i + 1; j < argc; ++j) {
                strcat_s(cmdline, " ");
                strcat_s(cmdline, argv[j]);
            }
            break;
        }
    }

    STARTUPINFOA si = {0}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(0, cmdline, 0, 0, FALSE, CREATE_SUSPENDED, 0,
                        workDir, &si, &pi)) {
        printf("CreateProcess failed (%lu): %s\n", GetLastError(), cmdline);
        return 1;
    }
    printf("started suspended  pid=%lu  %s\n", pi.dwProcessId, exePath);
    printf("injecting          %s\n", dllPath);

    if (!Inject(pi.hProcess, dllPath)) {
        printf("injection failed — killing the suspended process\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 1;
    }
    ResumeThread(pi.hThread);
    printf("resumed. tracer logs land next to the DLL.\n");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
