/*
 * wow64log.dll - WoW64 Syscall Throttle for Metal Rage Online
 *
 * Windows WoW64 automatically loads C:\Windows\System32\wow64log.dll
 * and calls Wow64LogSystemService() for every WoW64 syscall transition.
 *
 * Add a tiny yield to y0da's NtQueryInformationThread monitoring
 * loop (232,874 calls in 120 seconds). This replicates the timing benefit
 * that DynamoRIO's binary translation overhead provided -- enough to prevent
 * the FMallocWindows heap corruption race condition on Win11.
 *
 * Syscall numbers are the raw 'mov eax' values from the 32-bit ntdll stub:
 *   NtQueryInformationThread = 0x25 (37 decimal) - 55.5% of all calls
 *   NtSuspendThread          = 0x701CF (459215) - 98 calls total
 *   NtResumeThread           = 0x70052 (458834)
 *   NtQueryInformationProcess= 0x19 (25) - anti-debug checks
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -shared -O2 wow64log.c -o wow64log.dll
 *     -Wl,--kill-at -lkernel32
 */

#include <windows.h>

/* Syscall raw values from 32-bit WoW64 ntdll stubs */
#define SYS_NtQueryInformationThread  0x25     /* 37 decimal, 232K calls/120s */
#define SYS_NtSuspendThread           0x701CF  /* 459215, 98 calls total */
#define SYS_NtQueryInformationProcess 0x19     /* 25, includes anti-debug */

/* Counter for monitoring (visible to debuggers) */
static volatile LONG g_throttle_count = 0;
static volatile LONG g_total_calls = 0;

/* Flag to enable/disable throttling (set by checking process name) */
static volatile LONG g_active = 0;

/* Check if the current process is MetalRage.exe */
static int is_metalrage(void) {
    WCHAR path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (len == 0) return 0;

    /* Walk backwards to find the filename */
    WCHAR *name = path + len;
    while (name > path && *(name - 1) != L'\\' && *(name - 1) != L'/') name--;

    /* Case-insensitive compare */
    if (_wcsicmp(name, L"MetalRage.exe") == 0) return 1;
    if (_wcsicmp(name, L"metalrage.exe") == 0) return 1;
    return 0;
}

/*
 * Wow64LogInitialize - Called once by WoW64 during process initialization.
 * Return STATUS_SUCCESS (0) to indicate logging is enabled.
 */
/* Write a beacon file to verify the DLL was loaded */
static void write_beacon(const char *msg) {
    HANDLE hFile = CreateFileA("C:\\wow64log_beacon.txt",
                               GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, msg, (DWORD)lstrlenA(msg), &written, NULL);
        CloseHandle(hFile);
    }
}

/* Write stats on exit for logging */
static void write_stats(void) {
    char buf[512];
    wsprintfA(buf, "wow64log.dll stats:\r\n"
              "  active = %d\r\n"
              "  total_calls = %ld\r\n"
              "  throttle_count = %ld\r\n",
              (int)g_active, g_total_calls, g_throttle_count);
    HANDLE hFile = CreateFileA("C:\\wow64log_stats.txt",
                               GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, buf, (DWORD)lstrlenA(buf), &written, NULL);
        CloseHandle(hFile);
    }
}

__declspec(dllexport) LONG WINAPI Wow64LogInitialize(void) {
    write_beacon("Wow64LogInitialize called\r\n");
    if (is_metalrage()) {
        g_active = 1;
        write_beacon("Wow64LogInitialize: MetalRage detected, ACTIVE\r\n");
    } else {
        write_beacon("Wow64LogInitialize: not MetalRage, inactive\r\n");
    }
    return 0; /* STATUS_SUCCESS */
}

/*
 * Wow64LogSystemService - Called for every WoW64 syscall transition.
 *
 * ServiceNumber is the raw value from the 32-bit ntdll stub's 'mov eax'.
 * This runs in the 64-bit context of the WoW64 layer.
 */
__declspec(dllexport) LONG WINAPI Wow64LogSystemService(ULONG ServiceNumber) {
    if (!g_active) return 0;

    InterlockedIncrement(&g_total_calls);

    /*
     * Throttle NtQueryInformationThread - y0da's primary monitoring syscall.
     * Called ~1,940 times/second by 8 monitor threads.
     * Adding SwitchToThread() yields the CPU for ~0-15 microseconds.
     * Over 232K calls, this adds ~0.5-3.5 seconds total overhead,
     * replicating DynamoRIO's timing benefit.
     */
    if (ServiceNumber == SYS_NtQueryInformationThread) {
        SwitchToThread();
        InterlockedIncrement(&g_throttle_count);
    }

    return 0; /* STATUS_SUCCESS - continue with the syscall */
}

/*
 * Wow64LogMessageArgList - Logging callback, we don't need it.
 */
__declspec(dllexport) LONG WINAPI Wow64LogMessageArgList(
    UCHAR LogLevel, const char *Format, va_list ArgList) {
    return 0;
}

/*
 * Wow64LogTerminate - Cleanup callback.
 */
__declspec(dllexport) void WINAPI Wow64LogTerminate(void) {
    write_stats();
    g_active = 0;
}

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)hInstDLL;
    (void)lpReserved;
    if (fdwReason == DLL_PROCESS_DETACH) {
        write_stats();
        g_active = 0;
    }
    return TRUE;
}
