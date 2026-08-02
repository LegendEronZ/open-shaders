// Deterministic per-tile MIN/MAX depth reduction, replacing ShadowDemandCS's
// single representative-tap read: both extremes together cover a light
// behind near geometry, which a MAX-only or single-tap read would always
// miss. Depth==1 is far (sky) in this engine's convention.

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
	uint4 DepthExtent;  // .xy = active (dynamic-resolution) depth extent in texels
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
	// ClusterSize.xy is the tile count rounded up, so the last row/column of
	// tiles overhangs the depth texture; an out-of-range Load returns 0
	// (nearest depth in this convention), which would pin those tiles at
	// min==0 forever and permanently defeat their zero-demand streak. Clamp
	// to the active render extent, not the texture's full allocation --
	// under dynamic resolution the allocation is larger than what's live
	// this frame, and GetDimensions() would let edge tiles read stale
	// texels outside the active viewport instead of simply skipping them.
	[unroll] for (uint by = 0; by < 8; by++)
	{
		[unroll] for (uint bx = 0; bx < 8; bx++)
		{
			uint2 texel = tileBase + uint2(bx, by);
			if (any(texel >= DepthExtent.xy))
				continue;
			float d = Depth.Load(int3(texel, 0));
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
