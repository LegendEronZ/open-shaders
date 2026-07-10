#pragma once

#include <atomic>
#include <d3d11.h>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <winrt/base.h>

/**
 * @brief GPU and CPU profiler using D3D11 timestamp queries.
 *
 * Maintains a ring buffer of frames with paired begin/end timestamp queries
 * and rolling statistics (average, p95, p99) per named pass. Capture is
 * request-driven: no queries or QPC reads are issued unless a capture is
 * active for the current frame.
 */
class Profiler
{
public:
	static constexpr uint32_t kMaxTimers = 256;
	static constexpr uint32_t kFrameLatency = 3;
	static constexpr uint32_t kHistorySize = 300;

	using PerfEventCallback = std::function<void(std::string_view)>;

	/** @brief Circular buffer tracking per-timer timing samples with statistics. */
	struct RollingHistory
	{
		float history[kHistorySize]{};
		uint32_t head = 0;
		uint32_t count = 0;
		float lastMs = 0.0f;

		/** @brief Appends a timing sample, overwriting the oldest if full. */
		void PushSample(float ms)
		{
			history[head] = ms;
			head = (head + 1) % kHistorySize;
			if (count < kHistorySize)
				count++;
			lastMs = ms;
		}

		/** @brief Gets the arithmetic mean of all buffered samples. */
		float GetAverage() const;

		/**
		 * @brief Gets an interpolated percentile from the buffered samples.
		 * @param p Percentile in [0, 100].
		 */
		float GetPercentile(float p) const;
	};

	/** @brief Snapshot of GPU/CPU timing data for a single named pass. */
	struct TimerResult
	{
		std::string name;
		float gpuTimeMs = 0.0f;
		float avgMs = 0.0f;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		float cpuTimeMs = 0.0f;
		float cpuAvgMs = 0.0f;
		float cpuP95Ms = 0.0f;
		float cpuP99Ms = 0.0f;
		bool hasGpu = false;
		bool hasCpu = false;
		bool activeGpu = false;
		bool activeCpu = false;
		bool valid = false;

		const float* historyBuffer = nullptr;
		uint32_t historyHead = 0;
		uint32_t historyCount = 0;
		const float* cpuHistoryBuffer = nullptr;
		uint32_t cpuHistoryHead = 0;
		uint32_t cpuHistoryCount = 0;

		/**
		 * @brief Gets a GPU history sample by age-ordered index (0 = oldest).
		 * @param index Zero-based index into the history ring buffer.
		 */
		float GetHistorySample(uint32_t index) const
		{
			if (!historyBuffer || index >= historyCount)
				return 0.0f;
			return historyBuffer[(historyHead - historyCount + index + kHistorySize) % kHistorySize];
		}

		/**
		 * @brief Gets a CPU history sample by age-ordered index (0 = oldest).
		 * @param index Zero-based index into the history ring buffer.
		 */
		float GetCpuHistorySample(uint32_t index) const
		{
			if (!cpuHistoryBuffer || index >= cpuHistoryCount)
				return 0.0f;
			return cpuHistoryBuffer[(cpuHistoryHead - cpuHistoryCount + index + kHistorySize) % kHistorySize];
		}
	};

	/**
	 * @brief Creates timestamp query objects and prepares the frame ring buffer.
	 * @param device D3D11 device used to create query objects.
	 * @param context Device context used for issuing and collecting queries.
	 */
	void Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

	/** @brief Releases all D3D11 query objects and resets state. */
	void Release();

	/** @brief Enables or disables runtime profiling; disabling cancels any pending capture. */
	void SetUserEnabled(bool a_enabled);

	/** @brief Gets whether the user has runtime profiling enabled. */
	bool IsUserEnabled() const { return userEnabled.load(std::memory_order_acquire); }

	/** @brief Requests a timing capture for the next frame; consumers must re-request every frame. */
	void RequestCapture();

	/** @brief True while a capture is active; gates all query issuance and CPU timing. */
	bool IsEnabled() const { return IsUserEnabled() && captureActive.load(std::memory_order_acquire); }

	/**
	 * @brief Registers optional callbacks invoked at pass begin/end (e.g. for RenderDoc markers).
	 * @param beginCb Called with the pass name when a pass begins.
	 * @param endCb Called when a pass ends.
	 */
	void SetPerfEventCallbacks(PerfEventCallback beginCb, PerfEventCallback endCb)
	{
		beginPerfEvent = std::move(beginCb);
		endPerfEvent = std::move(endCb);
	}

	/** @brief Begins a new profiling frame; collects results from the oldest in-flight frame. */
	void BeginFrame();

	/**
	 * @brief Begins a GPU pass timer if a capture is active.
	 * @return true if the pass was started and must be closed with EndPass.
	 */
	bool BeginPass(std::string_view name, bool fireCallbacks = true);
	void EndPass(bool fireCallbacks = true);

	/**
	 * @brief Begins a CPU-only timer (no D3D queries) if a capture is active.
	 * @return true if the timer was started and must be closed with EndCpuPass.
	 */
	bool BeginCpuPass(std::string_view name);
	void EndCpuPass();
	void EndFrame();

	/** @brief Gets the per-pass timing results from the last collected frame. */
	const std::vector<TimerResult>& GetResults() const { return results; }

	/** @brief Gets the total GPU time in milliseconds for the last collected frame. */
	float GetTotalTimeMs() const { return totalTimeMs; }

	/** @brief Gets the total CPU time in milliseconds for the last collected frame. */
	float GetCpuTotalTimeMs() const { return cpuTotalTimeMs; }

	/** @brief Resets all timer history and results. */
	void ClearTimers();

	/**
	 * @brief Removes all timers whose names start with the given feature prefix.
	 * @param featureName Feature name; timers matching "featureName::*" are removed.
	 */
	void ClearTimersForFeature(const std::string& featureName);

	/** @brief RAII GPU pass scope; no-op unless a capture is active. */
	class ScopedPass
	{
	public:
		ScopedPass(Profiler* a_profiler, std::string_view a_name)
		{
			if (a_profiler && a_profiler->IsEnabled() && a_profiler->BeginPass(a_name)) {
				profiler = a_profiler;
			}
		}

		~ScopedPass()
		{
			if (profiler) {
				profiler->EndPass();
			}
		}

		ScopedPass(const ScopedPass&) = delete;
		ScopedPass& operator=(const ScopedPass&) = delete;
		ScopedPass(ScopedPass&&) = delete;
		ScopedPass& operator=(ScopedPass&&) = delete;

	private:
		Profiler* profiler = nullptr;
	};

	/** @brief RAII CPU-only pass scope; no-op unless a capture is active. */
	class ScopedCpuPass
	{
	public:
		ScopedCpuPass(Profiler* a_profiler, std::string_view a_name)
		{
			if (a_profiler && a_profiler->IsEnabled() && a_profiler->BeginCpuPass(a_name)) {
				profiler = a_profiler;
			}
		}

		~ScopedCpuPass()
		{
			if (profiler) {
				profiler->EndCpuPass();
			}
		}

		ScopedCpuPass(const ScopedCpuPass&) = delete;
		ScopedCpuPass& operator=(const ScopedCpuPass&) = delete;
		ScopedCpuPass(ScopedCpuPass&&) = delete;
		ScopedCpuPass& operator=(ScopedCpuPass&&) = delete;

	private:
		Profiler* profiler = nullptr;
	};

private:
	struct ActiveTimerData
	{
		float gpuMs = 0.0f;
		float cpuMs = 0.0f;
		bool hasGpu = false;
		bool hasCpu = false;
	};

	struct CompletedCpuTimer
	{
		std::string name;
		float cpuMs = 0.0f;
	};

	struct FrameQueries
	{
		winrt::com_ptr<ID3D11Query> disjoint;
		struct TimerPair
		{
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
			std::string name;
			LARGE_INTEGER cpuBegin{};
			float cpuMs = 0.0f;
			bool ended = false;
		};
		std::vector<TimerPair> timers;
		std::vector<CompletedCpuTimer> cpuTimers;
		std::vector<uint32_t> activeTimerStack;
		uint32_t activeCount = 0;
		bool inFlight = false;
	};

	ID3D11DeviceContext* context = nullptr;

	FrameQueries frames[kFrameLatency];
	uint32_t writeFrame = 0;
	uint32_t readFrame = 0;
	uint32_t framesSinceInit = 0;
	bool initialized = false;
	bool frameActive = false;
	// Enabled by default so profiling pages show data without an extra toggle;
	// zero-overhead idling still holds because captureActive stays false until requested.
	std::atomic_bool userEnabled{ true };
	std::atomic_bool captureRequested{ false };
	std::atomic_bool captureActive{ false };
	double cpuTicksToMs = 0.0;

	PerfEventCallback beginPerfEvent;
	PerfEventCallback endPerfEvent;

	std::vector<TimerResult> results;

	struct CpuTimer
	{
		std::string name;
		LARGE_INTEGER cpuBegin{};
	};

	struct KnownTimer
	{
		std::string name;
		RollingHistory gpu;
		RollingHistory cpu;
		bool hasGpu = false;
		bool hasCpu = false;
	};
	std::vector<KnownTimer> knownTimers;
	std::unordered_map<std::string, size_t> knownTimerIndex;
	std::vector<CpuTimer> activeCpuTimers;
	std::vector<CompletedCpuTimer> completedCpuTimers;
	float totalTimeMs = 0.0f;
	float cpuTotalTimeMs = 0.0f;

	bool CollectResults();
	KnownTimer& GetOrCreateTimer(const std::string& name);
	void RebuildResults(const std::unordered_map<std::string, ActiveTimerData>* activeTimers);
	void StoreCompletedCpuTimers(FrameQueries& frame);
	void ResetFrameState(FrameQueries& frame);
	static bool HasPendingFrameData(const FrameQueries& frame);
};

#define CS_PROFILE_SCOPE_CONCAT_INNER(a, b) a##b
#define CS_PROFILE_SCOPE_CONCAT(a, b) CS_PROFILE_SCOPE_CONCAT_INNER(a, b)

/// Standalone profiler scopes; the including TU must also see Globals.h for globals::profiler.
#define CS_PROFILE_SCOPE(name) Profiler::ScopedPass CS_PROFILE_SCOPE_CONCAT(csProfileScope_, __LINE__)(globals::profiler, name)
#define CS_PROFILE_CPU_SCOPE(name) Profiler::ScopedCpuPass CS_PROFILE_SCOPE_CONCAT(csCpuProfileScope_, __LINE__)(globals::profiler, name)
