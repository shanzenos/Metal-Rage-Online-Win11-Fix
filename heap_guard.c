/*
 * heap_guard.dll - FMallocWindows Thread-Safety Wrapper for Metal Rage
 * ====================================================================
 *
 * UE2's FMallocWindows is a pool allocator with ZERO thread synchronization.
 * y0da Protector's monitor threads suspend game threads mid-allocation,
 * corrupting pool linked lists and causing GPF crashes on Win11.
 *
 * This DLL wraps the FMalloc vtable methods (Malloc, Realloc, Free) with
 * a CRITICAL_SECTION, preventing heap corruption from concurrent access.
 * Windows CRITICAL_SECTION is recursive, so Realloc calling Malloc internally
 * won't deadlock.
 *
 * Build (mingw32): gcc -shared -o heap_guard.dll heap_guard.c -lkernel32 -m32
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

/* ---- Configuration ---- */

/* GMalloc is exported from Core.dll as: ?GMalloc@@3PAVFMalloc@@A
 * It's a pointer to the active FMalloc-derived allocator instance.
 * RVA in Core.dll: 0x001126B0 */
#define GMALLOC_EXPORT "?GMalloc@@3PAVFMalloc@@A"

/* UE2 FMalloc vtable layout (MSVC, no virtual destructor):
 *   [0] = Malloc(DWORD Count, const TCHAR* Tag) -> void*
 *   [1] = Realloc(void* Original, DWORD Count, const TCHAR* Tag) -> void*
 *   [2] = Free(void* Original) -> void
 *   [3] = DumpAllocs()
 *   [4] = HeapCheck()
 *   [5] = Init()
 *   [6] = Exit()
 *   [7+] = other methods
 */
#define VTABLE_IDX_MALLOC  0
#define VTABLE_IDX_REALLOC 1
#define VTABLE_IDX_FREE    2

/* Number of vtable entries to copy (generous upper bound) */
#define VTABLE_COPY_COUNT 16

/* ---- Types ---- */

/* FMalloc method signatures (__thiscall via __fastcall trick for GCC) */
typedef void* (__fastcall *FMalloc_Malloc_t)(void* thisptr, void* edx_unused,
                                              DWORD Count, const wchar_t* Tag);
typedef void* (__fastcall *FMalloc_Realloc_t)(void* thisptr, void* edx_unused,
                                               void* Original, DWORD Count, const wchar_t* Tag);
typedef void  (__fastcall *FMalloc_Free_t)(void* thisptr, void* edx_unused,
                                            void* Original);

/* ---- Globals ---- */

static CRITICAL_SECTION g_heap_cs;
static int g_hooked = 0;

/* Original function pointers */
static FMalloc_Malloc_t  g_orig_malloc  = NULL;
static FMalloc_Realloc_t g_orig_realloc = NULL;
static FMalloc_Free_t    g_orig_free    = NULL;

/* Replacement vtable */
static void* g_new_vtable[VTABLE_COPY_COUNT];

/* GMalloc instance pointer */
static void** g_gmalloc_ptr = NULL;

/* Log file */
static HANDLE g_log = INVALID_HANDLE_VALUE;

static void log_msg(const char* msg) {
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_log, msg, (DWORD)lstrlenA(msg), &written, NULL);
        WriteFile(g_log, "\r\n", 2, &written, NULL);
        FlushFileBuffers(g_log);
    }
}

static void log_hex(const char* prefix, DWORD val) {
    char buf[128];
    wsprintfA(buf, "%s 0x%08X", prefix, val);
    log_msg(buf);
}

/* ---- Hook Functions ---- */

static void* __fastcall hooked_malloc(void* thisptr, void* edx_unused,
                                       DWORD Count, const wchar_t* Tag) {
    void* result;
    EnterCriticalSection(&g_heap_cs);
    result = g_orig_malloc(thisptr, edx_unused, Count, Tag);
    LeaveCriticalSection(&g_heap_cs);
    return result;
}

static void* __fastcall hooked_realloc(void* thisptr, void* edx_unused,
                                        void* Original, DWORD Count, const wchar_t* Tag) {
    void* result;
    EnterCriticalSection(&g_heap_cs);
    result = g_orig_realloc(thisptr, edx_unused, Original, Count, Tag);
    LeaveCriticalSection(&g_heap_cs);
    return result;
}

static void __fastcall hooked_free(void* thisptr, void* edx_unused, void* Original) {
    EnterCriticalSection(&g_heap_cs);
    g_orig_free(thisptr, edx_unused, Original);
    LeaveCriticalSection(&g_heap_cs);
}

/* ---- Hooking Logic ---- */

static int install_vtable_hook(void) {
    HMODULE hCore;
    void** gmalloc_global;  /* Address of the GMalloc global (pointer to pointer) */
    void*  fmalloc_inst;    /* The FMalloc instance */
    void** orig_vtable;     /* Original vtable pointer */
    DWORD  old_protect;
    int    i;
    char   buf[256];

    /* Find Core.dll */
    hCore = GetModuleHandleA("Core.dll");
    if (!hCore) {
        log_msg("[heap_guard] Core.dll not loaded yet");
        return 0;
    }
    log_hex("[heap_guard] Core.dll base:", (DWORD)hCore);

    /* Find GMalloc export */
    gmalloc_global = (void**)GetProcAddress(hCore, GMALLOC_EXPORT);
    if (!gmalloc_global) {
        log_msg("[heap_guard] GMalloc export not found!");
        return 0;
    }
    log_hex("[heap_guard] GMalloc global at:", (DWORD)gmalloc_global);

    /* Read the GMalloc pointer */
    fmalloc_inst = *gmalloc_global;
    if (!fmalloc_inst) {
        log_msg("[heap_guard] GMalloc is NULL (not initialized yet)");
        return 0;
    }
    log_hex("[heap_guard] FMalloc instance at:", (DWORD)fmalloc_inst);
    g_gmalloc_ptr = gmalloc_global;

    /* Read the vtable pointer (first DWORD of the instance) */
    orig_vtable = *(void***)fmalloc_inst;
    log_hex("[heap_guard] Original vtable at:", (DWORD)orig_vtable);

    /* Log the first few vtable entries for debugging */
    for (i = 0; i < 8; i++) {
        wsprintfA(buf, "[heap_guard]   vtable[%d] = 0x%08X", i, (DWORD)orig_vtable[i]);
        log_msg(buf);
    }

    /* Save original function pointers */
    g_orig_malloc  = (FMalloc_Malloc_t)orig_vtable[VTABLE_IDX_MALLOC];
    g_orig_realloc = (FMalloc_Realloc_t)orig_vtable[VTABLE_IDX_REALLOC];
    g_orig_free    = (FMalloc_Free_t)orig_vtable[VTABLE_IDX_FREE];

    log_hex("[heap_guard] Original Malloc:", (DWORD)g_orig_malloc);
    log_hex("[heap_guard] Original Realloc:", (DWORD)g_orig_realloc);
    log_hex("[heap_guard] Original Free:", (DWORD)g_orig_free);

    /* Check for all 3 in FMallocWindows */
    if (g_orig_malloc == NULL || g_orig_realloc == NULL || g_orig_free == NULL) {
        log_msg("[heap_guard] ERROR: NULL vtable entries!");
        return 0;
    }

    /* Build replacement vtable, copy all entries, override Malloc/Realloc/Free */
    for (i = 0; i < VTABLE_COPY_COUNT; i++) {
        g_new_vtable[i] = orig_vtable[i];
    }
    g_new_vtable[VTABLE_IDX_MALLOC]  = (void*)hooked_malloc;
    g_new_vtable[VTABLE_IDX_REALLOC] = (void*)hooked_realloc;
    g_new_vtable[VTABLE_IDX_FREE]    = (void*)hooked_free;

    /* Swap the vtable pointer on the instance */
    /* Make the instance's first DWORD writable */
    if (!VirtualProtect(fmalloc_inst, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        log_msg("[heap_guard] VirtualProtect failed on instance!");
        return 0;
    }
    *(void***)fmalloc_inst = g_new_vtable;
    VirtualProtect(fmalloc_inst, sizeof(void*), old_protect, &old_protect);

    log_msg("[heap_guard] === VTABLE HOOKED SUCCESSFULLY ===");
    log_msg("[heap_guard] FMallocWindows Malloc/Realloc/Free now wrapped with CRITICAL_SECTION");

    g_hooked = 1;
    return 1;
}

/* ---- Hook Thread ---- */

static DWORD WINAPI hook_thread(LPVOID param) {
    int attempts = 0;

    log_msg("[heap_guard] Hook thread started, waiting for Core.dll + GMalloc...");

    /* Poll until Core.dll is loaded and GMalloc is initialized */
    while (attempts < 300) {  /* 30 seconds max */
        if (install_vtable_hook()) {
            return 0;
        }
        Sleep(100);
        attempts++;
    }

    log_msg("[heap_guard] TIMEOUT: Could not hook after 30 seconds!");
    return 1;
}

/* ---- DLL Entry Point ---- */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    char log_path[MAX_PATH];

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);

        /* Initialize critical section with spin count for performance */
        if (!InitializeCriticalSectionAndSpinCount(&g_heap_cs, 4000)) {
            return FALSE;
        }

        /* Open log file next to the DLL */
        GetModuleFileNameA(hinstDLL, log_path, MAX_PATH);
        /* Replace .dll with .log */
        {
            char* dot = log_path;
            char* last_dot = NULL;
            while (*dot) {
                if (*dot == '.') last_dot = dot;
                dot++;
            }
            if (last_dot) {
                lstrcpyA(last_dot, ".log");
            } else {
                lstrcatA(log_path, ".log");
            }
        }
        g_log = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        log_msg("[heap_guard] DLL loaded - Metal Rage FMallocWindows Thread-Safety Wrapper");
        log_hex("[heap_guard] DLL base:", (DWORD)hinstDLL);

        /* Try to hook immediately (Core.dll might already be loaded) */
        if (!install_vtable_hook()) {
            /* Start background thread to wait for Core.dll */
            HANDLE hThread = CreateThread(NULL, 0, hook_thread, NULL, 0, NULL);
            if (hThread) {
                CloseHandle(hThread);
            }
        }
        break;

    case DLL_PROCESS_DETACH:
        if (g_hooked && g_gmalloc_ptr) {
            /* Restore original vtable (if instance is still alive) */
            void* fmalloc_inst = *g_gmalloc_ptr;
            if (fmalloc_inst) {
                void** orig_vtable_restore = g_new_vtable; /* Already contains originals for non-hooked entries */
                /* Original vtable ptr not saved, exit anyway*/
            }
        }
        if (g_log != INVALID_HANDLE_VALUE) {
            log_msg("[heap_guard] DLL unloading");
            CloseHandle(g_log);
        }
        DeleteCriticalSection(&g_heap_cs);
        break;
    }
    return TRUE;
}
