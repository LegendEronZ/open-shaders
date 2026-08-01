#include "Common/FrameBuffer.hlsli"
#include "LightLimitFix/Common.hlsli"

// Non-VR only: the depth SRV bound here is the flat (non-stereo-aware) copy and
// ClusterSize.xy is half-width in VR, so a straight texel*64 sample would only
// ever read the left eye. VR needs the same per-eye texcoord split as
// ClusterBuildingCS; deferred to a later phase.

// Independent of the live installed-slot count (ShadowCasterManager::GetInstalledSlotCount()),
// which the C++ side already clamps shadowMapIndex against; an index beyond this
// fixed cap falls into gOverflowLDS below rather than corrupting a neighboring slot.
#define MAX_SHADOW_DEMAND_SLOTS 128

cbuffer PerFrame : register(b0)
{
	float LightsNear;
	float LightsFar;
	float InvLogFarOverNear;  // 1 / log(LightsFar / LightsNear), precomputed CPU-side
	float pad0;
	uint4 ClusterSize;
}

Texture2D<float> Depth : register(t0);
StructuredBuffer<LightGrid> lightGridIn : register(t1);
StructuredBuffer<uint> lightIndexListIn : register(t2);
StructuredBuffer<Light> lights : register(t3);

RWStructuredBuffer<uint> demand : register(u0);
RWStructuredBuffer<uint> overflowCount : register(u1);

groupshared uint gDemandLDS[MAX_SHADOW_DEMAND_SLOTS];
groupshared uint gOverflowLDS;

// demandWeight is scaled by this before the atomic add so the uint32 accumulator
// keeps useful fractional precision; matched on readback (C++ side) when turning
// the raw counts back into a diagnostic float.
static const float kDemandScale = 1024.0;

[numthreads(16, 16, 1)] void main(
	uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	// Every thread must reach the barriers below, including tiles past
	// ClusterSize on a non-64-aligned screen -- a partial-group barrier is
	// undefined behavior on SM5.
	bool inBounds = all(dispatchThreadId.xy < ClusterSize.xy);

	if (groupIndex < MAX_SHADOW_DEMAND_SLOTS)
		gDemandLDS[groupIndex] = 0;
	if (groupIndex == 0)
		gOverflowLDS = 0;
	GroupMemoryBarrierWithGroupSync();

	if (inBounds) {
		uint2 texel = dispatchThreadId.xy * 64 + 32;
		float2 texcoord = (float2(texel) + 0.5) / (float2(ClusterSize.xy) * 64.0);
		float depth = Depth.Load(int3(texel, 0));

		float4 clip;
		clip.xy = texcoord * 2.0 - 1.0;
		clip.y *= -1;
		clip.z = depth;
		clip.w = 1.0;

		float4 posVS4 = mul(FrameBuffer::CameraProjInverse[0], clip);
		float3 posVS = posVS4.xyz / posVS4.w;
		float4 posWS4 = mul(FrameBuffer::CameraViewProjInverse[0], clip);
		float3 posWS = posWS4.xyz / posWS4.w;

		// Same log-space Z slicing ClusterBuildingCS derives its AABBs with;
		// clamp so a sky texel (depth==1) or near-plane degenerate can't
		// produce a NaN/negative log() -> an out-of-range uint cast.
		float viewZ = clamp(posVS.z, LightsNear, LightsFar);
		float zSlice = floor(ClusterSize.z * log(viewZ / LightsNear) * InvLogFarOverNear);
		uint zIndex = (uint)clamp(zSlice, 0.0, float(ClusterSize.z - 1));

		uint clusterIndex = dispatchThreadId.x + dispatchThreadId.y * ClusterSize.x + zIndex * (ClusterSize.x * ClusterSize.y);
		LightGrid grid = lightGridIn[clusterIndex];

		for (uint i = 0; i < grid.lightCount; i++) {
			uint lightIndex = lightIndexListIn[grid.offset + i];
			Light light = lights[lightIndex];
			if (!(light.lightFlags & LightFlags::Shadow))
				continue;

			// Windowed inverse-square falloff (Karis/Lagarde): demand must track
			// how strongly the light reaches this tile, not just AABB overlap.
			float3 toLight = light.positionWS[0].xyz - posWS;
			float distSq = dot(toLight, toLight);
			float t = saturate(1.0 - distSq * light.invRadius * light.invRadius);
			float atten = t * t;

			float luminance = dot(light.color, float3(0.2126, 0.7152, 0.0722));
			// NOT multiplied by any shadow-sample result: weighting by
			// GetShadowLightShadow would score an actively-shadowing light
			// (result 0) as "unused" and freeze it.
			float demandWeight = luminance * light.fade * atten;

			if (demandWeight <= 0.0)
				continue;

			if (light.shadowMapIndex < MAX_SHADOW_DEMAND_SLOTS) {
				uint scaled = (uint)(demandWeight * kDemandScale);
				if (scaled > 0)
					InterlockedAdd(gDemandLDS[light.shadowMapIndex], scaled);
			} else {
				InterlockedAdd(gOverflowLDS, 1);
			}
		}
	}

	GroupMemoryBarrierWithGroupSync();

	if (groupIndex < MAX_SHADOW_DEMAND_SLOTS && gDemandLDS[groupIndex] > 0)
		InterlockedAdd(demand[groupIndex], gDemandLDS[groupIndex]);
	if (groupIndex == 0 && gOverflowLDS > 0)
		InterlockedAdd(overflowCount[0], gOverflowLDS);
}
