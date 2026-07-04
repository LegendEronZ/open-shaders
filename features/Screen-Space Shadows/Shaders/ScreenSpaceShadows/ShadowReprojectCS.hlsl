// Stereo Shadow Reproject - view-independent cross-eye transfer for VR
//
// A screen-space shadow is view-independent: "is this surface occluded from the
// light" depends only on geometry + light, so the value at a world point is
// identical in both eyes. Rather than bilaterally blending two independent per-eye
// estimates (StereoSyncCS), transfer eye 0's shadow into eye 1 exactly by
// reprojection. Where eye 0 cannot see the point (disocclusion) the pixel keeps
// eye 1's buffer value — the per-frame clear (unshadowed) when the eye-1 march is
// skipped (measured ~0.1% of pixels).
//
// Drop-in replacement for StereoSyncCS (same bindings); selected at dispatch time.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#ifdef VR

#	if defined(TERRAIN_BLENDING)
Texture2D<float> SrcDepthTexture : register(t0);
#	else
Texture2D<unorm float> SrcDepthTexture : register(t0);
#	endif
Texture2D<unorm float> SrcShadowTexture : register(t1);

RWTexture2D<unorm float> OutShadowTexture : register(u0);

cbuffer StereoSyncCB : register(b1)
{
	float2 FrameDim;
	float2 RcpFrameDim;
};

static const float kDepthAgreeThreshold = 0.05;  // NDC depth diff above which the reprojected eye-0 point is a different surface (disocclusion)

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	if (any(dtid >= uint2(FrameDim)))
		return;

	float2 uv = (dtid + 0.5) * RcpFrameDim;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	// Eye 0 is the reference: keep its natively computed shadow unchanged.
	if (eyeIndex == 0) {
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	float depth = SrcDepthTexture[dtid];

	// depth == 0: VR HMD mask; depth == 1: sky/far plane. Not geometry, not a
	// disocclusion — pass through and exclude from the debug view.
	if (depth < 1e-5 || depth >= 1.0) {
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	// Reproject this eye-1 pixel to eye 0 and decide transfer vs disocclusion.
	bool disoccluded = false;
	// Fallback = eye 1's buffer value: the per-frame clear (unshadowed) when the
	// eye-1 march was skipped, or its native march when it ran (bilateral fallback).
	float result = SrcShadowTexture[dtid];

	Stereo::StereoBilateralResult r = Stereo::ReprojectToOtherEye(uv, depth, eyeIndex, FrameDim);
	if (!r.valid) {
		// Off eye 0's frame: eye 0 never saw this point.
		disoccluded = true;
	} else {
		float otherDepth = SrcDepthTexture[r.otherPx];
		if (otherDepth < 1e-5 || otherDepth >= 1.0 || abs(otherDepth - depth) > kDepthAgreeThreshold) {
			// Eye 0 sees a different surface at the reprojected point (occluder mismatch).
			disoccluded = true;
		} else {
			// Surfaces agree: shadow is view-independent, transfer eye 0's value exactly.
			result = SrcShadowTexture[r.otherPx];
		}
	}

#	ifdef DEBUG_DISOCCLUSION
	// Paint true-disocclusion pixels black so the A/B can measure how much of eye 1
	// has no eye-0 data — i.e. how much the unshadowed fallback covers.
	OutShadowTexture[dtid] = disoccluded ? 0.0 : 1.0;
#	else
	OutShadowTexture[dtid] = result;
#	endif
}

#endif  // VR
