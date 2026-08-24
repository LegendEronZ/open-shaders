#pragma once

#include <cstdint>
#include <optional>

namespace Util::GenerationClaim
{
	/**
	 * @brief Pure decision logic behind ShaderCache's generation-gated compile-result cache
	 *        (see ShaderCache::ClaimCompilation / AddCompletedShader). No storage, locking, or
	 *        engine types of its own; the caller supplies the map and wraps calls in a mutex
	 *        (one lock per TryClaim/TryPublish call) and a condition variable. MustWait means
	 *        wait on it, then call TryClaim again; a caller must notify waiters after any
	 *        TryPublish call that returns Published or RejectedStaleCleanedPending.
	 */
	enum class ClaimOutcome
	{
		CacheHit,  ///< A Completed entry with a payload exists; use it.
		Claimed,   ///< Caller now owns this key as Pending; must eventually call TryPublish for it.
		MustWait   ///< Another claimant owns this key as Pending; wait, then call TryClaim again.
	};

	enum class PublishOutcome
	{
		Published,                   ///< Inserted as Completed (success) or Failed (failure).
		RejectedStale,               ///< callerGeneration didn't match liveGeneration; nothing published.
		RejectedStaleCleanedPending  ///< As above; also erased this publisher's own orphaned Pending
									 ///< claim (same key, same generation). Caller must notify waiters.
	};

	/** @brief Decide (and apply) the outcome of a claim attempt. Never blocks.
	 *  Traits needs IsPending(const Entry&), IsCompleted(const Entry&), HasPayload(const Entry&),
	 *  GetGeneration(const Entry&). MakePending(uint64_t generation) -> Entry builds the Pending
	 *  stamp production/tests insert on a Claimed outcome. */
	template <class Traits, class Map, class MakePending>
	std::pair<ClaimOutcome, typename Map::iterator> TryClaim(Map& a_map, const typename Map::key_type& a_key,
		std::optional<uint64_t> a_callerGeneration, uint64_t a_liveGeneration, MakePending&& a_makePending)
	{
		if (auto it = a_map.find(a_key); it != a_map.end()) {
			if (Traits::IsPending(it->second)) {
				return { ClaimOutcome::MustWait, it };
			}
			if (Traits::IsCompleted(it->second) && Traits::HasPayload(it->second)) {
				return { ClaimOutcome::CacheHit, it };
			}
			// Failed, or Completed without a payload: fall through and reclaim as Pending.
		}
		auto inserted = a_map.insert_or_assign(a_key, a_makePending(a_callerGeneration.value_or(a_liveGeneration)));
		return { ClaimOutcome::Claimed, inserted.first };
	}

	/** @brief Decide (and apply) the outcome of a publish attempt. Never blocks.
	 *  MakeEntry(uint64_t generation, bool success) -> Entry builds the Completed/Failed entry
	 *  production/tests insert on a Published outcome. */
	template <class Traits, class Map, class MakeEntry>
	PublishOutcome TryPublish(Map& a_map, const typename Map::key_type& a_key,
		std::optional<uint64_t> a_callerGeneration, uint64_t a_liveGeneration, bool a_success, MakeEntry&& a_makeEntry)
	{
		if (a_callerGeneration && *a_callerGeneration != a_liveGeneration) {
			// Only erase a Pending claim stamped with EXACTLY this caller's own generation,
			// never a newer live claim or a Completed result.
			if (auto it = a_map.find(a_key); it != a_map.end() && Traits::IsPending(it->second) &&
											 Traits::GetGeneration(it->second) == *a_callerGeneration) {
				a_map.erase(it);
				return PublishOutcome::RejectedStaleCleanedPending;
			}
			return PublishOutcome::RejectedStale;
		}
		a_map.insert_or_assign(a_key, a_makeEntry(a_callerGeneration.value_or(a_liveGeneration), a_success));
		return PublishOutcome::Published;
	}
}
