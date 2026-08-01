// Deterministic per-tile MIN/MAX depth reduction: replaces ShadowDemandCS's
// single representative-tap depth read with both the nearest and farthest
// depth actually present anywhere in the 64x64 tile, so thin or off-center
// geometry a lone tap (or a MAX-only reduction) could miss no longer causes
// a false-occluded cluster. A light illuminating only near geometry against
// a distant background reads correctly at the near point even though the
// tile's far point alone would still miss it. Depth==1 is far (sky) in this
// engine's convention.

Texture2D<float> Depth : register(t0);
RWStructuredBuffer<float2> tileDepthRange : register(u0);  // x = min (nearest), y = max (farthest)

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

groupshared float gLocalMin[64];
groupshared float gLocalMax[64];

// One 64-thread group per output tile; each thread owns a distinct 8x8
// sub-block, so every one of the tile's 4096 texels is read exactly once.
[numthreads(8, 8, 1)] void main(
	uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex, uint2 groupThreadId : SV_GroupThreadID) {
	uint2 tileBase = groupId.xy * 64 + groupThreadId * 8;
	float localMin = 1.0;
	float localMax = 0.0;
	[unroll] for (uint by = 0; by < 8; by++)
	{
		[unroll] for (uint bx = 0; bx < 8; bx++)
		{
			float d = Depth.Load(int3(tileBase + uint2(bx, by), 0));
			localMin = min(localMin, d);
			localMax = max(localMax, d);
		}
	}

	gLocalMin[groupIndex] = localMin;
	gLocalMax[groupIndex] = localMax;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint stride = 32; stride > 0; stride >>= 1)
	{
		if (groupIndex < stride) {
			gLocalMin[groupIndex] = min(gLocalMin[groupIndex], gLocalMin[groupIndex + stride]);
			gLocalMax[groupIndex] = max(gLocalMax[groupIndex], gLocalMax[groupIndex + stride]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (groupIndex == 0)
		tileDepthRange[groupId.x + groupId.y * ClusterSize.x] = float2(gLocalMin[0], gLocalMax[0]);
}
