#ifndef __SPECULAR_AA_DEPENDENCY_HLSL__
#define __SPECULAR_AA_DEPENDENCY_HLSL__

#include "Common/SharedData.hlsli"

// [Tokuyoshi and Kaplanyan 2019, "Improved Geometric Specular Antialiasing"]
namespace SpecularAA
{
	/// @brief GGX alpha^2 widening from the screen-space variance of the shading normal; call in uniform control flow.
	float Kernel(float3 N)
	{
		float3 du = ddx(N);
		float3 dv = ddy(N);
		float variance = SharedData::specularAASettings.Variance * (dot(du, du) + dot(dv, dv));
		float threshold = SharedData::specularAASettings.MaxAddedRoughness;
		threshold *= threshold;
		threshold *= threshold;
		return SharedData::specularAASettings.Enable ? min(2.0 * variance, threshold) : 0.0;
	}

	/// @brief Filters perceptual roughness (alpha = roughness^2) by the kernel.
	float FilterRoughness(float roughness, float kernel)
	{
		float alpha2 = roughness * roughness;
		alpha2 *= alpha2;
		return sqrt(sqrt(saturate(alpha2 + kernel)));
	}

	/// @brief Filters a Phong exponent through its GGX equivalent alpha^2 = 2 / (shininess + 2).
	float FilterShininess(float shininess, float kernel)
	{
		return max(2.0 / (2.0 / (shininess + 2.0) + kernel) - 2.0, 0.0);
	}
}

#endif  // __SPECULAR_AA_DEPENDENCY_HLSL__
