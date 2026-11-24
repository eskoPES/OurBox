/*
 * GPU compute test title: dispatches a compute shader through D3D12
 * (imported as d3d12_xs.dll), then verifies every output element on the CPU.
 * Exercises UAV buffers, root UAV descriptors, copy to readback and fences.
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

#define N (1u << 20)
typedef HRESULT (WINAPI *PFN_D3DCompile)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *,
                                         const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);
static const char k_cs[] =
    "RWBuffer<uint> o : register(u0);\n"
    "[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  uint i = id.x; o[i] = i * i + 7u;\n"
    "}\n";

int main(void)
{
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompile comp = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
    CHECK(comp != NULL); if (!comp) DONE();
    ID3D12Device *dev = NULL;
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    CHECK(SUCCEEDED(hr)); if (FAILED(hr)) DONE();

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    ID3D12CommandQueue *queue; CHECK(SUCCEEDED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)));
    ID3D12CommandAllocator *alloc; CHECK(SUCCEEDED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_COMPUTE, &IID_ID3D12CommandAllocator, (void **)&alloc)));

    /* Typed UAV through a shader-visible descriptor table. */
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp; memset(&rp, 0, sizeof rp);
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable.NumDescriptorRanges = 1; rp.DescriptorTable.pDescriptorRanges = &range;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = NULL, *err = NULL;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
    ID3D12RootSignature *rs; CHECK(SUCCEEDED(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs)));
    ID3DBlob *cs = NULL;
    hr = comp(k_cs, sizeof k_cs - 1, "cs.hlsl", NULL, NULL, "cs", "cs_5_0", 0, 0, &cs, &err);
    CHECK(SUCCEEDED(hr)); if (FAILED(hr)) { printf("%s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : ""); DONE(); }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs; pd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs); pd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    ID3D12PipelineState *pso; CHECK(SUCCEEDED(ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso)));

    D3D12_HEAP_PROPERTIES hdef = { D3D12_HEAP_TYPE_DEFAULT, 0, 0, 1, 1 }, hrb = { D3D12_HEAP_TYPE_READBACK, 0, 0, 1, 1 };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, N * 4ull, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    ID3D12Resource *buf, *rb;
    CHECK(SUCCEEDED(ID3D12Device_CreateCommittedResource(dev, &hdef, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&buf)));
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    CHECK(SUCCEEDED(ID3D12Device_CreateCommittedResource(dev, &hrb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb)));

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    ID3D12DescriptorHeap *heap; CHECK(SUCCEEDED(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud; memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_UINT; ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = N;
    ID3D12Device_CreateUnorderedAccessView(dev, buf, NULL, &ud, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap));
    ID3D12GraphicsCommandList *cl;
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc, pso, &IID_ID3D12GraphicsCommandList, (void **)&cl)));
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 0, ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap));
    ID3D12GraphicsCommandList_Dispatch(cl, N / 64, 1, 1);
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = buf;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    ID3D12GraphicsCommandList_CopyResource(cl, rb, buf);
    CHECK(SUCCEEDED(ID3D12GraphicsCommandList_Close(cl)));

    ID3D12Fence *fence; CHECK(SUCCEEDED(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
    CHECK(WaitForSingleObject(ev, 20000) == WAIT_OBJECT_0);
    QueryPerformanceCounter(&t1);
    printf("PERF dispatch_threads=%u avg_frame_ms=%.3f\n", N, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);

    uint32_t *out = NULL; D3D12_RANGE rr = { 0, N * 4ull };
    CHECK(SUCCEEDED(ID3D12Resource_Map(rb, 0, &rr, (void **)&out)));
    if (out) {
        uint32_t bad = 0, hash = 2166136261u;
        for (uint32_t i = 0; i < N; i++) { if (out[i] != i * i + 7u) bad++; hash = (hash ^ out[i]) * 16777619u; }
        printf("IMAGE hash=%08x mismatches=%u\n", hash, bad);
        CHECK(bad == 0);
        ID3D12Resource_Unmap(rb, 0, NULL);
    }
    DONE();
}
