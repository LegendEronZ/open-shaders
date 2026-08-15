Texture2D<float4> SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target
{
	// SourceTexture is EffectManager::GetTextureOriginal()'s SRV -- kMAIN directly in
	// flatrim, or (in VR) a private texture already cropped to one eye's half by
	// EffectManager::RefreshEyeSourceTexture before this runs. Either way it's already
	// a coherent single view by the time it reaches here, so a plain full-UV sample is
	// correct in both cases; no VR-specific handling needed in this shader.
	return SourceTexture.SampleLevel(LinearSampler, input.txcoord0.xy, 0);
}
