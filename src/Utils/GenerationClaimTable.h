#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace Util
{
	/**
	 * @brief Pure decision logic behind ShaderCache's generation-gated compile-result cache
	 *        (see ShaderCache::ClaimCompilation / AddCompletedShader). No locking, no blocking,
	 *        no engine types -- a caller wraps this with an actual mutex (each TryClaim/TryPublish
	 *        call is meant to run under one lock acquisition) and a condition variable (MustWait
	 *        means "wait on it, then call TryClaim again"; a caller must also notify waiters after
	 *        any TryPublish call that returns Published or RejectedStaleCleanedPending -- those are
	 *        the two outcomes that can unblock someone).
	 *
	 *        Ties every cache entry to the generation counter active when it was claimed or
	 *        published, so a compile-worker that finishes after that generation was invalidated
	 *        cannot publish a stale result (durable cache poisoning) and cannot leave its claim
	 *        orphaned (a future request hanging forever on a completion that will never arrive).
	 */
	class GenerationClaimTable
	{
	public:
		enum class EntryStatus
		{
			Absent,
			Pending,
			Completed,
			Failed
		};

		struct Entry
		{
			EntryStatus status = EntryStatus::Absent;
			uint64_t generation = 0;  ///< Generation the claimant (Pending) or publisher (Completed/Failed) captured.
			bool hasPayload = false;  ///< Stand-in for "blob != nullptr". TryPublish always couples
									  ///< status and payload, so Completed with hasPayload == false
									  ///< is unreachable through this class's own API; TryClaim's
									  ///< defensive check for it is untestable without a raw state injector.
		};

		enum class ClaimOutcome
		{
			CacheHit,  ///< A Completed entry with a payload exists -- use it. Trusted regardless of
					   ///< generation until explicitly evicted (see Erase): a read-side freshness
					   ///< re-check here would force every OTHER shader type to recompile whenever
					   ///< any ONE type is cleared, since the generation counter is shared across
					   ///< types but a typed clear only wipes entries of that one type.
			Claimed,   ///< Caller now owns this key as Pending; must eventually call TryPublish for it.
			MustWait   ///< Another claimant owns this key as Pending; wait, then call TryClaim again.
		};

		enum class PublishOutcome
		{
			Published,                   ///< Inserted as Completed (success) or Failed (failure).
			RejectedStale,               ///< callerGeneration didn't match liveGeneration; nothing published.
			RejectedStaleCleanedPending  ///< As above, and this publisher's own orphaned Pending
										 ///< claim (same key, same generation) was removed -- a
										 ///< caller must notify waiters after this outcome.
		};

		/** @brief Decide the outcome of a claim attempt. Never blocks. */
		ClaimOutcome TryClaim(const std::string& a_key, std::optional<uint64_t> a_callerGeneration, uint64_t a_liveGeneration)
		{
			auto it = _entries.find(a_key);
			if (it != _entries.end()) {
				const auto& entry = it->second;
				if (entry.status == EntryStatus::Pending) {
					return ClaimOutcome::MustWait;
				}
				if (entry.status == EntryStatus::Completed && entry.hasPayload) {
					return ClaimOutcome::CacheHit;
				}
				// Failed, or Completed without a payload -- fall through and reclaim as Pending.
			}
			_entries.insert_or_assign(a_key,
				Entry{ EntryStatus::Pending, a_callerGeneration.value_or(a_liveGeneration), false });
			return ClaimOutcome::Claimed;
		}

		/** @brief Decide the outcome of a publish attempt. Never blocks. */
		PublishOutcome TryPublish(const std::string& a_key, std::optional<uint64_t> a_callerGeneration, uint64_t a_liveGeneration, bool a_success)
		{
			if (a_callerGeneration && *a_callerGeneration != a_liveGeneration) {
				// A stale publisher must never touch an entry it doesn't own: only erase a Pending
				// claim stamped with EXACTLY this caller's own (now-invalid) generation. Erasing
				// any other entry -- a newer live claim, or a Completed result -- would cancel or
				// corrupt someone else's real work, not just clean up after this caller.
				auto it = _entries.find(a_key);
				if (it != _entries.end() && it->second.status == EntryStatus::Pending &&
					it->second.generation == *a_callerGeneration) {
					_entries.erase(it);
					return PublishOutcome::RejectedStaleCleanedPending;
				}
				return PublishOutcome::RejectedStale;
			}
			_entries.insert_or_assign(a_key,
				Entry{ a_success ? EntryStatus::Completed : EntryStatus::Failed,
					a_callerGeneration.value_or(a_liveGeneration), a_success });
			return PublishOutcome::Published;
		}

		/** @brief Test/introspection only -- not part of the decision contract real callers use. */
		std::optional<Entry> Peek(const std::string& a_key) const
		{
			auto it = _entries.find(a_key);
			return it != _entries.end() ? std::optional<Entry>{ it->second } : std::nullopt;
		}

		/** @brief Unconditional removal, mirroring the physical wipe a real Clear() performs on the
		 *  storage layer -- no deferred-eviction awareness, no generation check. A caller needing
		 *  scoped/deferred eviction semantics builds that on top; this is the raw primitive a real
		 *  Clear() implementation (and these tests) need underneath it. */
		void Erase(const std::string& a_key)
		{
			_entries.erase(a_key);
		}

	private:
		std::unordered_map<std::string, Entry> _entries;
	};
}
