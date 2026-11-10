// SPDX-License-Identifier: Apache-2.0
// GroomLab hair's density on a grid (line_frames.h runDensity): the strand lines' points, already on
// the GPU, splatted into a 3D grid of how much hair each cell holds, as the light sees it: the
// optical density sigma (per unit of length) of the fibers there, each point standing for a piece of
// its strand (the mean segment's length) as wide as its width, a fiber of width w and length l
// blocking (pi / 4) w l of the light on average over directions. The viewport's hair shading
// (hosts/maya hair_shading.h) marches toward each light through it: the hairs in the way, for its
// shadow and the light scattered on through them; and reads each cell's ambient occlusion, the
// light that gets through from six directions around it.
//
// The resources as look.slang lays them out (its cbuffer's rows, gIn the points, gAux1 the lines'
// uvs: u and the width a point, gOut the grid as unsigned integers: 16.16 fixed point for the
// atomic adds), and a 3D texture out (rg: sigma, ambient occlusion).
//   gValue: the grid's origin (xyz, its first cell's corner) and cell size (w).
//   gFlag: the grid's cells along x, y, z; w: the points.
//   gValue2: x the share of a cell's cross-section a unit of width blocks ((pi / 4) x the mean
//   segment / cell^3, over the preview's density), y the width of a point without one, z the
//   occlusion's reach (cells).
//   gDispatch.x: the threads in a row of the dispatch.

#define GROUP_SIZE 256

[[vk::binding(0)]] cbuffer Look : register(b0) {
    uint gStrandCount;
    uint gUniformCount;
    uint gPointCount;
    uint gDataBlock;
    float4 gValue;
    uint4 gFlag;
    uint4 gOctaveSeeds[4];
    uint4 gRamp;
    uint4 gTable;
    uint4 gField;
    uint4 gAux;
    uint4 gDispatch;
    float4 gValue2;
    uint4 gFlag2;
    uint4 gRamp2;
    uint4 gTable2;
};

// (DirectX registers, as look.slang's; Metal's one buffer table: Slang lays them out, GROOMLAB_SLANG_LAYOUT.)
#ifdef GROOMLAB_SLANG_LAYOUT
#define D3D_REGISTER(r)
#else
#define D3D_REGISTER(r) : register(r)
#endif
[[vk::binding(3)]] ByteAddressBuffer gIn D3D_REGISTER(t2);
[[vk::binding(10)]] ByteAddressBuffer gAux1 D3D_REGISTER(t6);
[[vk::binding(4)]] RWByteAddressBuffer gOut D3D_REGISTER(u0);
#ifdef GROOMLAB_SLANG_GL
[[vk::binding(0, 1)]] [format("rg32f")] RWTexture3D<float2> gVolume D3D_REGISTER(u3);
#else
[[vk::binding(11)]] [format("rg32f")] RWTexture3D<float2> gVolume D3D_REGISTER(u3);
#endif

static const float kFixed = 65536.0f;

uint cellIndex(int3 c) { return uint(c.x) + gFlag.x * (uint(c.y) + gFlag.y * uint(c.z)); }
bool inside(int3 c) { return c.x >= 0 && c.y >= 0 && c.z >= 0 && uint(c.x) < gFlag.x && uint(c.y) < gFlag.y && uint(c.z) < gFlag.z; }

[shader("compute")]
[numthreads(GROUP_SIZE, 1, 1)]
void DensityClearCS(uint3 id : SV_DispatchThreadID) {
    const uint v = id.x + id.y * gDispatch.x;
    if (v >= gFlag.x * gFlag.y * gFlag.z) return;
    gOut.Store(v * 4, 0u);
}

// A thread a point: its share of the light blocked, trilinear into the eight cells around it. The
// group's adds gather in groupshared memory first, a table of the cells they touch (open addressing
// on the cell's number), then go to the grid once a cell: a group's points are neighboring strands'
// (a strand's points in a row, strands made side by side), so several land in each cell, and the
// grid's atomics (contended: every strand's points go to a few cells each) were the grid's cost. A
// cell the table has no room for goes straight to the grid. The sums are the same integers either
// way (16.16 fixed point), in any order. (1M strands of 16 points, 159 cells a side: 2.9 ms on the
// GPU before, 1.1 after on DirectX 11; 3.6 and 1.5 on OpenGL: test_line_frames.cpp's
// DISABLED_DensityGridTimes.)
#define SPLAT_TABLE 2048
#define SPLAT_EMPTY 0xFFFFFFFFu
#define SPLAT_PROBES 8
groupshared uint gSplatCell[SPLAT_TABLE];
groupshared uint gSplatSum[SPLAT_TABLE];

// The group's barrier (look.slang's groupSync: Slang's own asks GLSL for KHR_memory_scope_semantics,
// which OpenGL drivers lack).
void splatSync() {
    __target_switch {
    case glsl: __intrinsic_asm "memoryBarrierShared(); barrier()";
    default: GroupMemoryBarrierWithGroupSync();
    }
}

void splatAdd(uint cell, uint add) {
    uint h = (cell * 2654435761u) >> 21;  // (11 bits: SPLAT_TABLE)
    for (int probe = 0; probe < SPLAT_PROBES; ++probe) {
        uint was;
        InterlockedCompareExchange(gSplatCell[h], SPLAT_EMPTY, cell, was);
        if (was == SPLAT_EMPTY || was == cell) {
            InterlockedAdd(gSplatSum[h], add);
            return;
        }
        h = (h + 1u) & (SPLAT_TABLE - 1u);
    }
    uint before;
    gOut.InterlockedAdd(cell * 4, add, before);
}

[shader("compute")]
[numthreads(GROUP_SIZE, 1, 1)]
void DensitySplatCS(uint3 id : SV_DispatchThreadID, uint3 lane : SV_GroupThreadID) {
    [ForceUnroll]
    for (uint e = 0u; e < SPLAT_TABLE / GROUP_SIZE; ++e) {
        gSplatCell[lane.x + e * GROUP_SIZE] = SPLAT_EMPTY;
        gSplatSum[lane.x + e * GROUP_SIZE] = 0u;
    }
    splatSync();
    const uint p = id.x + id.y * gDispatch.x;
    if (p < gFlag.w) {
        const float3 q = asfloat(gIn.Load3(p * 12));
        float width = asfloat(gAux1.Load(p * 8 + 4));
        if (!(width >= 0.0f)) width = gValue2.y;  // (no width: kNoLineWidth)
        const float amount = width * gValue2.x * kFixed;
        if (amount > 0.0f) {
            const float3 g = (q - gValue.xyz) / gValue.w - 0.5f;
            const float3 lo = floor(g);
            const float3 f = g - lo;
            const int3 b = int3(lo);
            [ForceUnroll]
            for (int k = 0; k < 8; ++k) {
                const int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
                const int3 c = b + o;
                if (!inside(c)) continue;
                const float3 t = float3(o.x != 0 ? f.x : 1.0f - f.x, o.y != 0 ? f.y : 1.0f - f.y, o.z != 0 ? f.z : 1.0f - f.z);
                const uint add = uint(amount * t.x * t.y * t.z + 0.5f);
                if (add != 0u) splatAdd(cellIndex(c), add);
            }
        }
    }
    splatSync();
    [ForceUnroll]
    for (uint e = 0u; e < SPLAT_TABLE / GROUP_SIZE; ++e) {
        const uint slot = lane.x + e * GROUP_SIZE;
        const uint cell = gSplatCell[slot];
        if (cell != SPLAT_EMPTY && gSplatSum[slot] != 0u) {
            uint before;
            gOut.InterlockedAdd(cell * 4, gSplatSum[slot], before);
        }
    }
}

float sigmaAt(int3 c) { return float(gOut.Load(cellIndex(c) * 4)) / kFixed; }

// A thread a cell: its sigma, and its ambient occlusion (the mean, over the six axis directions, of
// the light getting through the hair within gValue2.z cells) into the texture.
[shader("compute")]
[numthreads(GROUP_SIZE, 1, 1)]
void DensityResolveCS(uint3 id : SV_DispatchThreadID) {
    const uint v = id.x + id.y * gDispatch.x;
    if (v >= gFlag.x * gFlag.y * gFlag.z) return;
    const int3 c = int3(int(v % gFlag.x), int((v / gFlag.x) % gFlag.y), int(v / (gFlag.x * gFlag.y)));
    const float sigma = sigmaAt(c);
    const int reach = int(gValue2.z);
    float through = 0.0f;
    [ForceUnroll]
    for (int d = 0; d < 6; ++d) {
        const int3 step = int3(d == 0 ? 1 : d == 1 ? -1 : 0, d == 2 ? 1 : d == 3 ? -1 : 0, d == 4 ? 1 : d == 5 ? -1 : 0);
        float tau = 0.5f * sigma;
        for (int k = 1; k <= reach; ++k) {
            const int3 at = c + step * k;
            if (!inside(at)) break;
            tau += sigmaAt(at);
        }
        through += exp(-tau * gValue.w);
    }
    gVolume[uint3(c)] = float2(sigma, through / 6.0f);
}
