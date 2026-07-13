// ShadowAtlas.cpp
// Tier 2 shadow storage: one SCM-owned depth atlas holding variable-size tiles instead of a kSHADOWMAPS slice per light.

#include <d3d11_1.h>

#include <DirectXTex.h>

#include <filesystem>
#include <fstream>

#include "../../Globals.h"
#include "../../State.h"
#include "../../Util.h"
#include "AtlasAllocator.h"
#include "ShadowCasterInternal.h"
#include "ShadowCasterMath.h"

namespace ShadowCasterManager
{
	namespace
	{
		struct SlotTile
		{
			AtlasAllocator::Tile tile;
			float scale = 0.0f;        ///< class the tile was allocated for
			uint32_t renderFrame = 0;  ///< frame stamp of the last raster into the tile
			bool valid = false;        ///< content rendered at least once
		};

		std::atomic<uint32_t> s_clearsSwallowed{ 0 };
		std::atomic<uint32_t> s_clearsPassed{ 0 };
		std::atomic<uint32_t> s_tileClears{ 0 };
		std::atomic<uint32_t> s_tileReallocs{ 0 };

		struct AtlasState
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11DepthStencilView> dsv;
			winrt::com_ptr<ID3D11DepthStencilView> dsvReadOnly;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11DeviceContext1> context1;
			AtlasAllocator allocator;
			std::vector<SlotTile> slots;
			uint32_t dim = 0;
			uint32_t baseTile = 0;       ///< full-class tile size (engine slice res)
			uint32_t cell = 0;           ///< allocator cell size (quarter class)
			uint32_t levels = 0;         ///< buddy levels the allocator was built with
			uint32_t bytesPerPixel = 0;  ///< from the depth format (2 or 4)
			bool ready = false;
			bool failed = false;
		};
		AtlasState s_atlas;

		// Clamp a requested atlas resolution to the legal range and snap it
		// down to a buddy-aligned size for the given cell geometry.
		uint32_t SnapAtlasDim(uint32_t requested, uint32_t baseTile, uint32_t cell, uint32_t& levelsOut)
		{
			const uint32_t clamped = std::clamp(requested, baseTile, kAtlasMaxResolution);
			uint32_t levels = 0;
			while ((cell << (levels + 1)) <= clamped && levels < AtlasAllocator::kMaxLevels)
				levels++;
			levelsOut = levels;
			return cell << levels;
		}

		// Depth formats pair as (typeless resource, DSV format, SRV format).
		bool DepthFormats(DXGI_FORMAT sliceFormat, DXGI_FORMAT& tex, DXGI_FORMAT& dsv, DXGI_FORMAT& srv)
		{
			switch (sliceFormat) {
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_UNORM:
				tex = DXGI_FORMAT_R16_TYPELESS;
				dsv = DXGI_FORMAT_D16_UNORM;
				srv = DXGI_FORMAT_R16_UNORM;
				return true;
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
				tex = DXGI_FORMAT_R32_TYPELESS;
				dsv = DXGI_FORMAT_D32_FLOAT;
				srv = DXGI_FORMAT_R32_FLOAT;
				return true;
			default:
				return false;  // stencil-packed slices are not atlas-tileable
			}
		}

		// The engine full-surface-clears its depth target before each shadow
		// light; on the shared atlas DSV that wipes every other light's tile
		// (only the last-rendered light keeps shadow). Do not remove: tiles
		// are rect-cleared per redraw, and non-atlas views pass through.
		//
		// Detours a function's own prologue (via stl::detour_vfunc, matching
		// Globals.cpp's ClearDepthStencilView hook on the same vtable slot)
		// instead of overwriting the vtable slot in place: a raw slot
		// overwrite corrupts RenderDoc's own dispatch bookkeeping on that
		// COM object and deadlocks the render thread once RenderDoc is
		// attached. Detours composes correctly with RenderDoc's interception
		// and with any other hook already chained onto this slot.
		struct Hook_ClearDepthStencilView
		{
			static void thunk(ID3D11DeviceContext* self, ID3D11DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil)
			{
				if (view && (view == s_atlas.dsv.get() || view == s_atlas.dsvReadOnly.get())) {
					s_clearsSwallowed.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				s_clearsPassed.fetch_add(1, std::memory_order_relaxed);
				func(self, view, flags, depth, stencil);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void InstallClearHook(ID3D11DeviceContext* context)
		{
			static bool installed = false;
			if (installed)
				return;
			installed = true;
			stl::detour_vfunc<53, Hook_ClearDepthStencilView>(context);
		}

		// Functional ClearView probe: rect-clear a tiny depth texture and read
		// it back. Some runtimes accept the call but ignore depth views; only
		// the readback proves the path works.
		bool ProbeClearViewDepth(ID3D11Device* device, ID3D11DeviceContext1* context1)
		{
			constexpr uint32_t kProbeDim = 16;
			winrt::com_ptr<ID3D11Texture2D> tex, staging;
			winrt::com_ptr<ID3D11DepthStencilView> dsv;

			DXGI_FORMAT texFmt, dsvFmt, srvFmt;
			DepthFormats(DXGI_FORMAT_R16_TYPELESS, texFmt, dsvFmt, srvFmt);

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = kProbeDim;
			desc.MipLevels = desc.ArraySize = 1;
			desc.Format = texFmt;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, tex.put())))
				return false;
			Util::SetResourceName(tex.get(), "SCM::AtlasProbe");
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = dsvFmt;
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			if (FAILED(device->CreateDepthStencilView(tex.get(), &dsvDesc, dsv.put())))
				return false;
			Util::SetResourceName(dsv.get(), "SCM::AtlasProbe DSV");

			context1->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
			const FLOAT one[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
			D3D11_RECT rect{ kProbeDim / 2, kProbeDim / 2, kProbeDim, kProbeDim };
			context1->ClearView(dsv.get(), one, &rect, 1);

			desc.BindFlags = 0;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put())))
				return false;
			Util::SetResourceName(staging.get(), "SCM::AtlasProbe Staging");
			context1->CopyResource(staging.get(), tex.get());

			// Bounded poll, not a blocking Map: a fence that never signals (seen
			// on VR the first time a shadow tile is needed mid-session) must
			// degrade to a failed probe, not hang the render thread forever.
			D3D11_MAPPED_SUBRESOURCE mapped{};
			HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;
			for (int i = 0; i < 1000 && hr == DXGI_ERROR_WAS_STILL_DRAWING; i++) {
				hr = context1->Map(staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (FAILED(hr))
				return false;
			auto row = [&](uint32_t y) { return reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch); };
			const bool outside = row(2)[2] == 0;
			const bool inside = row(kProbeDim - 4)[kProbeDim - 4] == 0xFFFF;
			context1->Unmap(staging.get(), 0);
			return outside && inside;
		}

		bool EnsureResources()
		{
			if (s_atlas.ready)
				return true;
			if (s_atlas.failed)
				return false;

			auto* device = globals::d3d::device;
			auto* context = globals::d3d::context;
			if (!device || !context)
				return false;

			// Follow the engine's slice format and resolution so atlas tiles
			// keep the exact precision the array slices had.
			const auto info = GetVRAMInfo();
			if (!info.valid || info.shadowWidth == 0) {
				return false;  // kSHADOWMAPS not readable yet; retry next frame
			}

			auto fail = [&](const char* why) {
				s_atlas.failed = true;
				logger::warn("[SCM] Shadow atlas disabled: {}", why);
				return false;
			};

			if (FAILED(context->QueryInterface(s_atlas.context1.put())))
				return fail("D3D11.1 context unavailable (ClearView needed for tile clears)");

			D3D11_TEXTURE2D_DESC sliceDesc{};
			DXGI_FORMAT texFmt, dsvFmt, srvFmt;
			{
				// Format from the live texture when possible; D16 fallback
				// matches the INI-fallback path in GetVRAMInfo.
				DXGI_FORMAT sliceFmt = DXGI_FORMAT_R16_TYPELESS;
				if (TryReadShadowTextureDesc(sliceDesc))
					sliceFmt = sliceDesc.Format;
				if (!DepthFormats(sliceFmt, texFmt, dsvFmt, srvFmt))
					return fail("unsupported kSHADOWMAPS format for tiling");
			}

			if (!ProbeClearViewDepth(device, s_atlas.context1.get()))
				return fail("ClearView depth-rect probe failed on this driver");

			s_atlas.baseTile = info.shadowWidth;
			s_atlas.cell = std::max(1u, static_cast<uint32_t>(s_atlas.baseTile * kTileScaleFloor));
			uint32_t levels = 0;
			s_atlas.dim = SnapAtlasDim(s_settings.AtlasResolution, s_atlas.baseTile, s_atlas.cell, levels);
			s_atlas.levels = levels;
			s_atlas.bytesPerPixel = (texFmt == DXGI_FORMAT_R32_TYPELESS) ? 4u : 2u;
			s_atlas.allocator.Reset(levels);

			// Raw creation instead of the Buffer.h Texture2D wrapper: the
			// wrapper throws on failure (this path must fail-latch gracefully)
			// and has no read-only DSV slot.
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = s_atlas.dim;
			desc.MipLevels = desc.ArraySize = 1;
			desc.Format = texFmt;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, s_atlas.texture.put())))
				return fail("atlas texture creation failed");
			Util::SetResourceName(s_atlas.texture.get(), "SCM::ShadowAtlas");

			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = dsvFmt;
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			if (FAILED(device->CreateDepthStencilView(s_atlas.texture.get(), &dsvDesc, s_atlas.dsv.put())))
				return fail("atlas DSV creation failed");
			Util::SetResourceName(s_atlas.dsv.get(), "SCM::ShadowAtlas DSV");
			dsvDesc.Flags = D3D11_DSV_READ_ONLY_DEPTH;
			if (FAILED(device->CreateDepthStencilView(s_atlas.texture.get(), &dsvDesc, s_atlas.dsvReadOnly.put())))
				return fail("atlas read-only DSV creation failed");
			Util::SetResourceName(s_atlas.dsvReadOnly.get(), "SCM::ShadowAtlas DSV (read-only)");

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = srvFmt;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			if (FAILED(device->CreateShaderResourceView(s_atlas.texture.get(), &srvDesc, s_atlas.srv.put())))
				return fail("atlas SRV creation failed");
			Util::SetResourceName(s_atlas.srv.get(), "SCM::ShadowAtlas SRV");

			InstallClearHook(context);

			// Whole-atlas clear once at creation so never-rendered regions read
			// as far depth (fully lit) rather than driver garbage. ClearView,
			// not ClearDepthStencilView: the hook above swallows the latter
			// for atlas views.
			const FLOAT farDepth[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
			D3D11_RECT all{ 0, 0, static_cast<LONG>(s_atlas.dim), static_cast<LONG>(s_atlas.dim) };
			s_atlas.context1->ClearView(s_atlas.dsv.get(), farDepth, &all, 1);

			s_atlas.slots.assign(static_cast<size_t>(std::max(s_lights.Size, 1)), {});
			const uint32_t capacity = s_atlas.allocator.CellsPerAxis() * s_atlas.allocator.CellsPerAxis();
			if (capacity < static_cast<uint32_t>(s_lights.Size))
				logger::warn(
					"[SCM] Shadow atlas holds {} quarter tiles for {} pool slots; "
					"lowest-importance lights will get no tile (raise AtlasResolution "
					"or lower ShadowLightCount)",
					capacity, s_lights.Size);
			s_atlas.ready = true;
			logger::info("[SCM] Shadow atlas ready: {0}x{0}, base tile {1}, cell {2}", s_atlas.dim, s_atlas.baseTile, s_atlas.cell);
			return true;
		}

		uint32_t OrderForScale(float scale)
		{
			// Order 0 = the floor class (one allocator cell); each order
			// doubles the tile per axis up to the full class.
			uint32_t order = 0;
			for (float s = kTileScaleFloor; s < scale && s < kTileScaleFull; s *= 2.0f)
				order++;
			return order;
		}
	}

	bool AtlasActive()
	{
		// Cheap flag check only: this runs inside draw-time hooks, where
		// resource creation (and the probe's GPU-sync readback) must never
		// happen mid-pass. UpdateAtlas owns creation at the frame boundary.
		return s_bootAtlasEnabled && s_atlas.ready;
	}

	namespace
	{
		std::atomic<bool> s_dumpRequested{ false };

		// One-shot debug dump: atlas depth DDS + a slot manifest, written on
		// the render thread (the only safe owner of the immediate context).
		// The CaptureTexture readback stalls the GPU; acceptable for an
		// explicitly requested diagnostic.
		void ServiceAtlasDump()
		{
			if (!s_dumpRequested.exchange(false, std::memory_order_acq_rel))
				return;
			if (!s_atlas.ready)
				return;
			auto* device = globals::d3d::device;
			auto* context = globals::d3d::context;
			if (!device || !context)
				return;

			const auto stamp = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
			std::filesystem::path dir = "Data\\SKSE\\Plugins\\CommunityShaders\\Captures";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);

			DirectX::ScratchImage image;
			if (FAILED(DirectX::CaptureTexture(device, context, s_atlas.texture.get(), image))) {
				logger::warn("[SCM] Atlas dump: CaptureTexture failed");
				return;
			}
			const auto ddsPath = dir / std::format("shadow_atlas_frame{}.dds", stamp);
			if (FAILED(DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::DDS_FLAGS_NONE, ddsPath.c_str()))) {
				logger::warn("[SCM] Atlas dump: SaveToDDSFile failed");
				return;
			}

			// Manifest: per-slot tile rects so the DDS regions map back to
			// pool slots without cross-referencing the live dump.
			std::ofstream manifest(dir / std::format("shadow_atlas_frame{}.json", stamp));
			manifest << "{\n  \"dim\": " << s_atlas.dim
					 << ",\n  \"frame\": " << stamp
					 << ",\n  \"clearsSwallowed\": " << s_clearsSwallowed.load(std::memory_order_relaxed)
					 << ",\n  \"clearsPassed\": " << s_clearsPassed.load(std::memory_order_relaxed)
					 << ",\n  \"tileClears\": " << s_tileClears.load(std::memory_order_relaxed)
					 << ",\n  \"slots\": [\n";
			bool first = true;
			for (size_t i = 0; i < s_atlas.slots.size(); i++) {
				const auto& slot = s_atlas.slots[i];
				if (!slot.tile.valid)
					continue;
				if (!first)
					manifest << ",\n";
				first = false;
				manifest << "    { \"slot\": " << i
						 << ", \"x\": " << slot.tile.x * s_atlas.cell
						 << ", \"y\": " << slot.tile.y * s_atlas.cell
						 << ", \"size\": " << (1u << slot.tile.order) * s_atlas.cell
						 << ", \"lastRenderFrame\": " << slot.renderFrame
						 << ", \"contentValid\": " << (slot.valid ? "true" : "false") << " }";
			}
			manifest << "\n  ]\n}\n";
			logger::info("[SCM] Atlas dumped: {}", ddsPath.string());
		}
	}

	void RequestAtlasDump()
	{
		s_dumpRequested.store(true, std::memory_order_release);
	}

	void UpdateAtlas()
	{
		if (!s_bootAtlasEnabled || s_atlas.failed)
			return;
		ServiceAtlasDump();
		if (!EnsureResources())
			return;
		// Track pool reallocation (runtime ShadowLightCount changes): slots
		// beyond the vector would silently render tile-less otherwise.
		const size_t poolSize = static_cast<size_t>(std::max(s_lights.Size, 1));
		if (s_atlas.slots.size() != poolSize) {
			for (size_t i = poolSize; i < s_atlas.slots.size(); i++)
				if (s_atlas.slots[i].tile.valid)
					s_atlas.allocator.Free(s_atlas.slots[i].tile);
			s_atlas.slots.resize(poolSize);
		}

		// Reclaim hoarded space every frame: a slot whose light left (any
		// removal path) or whose class the rank budget demoted would keep
		// its tile until reassignment/redraw, and the accumulated orphans
		// starve newly chosen lights of tiles entirely.
		for (size_t i = 0; i < s_atlas.slots.size(); i++) {
			auto& slot = s_atlas.slots[i];
			if (!slot.tile.valid)
				continue;
			const auto* entry = i < static_cast<size_t>(s_lights.Size) ? &s_lights.Lights[i] : nullptr;
			if (!entry || !entry->Light || slot.tile.order > OrderForScale(entry->pendingScale))
				FreeSlotTile(static_cast<int32_t>(i));
		}
	}

	ID3D11DepthStencilView* AtlasDSV(bool readOnly)
	{
		return readOnly ? s_atlas.dsvReadOnly.get() : s_atlas.dsv.get();
	}

	ID3D11ShaderResourceView* AtlasSRV()
	{
		return s_atlas.ready ? s_atlas.srv.get() : nullptr;
	}

	uint32_t AtlasDim()
	{
		return s_atlas.dim;
	}

	uint32_t AtlasBaseTile()
	{
		return s_atlas.baseTile;
	}

	float AtlasOccupancy()
	{
		return s_atlas.ready ? s_atlas.allocator.Occupancy() : 0.0f;
	}

	uint64_t AtlasVRAMBytes()
	{
		return s_atlas.ready ? static_cast<uint64_t>(s_atlas.dim) * s_atlas.dim * s_atlas.bytesPerPixel : 0;
	}

	uint32_t AtlasSnapResolution(uint32_t requested)
	{
		if (!s_atlas.ready)
			return 0;
		uint32_t levels = 0;
		return SnapAtlasDim(requested, s_atlas.baseTile, s_atlas.cell, levels);
	}

	uint32_t AtlasCapacityCells()
	{
		return s_atlas.ready ? s_atlas.allocator.CellsPerAxis() * s_atlas.allocator.CellsPerAxis() : 0;
	}

	uint32_t CellsForScale(float scale)
	{
		return 1u << (2 * OrderForScale(scale));
	}

	bool EnsureSlotTile(int32_t poolSlot, float scale)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return false;
		auto& slot = s_atlas.slots[poolSlot];
		uint32_t order = OrderForScale(scale);
		if (slot.tile.valid && slot.tile.order == order)
			return true;
		if (slot.tile.valid) {
			s_atlas.allocator.Free(slot.tile);
			slot = {};
			s_tileReallocs.fetch_add(1, std::memory_order_relaxed);
		}
		// Walk down classes on atlas pressure; the quarter class always fits
		// for any sane pool size vs atlas size.
		for (;;) {
			slot.tile = s_atlas.allocator.Allocate(order);
			if (slot.tile.valid)
				break;
			if (order == 0)
				return false;
			order--;
		}
		slot.scale = scale;
		slot.valid = false;  // content pending first render
		return true;
	}

	void MarkSlotTileRendered(int32_t poolSlot)
	{
		if (s_atlas.ready && poolSlot >= 0 && static_cast<size_t>(poolSlot) < s_atlas.slots.size() &&
			s_atlas.slots[poolSlot].tile.valid) {
			s_atlas.slots[poolSlot].valid = true;
			s_atlas.slots[poolSlot].renderFrame =
				globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
		}
	}

	void FreeSlotTile(int32_t poolSlot)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return;
		auto& slot = s_atlas.slots[poolSlot];
		if (slot.tile.valid)
			s_atlas.allocator.Free(slot.tile);
		slot = {};
	}

	void FreeAllTiles()
	{
		if (!s_atlas.ready)
			return;
		for (auto& slot : s_atlas.slots)
			slot = {};
		s_atlas.allocator.Reset(s_atlas.levels);
	}

	bool GetSlotTileTexels(int32_t poolSlot, AtlasTileTexels& out)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return false;
		const auto& slot = s_atlas.slots[poolSlot];
		if (!slot.tile.valid)
			return false;
		out.x = slot.tile.x * s_atlas.cell;
		out.y = slot.tile.y * s_atlas.cell;
		out.size = (1u << slot.tile.order) * s_atlas.cell;
		out.lastRenderFrame = slot.renderFrame;
		out.contentValid = slot.valid;
		return true;
	}

	AtlasClearStats GetAtlasClearStats()
	{
		return {
			s_clearsSwallowed.load(std::memory_order_relaxed),
			s_clearsPassed.load(std::memory_order_relaxed),
			s_tileClears.load(std::memory_order_relaxed),
			s_tileReallocs.load(std::memory_order_relaxed)
		};
	}

	bool GetSlotAtlasRectUV(int32_t poolSlot, AtlasRectUV& out)
	{
		AtlasTileTexels t{};
		if (!GetSlotTileTexels(poolSlot, t) || !t.contentValid || s_atlas.dim == 0)
			return false;
		const float dim = static_cast<float>(s_atlas.dim);
		out.scaleX = out.scaleY = t.size / dim;
		out.biasX = t.x / dim;
		out.biasY = t.y / dim;
		return true;
	}

	void ClearSlotTile(int32_t poolSlot)
	{
		AtlasTileTexels t{};
		if (!GetSlotTileTexels(poolSlot, t))
			return;
		const FLOAT farDepth[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		D3D11_RECT rect{ static_cast<LONG>(t.x), static_cast<LONG>(t.y),
			static_cast<LONG>(t.x + t.size), static_cast<LONG>(t.y + t.size) };
		s_atlas.context1->ClearView(s_atlas.dsv.get(), farDepth, &rect, 1);
		s_tileClears.fetch_add(1, std::memory_order_relaxed);
	}
}
