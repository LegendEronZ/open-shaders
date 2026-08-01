// Deterministic per-tile MAX-depth reduction: replaces ShadowDemandCS's single
// representative-tap depth read with the farthest depth actually present
// anywhere in the 64x64 tile, so thin or off-center geometry a lone tap could
// miss no longer causes a false-occluded cluster. Depth==1 is far (sky) in
// this engine's convention, so MAX is the conservative "farthest visible"
// choice -- it can only ever report a tile as more occluded than reality is
// wrong in the safe direction, never the other way.

Texture2D<float> Depth : register(t0);
RWStructuredBuffer<float> tileMaxDepth : register(u0);

// Shares LightLimitFix::ShadowDemandCB's layout (only ClusterSize is used
// here) so both dispatches can bind the same constant buffer.
cbuffer PerFrame : register(b0)
{
	float LightsNear;
	float LightsFar;
	float InvLogFarOverNear;
	float pad0;
	uint4 ClusterSize;
}

groupshared float gLocalMax[64];

// One 64-thread group per output tile; each thread owns a distinct 8x8
// sub-block, so every one of the tile's 4096 texels is read exactly once.
[numthreads(8, 8, 1)] void main(
	uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex, uint2 groupThreadId : SV_GroupThreadID) {
	uint2 tileBase = groupId.xy * 64 + groupThreadId * 8;
	float localMax = 0.0;
	[unroll] for (uint by = 0; by < 8; by++)
		[unroll] for (uint bx = 0; bx < 8; bx++)
			localMax = max(localMax, Depth.Load(int3(tileBase + uint2(bx, by), 0)));

	gLocalMax[groupIndex] = localMax;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint stride = 32; stride > 0; stride >>= 1)
	{
		if (groupIndex < stride)
			gLocalMax[groupIndex] = max(gLocalMax[groupIndex], gLocalMax[groupIndex + stride]);
		GroupMemoryBarrierWithGroupSync();
	}

	if (groupIndex == 0)
		tileMaxDepth[groupId.x + groupId.y * ClusterSize.x] = gLocalMax[0];
}
