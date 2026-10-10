#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/ReverseZ.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

static const float SkyDepthTolerance = 0.000003;
static const float BillboardRadiusEpsilon = 1e-8;
static const float CompositeDirectionDepth = 0.035;
static const float CompositeSampleSpacing = 6.0;

SamplerState LinearSampler : register(s0);

#if defined(MASK) && defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

Texture2D<float> RaysTexture : register(t0);

cbuffer SunRaysData : register(b1)
{
	float4 LightUV;
	float2 UVScale;
	float2 UVMax;
	float BlurFactor;
	uint StepCount;
	float2 Padding;
	float3 LightDirection;
	float LightBillboardTan;
	float3 RaysColor;
	float MaskBrightness;
	float MaskExponent;
	uint WeightedSteps;
	uint UseNoise;
	float InvScreenWidth;
}

struct VS_OUTPUT_POST
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

float SampleRays(float2 uv, uint eyeIndex)
{
	float2 halfTexel = UVScale - UVMax;
#ifdef VR
	halfTexel.x *= 2.0;
#endif
	uv = clamp(uv, halfTexel / UVScale, 1.0 - halfTexel / UVScale);
	return RaysTexture.SampleLevel(LinearSampler, Stereo::ConvertToStereoUV(uv, eyeIndex) * UVScale, 0);
}

float RadialBlur(float2 uv, float2 target, float noise, uint eyeIndex)
{
	float stepInv = rcp(float(StepCount));
	float shift = -stepInv * noise;
	float rays = 0.0;
	[loop] for (uint i = 0; i < StepCount; i++)
	{
		shift += stepInv;
		float weight = WeightedSteps ? 1.0 - shift : 1.0;
		rays += SampleRays(lerp(uv, target, shift), eyeIndex) * weight;
	}
	return rays * stepInv;
}

float GetNoise(float2 pixel)
{
	return UseNoise ? Random::InterleavedGradientNoise(pixel, SharedData::FrameCount) : 0.5;
}

#if defined(MASK)
float main(VS_OUTPUT_POST input) : SV_Target0
{
	Stereo::EyeUV eye = Stereo::UnpackEyeUV(input.txcoord0);
	float2 uv = eye.uv;
	float depth = SharedData::GetDepth(uv, eye.index);
	bool isSky = abs(depth - FrameBuffer::FarPlaneClipZ(1.0, FrameBuffer::IsReverseProjection(eye.index))) <= SkyDepthTolerance;
	if (!isSky)
		return 0.0;

	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, 0.5, 1);
	float4 positionMS = mul(FrameBuffer::CameraViewProjInverse[eye.index], positionCS);
	float3 viewDirection = normalize(positionMS.xyz / positionMS.w);

	float cosAngle = dot(viewDirection, LightDirection);
	if (cosAngle <= 0.0)
		return 0.0;

	float tanAngleSq = max(1.0 - cosAngle * cosAngle, 0.0) / (cosAngle * cosAngle);
	float radiusSq = tanAngleSq / max(LightBillboardTan * LightBillboardTan, BillboardRadiusEpsilon);
	float mask = MaskBrightness * pow(saturate(1.0 - radiusSq), MaskExponent);

#	if defined(CLOUD_SHADOWS)
	mask *= saturate(1.0 - CloudShadows::CloudShadowsTexture.SampleLevel(LinearSampler, viewDirection, 0));
#	endif

	float2 edge = abs(uv * 2.0 - 1.0);
	float vignette = max(edge.x, edge.y);
	return mask * (1.0 - vignette * vignette);
}
#elif defined(BLUR)
float main(VS_OUTPUT_POST input) : SV_Target0
{
	Stereo::EyeUV eye = Stereo::UnpackEyeUV(input.txcoord0);
	float2 uv = eye.uv;
	return RadialBlur(uv, lerp(uv, eye.index == 0 ? LightUV.xy : LightUV.zw, BlurFactor), GetNoise(eye.uv * float2(rcp(InvScreenWidth), SharedData::BufferDim.y)), eye.index);
}
#elif defined(COMPOSITE)
float4 main(VS_OUTPUT_POST input) : SV_Target0
{
	Stereo::EyeUV eye = Stereo::UnpackEyeUV(input.txcoord0);
	float2 uv = eye.uv;
	float3 direction = normalize(float3((eye.index == 0 ? LightUV.xy : LightUV.zw) - uv, CompositeDirectionDepth));
	float2 target = uv + direction.xy * (InvScreenWidth * float(StepCount) * CompositeSampleSpacing);
	float rays = RadialBlur(uv, target, GetNoise(eye.uv * float2(rcp(InvScreenWidth), SharedData::BufferDim.y)), eye.index);
	return float4(rays * Color::Sky(RaysColor), 1.0);
}
#endif
