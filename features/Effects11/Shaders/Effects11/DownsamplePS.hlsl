#include "Common/VR.hlsli"

Texture2D<float4> SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target
{
	// SourceTexture is the packed side-by-side stereo buffer in VR; sampling the full
	// [0,1] UV here would squish both eyes into this one square downsample instead of
	// producing a coherent single view. Downsampling per eye and blending back in would
	// need this pass (and everything downstream that samples it, including arbitrary
	// user .fx content we can't touch) to be eye-aware, so approximate with eye 0 (left)
	// only -- a single undistorted reference shared across both eyes, same tradeoff this
	// codebase already makes for other expensive per-frame reference textures.
	float2 uv = Stereo::ConvertToStereoUV(input.txcoord0.xy, 0);
	return SourceTexture.SampleLevel(LinearSampler, uv, 0);
}
