// Unit tests for ShadowCasterManager pure helpers
// (src/Features/LightLimitFix/ShadowCasterMath.h): the shadow-light pointer
// plausibility check and the frame-time 90th-percentile.

#include "Features/LightLimitFix/ShadowCasterMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using ShadowCasterManager::FrameTimePercentile90;
using ShadowCasterManager::IsPlausibleShadowLightPtr;
using ShadowCasterManager::TileScaleForImportance;

TEST_CASE("TileScaleForImportance maps importance bands to classes", "[scm]")
{
	// Fresh entries start at 1.0; only genuinely low importance demotes.
	REQUIRE(TileScaleForImportance(0.5f, 1.0f) == 1.0f);
	REQUIRE(TileScaleForImportance(0.25f, 1.0f) == 1.0f);    // at boundary: stays full
	REQUIRE(TileScaleForImportance(0.10f, 0.5f) == 0.5f);    // mid band
	REQUIRE(TileScaleForImportance(0.01f, 0.25f) == 0.25f);  // low band
}

TEST_CASE("TileScaleForImportance promotes immediately", "[scm]")
{
	REQUIRE(TileScaleForImportance(0.30f, 0.25f) == 1.0f);
	REQUIRE(TileScaleForImportance(0.10f, 0.25f) == 0.5f);
}

TEST_CASE("TileScaleForImportance demotes lazily (hysteresis)", "[scm]")
{
	// Just below the 0.25 boundary: hold full class until clearly below
	// boundary * 0.7 so oscillating importance can't flip classes and force
	// redraw churn.
	REQUIRE(TileScaleForImportance(0.20f, 1.0f) == 1.0f);
	REQUIRE(TileScaleForImportance(0.17f, 1.0f) == 0.5f);  // < 0.25*0.7 = 0.175
	// Same band logic at the 0.05 boundary for the half class.
	REQUIRE(TileScaleForImportance(0.04f, 0.5f) == 0.5f);
	REQUIRE(TileScaleForImportance(0.03f, 0.5f) == 0.25f);  // < 0.05*0.7 = 0.035
}

TEST_CASE("IsPlausibleShadowLightPtr rejects null, near-null, misaligned, and non-canonical", "[scm]")
{
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0));                      // null
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0x8));                    // near-null: below the 64 KiB floor
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0xFFF8ull));              // aligned but still below the floor
	REQUIRE(IsPlausibleShadowLightPtr(0x10000ull));                   // at the floor, aligned -> plausible
	REQUIRE(IsPlausibleShadowLightPtr(0x00007FFFFFFFFFF8ull));        // top of user-mode range
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0x0000800000000000ull));  // first non-canonical
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0xFFFFF80000000000ull));  // kernel-space garbage

	// Any non-8-byte alignment is rejected (use a base above the floor so the
	// alignment check is what fails, not the minimum-address check).
	for (std::uintptr_t off = 1; off < 8; ++off)
		REQUIRE_FALSE(IsPlausibleShadowLightPtr(0x10000 + off));
}

TEST_CASE("FrameTimePercentile90 returns the 60fps fallback with no samples", "[scm]")
{
	float ring[8]{};
	REQUIRE(FrameTimePercentile90(ring, 0) == Approx(16.67f));
	// A negative count (corruption / future refactor) must not drive a negative
	// n into std::copy / std::nth_element -- it takes the fallback too.
	REQUIRE(FrameTimePercentile90(ring, -1) == Approx(16.67f));
}

TEST_CASE("FrameTimePercentile90 picks the P90 element", "[scm]")
{
	// 10 samples 1..10: idx = int(10*0.9) = 9 -> the largest sorted element.
	float ring[10] = { 5, 2, 9, 1, 7, 3, 10, 4, 8, 6 };
	REQUIRE(FrameTimePercentile90(ring, 10) == Approx(10.0f));
}

TEST_CASE("FrameTimePercentile90 honors count below the window size", "[scm]")
{
	// Only the first 5 entries are valid; trailing slots must not be sampled.
	float ring[10] = { 10, 20, 30, 40, 50, 999, 999, 999, 999, 999 };
	// n = 5, idx = int(5*0.9) = 4 -> the largest of {10..50} = 50.
	REQUIRE(FrameTimePercentile90(ring, 5) == Approx(50.0f));
}

TEST_CASE("FrameTimePercentile90 clamps count above the window size", "[scm]")
{
	// count > Window: std::min clamps n to Window (10) so the copy stays in
	// bounds; all slots are sampled. Pins the clamp against a regression.
	float ring[10] = { 5, 2, 9, 1, 7, 3, 10, 4, 8, 6 };
	REQUIRE(FrameTimePercentile90(ring, 99) == Approx(10.0f));
}
