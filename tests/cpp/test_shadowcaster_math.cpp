// Unit tests for ShadowCasterManager pure helpers
// (src/Features/LightLimitFix/ShadowCasterMath.h): the shadow-light pointer
// plausibility check and the frame-time 90th-percentile.

#include "Features/LightLimitFix/ShadowCasterMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using ShadowCasterManager::FrameTimePercentile90;
using ShadowCasterManager::IsPlausibleShadowLightPtr;
using ShadowCasterManager::kShadowSizeToTexels;
using ShadowCasterManager::kTileScaleFloor;
using ShadowCasterManager::TileScaleForCoverage;
using ShadowCasterManager::TileScaleTarget;

namespace
{
	// sizeProxy that asks for exactly `texels` at the classifier's scale.
	float SizeForTexels(float texels)
	{
		return texels / kShadowSizeToTexels;
	}
}

TEST_CASE("TileScaleTarget picks the smallest class meeting the target", "[scm]")
{
	// 2048 base: targets above half-class resolution stay full.
	REQUIRE(TileScaleTarget(SizeForTexels(2000.0f), 2048.0f) == 1.0f);
	REQUIRE(TileScaleTarget(SizeForTexels(1025.0f), 2048.0f) == 1.0f);
	// Exactly satisfiable by a smaller class steps down.
	REQUIRE(TileScaleTarget(SizeForTexels(1024.0f), 2048.0f) == 0.5f);
	REQUIRE(TileScaleTarget(SizeForTexels(512.0f), 2048.0f) == 0.25f);
	// Tiny lights clamp at the ladder floor, never zero.
	REQUIRE(TileScaleTarget(0.0f, 2048.0f) == kTileScaleFloor);
	// The base resolution scales the whole ladder.
	REQUIRE(TileScaleTarget(SizeForTexels(1024.0f), 4096.0f) == 0.25f);
}

TEST_CASE("TileScaleForCoverage promotes immediately", "[scm]")
{
	REQUIRE(TileScaleForCoverage(SizeForTexels(2000.0f), 2048.0f, 0.25f) == 1.0f);
	REQUIRE(TileScaleForCoverage(SizeForTexels(700.0f), 2048.0f, 0.25f) == 0.5f);
}

TEST_CASE("TileScaleForCoverage demotes lazily (hysteresis)", "[scm]")
{
	// Just below the class boundary: the headroom-inflated size still asks
	// for the current class, so it holds (a flip would force a redraw).
	REQUIRE(TileScaleForCoverage(SizeForTexels(900.0f), 2048.0f, 1.0f) == 1.0f);
	// Clearly below even with headroom: demotes to the target class.
	REQUIRE(TileScaleForCoverage(SizeForTexels(600.0f), 2048.0f, 1.0f) == 0.5f);
	REQUIRE(TileScaleForCoverage(SizeForTexels(300.0f), 2048.0f, 0.5f) == 0.25f);
	// The extended ladder reaches the sixteenth class for tiny lights.
	REQUIRE(TileScaleForCoverage(SizeForTexels(100.0f), 2048.0f, 0.5f) == kTileScaleFloor);
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
