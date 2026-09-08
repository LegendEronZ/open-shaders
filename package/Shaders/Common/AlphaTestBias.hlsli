#ifndef __ALPHA_TEST_BIAS_DEPENDENCY_HLSL__
#define __ALPHA_TEST_BIAS_DEPENDENCY_HLSL__

#include "Common/SharedData.hlsli"

namespace AlphaTestBias
{
	/**
	 * @brief Alpha for an alpha-test discard, sampled at SharedData::AlphaTestMipBias.
	 *
	 * The upscaler biases mips negative for texture detail, which on alpha-tested
	 * foliage also sharpens the cutout into thinner sub-pixel coverage -- the content
	 * that shimmers under temporal reconstruction. Callers pass the alpha they already
	 * sampled at SharedData::MipBias; it is returned unchanged when the two biases
	 * match, so the default configuration costs no extra fetch.
	 */
	float SampleAlpha(Texture2D<float4> a_texture, SamplerState a_sampler, float2 a_uv, float a_colorAlpha)
	{
		if (SharedData::AlphaTestMipBias == SharedData::MipBias)
			return a_colorAlpha;

		return a_texture.SampleBias(a_sampler, a_uv, SharedData::AlphaTestMipBias).w;
	}
}

#endif  // __ALPHA_TEST_BIAS_DEPENDENCY_HLSL__
