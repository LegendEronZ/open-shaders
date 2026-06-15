// Stereo Shadow Reproject - Class A (view-independent) cross-eye transfer for VR
//
// A screen-space shadow is view-independent: "is this surface occluded from the
// light" depends only on geometry + light, so the value at a world point is
// identical in both eyes. Rather than bilaterally blending two independent per-eye
// estimates (StereoSyncCS), transfer eye 0's shadow into eye 1 exactly by
// reprojection, falling back to eye 1's native shadow only where eye 0 cannot see
// the point (disocclusion). See docs/development/vr-stereo-screen-space.md.
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

	// depth == 0: VR HMD mask; depth == 1: sky/far plane. Nothing to transfer.
	if (depth < 1e-5 || depth >= 1.0) {
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	// Reproject this eye-1 pixel to eye 0.
	Stereo::StereoBilateralResult r = Stereo::ReprojectToOtherEye(uv, depth, eyeIndex, FrameDim);
	if (!r.valid) {
		// Off eye 0's frame: disocclusion, keep eye 1's native shadow.
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	float otherDepth = SrcDepthTexture[r.otherPx];

	// Occluder mismatch (eye 0 sees a different surface at the reprojected point):
	// disocclusion, keep eye 1's native shadow.
	if (otherDepth < 1e-5 || otherDepth >= 1.0 || abs(otherDepth - depth) > kDepthAgreeThreshold) {
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	// Surfaces agree: the shadow is view-independent, so transfer eye 0's value exactly.
	OutShadowTexture[dtid] = SrcShadowTexture[r.otherPx];
}

#endif  // VR
