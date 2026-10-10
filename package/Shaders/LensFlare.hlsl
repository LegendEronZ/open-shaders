#include "Common/FlareOcclusion.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/ReverseZ.hlsli"

struct VS_INPUT
{
	float4 Position: POSITION0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
};

#if defined(VSHADER)
VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;
	vsout.Position = float4(input.Position.xy, 0.0, 1.0);
	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Visibility: SV_Target0;
};

#if defined(PSHADER)
static const uint MaxLights = 16;

SamplerState DepthSampler : register(s0);
Texture2D<SCENE_DEPTH_FORMAT> DepthTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 ScreenSpaceLightPos[MaxLights];
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	float4 light = ScreenSpaceLightPos[uint(input.Position.x)];
	float visibleSamples = 0.0;
	[unroll] for (uint i = 0; i < FlareOcclusion::SampleCount; ++i)
	{
		float2 sampleUV = light.xy + FlareOcclusion::GetSampleOffset(i);
		if (!FrameBuffer::IsOutsideFrame(sampleUV)) {
			// Engine light depths use the standard projection.
			float depth = FrameBuffer::ToStandardDepth(DepthTex.Sample(DepthSampler, FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(sampleUV)));
			visibleSamples += depth >= light.z;
		}
	}
	psout.Visibility = FlareOcclusion::GetVisibility(visibleSamples);
	return psout;
}
#endif
