// Unit tests for Util::GenerationClaim (ClaimCompilation/AddCompletedShader's
// generation-gated decision logic, src/ShaderCache.cpp). Tests instantiate the
// same TryClaim/TryPublish templates production does, over a standalone map.

#include "Utils/GenerationClaim.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <latch>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

	// Wraps the same TryClaim/TryPublish templates in a real mutex + condition variable,
	// wired the same way ClaimCompilation/AddCompletedShader wire them in ShaderCache.cpp.
	// Only real concurrent threads through this lock/wait/notify path can catch a lost
	// wakeup or a claim/publish race that a hand-sequenced test ordering cannot.
	class ThreadSafeClaimTable
	{
	public:
		// a_onWaiting, if set, fires while still holding _mutex, immediately before the
		// wait; lets a caller synchronize on "this thread is actually parked" instead of
		// guessing with a sleep.
		ClaimOutcome ClaimBlocking(const std::string& a_key, std::optional<uint64_t> a_callerGen, uint64_t a_liveGen, TestEntry& a_out,
			const std::function<void()>& a_onWaiting = nullptr)
		{
			std::unique_lock lock{ _mutex };
			for (;;) {
				auto [outcome, it] = Util::GenerationClaim::TryClaim<TestTraits>(_map, a_key, a_callerGen, a_liveGen,
					[](uint64_t a_gen) { return TestEntry{ EntryStatus::Pending, a_gen, false }; });
				if (outcome == ClaimOutcome::MustWait) {
					if (a_onWaiting) {
						a_onWaiting();
					}
					_cv.wait(lock);
					continue;
				}
				a_out = it->second;
				return outcome;
			}
		}

		PublishOutcome PublishNotify(const std::string& a_key, std::optional<uint64_t> a_callerGen, uint64_t a_liveGen, bool a_success)
		{
			PublishOutcome outcome;
			{
				std::unique_lock lock{ _mutex };
				outcome = Util::GenerationClaim::TryPublish<TestTraits>(_map, a_key, a_callerGen, a_liveGen, a_success,
					[](uint64_t a_gen, bool a_ok) { return TestEntry{ a_ok ? EntryStatus::Completed : EntryStatus::Failed, a_gen, a_ok }; });
			}
			// Matches AddCompletedShader exactly: RejectedStale returns silently, with no
			// notify; only a real state change (Published) or a cleanup that freed a
			// waiter's key (RejectedStaleCleanedPending) can have anyone to wake.
			if (outcome != PublishOutcome::RejectedStale) {
				_cv.notify_all();
			}
			return outcome;
		}

		std::optional<TestEntry> Peek(const std::string& a_key)
		{
			std::scoped_lock lock{ _mutex };
			return ::Peek(_map, a_key);
		}

		// Mirrors the physical wipe a real Clear() performs on shaderMap, and like Clear()
		// does NOT notify; any parked waiter on the erased key relies entirely on some
		// other notify_all() eventually reaching it.
		void Erase(const std::string& a_key)
		{
			std::scoped_lock lock{ _mutex };
			_map.erase(a_key);
		}

		// Mirrors ShaderCache::Clear()/ClearShaderMap(): wipe, then notify outside the
		// lock so a waiter parked on the erased key wakes and re-evaluates.
		void ClearAndNotify(const std::string& a_key)
		{
			{
				std::scoped_lock lock{ _mutex };
				_map.erase(a_key);
			}
			_cv.notify_all();
		}

	private:
		std::mutex _mutex;
		std::condition_variable _cv;
		Map _map;
	};
}

TEST_CASE("GenerationClaim: concurrent claimers on the same key, exactly one compiles, the rest wait then hit cache", "[generationclaim][thread]")
{
	constexpr int kThreads = 8;
	ThreadSafeClaimTable table;
	std::latch start{ kThreads };
	std::atomic<int> claimedCount{ 0 };
	std::atomic<int> cacheHitCount{ 0 };

	std::vector<std::thread> threads;
	for (int i = 0; i < kThreads; ++i) {
		threads.emplace_back([&] {
			start.arrive_and_wait();  // maximize actual contention on the same key
			TestEntry entry;
			auto outcome = table.ClaimBlocking("k", std::nullopt, 1, entry);
			if (outcome == ClaimOutcome::Claimed) {
				claimedCount.fetch_add(1);
				std::this_thread::yield();  // simulate compile work before publishing
				table.PublishNotify("k", std::nullopt, 1, true);
			} else {
				cacheHitCount.fetch_add(1);
			}
		});
	}
	for (auto& t : threads) {
		t.join();
	}

	// Exactly one thread must win the claim: a lock/decision race here would let
	// two threads both observe Claimed and compile the same shader twice.
	CHECK(claimedCount.load() == 1);
	CHECK(cacheHitCount.load() == kThreads - 1);
	auto final = table.Peek("k");
	REQUIRE(final.has_value());
	CHECK(final->status == EntryStatus::Completed);
	CHECK(final->hasPayload);
}

TEST_CASE("GenerationClaim: many threads claiming distinct keys never deadlock or drop an entry", "[generationclaim][thread]")
{
	constexpr int kThreads = 16;
	ThreadSafeClaimTable table;
	std::latch start{ kThreads };
	std::vector<std::atomic<bool>> claimedOk(kThreads);
	std::vector<std::atomic<bool>> publishedOk(kThreads);

	std::vector<std::thread> threads;
	for (int i = 0; i < kThreads; ++i) {
		threads.emplace_back([&, i] {
			start.arrive_and_wait();
			std::string key = "k" + std::to_string(i);
			TestEntry entry;
			claimedOk[i].store(table.ClaimBlocking(key, std::nullopt, 1, entry) == ClaimOutcome::Claimed);
			publishedOk[i].store(table.PublishNotify(key, std::nullopt, 1, true) == PublishOutcome::Published);
		});
	}
	for (auto& t : threads) {
		t.join();
	}

	for (int i = 0; i < kThreads; ++i) {
		CHECK(claimedOk[i].load());
		CHECK(publishedOk[i].load());
	}

	for (int i = 0; i < kThreads; ++i) {
		auto entry = table.Peek("k" + std::to_string(i));
		REQUIRE(entry.has_value());
		CHECK(entry->status == EntryStatus::Completed);
	}
}

TEST_CASE("GenerationClaim: a stale publisher racing a fresh reclaimer never corrupts the fresh claim or hangs it", "[generationclaim][thread]")
{
	// A task claims at generation 1; a concurrent Clear() physically wipes the entry
	// and bumps the live generation to 2; a fresh claimer immediately reclaims the
	// key at generation 2 while the stale task is still racing to publish late.
	constexpr int kIterations = 200;
	for (int iter = 0; iter < kIterations; ++iter) {
		ThreadSafeClaimTable table;
		TestEntry ignored;
		table.ClaimBlocking("k", 1, 1, ignored);  // stale task's own claim, generation 1
		table.Erase("k");                         // Clear()'s physical wipe of shaderMap

		std::latch start{ 2 };
		std::atomic<PublishOutcome> staleOutcome{ PublishOutcome::Published };
		std::atomic<ClaimOutcome> freshOutcome{ ClaimOutcome::Claimed };

		std::thread stalePublisher([&] {
			start.arrive_and_wait();
			// The original task finally finishes, still carrying its stale generation-1
			// stamp; live generation is now 2 (Clear() bumped it alongside the wipe above).
			staleOutcome.store(table.PublishNotify("k", 1, 2, true));
		});
		std::thread freshClaimer([&] {
			start.arrive_and_wait();
			TestEntry entry;
			freshOutcome.store(table.ClaimBlocking("k", 2, 2, entry));
		});
		stalePublisher.join();
		freshClaimer.join();

		// Never RejectedStaleCleanedPending here: that would mean the stale publish
		// erased an entry it doesn't own (the fresh claimer's own Pending@2).
		CHECK(staleOutcome.load() == PublishOutcome::RejectedStale);
		CHECK(freshOutcome.load() == ClaimOutcome::Claimed);
		auto entry = table.Peek("k");
		REQUIRE(entry.has_value());
		CHECK(entry->generation == 2);  // the fresh claim's stamp, never overwritten by the stale one
	}
}

TEST_CASE("GenerationClaim: a waiter parked across a Clear() is woken, not stranded", "[generationclaim][thread]")
{
	// Heap-owned via shared_ptr and detached rather than joined: a stack-captured,
	// un-joined thread's destructor calls std::terminate() on a REQUIRE failure below.
	auto table = std::make_shared<ThreadSafeClaimTable>();
	auto waiterOutcome = std::make_shared<std::promise<ClaimOutcome>>();
	auto waiterFuture = waiterOutcome->get_future();
	auto waiterParked = std::make_shared<std::promise<void>>();
	auto waiterParkedFuture = waiterParked->get_future();

	TestEntry ignored;
	table->ClaimBlocking("k", 1, 1, ignored);  // A's own claim, generation 1

	std::thread waiter([table, waiterOutcome, waiterParked] {
		TestEntry entry;
		waiterOutcome->set_value(table->ClaimBlocking("k", 2, 2, entry, [waiterParked] { waiterParked->set_value(); }));
	});
	waiter.detach();

	// Deterministic sync point: block until B has actually reached cv.wait(), not a
	// fixed sleep that could still fire before B is scheduled under CI load.
	REQUIRE(waiterParkedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

	table->ClearAndNotify("k");                                 // Clear()'s wipe + notify
	auto staleOutcome = table->PublishNotify("k", 1, 2, true);  // A's late, stale publish
	CHECK(staleOutcome == PublishOutcome::RejectedStale);       // silent; no notify from this call

	// A bounded wait, not a plain join(): if Clear() ever stops notifying, this fails
	// with a clear timeout instead of hanging the whole test binary.
	REQUIRE(waiterFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	CHECK(waiterFuture.get() == ClaimOutcome::Claimed);
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
	// generation on CacheHit; that forces every other type to needlessly recompile.
	Map map;
	Claim(map, "k", 5, 5);
	Publish(map, "k", 5, 5, true);

	CHECK(Claim(map, "k", 7, 7) == ClaimOutcome::CacheHit);
	CHECK(Peek(map, "k")->generation == 5);  // unchanged, still the original publisher's stamp
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
	// real in-flight work, the exact inverse of the orphan-cleanup bug above.
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
	// stale bytecode through the back door: the durable-stale-hit bug.
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
	// ownership check below stops matching and cleanup never fires: the orphan bug, reintroduced.
	Map map;
	Claim(map, "k", 5, 5);  // Pending@5

	CHECK(Claim(map, "k", 6, 6) == ClaimOutcome::MustWait);
	auto entry = Peek(map, "k");
	REQUIRE(entry.has_value());
	CHECK(entry->generation == 5);  // still 5, MustWait must not touch it

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
	CHECK(Peek(map, "k")->generation == 5);  // stamped with the LIVE value, no captured one exists

	CHECK(Publish(map, "k", std::nullopt, 6, true) == PublishOutcome::Published);
	CHECK(Peek(map, "k")->generation == 6);  // stamped with live at publish time, always "fresh"
}

TEST_CASE("GenerationClaim: a caller can claim even if already stale (documented current behavior)", "[generationclaim]")
{
	// TryClaim has no pre-check against liveGeneration: a caller already stale
	// can still claim; its own eventual stale TryPublish cleans up after itself.
	Map map;
	CHECK(Claim(map, "k", 4, 6) == ClaimOutcome::Claimed);
	auto entry = Peek(map, "k");
	REQUIRE(entry.has_value());
	CHECK(entry->generation == 4);  // stamped with the caller's OWN (stale) value, not live
}

TEST_CASE("GenerationClaim: publish does not require a prior claim (documented current behavior)", "[generationclaim]")
{
	// Not reachable via real call sites today (CompileShader always claims first);
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
