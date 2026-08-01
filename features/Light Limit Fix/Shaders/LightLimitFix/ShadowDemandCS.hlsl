#include "Common/FrameBuffer.hlsli"
#include "LightLimitFix/Common.hlsli"

// VR: Depth is the double-wide stereo copy (left eye x in [0, dim.x/2), right
// in [dim.x/2, dim.x)) and ClusterSize.xy is already the per-eye (half) tile
// width (set CPU-side), so the tile grid itself is eye-local -- only the
// physical depth texel, the two inverse matrices and positionWS need the eye
// index, matching the split ClusterBuildingCS uses for its own AABB pass.

// Independent of the live installed-slot count (ShadowCasterManager::GetInstalledSlotCount()),
// which the C++ side already clamps shadowMapIndex against; an index beyond this
// fixed cap falls into gOverflowLDS below rather than corrupting a neighboring slot.
#define MAX_SHADOW_DEMAND_SLOTS 128

cbuffer PerFrame : register(b0)
{
	float LightsNear;
	float LightsFar;
	float InvLogFarOverNear;  // 1 / log(LightsFar / LightsNear), precomputed CPU-side
	uint FrameIndex;          // 0 disables the jitter, keeping the tap at the tile centre
	uint4 ClusterSize;
}

Texture2D<float> Depth : register(t0);
StructuredBuffer<LightGrid> lightGridIn : register(t1);
StructuredBuffer<uint> lightIndexListIn : register(t2);
StructuredBuffer<Light> lights : register(t3);

RWStructuredBuffer<uint> demand : register(u0);
RWStructuredBuffer<uint> overflowCount : register(u1);
// [MAX_SHADOW_DEMAND_SLOTS] per-slot maxima plus a trailing cluster-saturation
// flag at [MAX_SHADOW_DEMAND_SLOTS]; the flag rides the same buffer as the
// samples it invalidates so it can never arrive a readback later than they do.
RWStructuredBuffer<uint> demandMax : register(u2);

groupshared uint gDemandLDS[MAX_SHADOW_DEMAND_SLOTS];
groupshared uint gMaxLDS[MAX_SHADOW_DEMAND_SLOTS];
groupshared uint gOverflowLDS;
groupshared uint gSaturatedLDS;

// demandWeight is scaled by this before the atomic add so the uint32 accumulator
// keeps useful fractional precision; matched on readback (C++ side) when turning
// the raw counts back into a diagnostic float.
static const float kDemandScale = 1024.0;

// Per-sample ceiling. InterlockedAdd on uint32 wraps silently and a sum that
// wraps to exactly 0 reads as "light absent", freezing a bright light. At 4K
// (2040 tiles) the worst-case sum is ~8x under 2^32, and 1<<18 == 256 demand
// units stays well above the Phase-1 half-demand constant, so bright-light
// sorting is undistorted.
static const uint kDemandCeiling = 1u << 18;

// Stratified 8-rook base offsets over the 64x64 tile. A fixed centre tap probes
// the same pixel forever, so any lit region between taps is a permanent blind
// spot; rotating a stratified set keeps instantaneous coverage even while the
// per-tile hash below breaks the cross-cycle aliasing.
static const uint2 kJitterOffsets[8] = {
	uint2(4, 4), uint2(12, 36), uint2(20, 20), uint2(28, 52),
	uint2(36, 12), uint2(44, 44), uint2(52, 28), uint2(60, 60)
};

uint DemandTileHash(uint2 tile, uint cycle)
{
	uint h = tile.x * 73856093u ^ tile.y * 19349663u ^ cycle * 83492791u;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return h;
}

// Reprojects one eye's depth tap to view/world space, looks up its cluster's
// light list and accumulates demand for every shadow light found. eyeIndex is
// a compile-time-constant 0 on the non-VR call site, so this inlines down to
// the exact same instructions as the pre-VR single-eye body there.
void AccumulateEyeSample(int2 texel, float2 texcoord, uint eyeIndex, uint2 tileXY)
{
	float depth = Depth.Load(int3(texel, 0));

	float4 clip;
	clip.xy = texcoord * 2.0 - 1.0;
	clip.y *= -1;
	clip.z = depth;
	clip.w = 1.0;

	float4 posVS4 = mul(FrameBuffer::CameraProjInverse[eyeIndex], clip);
	float3 posVS = posVS4.xyz / posVS4.w;
	float4 posWS4 = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], clip);
	float3 posWS = posWS4.xyz / posWS4.w;

	// Same log-space Z slicing ClusterBuildingCS derives its AABBs with
	// (view space is +Z forward here, matching ClusterBuildingCS's own
	// GetPositionVS/IntersectionZPlane); clamp so a sky texel (depth == 1
	// -> viewZ beyond LightsFar) or a near-plane degenerate doesn't
	// produce a NaN/negative log() -> an out-of-range uint cast.
	float viewZ = clamp(posVS.z, LightsNear, LightsFar);
	float zSlice = floor(ClusterSize.z * log(viewZ / LightsNear) * InvLogFarOverNear);
	uint zIndex = (uint)clamp(zSlice, 0.0, float(ClusterSize.z - 1));

	// tileXY (the cluster's XY cell) is eye-invariant: ClusterBuildingCS's VR
	// path unions both eyes' frustum bounds into one shared per-tile AABB. Its
	// Z slice is not -- each eye's own posVS.z can land in a different slice
	// at the same tile, so the two eyes may legitimately read different light
	// lists here.
	uint clusterIndex = tileXY.x + tileXY.y * ClusterSize.x + zIndex * (ClusterSize.x * ClusterSize.y);
	LightGrid grid = lightGridIn[clusterIndex];

	// ClusterCullingCS drops every light past MAX_CLUSTER_LIGHTS, so a light
	// dropped from every cluster it touches reads as zero demand. Flag the
	// frame so the consumer discards the sample instead of trusting it.
	if (grid.lightCount >= MAX_CLUSTER_LIGHTS)
		InterlockedOr(gSaturatedLDS, 1);

	for (uint i = 0; i < grid.lightCount; i++) {
		uint lightIndex = lightIndexListIn[grid.offset + i];
		Light light = lights[lightIndex];
		if (!(light.lightFlags & LightFlags::Shadow))
			continue;

		// Windowed inverse-square falloff (Karis/Lagarde), the same shape the
		// engine's own lighting uses -- demand must track how strongly this
		// light actually reaches this tile, not just that its cluster AABB
		// overlaps it (a large-radius light must not out-score a tight one
		// merely for spanning more tiles).
		float3 toLight = light.positionWS[eyeIndex].xyz - posWS;
		float distSq = dot(toLight, toLight);
		float t = saturate(1.0 - distSq * light.invRadius * light.invRadius);
		float atten = t * t;

		float luminance = dot(light.color, float3(0.2126, 0.7152, 0.0722));
		// NOT multiplied by any shadow-sample result: GetShadowLightShadow
		// returns 0 for occluded, so weighting by it would score an
		// actively-shadowing light as "unused" and freeze it -- the exact
		// artifact this feature exists to avoid (see design doc).
		float demandWeight = luminance * light.fade * atten;

		if (demandWeight <= 0.0)
			continue;

		if (light.shadowMapIndex < MAX_SHADOW_DEMAND_SLOTS) {
			// Same scale for both eyes and both channels -- rescaling here to
			// average VR's two eye-samples would truncate low-demand lights to
			// an exact-zero raw count before the CPU ever sees them. The SUM
			// channel's eye-average normalization happens once, in the CPU-side
			// float readback (LightLimitFix::UpdateShadowDemand), where it costs
			// no precision. The MAX channel needs no normalization at all:
			// InterlockedMax across eyes already yields "visible to either eye"
			// at full scale.
			uint scaled = min((uint)(demandWeight * kDemandScale), kDemandCeiling);
			if (scaled > 0) {
				InterlockedAdd(gDemandLDS[light.shadowMapIndex], scaled);
				// Per-tile MAX, not the sum: a sum dilutes a light strongly
				// present in one tile and absent in the other 509, which is
				// exactly the case a hard skip must never freeze.
				InterlockedMax(gMaxLDS[light.shadowMapIndex], scaled);
			}
		} else {
			InterlockedAdd(gOverflowLDS, 1);
		}
	}
}

[numthreads(16, 16, 1)] void main(
	uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	// The cooperative zero/flush below must run on every thread in the group,
	// including tiles past ClusterSize on a non-64-aligned screen -- do not
	// early-return before the barriers, or out-of-bounds threads leave their
	// LDS slots unzeroed/unflushed and a partial-group barrier is undefined
	// behavior on SM5.
	bool inBounds = all(dispatchThreadId.xy < ClusterSize.xy);

	if (groupIndex < MAX_SHADOW_DEMAND_SLOTS) {
		gDemandLDS[groupIndex] = 0;
		gMaxLDS[groupIndex] = 0;
	}
	if (groupIndex == 0) {
		gOverflowLDS = 0;
		gSaturatedLDS = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	if (inBounds) {
		// ClusterSize covers a padded 64-multiple, so on a non-aligned
		// resolution a tap past the last valid row/column can leave the depth
		// texture; an out-of-range Load reads 0 and fabricates a near-plane
		// hit. Clamped on both paths, not just the jittered one -- the
		// unjittered centre tap (FrameIndex 0, dispatchThreadId.xy*64+32) hits
		// this on any resolution where dim % 64 is in (0, 32].
		uint2 dim;
		Depth.GetDimensions(dim.x, dim.y);
#if defined(VR)
		// Depth is double-wide (left eye then right); clamp the tile-local tap
		// against the PER-EYE width, not the full buffer, and derive both the
		// texcoord and the eventual per-eye Load address from that same
		// clamped value -- clamping against the full width here would let a
		// tile near an eye's right edge sample a texel whose texcoord (still
		// divided by the per-eye ClusterSize) points past the eye boundary.
		uint2 localDim = uint2(dim.x / 2, dim.y);
#else
		uint2 localDim = dim;
#endif
		// FrameIndex 0 is the CPU's "jitter off" sentinel and must reproduce the
		// unjittered centre tap exactly, or enabling the Phase-2 instrumentation
		// would silently move the Phase-1 accumulator's sample set.
		uint2 texelLocal = min(dispatchThreadId.xy * 64 + 32, localDim - 1);
		if (FrameIndex != 0) {
			uint h = DemandTileHash(dispatchThreadId.xy, FrameIndex / 8);
			uint2 jitter = (kJitterOffsets[FrameIndex % 8] + uint2(h & 63, (h >> 6) & 63)) % 64;
			texelLocal = min(dispatchThreadId.xy * 64 + jitter, localDim - 1);
		}
		float2 texcoord = (float2(texelLocal) + 0.5) / (float2(ClusterSize.xy) * 64.0);

#if defined(VR)
		[unroll] for (uint eyeIndex = 0; eyeIndex < 2; eyeIndex++)
		{
			int2 texel = int2(texelLocal.x + eyeIndex * localDim.x, texelLocal.y);
			AccumulateEyeSample(texel, texcoord, eyeIndex, dispatchThreadId.xy);
		}
#else
		AccumulateEyeSample(int2(texelLocal), texcoord, 0, dispatchThreadId.xy);
#endif
	}

	GroupMemoryBarrierWithGroupSync();

	if (groupIndex < MAX_SHADOW_DEMAND_SLOTS && gDemandLDS[groupIndex] > 0)
		InterlockedAdd(demand[groupIndex], gDemandLDS[groupIndex]);
	if (groupIndex < MAX_SHADOW_DEMAND_SLOTS && gMaxLDS[groupIndex] > 0)
		InterlockedMax(demandMax[groupIndex], gMaxLDS[groupIndex]);
	if (groupIndex == 0 && gOverflowLDS > 0)
		InterlockedAdd(overflowCount[0], gOverflowLDS);
	if (groupIndex == 0 && gSaturatedLDS > 0)
		InterlockedOr(demandMax[MAX_SHADOW_DEMAND_SLOTS], 1);
}
