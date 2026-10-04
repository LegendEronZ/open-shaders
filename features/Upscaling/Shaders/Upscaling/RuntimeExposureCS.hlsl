#include "Common/SceneExposure.hlsli"

cbuffer RuntimeExposureCB : register(b0)
{
	float2 LuminanceRange;
	float CompensationScale;
	float ConstantExposure;
	uint UseSceneExposure;
	uint3 pad0;
};

StructuredBuffer<float> AdaptedLuminance : register(t0);
RWTexture2D<float> ExposureOutput : register(u0);

[numthreads(1, 1, 1)] void main() {
	ExposureOutput[uint2(0, 0)] = UseSceneExposure ? SceneExposure::Evaluate(AdaptedLuminance[0], LuminanceRange, CompensationScale) : ConstantExposure;
}
