#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#if defined(IBL)
#	define IBL_DEFERRED
#	include "IBL/IBL.hlsli"
#endif

Texture2D<float> BlurredShadowTexture : register(t0);
Texture2D<float> RaymarchDepthTexture : register(t1);

#include "Effects11/VolumetricRaysCommon.hlsli"

static const float UpsampleDepthBias = 0.01;

struct VS_OUTPUT_POST
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

// Depth weighting keeps the half-resolution scattering from bleeding across depth edges.
float UpsampleScattering(float2 fullResPixel, float fullResDepth, uint eyeIndex)
{
	float2 halfPixel = fullResPixel * 0.5 - 0.5;
	int2 basePixel = int2(floor(halfPixel));
	float2 fraction = halfPixel - basePixel;

	const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
	float4 bilinearWeights = float4(
		(1.0 - fraction.x) * (1.0 - fraction.y),
		fraction.x * (1.0 - fraction.y),
		(1.0 - fraction.x) * fraction.y,
		fraction.x * fraction.y);

	float referenceDepth = SharedData::GetScreenDepth(fullResDepth);

	float weightedSum = 0.0;
	float weightSum = 0.0;
	[unroll] for (uint i = 0; i < 4; i++)
	{
		int2 tap = Stereo::ClampToEyeBounds(basePixel + offsets[i], eyeIndex, ScreenSize);
		float tapDepth = RaymarchDepthTexture[tap];
		float relativeDelta = abs(referenceDepth - tapDepth) / max(referenceDepth, VolumetricRays::DepthEpsilon);
		float weight = bilinearWeights[i] * rcp(UpsampleDepthBias + relativeDelta);
		weightedSum += weight * BlurredShadowTexture[tap];
		weightSum += weight;
	}
	return weightedSum / weightSum;
}

float4 main(VS_OUTPUT_POST input) : SV_Target0
{
	Stereo::EyeUV eye = Stereo::UnpackEyeUV(input.txcoord0);
	float depth = SharedData::GetDepth(eye.uv, eye.index);
	float rays = UpsampleScattering(input.pos.xy, depth, eye.index);
	float4 positionCS = float4(2 * float2(eye.uv.x, 1.0 - eye.uv.y) - 1, depth, 1);
	float4 positionMS = mul(FrameBuffer::CameraViewProjInverse[eye.index], positionCS);
	float3 viewDirection = normalize(positionMS.xyz / positionMS.w);
	float phase = dot(viewDirection, SharedData::SunDirection.xyz) * 0.5 + 0.5;
	float3 lightColor = SharedData::SunColor.xyz * phase;

#if defined(IBL)
	float3 ibl = ImageBasedLighting::GetSkyIBL(float3(0, 0, -1));
	ibl = lerp(dot(ibl, 1.0 / 3.0), ibl, 2.0);
	lightColor += ibl * SharedData::enbSettings.VolumetricRaysSkyColorAmount;
#endif

	return float4(rays * lightColor * SharedData::enbSettings.VolumetricRaysIntensity * SharedData::SunColor.w, 1.0);
}
