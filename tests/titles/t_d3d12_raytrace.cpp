// OurBox DXR test title: builds bottom/top-level acceleration structures for
// one triangle, traces one primary ray per pixel with DispatchRays, reads the
// image back and checks it. D3D12 is imported from the console module name
// (d3d12_xs.dll) so OurBox's redirect is exercised.
//
// Needs precompiled DXIL in rt.dxil next to the exe (tools/compile_shaders.sh;
// test titles ship precompiled shaders like console titles do). Prints
// "RESULT SKIP" when the GPU has no ray tracing or rt.dxil is missing.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#define W 256
#define H 256
static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKHR(e) do { HRESULT _hr = (e); if (FAILED(_hr)) { std::printf("FAIL %s:%d: %s -> 0x%08lx\n", __FILE__, __LINE__, #e, (unsigned long)_hr); return done(); } } while (0)
static int done() { std::printf(fails ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", fails); std::fflush(stdout); return fails ? 1 : 0; }
static int skip(const char *why) { std::printf("RESULT SKIP %s\n", why); std::fflush(stdout); return 0; }

static ID3D12Device5 *dev;

static ID3D12Resource *buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heap;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    ID3D12Resource *r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

static void uav_barrier(ID3D12GraphicsCommandList4 *cl, ID3D12Resource *r)
{
    D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

static std::vector<uint8_t> read_file_next_to_exe(const char *name)
{
    char path[MAX_PATH]; GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *s = std::strrchr(path, '\\'); if (s) s[1] = 0; else path[0] = 0;
    std::strncat(path, name, MAX_PATH - std::strlen(path) - 1);
    std::vector<uint8_t> v;
    FILE *f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END); v.resize(std::ftell(f)); std::fseek(f, 0, SEEK_SET);
    if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    std::fclose(f);
    return v;
}

int main()
{
    ID3D12Device *base = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&base)))) return skip("no D3D12 feature level 12_0 device");
    if (FAILED(base->QueryInterface(IID_PPV_ARGS(&dev)))) return skip("no ID3D12Device5");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
    std::printf("INFO raytracing_tier=%d\n", (int)o5.RaytracingTier);
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) return skip("GPU has no DXR");
    std::vector<uint8_t> dxil = read_file_next_to_exe("rt.dxil");
    if (dxil.empty()) return skip("rt.dxil missing (run tools/compile_shaders.sh)");

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue; CHECKHR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ID3D12CommandAllocator *alloc; CHECKHR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ID3D12GraphicsCommandList4 *cl; CHECKHR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)));
    ID3D12Fence *fence; CHECKHR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    auto submit_and_wait = [&]() {
        cl->Close();
        ID3D12CommandList *l[] = { cl };
        queue->ExecuteCommandLists(1, l);
        queue->Signal(fence, ++fv);
        fence->SetEventOnCompletion(fv, ev);
        WaitForSingleObject(ev, 20000);
    };

    // Geometry: the same triangle as t_d3d12_triangle, at z = 0.
    const float verts[9] = { 0.0f, 0.8f, 0.0f, 0.8f, -0.8f, 0.0f, -0.8f, -0.8f, 0.0f };
    ID3D12Resource *vb = buffer(sizeof verts, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    CHECK(vb); void *m; vb->Map(0, nullptr, &m); std::memcpy(m, verts, sizeof verts); vb->Unmap(0, nullptr);

    // Bottom-level acceleration structure.
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
    geom.Triangles.VertexBuffer.StrideInBytes = 12;
    geom.Triangles.VertexCount = 3;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bin = {};
    bin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; bin.NumDescs = 1; bin.pGeometryDescs = &geom;
    bin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bpi = {};
    dev->GetRaytracingAccelerationStructurePrebuildInfo(&bin, &bpi);
    ID3D12Resource *blas = buffer(bpi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    // Top-level: one instance, identity transform.
    D3D12_RAYTRACING_INSTANCE_DESC inst = {};
    inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;
    inst.InstanceMask = 0xFF;
    inst.AccelerationStructure = blas->GetGPUVirtualAddress();
    ID3D12Resource *ib = buffer(sizeof inst, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ib->Map(0, nullptr, &m); std::memcpy(m, &inst, sizeof inst); ib->Unmap(0, nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin = {};
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; tin.NumDescs = 1; tin.InstanceDescs = ib->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tpi = {};
    dev->GetRaytracingAccelerationStructurePrebuildInfo(&tin, &tpi);
    ID3D12Resource *tlas = buffer(tpi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    UINT64 scratch_size = bpi.ScratchDataSizeInBytes > tpi.ScratchDataSizeInBytes ? bpi.ScratchDataSizeInBytes : tpi.ScratchDataSizeInBytes;
    ID3D12Resource *scratch = buffer(scratch_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CHECK(blas && tlas && scratch && ib);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs = bin; bd.DestAccelerationStructureData = blas->GetGPUVirtualAddress(); bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    uav_barrier(cl, blas);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs = tin; td.DestAccelerationStructureData = tlas->GetGPUVirtualAddress(); td.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    uav_barrier(cl, tlas);
    submit_and_wait();

    // Output texture + descriptor heap.
    D3D12_HEAP_PROPERTIES hdef = {}; hdef.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC od = {};
    od.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; od.Width = W; od.Height = H; od.DepthOrArraySize = 1; od.MipLevels = 1;
    od.Format = DXGI_FORMAT_R8G8B8A8_UNORM; od.SampleDesc.Count = 1; od.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *out; CHECKHR(dev->CreateCommittedResource(&hdef, D3D12_HEAP_FLAG_NONE, &od, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    ID3D12DescriptorHeap *heap; CHECKHR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {}; ud.Format = od.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_h; heap->GetCPUDescriptorHandleForHeapStart(&cpu_h); // RetVal form: DirectX-Headers on non-MSVC
    dev->CreateUnorderedAccessView(out, nullptr, &ud, cpu_h);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_h; heap->GetGPUDescriptorHandleForHeapStart(&gpu_h);

    // Global root signature: t0 = TLAS (root SRV), u0 = output (table).
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *rsb = nullptr, *rse = nullptr;
    CHECKHR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse));
    ID3D12RootSignature *rs; CHECKHR(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs)));

    // Ray tracing pipeline state object.
    D3D12_DXIL_LIBRARY_DESC lib = {}; lib.DXILLibrary.pShaderBytecode = dxil.data(); lib.DXILLibrary.BytecodeLength = dxil.size();
    D3D12_HIT_GROUP_DESC hg = {}; hg.HitGroupExport = L"HitGroup"; hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES; hg.ClosestHitShaderImport = L"Hit";
    D3D12_RAYTRACING_SHADER_CONFIG sc = { 16, 8 };
    D3D12_GLOBAL_ROOT_SIGNATURE grs = { rs };
    D3D12_RAYTRACING_PIPELINE_CONFIG pc = { 1 };
    D3D12_STATE_SUBOBJECT subs[5] = {
        { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib },
        { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc },
        { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc },
    };
    D3D12_STATE_OBJECT_DESC sod = { D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 5, subs };
    ID3D12StateObject *so; CHECKHR(dev->CreateStateObject(&sod, IID_PPV_ARGS(&so)));
    ID3D12StateObjectProperties *props; CHECKHR(so->QueryInterface(IID_PPV_ARGS(&props)));

    // Shader tables: raygen @0, miss @64, hit group @128.
    const UINT rec = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    ID3D12Resource *sbt = buffer(3 * rec, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t *p; sbt->Map(0, nullptr, (void **)&p);
    const wchar_t *ids[3] = { L"RayGen", L"Miss", L"HitGroup" };
    for (int i = 0; i < 3; i++) {
        void *id = props->GetShaderIdentifier(ids[i]);
        CHECK(id != nullptr);
        if (id) std::memcpy(p + i * rec, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    sbt->Unmap(0, nullptr);
    D3D12_GPU_VIRTUAL_ADDRESS sa = sbt->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dr = {};
    dr.RayGenerationShaderRecord = { sa, rec };
    dr.MissShaderTable = { sa + rec, rec, rec };
    dr.HitGroupTable = { sa + 2 * rec, rec, rec };
    dr.Width = W; dr.Height = H; dr.Depth = 1;

    ID3D12Resource *rb = buffer((UINT64)W * 4 * H, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
    const int frames = 60;
    for (int frame = 0; frame <= frames; frame++) {
        if (frame == 1) QueryPerformanceCounter(&t0);
        alloc->Reset(); cl->Reset(alloc, nullptr);
        cl->SetDescriptorHeaps(1, &heap);
        cl->SetComputeRootSignature(rs);
        cl->SetComputeRootShaderResourceView(0, tlas->GetGPUVirtualAddress());
        cl->SetComputeRootDescriptorTable(1, gpu_h);
        cl->SetPipelineState1(so);
        cl->DispatchRays(&dr);
        if (frame == frames) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = out;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, W * 4 };
            src.pResource = out; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        submit_and_wait();
        if (frame == frames - 1) {
            QueryPerformanceCounter(&t1);
            std::printf("PERF frames=%d avg_frame_ms=%.3f\n", frames - 1, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 1));
        }
    }
    CHECK(dev->GetDeviceRemovedReason() == S_OK);

    uint8_t *px = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)W * 4 * H };
    CHECKHR(rb->Map(0, &rr, (void **)&px));
    uint32_t hash = 2166136261u; int red = 0, blue = 0;
    for (int i = 0; i < W * H * 4; i++) hash = (hash ^ px[i]) * 16777619u;
    for (int i = 0; i < W * H; i++) {
        const uint8_t *q = px + i * 4;
        if (q[0] == 255 && q[1] == 0 && q[2] == 0) red++;
        else if (q[0] == 0 && q[1] == 0 && q[2] == 255) blue++;
    }
    std::printf("IMAGE hash=%08x red=%d blue=%d\n", hash, red, blue);
    const uint8_t *c = px + (H / 2) * W * 4 + (W / 2) * 4, *k = px + 2 * W * 4 + 2 * 4;
    CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0);
    CHECK(k[0] == 0 && k[1] == 0 && k[2] == 255);
    int expect = (int)(0.32 * W * H);
    CHECK(red > expect * 97 / 100 && red < expect * 103 / 100);
    CHECK(red + blue == W * H);
    rb->Unmap(0, nullptr);
    return done();
}
