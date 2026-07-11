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

	// Variable-resolution tile classes: fraction of the kSHADOWMAPS slice a
	// light rasterizes into (corner-anchored viewport sub-rect). Takes the
	// CLAMPED [0,1] importance (raw importance is an unbounded HDR product,
	// meaningless against absolute boundaries). Promotion is immediate
	// (quality first); demotion requires importance to drop 30% below the
	// boundary so oscillating importance doesn't flip classes and force
	// redraw churn (a class change invalidates the cached shadow map).
	inline float TileScaleForImportance(float importance, float currentScale) noexcept
	{
		constexpr float kFullBoundary = 0.25f;
		constexpr float kHalfBoundary = 0.05f;
		constexpr float kDemoteFactor = 0.7f;

		float target;
		if (importance >= kFullBoundary)
			target = 1.0f;
		else if (importance >= kHalfBoundary)
			target = 0.5f;
		else
			target = 0.25f;

		if (target >= currentScale)
			return target;
		// Demotion path: keep the current class unless clearly below the boundary.
		const float boundary = (currentScale >= 1.0f) ? kFullBoundary : kHalfBoundary;
		return (importance < boundary * kDemoteFactor) ? target : currentScale;
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
