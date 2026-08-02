// Per-tile min/max depth reduction over the SAME 64x64 tile grid ShadowDemandCS
// clusters against (ClusterSize.xy). Exhaustive: every texel in a tile is read
// exactly once by exactly one of the 64 threads in its group (an 8x8 sub-block
// each), unlike ShadowDemandCS's own tap sampling. Output feeds a sphere-vs-
// visible-depth-slab occlusion test in ShadowDemandCS -- see the comment there
// for why a proximity-only demand metric needed this.
//
// Does NOT see alpha-blended/transparent geometry: the depth source is the
// opaque Z-prepass copy, so a light illuminating only fire/particle/glass in
// front of a distant opaque surface is invisible to this test and can read as
// occluded when it isn't. Bounded by the existing kZeroDemandRedrawIntervalFrames
// backstop in ShadowScheduler.cpp, not fixed here.

#include "LightLimitFix/Common.hlsli"

cbuffer PerFrame : register(b0)
{
	uint4 ClusterSize;
}

Texture2D<float> Depth : register(t0);
RWStructuredBuffer<float2> TileDepthRange : register(u0);  // (nearRaw, farRaw) per (tile, eye)

groupshared float gMin[64];
groupshared float gMax[64];

[numthreads(64, 1, 1)] void main(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex) {
	uint2 tileXY = groupId.xy;
	uint eyeIndex = groupId.z;

	uint2 dim;
	Depth.GetDimensions(dim.x, dim.y);
#if defined(VR)
	uint2 localDim = uint2(dim.x / 2, dim.y);
#else
	uint2 localDim = dim;
#endif

	// This thread's 8x8 sub-block within the tile's 64x64 footprint.
	uint2 subOrigin = uint2((groupIndex % 8) * 8, (groupIndex / 8) * 8);
	float localMin = 1.0;
	float localMax = 0.0;
	[unroll] for (uint y = 0; y < 8; y++)
	{
		[unroll] for (uint x = 0; x < 8; x++)
		{
			uint2 texelLocal = min(tileXY * 64 + subOrigin + uint2(x, y), localDim - 1);
#if defined(VR)
			int2 texel = int2(texelLocal.x + eyeIndex * localDim.x, texelLocal.y);
#else
			int2 texel = int2(texelLocal);
#endif
			float d = Depth.Load(int3(texel, 0));
			localMin = min(localMin, d);
			localMax = max(localMax, d);
		}
	}
	gMin[groupIndex] = localMin;
	gMax[groupIndex] = localMax;

	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0) {
		float tileMin = gMin[0];
		float tileMax = gMax[0];
		[unroll] for (uint i = 1; i < 64; i++)
		{
			tileMin = min(tileMin, gMin[i]);
			tileMax = max(tileMax, gMax[i]);
		}
		if (tileXY.x < ClusterSize.x && tileXY.y < ClusterSize.y) {
			uint tileIndex = eyeIndex * (ClusterSize.x * ClusterSize.y) + tileXY.y * ClusterSize.x + tileXY.x;
			TileDepthRange[tileIndex] = float2(tileMin, tileMax);
		}
	}
}
