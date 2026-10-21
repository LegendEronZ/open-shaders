#pragma once

#include <array>
#include <atomic>
#include <d3d12.h>
#include <deque>
#include <dxgi1_6.h>
#include <mutex>

#include "AntiLag2/ffx_antilag2_dx12.h"

/**
 * @brief AMD Anti-Lag 2 on the FSR frame generation D3D12 proxy, integrated as in AMD's Cauldron2 sample.
 *
 * Initialized with the proxy's D3D12 device, updated before input polling, and handed to FSR through swap-chain
 * private data so FSR reports generated frames. Radeon RX 5000 (RDNA1) and newer, Adrenalin 24.7.1 or newer.
 */
class AntiLag2
{
public:
	static constexpr UINT kLatencyWindow = 120;

	/** @brief Rolling mean of the last kLatencyWindow samples; readable from any thread. */
	class LatencyWindow
	{
	public:
		void Add(float a_ms);
		void Reset();
		/** @brief 0 until the window has filled once. */
		float GetMeanMs() const { return meanMs.load(std::memory_order_relaxed); }

	private:
		std::array<float, kLatencyWindow> samples{};
		UINT count = 0;
		std::atomic<float> meanMs = 0.0f;
	};

	/** @brief Binds the driver interface to the proxy's D3D12 device. Call once, right after the device is created. */
	void Initialize(ID3D12Device* a_device);

	/**
	 * @brief Inserts the latency-reducing delay and stamps the input time. Call immediately before the engine polls input.
	 *
	 * Reaches the driver once per presented frame however often the engine polls, and only while frame generation
	 * runs: FSR stops reporting frames in menus and loading screens.
	 */
	void Update(bool a_enable);

	/**
	 * @brief FSR proxy present: hands FSR the context, marks the end of rendering, and queues the input time.
	 *
	 * Call after the frame's command lists are executed and before the FSR swap chain's Present.
	 */
	void OnFsrPresent(IDXGISwapChain4* a_swapChain, bool a_frameGeneration);

	/** @brief FSR's frame-type report, called on its presenter thread right before it presents a frame. */
	HRESULT OnFsrFrameTypeReport(VOID* a_data);

	bool IsInitialized() const { return initialized; }
	/** @brief Input to the first present carrying that input: the interpolated frame under frame generation. */
	float GetInputToPresentMs() const { return presentLatency.GetMeanMs(); }

private:
	void RecordPresent(bool a_interpolated);

	bool initialized = false;
	bool requested = false;
	std::atomic<bool> enabled = false;
	bool updateFailureLogged = false;
	bool markFailureLogged = false;
	bool presentedSinceUpdate = true;
	int64_t lastInputQPC = 0;

	static constexpr uint32_t kPollRatioLogFrames = 600;
	uint32_t pollCount = 0;
	uint32_t presentCount = 0;

	AMD::AntiLag2DX12::Context context{};
	AMD::AntiLag2DX12::Context fsrContext{};

	// FSR presents real frames in submission order, so the oldest queued input belongs to the next real present.
	static constexpr size_t kMaxQueuedPresents = 8;
	std::mutex presentMutex;
	std::deque<int64_t> queuedInputQPC;
	int64_t firstPresentQPC = 0;
	LatencyWindow presentLatency;
};
