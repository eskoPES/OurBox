// DXR test shaders for t_d3d12_raytrace. Compiled ahead of time to DXIL
// (rt.dxil) with DXC, because test titles ship precompiled shaders the way
// console titles do:
//   dxc -T lib_6_3 -Fo rt.dxil rt.hlsl
RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float4> output : register(u0);

struct Payload { float4 color; };

[shader("raygeneration")]
void RayGen()
{
    uint2 px = DispatchRaysIndex().xy;
    float2 dims = (float2)DispatchRaysDimensions().xy;
    float2 ndc = (float2(px) + 0.5) / dims * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin = float3(ndc.x, -ndc.y, -1.0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.001;
    ray.TMax = 100.0;
    Payload p = { float4(0, 0, 0, 0) };
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, p);
    output[px] = p.color;
}

[shader("miss")]
void Miss(inout Payload p) { p.color = float4(0, 0, 1, 1); }

[shader("closesthit")]
void Hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { p.color = float4(1, 0, 0, 1); }
