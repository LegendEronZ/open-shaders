#pragma once

#include <algorithm>
#include <cstdint>

// Pure helpers extracted from ShadowCasterManager so they can be unit-tested
// without the game/RE runtime.
namespace ShadowCasterManager
{
	// A shadow-light accumulator slot can hold heap garbage between our prepass
	// and the engine's read. Treat a pointer as plausible only if it is at or
	// above the low 64 KiB (the Windows x64 null-guard region is never a valid
	// user mapping, so a near-null garbage value like 0x8 must be rejected --
	// dereferencing it is a guaranteed AV/CTD), inside the x64 user-mode
	// canonical range, and 8-byte aligned (BSShadowLight is pointer-aligned).
	// ForEachShadowLight stops iterating at the first implausible entry rather
	// than dereferencing garbage.
	inline bool IsPlausibleShadowLightPtr(std::uintptr_t raw) noexcept
	{
		return raw >= 0x10000ull && raw < 0x0000800000000000ull && (raw & 0x7) == 0;
	}

	// The tile class ladder, shared by the coverage classifier below and the
	// atlas allocator's order mapping (the floor class == one allocator cell).
	inline constexpr float kTileScaleFull = 1.0f;
	inline constexpr float kTileScaleHalf = 0.5f;
	inline constexpr float kTileScaleQuarter = 0.25f;
	inline constexpr float kTileScaleFloor = kTileScaleQuarter;

	// Shadow texels per unit of projected light size. sizeProxy 1.0 (a light
	// dominating the view, or sitting on the viewer) maps past the full
	// class at any sane base resolution; calibration knob.
	inline constexpr float kShadowSizeToTexels = 3072.0f;

	// Demotion evaluates with the size inflated by this factor, so a light
	// hovering at a class boundary doesn't flip classes (a class change
	// invalidates the cached shadow map and forces a redraw).
	inline constexpr float kDemoteHeadroom = 1.4f;

	/// Smallest ladder class whose resolution still meets the target texel
	/// count for a light of the given projected size.
	inline float TileScaleTarget(float sizeProxy, float baseTileTexels) noexcept
	{
		const float targetTexels = sizeProxy * kShadowSizeToTexels;
		float scale = kTileScaleFull;
		while (scale > kTileScaleFloor && baseTileTexels * scale * 0.5f >= targetTexels)
			scale *= 0.5f;
		return scale;
	}

	// Coverage-driven tile class ("texels per pixel" style). sizeProxy is
	// the light's projected angular diameter, or the attenuation at the
	// viewer for out-of-view lights whose shadows are still visible.
	// Promotion is immediate (quality first); demotion only when even the
	// headroom-inflated size stays below the current class.
	inline float TileScaleForCoverage(float sizeProxy, float baseTileTexels, float currentScale) noexcept
	{
		const float target = TileScaleTarget(sizeProxy, baseTileTexels);
		if (target >= currentScale)
			return target;
		return (TileScaleTarget(sizeProxy * kDemoteHeadroom, baseTileTexels) < currentScale) ? target : currentScale;
	}

	// 90th-percentile of the most-recent min(count, Window) frame-time samples
	// in `ring`. Percentile is order-independent, so the first `n` entries are
	// sampled directly (ring head/wraparound doesn't matter). Returns the 60fps
	// fallback (16.67 ms) before any samples exist. A non-positive count (no
	// samples, or a corrupt/negative value) takes the fallback -- a negative n
	// would otherwise drive std::copy / std::nth_element out of bounds.
	template <int Window>
	inline float FrameTimePercentile90(const float (&ring)[Window], int count)
	{
		if (count <= 0)
			return 16.67f;
		const int n = std::min(count, Window);
		float tmp[Window];
		std::copy(ring, ring + n, tmp);
		const int idx = static_cast<int>(n * 0.9f);
		std::nth_element(tmp, tmp + idx, tmp + n);
		return tmp[idx];
	}
}
