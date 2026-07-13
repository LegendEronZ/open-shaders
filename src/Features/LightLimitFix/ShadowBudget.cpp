// ShadowBudget.cpp
// Per-light GPU cost tracking (BudgetTracker) and the frame-time percentile
// helper used by the scheduler's redraw budget and the stats UI.

#include "ShadowCasterInternal.h"

#include <Tracy/Tracy.hpp>  // ZoneScopedN

namespace ShadowCasterManager
{
	float ComputeFrameTimePercentile90()
	{
		return FrameTimePercentile90(s_ftRing, s_ftCount);
	}

	static int64_t GetPerfCounter()
	{
		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);

		int64_t t = (int64_t)counter.QuadPart;

		static int64_t freq = 0;
		if (freq == 0) {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			// Clamp to 1: a sub-1 MHz (or unsupported, 0 Hz) counter would
			// otherwise divide by zero on every call.
			freq = std::max<int64_t>(f.QuadPart / 1000000, 1);
		}

		return t / freq;
	}

	void BudgetEntry::BeginStep(int32_t /*step*/)
	{
		_startTime = GetPerfCounter();
	}

	void BudgetEntry::EndStep(int32_t step, int32_t helperCounter)
	{
		int64_t diff = GetPerfCounter() - _startTime;

		if (step == 0) {
			Progress = static_cast<uint32_t>(std::min(diff, (int64_t)0xFFFFFFFF));
		} else if (step == 1) {
			diff += Progress;
			int32_t ix = TrackedCount % kBudgetWindowSize;
			Current -= Tracked[ix];
			Tracked[ix] = static_cast<uint32_t>(std::min(diff, (int64_t)0xFFFFFFFF));
			Current += Tracked[ix];
			TrackedCount++;
			LastTrackedHelper = helperCounter;
		}
	}

	bool BudgetEntry::IsExpired(int32_t helperCounter) const
	{
		return LastTrackedHelper < 0 || (helperCounter - LastTrackedHelper) >= 600;
	}

	void BudgetTracker::Begin(int32_t step)
	{
		if (step == 0) {
			_counter++;
			// Amortise the GC: a periodic full-map walk that freed every
			// expired BudgetEntry in one frame caused ~10s-cadence stutters
			// (300 frames at 30 fps) because the heap freed dozens of
			// unique_ptr<BudgetEntry> back to back, taking a heap lock for
			// each. Run incrementally every 30 frames (~0.5s at 60fps) and
			// cap erasures per call so the cost spreads across many frames
			// instead of spiking once.
			if ((_counter % 30) == 0)
				CleanupExpired();
		}
	}

	void BudgetTracker::BeginLight(RE::BSShadowLight* light, int32_t step)
	{
		uint64_t key = reinterpret_cast<uint64_t>(light);
		auto& e = _map[key];
		if (!e) {
			e = std::make_unique<BudgetEntry>();
			e->Key = key;
		}
		e->BeginStep(step);
	}

	void BudgetTracker::EndLight(RE::BSShadowLight* light, int32_t step)
	{
		uint64_t key = reinterpret_cast<uint64_t>(light);
		auto it = _map.find(key);
		if (it == _map.end())
			return;
		it->second->EndStep(step, _counter);
	}

	int32_t BudgetTracker::GetCost(RE::BSShadowLight* light) const
	{
		uint64_t key = reinterpret_cast<uint64_t>(light);
		auto it = _map.find(key);
		if (it == _map.end() || it->second->TrackedCount == 0)
			return GetAverageCostUs();  // unknown light: fall back to fleet average
		int32_t n = std::min(kBudgetWindowSize, it->second->TrackedCount);
		return it->second->Current / std::max(1, n);
	}

	void BudgetTracker::CleanupExpired()
	{
		ZoneScopedN("SCM::BudgetTracker::CleanupExpired");
		// Hard cap on erasures per call so a wave of expirations (e.g. the
		// player crossed a cell boundary 600 frames ago and dozens of
		// shadow lights all expire on the same tick) spreads its heap-free
		// cost across many frames instead of stalling one frame. With
		// kMaxErasePerCall=4 and Begin() calling this every 30 frames, the
		// tracker can drain ~8 expired entries per second steady-state and
		// up to 4 per call worst-case -- enough to keep the map bounded
		// in practice without the periodic stutter.
		constexpr size_t kMaxErasePerCall = 4;
		size_t erased = 0;
		for (auto it = _map.begin(); it != _map.end() && erased < kMaxErasePerCall;) {
			if (it->second->IsExpired(_counter)) {
				it = _map.erase(it);
				++erased;
			} else {
				++it;
			}
		}
	}

	int32_t BudgetTracker::GetAverageCostUs() const
	{
		int64_t sum = 0;
		int32_t count = 0;
		for (auto& [k, entry] : _map) {
			int32_t n = std::min(kBudgetWindowSize, entry->TrackedCount);
			if (n == 0)
				continue;
			sum += entry->Current / std::max(1, n);
			count++;
		}
		return count > 0 ? static_cast<int32_t>(sum / count) : 0;
	}
}
