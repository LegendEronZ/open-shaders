#ifndef __FLARE_OCCLUSION_DEPENDENCY_HLSL__
#define __FLARE_OCCLUSION_DEPENDENCY_HLSL__

#include "Common/Random.hlsli"

namespace FlareOcclusion
{
	static const uint SampleCount = 16;
	static const float SampleRadius = 0.02;

	float2 GetSampleOffset(uint index)
	{
		return Random::PoissonSampleOffsets16[index] * SampleRadius;
	}

	float GetVisibility(float visibleSamples)
	{
		return smoothstep(0.0, 1.0, visibleSamples / SampleCount);
	}
}

#endif
