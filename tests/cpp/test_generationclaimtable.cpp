// Unit tests for the pure decision logic behind ShaderCache's generation-gated
// compile-result cache (see Utils/GenerationClaimTable.h). This matrix was
// independently derived twice by separate reviewers, then adjudicated and
// hand-verified against the real ShaderCache::ClaimCompilation /
// AddCompletedShader. The two real bugs this mechanism exists to prevent --
// durable stale-cache-hit publication, and a caller left waiting forever on
// an orphaned claim -- were both found AFTER an earlier design pass that
// looked complete, so several of these cases exist specifically to catch
// implementer mistakes a first pass would not think to check.

#include "Utils/GenerationClaimTable.h"

#include <catch2/catch_test_macros.hpp>

using Util::GenerationClaimTable;
using ClaimOutcome = GenerationClaimTable::ClaimOutcome;
using EntryStatus = GenerationClaimTable::EntryStatus;
using PublishOutcome = GenerationClaimTable::PublishOutcome;

TEST_CASE("GenerationClaimTable: baseline claim-publish-hit lifecycle", "[generationclaimtable]")
{
	GenerationClaimTable table;
	CHECK(table.TryClaim("k", 5, 5) == ClaimOutcome::Claimed);
	auto pending = table.Peek("k");
	REQUIRE(pending.has_value());
	CHECK(pending->status == EntryStatus::Pending);
	CHECK(pending->generation == 5);

	CHECK(table.TryPublish("k", 5, 5, true) == PublishOutcome::Published);
	auto completed = table.Peek("k");
	REQUIRE(completed.has_value());
	CHECK(completed->status == EntryStatus::Completed);
	CHECK(completed->hasPayload);

	CHECK(table.TryClaim("k", 5, 5) == ClaimOutcome::CacheHit);
}

TEST_CASE("GenerationClaimTable: a Completed entry is trusted across a later generation bump", "[generationclaimtable]")
{
	// Deliberate, not an oversight: Clear(Type) only wipes entries of ONE type, but
	// the generation counter is shared across types. Do NOT "fix" this to re-check
	// generation on CacheHit -- that forces every other type to needlessly recompile.
	GenerationClaimTable table;
	table.TryClaim("k", 5, 5);
	table.TryPublish("k", 5, 5, true);

	CHECK(table.TryClaim("k", 7, 7) == ClaimOutcome::CacheHit);
	CHECK(table.Peek("k")->generation == 5);  // unchanged -- still the original publisher's stamp
}

TEST_CASE("GenerationClaimTable: stale publish cleans up its own orphaned Pending claim", "[generationclaimtable]")
{
	GenerationClaimTable table;
	table.TryClaim("k", 5, 5);

	CHECK(table.TryPublish("k", 5, 6, true) == PublishOutcome::RejectedStaleCleanedPending);
	CHECK_FALSE(table.Peek("k").has_value());
}

TEST_CASE("GenerationClaimTable: cleanup actually reopens the key for a fresh claim", "[generationclaimtable]")
{
	// Cleanup without re-claimability would still be a hang.
	GenerationClaimTable table;
	table.TryClaim("k", 5, 5);
	table.TryPublish("k", 5, 6, true);

	CHECK(table.TryClaim("k", 6, 6) == ClaimOutcome::Claimed);
	CHECK(table.Peek("k")->generation == 6);
}

TEST_CASE("GenerationClaimTable: stale publish must not erase a newer live Pending claim", "[generationclaimtable]")
{
	// The ownership half of the guard: erasing here would cancel someone else's
	// real in-flight work -- the exact inverse of the orphan-cleanup bug above.
	GenerationClaimTable table;
	table.TryClaim("k", 5, 5);
	table.Erase("k");  // simulates the physical wipe a real Clear() performs
	table.TryClaim("k", 6, 6);

	CHECK(table.TryPublish("k", 5, 6, true) == PublishOutcome::RejectedStale);
	auto entry = table.Peek("k");
	REQUIRE(entry.has_value());
	CHECK(entry->status == EntryStatus::Pending);
	CHECK(entry->generation == 6);
}

TEST_CASE("GenerationClaimTable: stale publish on an absent key is fully side-effect-free", "[generationclaimtable]")
{
	// Rejection must not insert. An unconditional insert here would republish
	// stale bytecode through the back door -- the durable-stale-hit bug.
	GenerationClaimTable table;
	table.TryClaim("k", 5, 5);
	table.Erase("k");

	CHECK(table.TryPublish("k", 5, 6, true) == PublishOutcome::RejectedStale);
	CHECK_FALSE(table.Peek("k").has_value());
}

TEST_CASE("GenerationClaimTable: stale publish must not overwrite a Completed entry even with a matching-looking generation", "[generationclaimtable]")
{
	// Isolates the status==Pending half of the ownership guard from the generation
	// half (the previous test attacks the reverse: right status, wrong generation).
	GenerationClaimTable table;
	table.TryClaim("k", 5, 5);
	table.TryPublish("k", 5, 5, true);  // Completed@5

	CHECK(table.TryPublish("k", 5, 6, true) == PublishOutcome::RejectedStale);
	auto entry = table.Peek("k");
	REQUIRE(entry.has_value());
	CHECK(entry->status == EntryStatus::Completed);
	CHECK(entry->generation == 5);
	CHECK(table.TryClaim("k", 6, 6) == ClaimOutcome::CacheHit);
}

TEST_CASE("GenerationClaimTable: MustWait does not restamp the entry it observes", "[generationclaimtable]")
{
	// If MustWait ever mutated the entry it's checking, the stale publisher's
	// ownership check below stops matching and cleanup never fires -- the orphan bug, reintroduced.
	GenerationClaimTable table;
	table.TryClaim("k", 5, 5);  // Pending@5

	CHECK(table.TryClaim("k", 6, 6) == ClaimOutcome::MustWait);
	auto entry = table.Peek("k");
	REQUIRE(entry.has_value());
	CHECK(entry->generation == 5);  // still 5 -- MustWait must not touch it

	CHECK(table.TryPublish("k", 5, 6, true) == PublishOutcome::RejectedStaleCleanedPending);
}

TEST_CASE("GenerationClaimTable: a failed compile also publishes and unblocks waiters", "[generationclaimtable]")
{
	GenerationClaimTable table;
	SECTION("fresh failure is re-claimable")
	{
		table.TryClaim("k", 5, 5);
		CHECK(table.TryPublish("k", 5, 5, false) == PublishOutcome::Published);
		CHECK(table.Peek("k")->status == EntryStatus::Failed);
		CHECK(table.TryClaim("k", 5, 5) == ClaimOutcome::Claimed);
	}
	SECTION("stale failure still cleans up its own orphaned claim")
	{
		// A guard placed only on the success branch would pass every other test here.
		table.TryClaim("k", 5, 5);
		CHECK(table.TryPublish("k", 5, 6, false) == PublishOutcome::RejectedStaleCleanedPending);
		CHECK_FALSE(table.Peek("k").has_value());
	}
}

TEST_CASE("GenerationClaimTable: a caller with no captured generation is always treated as fresh", "[generationclaimtable]")
{
	GenerationClaimTable table;
	CHECK(table.TryClaim("k", std::nullopt, 5) == ClaimOutcome::Claimed);
	CHECK(table.Peek("k")->generation == 5);  // stamped with the LIVE value -- no captured one exists

	CHECK(table.TryPublish("k", std::nullopt, 6, true) == PublishOutcome::Published);
	CHECK(table.Peek("k")->generation == 6);  // stamped with live at publish time -- always "fresh"
}

TEST_CASE("GenerationClaimTable: a caller can claim even if already stale (documented current behavior)", "[generationclaimtable]")
{
	// TryClaim has no pre-check against liveGeneration -- a caller already stale
	// can still claim; its own eventual stale TryPublish cleans up after itself.
	GenerationClaimTable table;
	CHECK(table.TryClaim("k", 4, 6) == ClaimOutcome::Claimed);
	auto entry = table.Peek("k");
	REQUIRE(entry.has_value());
	CHECK(entry->generation == 4);  // stamped with the caller's OWN (stale) value, not live
}

TEST_CASE("GenerationClaimTable: publish does not require a prior claim (documented current behavior)", "[generationclaimtable]")
{
	// Not reachable via real call sites today (CompileShader always claims first) --
	// pins the table's own contract for direct/defensive use, matching AddCompletedShader.
	GenerationClaimTable table;
	CHECK(table.TryPublish("k", 5, 5, true) == PublishOutcome::Published);
	CHECK(table.Peek("k")->status == EntryStatus::Completed);
}

TEST_CASE("GenerationClaimTable: a stale rejection on one key never touches another key's entry", "[generationclaimtable]")
{
	GenerationClaimTable table;
	table.TryClaim("k1", 5, 5);
	table.TryClaim("k2", 5, 5);

	CHECK(table.TryPublish("k1", 5, 6, true) == PublishOutcome::RejectedStaleCleanedPending);
	auto k2 = table.Peek("k2");
	REQUIRE(k2.has_value());
	CHECK(k2->status == EntryStatus::Pending);
	CHECK(k2->generation == 5);
}
