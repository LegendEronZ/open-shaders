#include "Common/VR.hlsli"

Texture2D<float> InputTexture : register(t0);
Texture2D<float> LinearDepthTexture : register(t1);
RWTexture2D<float> OutputTexture : register(u0);

#include "Effects11/VolumetricRaysCommon.hlsli"

#define TG_DIM 256
#define WINDOW 12

groupshared float scattering[TG_DIM];
groupshared float linearDepth[TG_DIM];

static const float DepthFalloffStart = 0.1;
static const float DepthFalloffEnd = 0.3;

static const int TapOffsets[5] = { -12, -6, 0, 6, 12 };
static const float TapWeights[5] = { 0.178400, 0.210431, 0.222338, 0.210431, 0.178400 };

#if defined(HORIZONTAL)
[numthreads(TG_DIM, 1, 1)] void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID) {
	int idx = groupThreadId.x;
	int base = idx - WINDOW;
	int2 pixel = int2(groupId.x * (TG_DIM - WINDOW * 2) + base, groupId.y);
#else
[numthreads(1, TG_DIM, 1)] void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID) {
	int idx = groupThreadId.y;
	int base = idx - WINDOW;
	int2 pixel = int2(groupId.x, groupId.y * (TG_DIM - WINDOW * 2) + base);
#endif

	int2 clampedPixel = clamp(pixel, 0, ScreenSizeMin1);
	scattering[idx] = InputTexture[clampedPixel];
	linearDepth[idx] = LinearDepthTexture[clampedPixel];

	GroupMemoryBarrierWithGroupSync();

	if (base < 0 || base >= TG_DIM - WINDOW * 2 || any(pixel > ScreenSizeMin1))
		return;

	float centerDepth = linearDepth[idx];
	float rcpCenterDepth = rcp(max(centerDepth, VolumetricRays::DepthEpsilon));

	float weightedSum = 0.0;
	float weightSum = 0.0;
	[unroll] for (uint i = 0; i < 5; i++)
	{
		int tap = idx + TapOffsets[i];
#if defined(VR) && defined(HORIZONTAL)
		uint eyeIndex = (uint)pixel.x >= ((uint)ScreenSize.x >> 1);
		int2 tapPixel = Stereo::ClampToEyeBounds(pixel + int2(TapOffsets[i], 0), eyeIndex, ScreenSize);
		tap = idx + tapPixel.x - pixel.x;
#endif
		float relativeDelta = abs(linearDepth[tap] - centerDepth) * rcpCenterDepth;
		float weight = TapWeights[i] * (1.0 - smoothstep(DepthFalloffStart, DepthFalloffEnd, relativeDelta));
		weightedSum += weight * scattering[tap];
		weightSum += weight;
	}

	OutputTexture[pixel] = weightedSum / weightSum;
}
