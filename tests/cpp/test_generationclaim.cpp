// Unit tests for Util::GenerationClaim -- the templated decision logic behind
// ShaderCache's generation-gated compile-result cache (ClaimCompilation /
// AddCompletedShader in src/ShaderCache.cpp). These tests instantiate the exact
// same TryClaim/TryPublish templates production instantiates over shaderMap, via
// a standalone map/traits pair, so a regression in the real decision logic fails
// here too -- there is no separate reference implementation to drift out of sync.
//
// This matrix was independently derived twice by separate reviewers, then
// adjudicated and hand-verified against the real ShaderCache::ClaimCompilation /
// AddCompletedShader. The two real bugs this mechanism exists to prevent --
// durable stale-cache-hit publication, and a caller left waiting forever on an
// orphaned claim -- were both found AFTER an earlier design pass that looked
// complete, so several of these cases exist specifically to catch implementer
// mistakes a first pass would not think to check.

#include "Utils/GenerationClaim.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <unordered_map>

using Util::GenerationClaim::ClaimOutcome;
using Util::GenerationClaim::PublishOutcome;

namespace
{
	enum class EntryStatus
	{
		Pending,
		Completed,
		Failed
	};

	struct TestEntry
	{
		EntryStatus status = EntryStatus::Pending;
		uint64_t generation = 0;
		bool hasPayload = false;
	};

	struct TestTraits
	{
		static bool IsPending(const TestEntry& a_entry) { return a_entry.status == EntryStatus::Pending; }
		static bool IsCompleted(const TestEntry& a_entry) { return a_entry.status == EntryStatus::Completed; }
		static bool HasPayload(const TestEntry& a_entry) { return a_entry.hasPayload; }
		static uint64_t GetGeneration(const TestEntry& a_entry) { return a_entry.generation; }
	};

	using Map = std::unordered_map<std::string, TestEntry>;

	ClaimOutcome Claim(Map& a_map, const std::string& a_key, std::optional<uint64_t> a_callerGen, uint64_t a_liveGen)
	{
		return Util::GenerationClaim::TryClaim<TestTraits>(a_map, a_key, a_callerGen, a_liveGen,
			[](uint64_t a_gen) { return TestEntry{ EntryStatus::Pending, a_gen, false }; })
		    .first;
	}

	PublishOutcome Publish(Map& a_map, const std::string& a_key, std::optional<uint64_t> a_callerGen, uint64_t a_liveGen, bool a_success)
	{
		return Util::GenerationClaim::TryPublish<TestTraits>(a_map, a_key, a_callerGen, a_liveGen, a_success,
			[](uint64_t a_gen, bool a_ok) { return TestEntry{ a_ok ? EntryStatus::Completed : EntryStatus::Failed, a_gen, a_ok }; });
	}

	std::optional<TestEntry> Peek(const Map& a_map, const std::string& a_key)
	{
		auto it = a_map.find(a_key);
		return it != a_map.end() ? std::optional<TestEntry>{ it->second } : std::nullopt;
	}
}

TEST_CASE("GenerationClaim: baseline claim-publish-hit lifecycle", "[generationclaim]")
{
	Map map;
	CHECK(Claim(map, "k", 5, 5) == ClaimOutcome::Claimed);
	auto pending = Peek(map, "k");
	REQUIRE(pending.has_value());
	CHECK(pending->status == EntryStatus::Pending);
	CHECK(pending->generation == 5);

	CHECK(Publish(map, "k", 5, 5, true) == PublishOutcome::Published);
	auto completed = Peek(map, "k");
	REQUIRE(completed.has_value());
	CHECK(completed->status == EntryStatus::Completed);
	CHECK(completed->hasPayload);

	CHECK(Claim(map, "k", 5, 5) == ClaimOutcome::CacheHit);
}

TEST_CASE("GenerationClaim: a Completed entry is trusted across a later generation bump", "[generationclaim]")
{
	// Deliberate, not an oversight: Clear(Type) only wipes entries of ONE type, but
	// the generation counter is shared across types. Do NOT "fix" this to re-check
	// generation on CacheHit -- that forces every other type to needlessly recompile.
	Map map;
	Claim(map, "k", 5, 5);
	Publish(map, "k", 5, 5, true);

	CHECK(Claim(map, "k", 7, 7) == ClaimOutcome::CacheHit);
	CHECK(Peek(map, "k")->generation == 5);  // unchanged -- still the original publisher's stamp
}

TEST_CASE("GenerationClaim: stale publish cleans up its own orphaned Pending claim", "[generationclaim]")
{
	Map map;
	Claim(map, "k", 5, 5);

	CHECK(Publish(map, "k", 5, 6, true) == PublishOutcome::RejectedStaleCleanedPending);
	CHECK_FALSE(Peek(map, "k").has_value());
}

TEST_CASE("GenerationClaim: cleanup actually reopens the key for a fresh claim", "[generationclaim]")
{
	// Cleanup without re-claimability would still be a hang.
	Map map;
	Claim(map, "k", 5, 5);
	Publish(map, "k", 5, 6, true);

	CHECK(Claim(map, "k", 6, 6) == ClaimOutcome::Claimed);
	CHECK(Peek(map, "k")->generation == 6);
}

TEST_CASE("GenerationClaim: stale publish must not erase a newer live Pending claim", "[generationclaim]")
{
	// The ownership half of the guard: erasing here would cancel someone else's
	// real in-flight work -- the exact inverse of the orphan-cleanup bug above.
	Map map;
	Claim(map, "k", 5, 5);
	map.erase("k");  // simulates the physical wipe a real Clear() performs
	Claim(map, "k", 6, 6);

	CHECK(Publish(map, "k", 5, 6, true) == PublishOutcome::RejectedStale);
	auto entry = Peek(map, "k");
	REQUIRE(entry.has_value());
	CHECK(entry->status == EntryStatus::Pending);
	CHECK(entry->generation == 6);
}

TEST_CASE("GenerationClaim: stale publish on an absent key is fully side-effect-free", "[generationclaim]")
{
	// Rejection must not insert. An unconditional insert here would republish
	// stale bytecode through the back door -- the durable-stale-hit bug.
	Map map;
	Claim(map, "k", 5, 5);
	map.erase("k");

	CHECK(Publish(map, "k", 5, 6, true) == PublishOutcome::RejectedStale);
	CHECK_FALSE(Peek(map, "k").has_value());
}

TEST_CASE("GenerationClaim: stale publish must not overwrite a Completed entry even with a matching-looking generation", "[generationclaim]")
{
	// Isolates the status==Pending half of the ownership guard from the generation
	// half (the previous test attacks the reverse: right status, wrong generation).
	Map map;
	Claim(map, "k", 5, 5);
	Publish(map, "k", 5, 5, true);  // Completed@5

	CHECK(Publish(map, "k", 5, 6, true) == PublishOutcome::RejectedStale);
	auto entry = Peek(map, "k");
	REQUIRE(entry.has_value());
	CHECK(entry->status == EntryStatus::Completed);
	CHECK(entry->generation == 5);
	CHECK(Claim(map, "k", 6, 6) == ClaimOutcome::CacheHit);
}

TEST_CASE("GenerationClaim: MustWait does not restamp the entry it observes", "[generationclaim]")
{
	// If MustWait ever mutated the entry it's checking, the stale publisher's
	// ownership check below stops matching and cleanup never fires -- the orphan bug, reintroduced.
	Map map;
	Claim(map, "k", 5, 5);  // Pending@5

	CHECK(Claim(map, "k", 6, 6) == ClaimOutcome::MustWait);
	auto entry = Peek(map, "k");
	REQUIRE(entry.has_value());
	CHECK(entry->generation == 5);  // still 5 -- MustWait must not touch it

	CHECK(Publish(map, "k", 5, 6, true) == PublishOutcome::RejectedStaleCleanedPending);
}

TEST_CASE("GenerationClaim: a failed compile also publishes and unblocks waiters", "[generationclaim]")
{
	Map map;
	SECTION("fresh failure is re-claimable")
	{
		Claim(map, "k", 5, 5);
		CHECK(Publish(map, "k", 5, 5, false) == PublishOutcome::Published);
		CHECK(Peek(map, "k")->status == EntryStatus::Failed);
		CHECK(Claim(map, "k", 5, 5) == ClaimOutcome::Claimed);
	}
	SECTION("stale failure still cleans up its own orphaned claim")
	{
		// A guard placed only on the success branch would pass every other test here.
		Claim(map, "k", 5, 5);
		CHECK(Publish(map, "k", 5, 6, false) == PublishOutcome::RejectedStaleCleanedPending);
		CHECK_FALSE(Peek(map, "k").has_value());
	}
}

TEST_CASE("GenerationClaim: a caller with no captured generation is always treated as fresh", "[generationclaim]")
{
	Map map;
	CHECK(Claim(map, "k", std::nullopt, 5) == ClaimOutcome::Claimed);
	CHECK(Peek(map, "k")->generation == 5);  // stamped with the LIVE value -- no captured one exists

	CHECK(Publish(map, "k", std::nullopt, 6, true) == PublishOutcome::Published);
	CHECK(Peek(map, "k")->generation == 6);  // stamped with live at publish time -- always "fresh"
}

TEST_CASE("GenerationClaim: a caller can claim even if already stale (documented current behavior)", "[generationclaim]")
{
	// TryClaim has no pre-check against liveGeneration -- a caller already stale
	// can still claim; its own eventual stale TryPublish cleans up after itself.
	Map map;
	CHECK(Claim(map, "k", 4, 6) == ClaimOutcome::Claimed);
	auto entry = Peek(map, "k");
	REQUIRE(entry.has_value());
	CHECK(entry->generation == 4);  // stamped with the caller's OWN (stale) value, not live
}

TEST_CASE("GenerationClaim: publish does not require a prior claim (documented current behavior)", "[generationclaim]")
{
	// Not reachable via real call sites today (CompileShader always claims first) --
	// pins the shared contract for direct/defensive use, matching AddCompletedShader.
	Map map;
	CHECK(Publish(map, "k", 5, 5, true) == PublishOutcome::Published);
	CHECK(Peek(map, "k")->status == EntryStatus::Completed);
}

TEST_CASE("GenerationClaim: a stale rejection on one key never touches another key's entry", "[generationclaim]")
{
	Map map;
	Claim(map, "k1", 5, 5);
	Claim(map, "k2", 5, 5);

	CHECK(Publish(map, "k1", 5, 6, true) == PublishOutcome::RejectedStaleCleanedPending);
	auto k2 = Peek(map, "k2");
	REQUIRE(k2.has_value());
	CHECK(k2->status == EntryStatus::Pending);
	CHECK(k2->generation == 5);
}
