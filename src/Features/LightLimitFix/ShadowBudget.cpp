// ShadowBudget.cpp
// Per-light GPU cost tracking (BudgetTracker) and the frame-time percentile
// helper used by the scheduler's redraw budget and the stats UI. Render cost
// is measured with D3D11 timestamp queries on the GPU timeline (the QPC path
// only measures CPU submit time, which is blind to fill-rate savings such as
// variable-resolution tiles); QPC remains the fallback when queries are
// unavailable or an interval lands disjoint.

#include "../../Globals.h"
#include "../../State.h"
#include "../../Util.h"
#include "ShadowCasterInternal.h"

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
			freq = f.QuadPart / 1000000;
		}

		return t / freq;
	}

	// ---------------------------------------------------------------------
	// GPU timestamp batches
	// ---------------------------------------------------------------------

	struct BudgetGpuTimer
	{
		// One batch per frame; results resolve a few frames later, so keep a
		// small ring in flight. 8 covers any sane GPU queue depth.
		static constexpr int kBatchCount = 8;
		// Redraws per frame are budget-capped far below this; overflow lights
		// silently keep the CPU fallback.
		static constexpr uint32_t kMaxSamplesPerBatch = 64;

		struct Sample
		{
			uint64_t key = 0;
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
			int64_t cpuStart = 0;
			uint32_t fallbackUs = 0;  ///< CPU-measured cost incl. step-0 progress
		};

		struct Batch
		{
			winrt::com_ptr<ID3D11Query> disjoint;
			std::vector<Sample> samples;
			uint32_t used = 0;
			bool inFlight = false;
		};

		Batch batches[kBatchCount];
		int recording = -1;  ///< batch index being recorded, -1 outside a frame
		int cursor = 0;      ///< next batch slot to record into
		bool unavailable = false;

		bool EnsureDisjoint(Batch& batch, int index)
		{
			if (batch.disjoint)
				return true;
			D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			if (FAILED(globals::d3d::device->CreateQuery(&desc, batch.disjoint.put()))) {
				unavailable = true;
				return false;
			}
			Util::SetResourceName(batch.disjoint.get(), "SCM::BudgetDisjoint[%d]", index);
			return true;
		}

		Sample* AcquireSample(Batch& batch, int batchIndex)
		{
			if (batch.used >= kMaxSamplesPerBatch)
				return nullptr;
			if (batch.samples.size() <= batch.used)
				batch.samples.resize(batch.used + 1);
			auto& sample = batch.samples[batch.used];
			if (!sample.begin || !sample.end) {
				D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP, 0 };
				if (FAILED(globals::d3d::device->CreateQuery(&desc, sample.begin.put())) ||
					FAILED(globals::d3d::device->CreateQuery(&desc, sample.end.put()))) {
					unavailable = true;
					return nullptr;
				}
				Util::SetResourceName(sample.begin.get(), "SCM::BudgetTimestamp[%d][%u] begin", batchIndex, batch.used);
				Util::SetResourceName(sample.end.get(), "SCM::BudgetTimestamp[%d][%u] end", batchIndex, batch.used);
			}
			batch.used++;
			return &sample;
		}

		/// Commits every resolved batch into the tracker; leaves unresolved
		/// batches in flight. Returns each batch's samples through
		/// tracker.CommitResolved.
		void Drain(BudgetTracker& tracker)
		{
			auto* context = globals::d3d::context;
			for (auto& batch : batches) {
				if (!batch.inFlight)
					continue;
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT info{};
				const HRESULT hr = context->GetData(batch.disjoint.get(), &info, sizeof(info), D3D11_ASYNC_GETDATA_DONOTFLUSH);
				if (hr == S_FALSE)
					continue;  // still in flight; try again next frame
				for (uint32_t i = 0; i < batch.used; i++) {
					auto& sample = batch.samples[i];
					uint32_t costUs = sample.fallbackUs;
					uint64_t tsBegin = 0, tsEnd = 0;
					// Timestamps issued before the disjoint's End resolve with
					// it; a disjoint interval (clock change) falls back to CPU.
					if (hr == S_OK && !info.Disjoint && info.Frequency &&
						context->GetData(sample.begin.get(), &tsBegin, sizeof(tsBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
						context->GetData(sample.end.get(), &tsEnd, sizeof(tsEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
						tsEnd > tsBegin) {
						costUs = static_cast<uint32_t>(std::min<uint64_t>(
							(tsEnd - tsBegin) * 1000000ull / info.Frequency, 0xFFFFFFFFull));
					}
					tracker.CommitResolved(sample.key, costUs);
				}
				batch.used = 0;
				batch.inFlight = false;
			}
		}
	};

	BudgetTracker::BudgetTracker() = default;
	BudgetTracker::~BudgetTracker() = default;

	void BudgetTracker::BeginRenderBatch()
	{
		if (!_gpu)
			_gpu = std::make_unique<BudgetGpuTimer>();
		if (_gpu->unavailable || !globals::d3d::device || !globals::d3d::context)
			return;

		_gpu->Drain(*this);

		auto& batch = _gpu->batches[_gpu->cursor];
		if (batch.inFlight) {
			// GPU is a full ring behind (or results never resolved); commit
			// the CPU fallbacks rather than stalling on GetData.
			for (uint32_t i = 0; i < batch.used; i++)
				CommitResolved(batch.samples[i].key, batch.samples[i].fallbackUs);
			batch.used = 0;
			batch.inFlight = false;
		}
		if (!_gpu->EnsureDisjoint(batch, _gpu->cursor))
			return;

		globals::d3d::context->Begin(batch.disjoint.get());
		_gpu->recording = _gpu->cursor;
		_gpu->cursor = (_gpu->cursor + 1) % BudgetGpuTimer::kBatchCount;
	}

	void BudgetTracker::EndRenderBatch()
	{
		if (!_gpu || _gpu->recording < 0)
			return;
		auto& batch = _gpu->batches[_gpu->recording];
		globals::d3d::context->End(batch.disjoint.get());
		batch.inFlight = batch.used > 0;
		_gpu->recording = -1;
	}

	void BudgetTracker::CommitResolved(uint64_t key, uint32_t costUs)
	{
		auto& e = _map[key];
		if (!e) {
			e = std::make_unique<BudgetEntry>();
			e->Key = key;
		}
		e->CommitCost(costUs, _counter);
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
			CommitCost(static_cast<uint32_t>(std::min(diff, (int64_t)0xFFFFFFFF)), helperCounter);
		}
	}

	void BudgetEntry::CommitCost(uint32_t costUs, int32_t helperCounter)
	{
		int32_t ix = TrackedCount % kBudgetWindowSize;
		Current -= Tracked[ix];
		Tracked[ix] = costUs;
		Current += Tracked[ix];
		TrackedCount++;
		LastTrackedHelper = helperCounter;
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

		if (step == 1 && _gpu && _gpu->recording >= 0) {
			auto& batch = _gpu->batches[_gpu->recording];
			if (auto* sample = _gpu->AcquireSample(batch, _gpu->recording)) {
				sample->key = key;
				sample->cpuStart = GetPerfCounter();
				globals::d3d::context->End(sample->begin.get());
			}
		}
	}

	void BudgetTracker::EndLight(RE::BSShadowLight* light, int32_t step)
	{
		uint64_t key = reinterpret_cast<uint64_t>(light);
		auto it = _map.find(key);
		if (it == _map.end())
			return;

		if (step == 1 && _gpu && _gpu->recording >= 0) {
			auto& batch = _gpu->batches[_gpu->recording];
			// The last acquired sample belongs to this light: BeginLight/
			// EndLight pairs never nest in the render loop.
			if (batch.used > 0 && batch.samples[batch.used - 1].key == key) {
				auto& sample = batch.samples[batch.used - 1];
				globals::d3d::context->End(sample.end.get());
				const int64_t cpuUs = GetPerfCounter() - sample.cpuStart + it->second->Progress;
				sample.fallbackUs = static_cast<uint32_t>(std::min(cpuUs, (int64_t)0xFFFFFFFF));
				it->second->Progress = 0;
				return;  // ring commit happens when the batch resolves
			}
		}
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
