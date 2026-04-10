/*
 * dxdiagn.dll - Minimal DxDiag Provider stub for Metal Rage Online
 *
 * Implements just enough of IDxDiagProvider and IDxDiagContainer COM interfaces
 * to satisfy D3D9Drv.dll's GetHWInfo function without hanging on Win11's WMI.
 *
 * The game calls:
 *   1. CoCreateInstance(CLSID_DxDiagProvider, ..., IID_IDxDiagProvider, &pProvider)
 *   2. pProvider->Initialize(&initParams)
 *   3. pProvider->GetRootContainer(&pRoot)
 *   4. pRoot->GetChildContainer(L"DxDiag_DisplayDevices", &pDisplay)
 *   5. pDisplay->GetNumberOfChildContainers(&count)
 *   6. pDisplay->EnumChildContainerNames(0, name, maxlen)
 *   7. pDisplay->GetChildContainer(name, &pAdapter)
 *   8. pAdapter->GetProp(L"szDescription", &variant)   -> GPU name
 *   9. pAdapter->GetProp(L"szDriverName", &variant)     -> driver DLL
 *  10. Various other properties
 *
 * Returns GPU info so the game's Init code has properly initialized data.
 *
 * Build (32-bit, MSVC):
 *   cl /nologo /O2 /LD dxdiagn_stub.c /link /DLL /OUT:dxdiagn.dll ole32.lib oleaut32.lib
 */

#include <windows.h>
#include <ole2.h>

/* GUIDs */
static const GUID CLSID_DxDiagProvider_local =
    {0xA65B8071, 0x3BFE, 0x4213, {0x9A, 0x5B, 0x49, 0x1D, 0xA4, 0x46, 0x1C, 0xA7}};
static const GUID IID_IDxDiagProvider_local =
    {0x9C6B4CB0, 0x23F8, 0x49CC, {0xA3, 0xED, 0x45, 0xA5, 0x50, 0x00, 0xA6, 0xD2}};
static const GUID IID_IDxDiagContainer_local =
    {0x7D0F462F, 0x4064, 0x4862, {0xBC, 0x7F, 0x93, 0x3E, 0x50, 0x58, 0xC1, 0x0F}};

/* Simple file logger */
static void dxlog(const char *msg) {
    HANDLE hFile = CreateFileA("D:\\dxdiagn_stub.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, msg, lstrlenA(msg), &written, NULL);
        WriteFile(hFile, "\r\n", 2, &written, NULL);
        CloseHandle(hFile);
    }
}

/* Forward declarations */
typedef struct DxDiagContainer DxDiagContainer;
typedef struct DxDiagProvider DxDiagProvider;

/* ---------- IDxDiagContainer implementation ---------- */

typedef struct DxDiagContainerVtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(DxDiagContainer*, REFIID, void**);
    ULONG   (__stdcall *AddRef)(DxDiagContainer*);
    ULONG   (__stdcall *Release)(DxDiagContainer*);
    /* IDxDiagContainer */
    HRESULT (__stdcall *GetNumberOfChildContainers)(DxDiagContainer*, DWORD*);
    HRESULT (__stdcall *EnumChildContainerNames)(DxDiagContainer*, DWORD, LPWSTR, DWORD);
    HRESULT (__stdcall *GetChildContainer)(DxDiagContainer*, LPCWSTR, DxDiagContainer**);
    HRESULT (__stdcall *GetNumberOfProps)(DxDiagContainer*, DWORD*);
    HRESULT (__stdcall *EnumPropNames)(DxDiagContainer*, DWORD, LPWSTR, DWORD);
    HRESULT (__stdcall *GetProp)(DxDiagContainer*, LPCWSTR, VARIANT*);
} DxDiagContainerVtbl;

struct DxDiagContainer {
    DxDiagContainerVtbl *lpVtbl;
    LONG refCount;
    int level; /* 0=root, 1=DisplayDevices, 2=adapter */
};

static HRESULT __stdcall Container_QueryInterface(DxDiagContainer *self, REFIID riid, void **ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDxDiagContainer_local)) {
        *ppv = self;
        self->refCount++;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall Container_AddRef(DxDiagContainer *self) {
    return InterlockedIncrement(&self->refCount);
}

static ULONG __stdcall Container_Release(DxDiagContainer *self) {
    LONG ref = InterlockedDecrement(&self->refCount);
    if (ref == 0) {
        HeapFree(GetProcessHeap(), 0, self);
    }
    return ref;
}

static DxDiagContainer* CreateContainer(int level);

static HRESULT __stdcall Container_GetNumberOfChildContainers(DxDiagContainer *self, DWORD *count) {
    if (!count) return E_INVALIDARG;
    if (self->level == 0) *count = 1;       /* root has "DxDiag_DisplayDevices" */
    else if (self->level == 1) *count = 1;  /* DisplayDevices has "0" (one adapter) */
    else *count = 0;
    return S_OK;
}

static HRESULT __stdcall Container_EnumChildContainerNames(DxDiagContainer *self, DWORD idx, LPWSTR name, DWORD maxlen) {
    if (!name || maxlen == 0) return E_INVALIDARG;
    if (self->level == 0 && idx == 0) {
        lstrcpynW(name, L"DxDiag_DisplayDevices", maxlen);
        return S_OK;
    }
    if (self->level == 1 && idx == 0) {
        lstrcpynW(name, L"0", maxlen);
        return S_OK;
    }
    return E_INVALIDARG;
}

static HRESULT __stdcall Container_GetChildContainer(DxDiagContainer *self, LPCWSTR name, DxDiagContainer **ppChild) {
    char buf[256];
    char namea[128];
    WideCharToMultiByte(CP_ACP, 0, name, -1, namea, sizeof(namea), NULL, NULL);
    wsprintfA(buf, "Container[%d]::GetChildContainer(\"%s\")", self->level, namea);
    dxlog(buf);

    if (!ppChild) return E_INVALIDARG;
    if (self->level == 0 && lstrcmpiW(name, L"DxDiag_DisplayDevices") == 0) {
        *ppChild = CreateContainer(1);
        return S_OK;
    }
    if (self->level == 1) {
        *ppChild = CreateContainer(2);
        return S_OK;
    }
    *ppChild = NULL;
    dxlog("  -> E_INVALIDARG (unknown child)");
    return E_INVALIDARG;
}

static HRESULT __stdcall Container_GetNumberOfProps(DxDiagContainer *self, DWORD *count) {
    if (!count) return E_INVALIDARG;
    *count = (self->level == 2) ? 10 : 0;
    return S_OK;
}

static HRESULT __stdcall Container_EnumPropNames(DxDiagContainer *self, DWORD idx, LPWSTR name, DWORD maxlen) {
    return E_INVALIDARG; /* Not needed by the game */
}

static HRESULT __stdcall Container_GetProp(DxDiagContainer *self, LPCWSTR name, VARIANT *pVar) {
    char buf[256];
    char namea[128];
    WideCharToMultiByte(CP_ACP, 0, name, -1, namea, sizeof(namea), NULL, NULL);
    wsprintfA(buf, "Container[%d]::GetProp(\"%s\")", self->level, namea);
    dxlog(buf);

    if (!pVar || !name) return E_INVALIDARG;
    VariantInit(pVar);

    if (self->level != 2) return E_INVALIDARG;

    /* Return GPU properties as strings (BSTR in VT_BSTR variant) */
    pVar->vt = VT_BSTR;

    if (lstrcmpiW(name, L"szDescription") == 0) {
        pVar->bstrVal = SysAllocString(L"Direct3D HAL");
    } else if (lstrcmpiW(name, L"szDriverName") == 0) {
        pVar->bstrVal = SysAllocString(L"d3d9.dll");
    } else if (lstrcmpiW(name, L"szDriverVersion") == 0) {
        pVar->bstrVal = SysAllocString(L"9.0.0.0");
    } else if (lstrcmpiW(name, L"szVendorId") == 0) {
        pVar->bstrVal = SysAllocString(L"0x10DE"); /* NVIDIA */
    } else if (lstrcmpiW(name, L"szDeviceId") == 0) {
        pVar->bstrVal = SysAllocString(L"0x1E04"); /* RTX 2080 Ti */
    } else if (lstrcmpiW(name, L"szSubSysId") == 0) {
        pVar->bstrVal = SysAllocString(L"0x00000000");
    } else if (lstrcmpiW(name, L"szRevisionId") == 0) {
        pVar->bstrVal = SysAllocString(L"0x00");
    } else if (lstrcmpiW(name, L"szDisplayMemoryLocalized") == 0) {
        pVar->bstrVal = SysAllocString(L"11264 MB");
    } else if (lstrcmpiW(name, L"szDisplayMemoryEnglish") == 0) {
        pVar->bstrVal = SysAllocString(L"11264 MB");
    } else if (lstrcmpiW(name, L"b3DAccelerationExists") == 0) {
        pVar->vt = VT_BOOL;
        pVar->boolVal = VARIANT_TRUE;
    } else if (lstrcmpiW(name, L"b3DAccelerationEnabled") == 0) {
        pVar->vt = VT_BOOL;
        pVar->boolVal = VARIANT_TRUE;
    } else if (lstrcmpiW(name, L"bDDAccelerationEnabled") == 0) {
        pVar->vt = VT_BOOL;
        pVar->boolVal = VARIANT_TRUE;
    } else if (lstrcmpiW(name, L"bAGPEnabled") == 0) {
        pVar->vt = VT_BOOL;
        pVar->boolVal = VARIANT_TRUE;
    } else if (lstrcmpiW(name, L"bAGPExistenceValid") == 0) {
        pVar->vt = VT_BOOL;
        pVar->boolVal = VARIANT_TRUE;
    } else if (lstrcmpiW(name, L"bAGPExists") == 0) {
        pVar->vt = VT_BOOL;
        pVar->boolVal = VARIANT_TRUE;
    } else {
        /* Unknown property - return empty string */
        pVar->bstrVal = SysAllocString(L"");
    }

    return S_OK;
}

static DxDiagContainerVtbl g_ContainerVtbl = {
    Container_QueryInterface,
    Container_AddRef,
    Container_Release,
    Container_GetNumberOfChildContainers,
    Container_EnumChildContainerNames,
    Container_GetChildContainer,
    Container_GetNumberOfProps,
    Container_EnumPropNames,
    Container_GetProp
};

static DxDiagContainer* CreateContainer(int level) {
    DxDiagContainer *c = (DxDiagContainer*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DxDiagContainer));
    if (c) {
        c->lpVtbl = &g_ContainerVtbl;
        c->refCount = 1;
        c->level = level;
    }
    return c;
}

/* ---------- IDxDiagProvider implementation ---------- */

typedef struct DxDiagProviderVtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(DxDiagProvider*, REFIID, void**);
    ULONG   (__stdcall *AddRef)(DxDiagProvider*);
    ULONG   (__stdcall *Release)(DxDiagProvider*);
    /* IDxDiagProvider */
    HRESULT (__stdcall *Initialize)(DxDiagProvider*, void* /* DXDIAG_INIT_PARAMS* */);
    HRESULT (__stdcall *GetRootContainer)(DxDiagProvider*, DxDiagContainer**);
} DxDiagProviderVtbl;

struct DxDiagProvider {
    DxDiagProviderVtbl *lpVtbl;
    LONG refCount;
};

static HRESULT __stdcall Provider_QueryInterface(DxDiagProvider *self, REFIID riid, void **ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDxDiagProvider_local)) {
        *ppv = self;
        self->refCount++;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall Provider_AddRef(DxDiagProvider *self) {
    return InterlockedIncrement(&self->refCount);
}

static ULONG __stdcall Provider_Release(DxDiagProvider *self) {
    LONG ref = InterlockedDecrement(&self->refCount);
    if (ref == 0) {
        HeapFree(GetProcessHeap(), 0, self);
    }
    return ref;
}

static HRESULT __stdcall Provider_Initialize(DxDiagProvider *self, void *pParams) {
    dxlog("Provider::Initialize called");
    return S_OK;
}

static HRESULT __stdcall Provider_GetRootContainer(DxDiagProvider *self, DxDiagContainer **ppRoot) {
    dxlog("Provider::GetRootContainer called");
    if (!ppRoot) return E_INVALIDARG;
    *ppRoot = CreateContainer(0);
    return (*ppRoot) ? S_OK : E_OUTOFMEMORY;
}

static DxDiagProviderVtbl g_ProviderVtbl = {
    Provider_QueryInterface,
    Provider_AddRef,
    Provider_Release,
    Provider_Initialize,
    Provider_GetRootContainer
};

/* ---------- IClassFactory implementation ---------- */

typedef struct ClassFactory ClassFactory;
typedef struct ClassFactoryVtbl {
    HRESULT (__stdcall *QueryInterface)(ClassFactory*, REFIID, void**);
    ULONG   (__stdcall *AddRef)(ClassFactory*);
    ULONG   (__stdcall *Release)(ClassFactory*);
    HRESULT (__stdcall *CreateInstance)(ClassFactory*, IUnknown*, REFIID, void**);
    HRESULT (__stdcall *LockServer)(ClassFactory*, BOOL);
} ClassFactoryVtbl;

struct ClassFactory {
    ClassFactoryVtbl *lpVtbl;
};

static HRESULT __stdcall CF_QueryInterface(ClassFactory *self, REFIID riid, void **ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory)) {
        *ppv = self;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall CF_AddRef(ClassFactory *self) { return 2; }
static ULONG __stdcall CF_Release(ClassFactory *self) { return 1; }

static HRESULT __stdcall CF_CreateInstance(ClassFactory *self, IUnknown *pOuter, REFIID riid, void **ppv) {
    DxDiagProvider *prov;
    if (pOuter) return CLASS_E_NOAGGREGATION;
    if (!ppv) return E_POINTER;

    prov = (DxDiagProvider*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DxDiagProvider));
    if (!prov) return E_OUTOFMEMORY;

    prov->lpVtbl = &g_ProviderVtbl;
    prov->refCount = 1;

    *ppv = prov;
    return S_OK;
}

static HRESULT __stdcall CF_LockServer(ClassFactory *self, BOOL lock) { return S_OK; }

static ClassFactoryVtbl g_CFVtbl = {
    CF_QueryInterface, CF_AddRef, CF_Release, CF_CreateInstance, CF_LockServer
};
static ClassFactory g_ClassFactory = { &g_CFVtbl };

/* ---------- DLL exports ---------- */

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
    dxlog("DllGetClassObject called");
    if (IsEqualGUID(rclsid, &CLSID_DxDiagProvider_local)) {
        dxlog("  -> CLSID_DxDiagProvider matched, returning class factory");
        return CF_QueryInterface(&g_ClassFactory, riid, ppv);
    }
    dxlog("  -> CLASS_E_CLASSNOTAVAILABLE");
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void) {
    return S_FALSE; /* Keep loaded */
}

STDAPI DllRegisterServer(void) { return S_OK; }
STDAPI DllUnregisterServer(void) { return S_OK; }

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        dxlog("dxdiagn.dll loaded (DLL_PROCESS_ATTACH)");
    }
    return TRUE;
}
