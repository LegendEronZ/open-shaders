#include "AntiLag2.h"

#include <numeric>
#include <utility>

#include "../Upscaling.h"

namespace
{
	// {5083ae5b-8070-4fca-8ee5-3582dd367d13}: FSR frame generation reads this from the swap chain's private data.
	constexpr GUID IID_IFfxAntiLag2Data = { 0x5083ae5b, 0x8070, 0x4fca, { 0x8e, 0xe5, 0x35, 0x82, 0xdd, 0x36, 0x7d, 0x13 } };

	struct FfxAntiLag2Data
	{
		AMD::AntiLag2DX12::Context* context;
		bool enabled;
	};

	// The context FSR receives: it relays FSR's frame-type reports to the driver and timestamps them on the way.
	class FsrReportRelay final : public AMD::AntiLag2DX12::IAmdExtAntiLagApi
	{
	public:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_object) override
		{
			if (!a_object)
				return E_POINTER;
			if (a_riid == __uuidof(IUnknown) || a_riid == __uuidof(AMD::AntiLag2DX12::IAmdExtAntiLagApi)) {
				*a_object = this;
				return S_OK;
			}
			*a_object = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
		ULONG STDMETHODCALLTYPE Release() override { return 1; }

		HRESULT STDMETHODCALLTYPE UpdateAntiLagState(VOID* a_data) override
		{
			return globals::features::upscaling.antiLag2.OnFsrFrameTypeReport(a_data);
		}
	};

	FsrReportRelay fsrReportRelay;

	int64_t QueryQPC()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	float QpcDeltaToMs(int64_t a_delta)
	{
		static const int64_t frequency = [] {
			LARGE_INTEGER qpf;
			QueryPerformanceFrequency(&qpf);
			return qpf.QuadPart;
		}();
		return static_cast<float>(static_cast<double>(a_delta) * 1000.0 / static_cast<double>(frequency));
	}
}

void AntiLag2::LatencyWindow::Add(float a_ms)
{
	// A frame matched across a stall or resize yields nonsense; keep it out of the mean.
	if (a_ms <= 0.0f || a_ms >= 1000.0f)
		return;
	samples[count % kLatencyWindow] = a_ms;
	if (++count >= kLatencyWindow)
		meanMs.store(std::accumulate(samples.begin(), samples.end(), 0.0f) / kLatencyWindow, std::memory_order_relaxed);
}

void AntiLag2::LatencyWindow::Reset()
{
	count = 0;
	meanMs.store(0.0f, std::memory_order_relaxed);
}

void AntiLag2::Initialize(ID3D12Device* a_device)
{
	if (initialized || !a_device || globals::game::isVR)
		return;

	const HRESULT hr = AMD::AntiLag2DX12::Initialize(&context, a_device);
	initialized = hr == S_OK;
	fsrContext.m_pAntiLagAPI = &fsrReportRelay;
	logger::info("[AntiLag2] DX12 initialize: 0x{:08X} ({})", static_cast<uint32_t>(hr), initialized ? "available" : "unavailable");
}

void AntiLag2::Update(bool a_enable)
{
	auto& upscaling = globals::features::upscaling;
	if (!initialized || upscaling.dx12SwapChain.useDLSSG)
		return;

	++pollCount;
	// The engine can poll input more than once per frame; a second Update would split the driver's frame.
	if (!std::exchange(presentedSinceUpdate, false))
		return;

	// Each reading must hold only frames from after the toggle, or on/off comparisons blend together.
	if (a_enable != std::exchange(requested, a_enable)) {
		std::scoped_lock lock(presentMutex);
		presentLatency.Reset();
	}

	const bool active = a_enable && upscaling.ShouldPrepareFrameGeneration();
	const HRESULT hr = AMD::AntiLag2DX12::Update(&context, active, 0);
	enabled = active && SUCCEEDED(hr);
	if (FAILED(hr) && !std::exchange(updateFailureLogged, true))
		logger::warn("[AntiLag2] Update failed: 0x{:08X}", static_cast<uint32_t>(hr));

	lastInputQPC = QueryQPC();
}

void AntiLag2::OnFsrPresent(IDXGISwapChain4* a_swapChain, bool a_frameGeneration)
{
	if (!initialized || !a_swapChain)
		return;

	presentedSinceUpdate = true;
	if (++presentCount == kPollRatioLogFrames)
		logger::info("[AntiLag2] Input polls per presented frame: {:.2f}", static_cast<float>(pollCount) / static_cast<float>(presentCount));

	{
		std::scoped_lock lock(presentMutex);
		// Frames presented without generation never reach the relay, so their inputs must not stay queued.
		if (!a_frameGeneration || queuedInputQPC.size() >= kMaxQueuedPresents) {
			queuedInputQPC.clear();
			firstPresentQPC = 0;
		}
		if (a_frameGeneration)
			queuedInputQPC.push_back(lastInputQPC);
	}

	const FfxAntiLag2Data data{ &fsrContext, true };
	a_swapChain->SetPrivateData(IID_IFfxAntiLag2Data, sizeof(data), &data);

	if (enabled) {
		const HRESULT hr = AMD::AntiLag2DX12::MarkEndOfFrameRendering(&context);
		if (FAILED(hr) && !std::exchange(markFailureLogged, true))
			logger::warn("[AntiLag2] MarkEndOfFrameRendering failed: 0x{:08X}", static_cast<uint32_t>(hr));
	}
}

HRESULT AntiLag2::OnFsrFrameTypeReport(VOID* a_data)
{
	const auto* data = static_cast<const AMD::AntiLag2DX12::APIData_v2*>(a_data);
	if (data && data->uiVersion == 2 && data->uiSize == sizeof(*data) && data->flags.signalFgFrameType)
		RecordPresent(data->flags.isInterpolatedFrame);

	// FSR only reports to an enabled context, so a disabled one must not reach the driver.
	if (!enabled || !context.m_pAntiLagAPI)
		return S_OK;
	return context.m_pAntiLagAPI->UpdateAntiLagState(a_data);
}

void AntiLag2::RecordPresent(bool a_interpolated)
{
	const int64_t now = QueryQPC();

	std::scoped_lock lock(presentMutex);
	if (queuedInputQPC.empty())
		return;

	// The generated frame presented before a real frame already blends that frame's content in.
	if (a_interpolated) {
		if (!firstPresentQPC)
			firstPresentQPC = now;
		return;
	}

	const int64_t presentQPC = firstPresentQPC ? firstPresentQPC : now;
	const int64_t inputQPC = queuedInputQPC.front();
	queuedInputQPC.pop_front();
	firstPresentQPC = 0;
	if (inputQPC)
		presentLatency.Add(QpcDeltaToMs(presentQPC - inputQPC));
}
