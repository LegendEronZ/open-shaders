// Unit tests for the shadow atlas buddy allocator
// (src/Features/LightLimitFix/AtlasAllocator.h).

#include "Features/LightLimitFix/AtlasAllocator.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

using ShadowCasterManager::AtlasAllocator;

TEST_CASE("AtlasAllocator packs full capacity at one order", "[scm][atlas]")
{
	AtlasAllocator a;
	a.Reset(2);  // 4x4 cells
	std::set<std::pair<uint32_t, uint32_t>> seen;
	for (int i = 0; i < 16; i++) {
		auto t = a.Allocate(0);
		REQUIRE(t.valid);
		REQUIRE(seen.insert({ t.x, t.y }).second);  // no overlaps
	}
	REQUIRE_FALSE(a.Allocate(0).valid);  // full
	REQUIRE(a.Occupancy() == Catch::Approx(1.0f));
}

TEST_CASE("AtlasAllocator mixes orders without overlap", "[scm][atlas]")
{
	AtlasAllocator a;
	a.Reset(3);                // 8x8 cells
	auto big = a.Allocate(2);  // 4x4 cells
	REQUIRE(big.valid);
	std::vector<AtlasAllocator::Tile> smalls;
	for (int i = 0; i < 48; i++) {  // remaining 64-16 cells as 1x1
		auto t = a.Allocate(0);
		REQUIRE(t.valid);
		// must not land inside the big tile's span
		const bool inBigX = t.x >= big.x && t.x < big.x + 4;
		const bool inBigY = t.y >= big.y && t.y < big.y + 4;
		REQUIRE_FALSE((inBigX && inBigY));
		smalls.push_back(t);
	}
	REQUIRE_FALSE(a.Allocate(0).valid);
}

TEST_CASE("AtlasAllocator frees and merges buddies", "[scm][atlas]")
{
	AtlasAllocator a;
	a.Reset(2);  // 4x4
	std::vector<AtlasAllocator::Tile> tiles;
	for (int i = 0; i < 16; i++)
		tiles.push_back(a.Allocate(0));
	REQUIRE_FALSE(a.Allocate(2).valid);
	for (auto& t : tiles)
		a.Free(t);
	REQUIRE(a.Occupancy() == Catch::Approx(0.0f));
	// After merging, the whole atlas is one free block again.
	REQUIRE(a.Allocate(2).valid);
}

TEST_CASE("AtlasAllocator handles the full 7-level tree", "[scm][atlas]")
{
	AtlasAllocator a;
	a.Reset(AtlasAllocator::kMaxLevels);  // 128x128 cells
	// One max-order tile fills the whole atlas.
	auto whole = a.Allocate(AtlasAllocator::kMaxLevels);
	REQUIRE(whole.valid);
	REQUIRE(a.Occupancy() == Catch::Approx(1.0f));
	a.Free(whole);
	// Mixed orders spanning the ladder (full=4 .. sixteenth=0 for 2048 base).
	REQUIRE(a.Allocate(4).valid);
	REQUIRE(a.Allocate(3).valid);
	REQUIRE(a.Allocate(0).valid);
	REQUIRE(a.Occupancy() > 0.0f);
}

TEST_CASE("AtlasAllocator reset clears occupancy", "[scm][atlas]")
{
	AtlasAllocator a;
	a.Reset(2);  // 4x4
	for (int i = 0; i < 5; i++)
		REQUIRE(a.Allocate(0).valid);
	REQUIRE(a.Occupancy() > 0.0f);
	// Reset without freeing (the FreeAllTiles path) must not leak used cells.
	a.Reset(2);
	REQUIRE(a.Occupancy() == Catch::Approx(0.0f));
	for (int i = 0; i < 16; i++)
		REQUIRE(a.Allocate(0).valid);
	REQUIRE(a.Occupancy() == Catch::Approx(1.0f));
}

TEST_CASE("AtlasAllocator rejects oversized orders and refills freed space", "[scm][atlas]")
{
	AtlasAllocator a;
	a.Reset(1);                          // 2x2
	REQUIRE_FALSE(a.Allocate(2).valid);  // larger than the atlas
	auto t0 = a.Allocate(1);
	REQUIRE(t0.valid);
	REQUIRE_FALSE(a.Allocate(0).valid);  // full
	a.Free(t0);
	auto t1 = a.Allocate(0);
	REQUIRE(t1.valid);  // splitting works after a full-size free
}
