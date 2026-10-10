#ifndef EFFECTS11_VOLUMETRIC_RAYS_COMMON_HLSLI
#define EFFECTS11_VOLUMETRIC_RAYS_COMMON_HLSLI

cbuffer VLData : register(b1)
{
	int2 ScreenSize;
	int2 ScreenSizeMin1;
}

namespace VolumetricRays
{
	static const float DepthEpsilon = 1e-4;
}

#endif
