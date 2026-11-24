/*
 * GPU test title: renders a triangle offscreen through D3D12 (imported from
 * the console module name d3d12_xs.dll, redirected by JackBox), reads the
 * image back and checks real pixels. Also times frames.
 *
 * Output lines parsed by tests/run_titles.sh:
 *   IMAGE hash=<fnv1a> red=<pixels> blue=<pixels>
 *   PERF frames=<n> avg_frame_ms=<x>
 * env JACKBOX_DUMP=<path.ppm> writes the rendered image.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <string.h>
#include "jbtest.h"

#define W 256
#define H 256

typedef HRESULT (WINAPI *PFN_D3DCompile)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *,
                                         const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

static const char k_hlsl[] =
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float x = (id == 1) ? 0.8 : ((id == 2) ? -0.8 : 0.0);\n"
    "  float y = (id == 0) ? 0.8 : -0.8;\n"
    "  return float4(x, y, 0.0, 1.0);\n"
    "}\n"
    "float4 ps() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";

static ID3DBlob *compile(PFN_D3DCompile c, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = c(k_hlsl, sizeof k_hlsl - 1, "jb.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) printf("shader %s failed 0x%08lx: %s\n", entry, (unsigned long)hr, err ? (char *)ID3D10Blob_GetBufferPointer(err) : "");
    return code;
}

int main(void)
{
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompile D3DCompileFn = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
    CHECK(D3DCompileFn != NULL);
    if (!D3DCompileFn) DONE();

    ID3D12Device *dev = NULL;
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) { printf("D3D12CreateDevice failed 0x%08lx\n", (unsigned long)hr); DONE(); }

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    ID3D12CommandQueue *queue; CHECK(SUCCEEDED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)));
    ID3D12CommandAllocator *alloc; CHECK(SUCCEEDED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc)));
    ID3D12GraphicsCommandList *cl; CHECK(SUCCEEDED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl)));
    ID3D12GraphicsCommandList_Close(cl);

    /* Render target */
    D3D12_HEAP_PROPERTIES hp_default = { D3D12_HEAP_TYPE_DEFAULT, 0, 0, 1, 1 };
    D3D12_HEAP_PROPERTIES hp_readback = { D3D12_HEAP_TYPE_READBACK, 0, 0, 1, 1 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R8G8B8A8_UNORM, { { 0.0f, 0.0f, 1.0f, 1.0f } } };
    ID3D12Resource *rt; CHECK(SUCCEEDED(ID3D12Device_CreateCommittedResource(dev, &hp_default, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&rt)));

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ID3D12DescriptorHeap *rtv_heap; CHECK(SUCCEEDED(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    /* Readback buffer */
    const UINT pitch = W * 4; /* 1024: already 256-aligned */
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, (UINT64)pitch * H, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *rb; CHECK(SUCCEEDED(ID3D12Device_CreateCommittedResource(dev, &hp_readback, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb)));

    /* Root signature + PSO */
    D3D12_ROOT_SIGNATURE_DESC rsd = { 0, NULL, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *rs_blob = NULL, *rs_err = NULL;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err)));
    ID3D12RootSignature *rs; CHECK(SUCCEEDED(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rs_blob), ID3D10Blob_GetBufferSize(rs_blob), &IID_ID3D12RootSignature, (void **)&rs)));
    ID3DBlob *vs = compile(D3DCompileFn, "vs", "vs_5_0"), *ps = compile(D3DCompileFn, "ps", "ps_5_0");
    CHECK(vs && ps);
    if (!vs || !ps) DONE();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFF;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = NULL;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) { printf("CreateGraphicsPipelineState 0x%08lx\n", (unsigned long)hr); DONE(); }

    ID3D12Fence *fence; UINT64 fv = 0;
    CHECK(SUCCEEDED(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);

    const float blue[4] = { 0, 0, 1, 1 };
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
    const int frames = 60;
    double first_ms = 0;
    for (int frame = 0; frame <= frames; frame++) {
        QueryPerformanceCounter(&t0);
        int last = frame == frames;
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(cl, alloc, pso);
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, blue, 0, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
        if (last) {
            D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = rt;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst, src; memset(&dst, 0, sizeof dst); memset(&src, 0, sizeof src);
            dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst.PlacedFootprint.Footprint.Width = W; dst.PlacedFootprint.Footprint.Height = H;
            dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
            src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
            ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        }
        CHECK(SUCCEEDED(ID3D12GraphicsCommandList_Close(cl)));
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)cl };
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        ID3D12CommandQueue_Signal(queue, fence, ++fv);
        if (ID3D12Fence_GetCompletedValue(fence) < fv) { ID3D12Fence_SetEventOnCompletion(fence, fv, ev); WaitForSingleObject(ev, 10000); }
        QueryPerformanceCounter(&t1);
        if (frame == 0) first_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        static LARGE_INTEGER s0; if (frame == 1) s0 = t0;
        if (frame == frames - 1) {
            double avg = (t1.QuadPart - s0.QuadPart) * 1000.0 / f.QuadPart / (frames - 1);
            printf("PERF frames=%d first_frame_ms=%.2f avg_frame_ms=%.3f\n", frames - 1, first_ms, avg);
        }
    }
    CHECK(ID3D12Device_GetDeviceRemovedReason(dev) == S_OK);

    uint8_t *px = NULL;
    D3D12_RANGE rr = { 0, (SIZE_T)pitch * H };
    CHECK(SUCCEEDED(ID3D12Resource_Map(rb, 0, &rr, (void **)&px)));
    if (px) {
        uint32_t hash = 2166136261u; int red = 0, bluec = 0;
        for (int i = 0; i < W * H * 4; i++) hash = (hash ^ px[i]) * 16777619u;
        for (int i = 0; i < W * H; i++) {
            const uint8_t *p = px + i * 4;
            if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255) red++;
            else if (p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255) bluec++;
        }
        printf("IMAGE hash=%08x red=%d blue=%d\n", hash, red, bluec);
        const uint8_t *c = px + (H / 2 * pitch) + (W / 2) * 4, *k = px + 2 * pitch + 2 * 4;
        CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0);     /* triangle centre is red */
        CHECK(k[0] == 0 && k[1] == 0 && k[2] == 255);     /* corner is clear colour */
        /* Triangle covers 0.32 of the target: 1.6 x 1.6 / 2 of a 2 x 2 NDC square. */
        int expect = (int)(0.32 * W * H);
        CHECK(red > expect * 97 / 100 && red < expect * 103 / 100);
        CHECK(red + bluec == W * H);
        char path[MAX_PATH];
        if (GetEnvironmentVariableA("JACKBOX_DUMP", path, sizeof path)) {
            FILE *o = fopen(path, "wb");
            if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); for (int i = 0; i < W * H; i++) fwrite(px + i * 4, 1, 3, o); fclose(o); }
        }
        ID3D12Resource_Unmap(rb, 0, NULL);
    }
    DONE();
}
