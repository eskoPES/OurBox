/*
 * jb_d3d12x.dll: JackBox's replacement for the console D3D12 module
 * (d3d12_x.dll / d3d12_xs.dll, names are (guess)).
 *
 * Stage 1 (this file): the standard D3D12 entry points, forwarded to the
 * host's hardware D3D12 runtime (native D3D12 on Windows, vkd3d on Linux),
 * with JackBox diagnostics. Console-only D3D12.X entry points and interface
 * extensions are not publicly documented; any the title imports show up as
 * loader traps naming the exact function (see docs/STATUS.md, blocker 2).
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>

typedef HRESULT (WINAPI *JB_PFN_SERIALIZE_RS)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *JB_PFN_CREATE_RS_DESER)(const void *, SIZE_T, REFIID, void **);

static HMODULE g_host;
static int g_verbose;
#define LOG(...) do { if (g_verbose) { fprintf(stderr, "[jb_d3d12x] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static FARPROC host(const char *name)
{
    if (!g_host) {
        char v[8]; g_verbose = GetEnvironmentVariableA("JACKBOX_LOG", v, sizeof v) && v[0] != '0';
        char sys[MAX_PATH]; GetSystemDirectoryA(sys, sizeof sys);
        char path[MAX_PATH + 16]; snprintf(path, sizeof path, "%s\\d3d12.dll", sys);
        g_host = LoadLibraryA(path);
        if (!g_host) { fprintf(stderr, "[jb_d3d12x] host d3d12.dll not found (%lu)\n", GetLastError()); return NULL; }
        LOG("host D3D12 runtime: %s", path);
    }
    FARPROC f = GetProcAddress(g_host, name);
    if (!f) fprintf(stderr, "[jb_d3d12x] host d3d12.dll lacks %s\n", name);
    return f;
}

__declspec(dllexport) HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **dev)
{
    PFN_D3D12_CREATE_DEVICE f = (PFN_D3D12_CREATE_DEVICE)host("D3D12CreateDevice");
    if (!f) return E_NOINTERFACE;
    HRESULT hr = f(adapter, fl, riid, dev);
    LOG("D3D12CreateDevice(fl=0x%x) -> 0x%08lx", fl, (unsigned long)hr);
    if (SUCCEEDED(hr) && dev && *dev && g_verbose) {
        ID3D12Device *d = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*dev, &IID_ID3D12Device, (void **)&d))) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {0};
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(d, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)))
                LOG("raytracing tier %d", o5.RaytracingTier);
            ID3D12Device_Release(d);
        }
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **out)
{
    PFN_D3D12_GET_DEBUG_INTERFACE f = (PFN_D3D12_GET_DEBUG_INTERFACE)host("D3D12GetDebugInterface");
    return f ? f(riid, out) : E_NOINTERFACE;
}

__declspec(dllexport) HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *d, D3D_ROOT_SIGNATURE_VERSION v, ID3DBlob **blob, ID3DBlob **err)
{
    JB_PFN_SERIALIZE_RS f = (JB_PFN_SERIALIZE_RS)host("D3D12SerializeRootSignature");
    return f ? f(d, v, blob, err) : E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *d, ID3DBlob **blob, ID3DBlob **err)
{
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE f = (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)host("D3D12SerializeVersionedRootSignature");
    return f ? f(d, blob, err) : E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **out)
{
    JB_PFN_CREATE_RS_DESER f = (JB_PFN_CREATE_RS_DESER)host("D3D12CreateRootSignatureDeserializer");
    return f ? f(data, size, riid, out) : E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT n, const IID *iids, void *cfg, UINT *sizes)
{
    typedef HRESULT (WINAPI *P)(UINT, const IID *, void *, UINT *);
    P f = (P)host("D3D12EnableExperimentalFeatures");
    return f ? f(n, iids, cfg, sizes) : E_NOTIMPL;
}
