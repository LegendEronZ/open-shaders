#include "Profiler.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
	constexpr float kMaxSaneProfilerSampleMs = 1000.0f;

	// Guards rolling stats against disjoint-frame spikes and non-finite samples.
	bool IsValidProfilerSample(float ms)
	{
		return std::isfinite(ms) && ms >= 0.0f && ms <= kMaxSaneProfilerSampleMs;
	}
}

float Profiler::RollingHistory::GetAverage() const
{
	if (count == 0)
		return lastMs;
	float sum = 0.0f;
	for (uint32_t i = 0; i < count; i++)
		sum += history[i];
	return sum / static_cast<float>(count);
}

float Profiler::RollingHistory::GetPercentile(float p) const
{
	if (count == 0)
		return lastMs;

	thread_local std::vector<float> sorted;
	sorted.resize(count);
	for (uint32_t i = 0; i < count; i++)
		sorted[i] = history[i];
	std::sort(sorted.begin(), sorted.end());

	float idx = (p / 100.0f) * static_cast<float>(count - 1);
	uint32_t lo = static_cast<uint32_t>(idx);
	uint32_t hi = std::min(lo + 1, count - 1);
	float frac = idx - static_cast<float>(lo);
	return sorted[lo] * (1.0f - frac) + sorted[hi] * frac;
}

void Profiler::Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
{
	Release();

	device = a_device;
	context = a_context;

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	cpuTicksToMs = 1000.0 / static_cast<double>(freq.QuadPart);

	for (auto& frame : frames) {
		frame.batch.Configure(kMaxTimers, "Profiler::Frame");
		frame.batch.Preallocate(a_device);
		frame.timers.resize(kMaxTimers);
		frame.activeStack.clear();
		frame.inFlight = false;
	}

	writeFrame = 0;
	readFrame = 0;
	framesSinceInit = 0;
	frameActive = false;
	initialized = true;
	userEnabled.store(true, std::memory_order_release);
	captureRequested.store(false, std::memory_order_release);
	captureActive.store(false, std::memory_order_release);
}

void Profiler::Release()
{
	for (auto& frame : frames) {
		frame.batch.ReleaseQueries();
		frame.timers.clear();
		frame.activeStack.clear();
		frame.inFlight = false;
	}
	results.clear();
	knownTimers.clear();
	knownTimerIndex.clear();
	totalTimeMs = 0.0f;
	cpuTotalTimeMs = 0.0f;
	frameActive = false;
	initialized = false;
	device = nullptr;
	context = nullptr;
	userEnabled.store(true, std::memory_order_release);
	captureRequested.store(false, std::memory_order_release);
	captureActive.store(false, std::memory_order_release);
}

void Profiler::SetUserEnabled(bool a_enabled)
{
	userEnabled.store(a_enabled, std::memory_order_release);
	if (!a_enabled) {
		captureRequested.store(false, std::memory_order_release);
		captureActive.store(false, std::memory_order_release);
	}
}

void Profiler::RequestCapture()
{
	if (!IsUserEnabled())
		return;
	captureRequested.store(true, std::memory_order_release);
}

void Profiler::BeginFrame()
{
	if (!initialized || !context || frameActive || !IsEnabled())
		return;

	if (!CollectResults())
		return;

	auto& frame = frames[writeFrame];
	frame.batch.Reset();
	frame.activeStack.clear();
	frame.inFlight = true;
	frameActive = true;
	frame.batch.BeginBatch(device, context);
}

bool Profiler::BeginPass(std::string_view name, bool fireCallbacks)
{
	if (!initialized || !context || !IsEnabled())
		return false;

	if (!frameActive)
		BeginFrame();
	if (!frameActive)
		return false;

	auto& frame = frames[writeFrame];
	const int slot = frame.batch.AcquireInterval(device, context);
	if (slot < 0)
		return false;

	auto& timer = frame.timers[slot];
	timer.name = name;
	timer.depth = static_cast<uint32_t>(frame.activeStack.size());
	QueryPerformanceCounter(&timer.cpuBegin);
	frame.activeStack.push_back(slot);

	if (fireCallbacks && beginPerfEvent)
		beginPerfEvent(name);
	return true;
}

void Profiler::EndPass(bool fireCallbacks)
{
	if (!initialized || !context || !frameActive)
		return;

	auto& frame = frames[writeFrame];
	if (frame.activeStack.empty())
		return;

	const int slot = frame.activeStack.back();
	frame.activeStack.pop_back();

	auto& timer = frame.timers[slot];

	LARGE_INTEGER cpuEnd;
	QueryPerformanceCounter(&cpuEnd);
	timer.cpuMs = static_cast<float>(static_cast<double>(cpuEnd.QuadPart - timer.cpuBegin.QuadPart) * cpuTicksToMs);

	frame.batch.CloseInterval(context, static_cast<uint32_t>(slot));

	if (fireCallbacks && endPerfEvent)
		endPerfEvent({});
}

void Profiler::EndFrame()
{
	if (!initialized || !context) {
		captureRequested.store(false, std::memory_order_release);
		captureActive.store(false, std::memory_order_release);
		return;
	}

	// Fully idle: no frame open now and the user has profiling off. Nothing
	// to drain -- publish zero totals so always-visible overlay rows don't
	// show a stale sum, and latch whatever capture state was requested.
	if (!IsUserEnabled() && !frameActive) {
		totalTimeMs = 0.0f;
		cpuTotalTimeMs = 0.0f;
		captureRequested.store(false, std::memory_order_release);
		captureActive.store(false, std::memory_order_release);
		return;
	}

	if (!frameActive) {
		// No frame open this cycle (capture was off, or nothing called
		// BeginPass), but the ring may still hold an older frame's results
		// pending resolution -- keep draining it even while otherwise idle.
		if (!CollectResults()) {
			// Oldest frame's GPU data isn't ready yet; still let capture
			// toggle on/off rather than blocking the latch on it.
			captureActive.store(captureRequested.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
			return;
		}
		totalTimeMs = 0.0f;
		cpuTotalTimeMs = 0.0f;
		// Idle: walk the ring so every slot left over from the capture that
		// just ended drains, instead of re-checking the same already-drained
		// slot forever -- otherwise the next capture's first frames would
		// replay this session's leftover samples as if they were current.
		if (std::ranges::any_of(frames, [](const FrameQueries& f) { return f.inFlight; }))
			writeFrame = (writeFrame + 1) % kFrameLatency;
		captureActive.store(captureRequested.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
		return;
	}

	frameActive = false;
	frames[writeFrame].batch.EndBatch(context);
	writeFrame = (writeFrame + 1) % kFrameLatency;
	framesSinceInit++;
	captureActive.store(captureRequested.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
}

bool Profiler::CollectResults()
{
	if (framesSinceInit < kFrameLatency)
		return true;

	readFrame = writeFrame;
	auto& frame = frames[readFrame];
	if (!frame.inFlight)
		return true;

	struct ActiveTimerData
	{
		float gpuMs = 0.0f;
		float cpuMs = 0.0f;
	};
	std::unordered_map<std::string, ActiveTimerData> activeTimers;
	float activeTotalMs = 0.0f;
	float activeCpuTotalMs = 0.0f;

	const auto status = frame.batch.TryResolve(context,
		[&](uint32_t i, uint64_t deltaTicks, uint64_t frequency) {
			auto& timer = frame.timers[i];
			float ms = static_cast<float>(static_cast<double>(deltaTicks) * 1000.0 / static_cast<double>(frequency));
			if (!IsValidProfilerSample(ms) || !IsValidProfilerSample(timer.cpuMs))
				return;

			auto& entry = activeTimers[timer.name];
			entry.gpuMs += ms;
			entry.cpuMs += timer.cpuMs;
			// Only top-level spans count toward the frame total -- nested
			// passes already fall within their parent's measured time.
			if (timer.depth == 0) {
				activeTotalMs += ms;
				activeCpuTotalMs += timer.cpuMs;
			}

			auto [it, inserted] = knownTimerIndex.try_emplace(timer.name, knownTimers.size());
			if (inserted) {
				KnownTimer kt;
				kt.name = timer.name;
				knownTimers.push_back(std::move(kt));
			}
			auto& known = knownTimers[it->second];
			known.gpu.PushSample(ms);
			known.cpu.PushSample(timer.cpuMs);
		});
	if (status == Util::TimestampQueryBatch::Status::NotReady)
		return false;

	frame.inFlight = false;

	totalTimeMs = activeTotalMs;
	cpuTotalTimeMs = activeCpuTotalMs;

	results.clear();
	results.reserve(knownTimers.size());
	for (const auto& known : knownTimers) {
		TimerResult result;
		result.name = known.name;
		auto it = activeTimers.find(known.name);
		if (it != activeTimers.end()) {
			result.gpuTimeMs = it->second.gpuMs;
			result.cpuTimeMs = it->second.cpuMs;
		} else {
			result.gpuTimeMs = known.gpu.lastMs;
			result.cpuTimeMs = known.cpu.lastMs;
		}
		result.avgMs = known.gpu.GetAverage();
		result.p95Ms = known.gpu.GetPercentile(95.0f);
		result.p99Ms = known.gpu.GetPercentile(99.0f);
		result.cpuAvgMs = known.cpu.GetAverage();
		result.cpuP95Ms = known.cpu.GetPercentile(95.0f);
		result.cpuP99Ms = known.cpu.GetPercentile(99.0f);
		result.valid = true;
		result.historyBuffer = known.gpu.history;
		result.historyHead = known.gpu.head;
		result.historyCount = known.gpu.count;
		results.push_back(std::move(result));
	}
	return true;
}
