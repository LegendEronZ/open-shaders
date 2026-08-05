// ShadowCasterClassifier.cpp
// Contribution-based caster culling, the static/dynamic split-cache caster
// classifier, the parabolic/base AppendVirtual cull hooks, and the
// multi-frame shadowmap diagnostic recorder.

#include <filesystem>
#include <fstream>

#include "../../Globals.h"
#include "../../State.h"
#include "../../Utils/Game.h"
#include "ShadowCasterInternal.h"

namespace ShadowCasterManager
{
	// =========================================================================
	// Contribution-based caster culling (experimental)
	//
	// A point light's shadow render submits one draw batch per visible caster;
	// small or distant casters produce sub-pixel shadows that cost CPU
	// submission for no visible result. The engine's parabolic (point-light)
	// culling registers each visible caster through BSCullingProcess::
	// AppendVirtual (vtable slot 0x18). We hook that slot on ONLY the parabolic
	// vtable -- so it fires exclusively for point-light shadow culling, never
	// the sun, spots, or main scene -- and skip the append for a caster whose
	// camera-relative screen size (bound radius / distance to camera) is below
	// CasterCullAngularMin. s_currentCullLight identifies the light being
	// accumulated (set around EnableLight's Accumulate call, same render
	// thread), so the cost metric uses the VR-validated BSShadowLight position
	// rather than the culling process's internal members.
	// =========================================================================

	/// Casters culled last frame across all lights (Tracy plot for A/B).
	std::atomic<uint32_t> s_casterCullCount{ 0 };

	/// Appends dropped because the culling process's free pool was near
	/// exhaustion (see the guard in Hook_ParabolicCullAppend).
	std::atomic<uint32_t> s_cullPoolDropCount{ 0 };

	/// Running total of s_cullPoolDropCount, published for headless inspection
	/// (devbench inspect kind=llfshadows) -- the per-frame counter above is
	/// exchanged (reset) every frame for the Tracy plot, so without this
	/// running total there was no way to see it outside of a live Tracy
	/// connection. A starved accumulate (casters dropped here) can still mark
	/// its tile contentValid via the empty-render guard's geomList.empty()
	/// check, which does NOT reflect what the accumulate actually appended --
	/// this counter is the only external signal that guard's blind spot fired.
	std::atomic<uint64_t> s_cullPoolDropTotal{ 0 };

	/// Running total of s_casterCullCount (the angular-size contribution-cull
	/// drop, distinct from the pool-exhaustion drop above -- see
	/// Hook_ParabolicCullAppend's angularMin check). A caster right at this
	/// threshold can flip in/out of the accumulate frame-to-frame on camera
	/// sway alone; if EVERY caster for a light flips out on the same frame,
	/// that accumulate is empty even though geomList (persistent membership)
	/// stays non-empty -- the same empty-render-guard blind spot as the pool
	/// drop, via a different gate. Published for the same reason.
	std::atomic<uint64_t> s_casterCullTotal{ 0 };

	/// The shadow light currently being accumulated; only non-null across an
	/// EnableLight Accumulate call, read synchronously by the AppendVirtual hook.
	std::atomic<RE::BSShadowLight*> s_currentCullLight{ nullptr };

	// True while accumulating a light whose geomList is EMPTY: the engine only
	// attaches geometry to lights once per geometry (AttachNearbyLights sets
	// kRenderUse and never revisits), so a light created after the scene
	// attached -- e.g. every light of an in-game same-cell load -- never
	// receives geometry and renders an empty shadow forever. The append hook
	// rebuilds the list from the cull walk via the engine's own AttachGeometry.
	std::atomic<bool> s_accumRebuildAttach{ false };

	// Geometry already re-attached in the current heal walk. Dual-paraboloid
	// walks (and the frustum-light vtable's own concurrent worker pops, per
	// the pool-exhaustion guards below) can append the same light from more
	// than one thread at once, so insert must be serialized -- concurrent
	// insert on unordered_set is UB, and GameAttachGeometry's raw pair-insert
	// has no dedupe of its own; this set IS the dedupe.
	std::mutex s_healAttachedMutex;
	std::unordered_set<const RE::BSGeometry*> s_healAttached;

	// =========================================================================
	// Multi-frame diagnostic recorder (devbench capture kind=shadowmaps with
	// frames=N[, slot=S]). Per shadow pass: numeric per-slot state; with a
	// target slot also that light's visited caster set per pass mode, so a
	// frozen-scene static/dynamic split is checkable against the unsplit set
	// (split valid <=> visited(All) == union of static and dynamic, disjoint). One JSON
	// under Captures/ at completion; zero cost while disarmed.
	// =========================================================================
	struct RecCaster
	{
		const void* geom;
		std::string name;
		bool dynamic;
		int mode;  ///< CasterPass value (ShadowCasterInternal.h)
	};
	struct RecSlot
	{
		int32_t slot;
		const void* light;
		uint32_t tx, ty, ts;
		bool valid, staticValid, redrew;
		float pending, rendered;
	};
	struct RecFrame
	{
		uint32_t frame, reallocs, ownerInv;
		std::vector<RecSlot> slots;
		std::vector<RecCaster> casters;
	};
	std::vector<RecFrame> s_recFrames;
	std::vector<RecCaster> s_recCasters;  // filled by the append hook during accumulate
	std::atomic<int32_t> s_recLeft{ 0 };
	std::atomic<int32_t> s_recTargetSlot{ -1 };
	bool s_recPixels = false;
	// Arm via atomics only: the devbench thread must not touch the vectors
	// the render thread owns. frames is the trigger; store slot first.
	std::atomic<uint32_t> s_recRequestFrames{ 0 };
	std::atomic<int32_t> s_recRequestSlot{ -1 };

	void ServiceShadowFrameRecord()
	{
		if (const uint32_t req = s_recRequestFrames.exchange(0, std::memory_order_acq_rel); req != 0) {
			s_recFrames.clear();
			s_recCasters.clear();
			s_recTargetSlot.store(s_recRequestSlot.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s_recPixels = s_recTargetSlot.load(std::memory_order_relaxed) >= 0 && req <= 16u;
			s_recFrames.reserve(req);
			s_recLeft.store(static_cast<int32_t>(req), std::memory_order_relaxed);
		}
		if (s_recLeft.load(std::memory_order_relaxed) <= 0)
			return;
		RecFrame rec{};
		rec.frame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
		const auto stats = GetAtlasClearStats();
		rec.reallocs = stats.tileReallocs;
		rec.ownerInv = stats.ownerInvalidations;
		for (int32_t i = 0; i < s_lights.Size; i++) {
			const auto& e = s_lights.Lights[i];
			if (!e.Light)
				continue;
			AtlasTileTexels t{};
			const bool hasTile = GetSlotTileTexels(i, t);
			uint64_t staticHash = 0;
			bool staticValid = false;
			GetSlotStaticState(i, staticHash, staticValid);
			rec.slots.push_back({ i, e.Light, hasTile ? t.x : 0u, hasTile ? t.y : 0u,
				hasTile ? t.size : 0u, hasTile && t.contentValid, staticValid,
				e.RedrawFrame, e.pendingScale, e.renderedScale });
		}
		rec.casters = std::move(s_recCasters);
		s_recCasters.clear();
		const int32_t target = s_recTargetSlot.load(std::memory_order_relaxed);
		if (s_recPixels && target >= 0)
			DumpSlotTileRegion(target, rec.frame);
		s_recFrames.push_back(std::move(rec));
		if (s_recLeft.fetch_sub(1, std::memory_order_relaxed) != 1)
			return;

		std::filesystem::path dir = "Data\\SKSE\\Plugins\\CommunityShaders\\Captures";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec) {
			logger::error("[SCM] Frame record directory failed: {} ({})", dir.string(), ec.message());
			s_recFrames.clear();
			return;
		}
		const auto path = dir / std::format("scm_frames_{}.json", rec.frame);
		std::ofstream out(path);
		if (!out) {
			logger::error("[SCM] Frame record open failed: {}", path.string());
			s_recFrames.clear();
			return;
		}
		out << "{\n\"frames\": [\n";
		for (size_t f = 0; f < s_recFrames.size(); f++) {
			const auto& fr = s_recFrames[f];
			out << (f ? ",\n" : "") << "{\"frame\":" << fr.frame << ",\"reallocs\":" << fr.reallocs
				<< ",\"ownerInv\":" << fr.ownerInv << ",\"slots\":[";
			for (size_t s = 0; s < fr.slots.size(); s++) {
				const auto& r = fr.slots[s];
				out << (s ? "," : "") << "{\"i\":" << r.slot << ",\"light\":\"" << r.light
					<< "\",\"tx\":" << r.tx << ",\"ty\":" << r.ty << ",\"ts\":" << r.ts
					<< ",\"valid\":" << (r.valid ? "true" : "false")
					<< ",\"staticValid\":" << (r.staticValid ? "true" : "false")
					<< ",\"redrew\":" << (r.redrew ? "true" : "false")
					<< ",\"pending\":" << r.pending << ",\"rendered\":" << r.rendered << "}";
			}
			out << "],\"casters\":[";
			for (size_t c = 0; c < fr.casters.size(); c++) {
				const auto& k = fr.casters[c];
				std::string nm = k.name;
				std::erase_if(nm, [](char ch) { return ch == '"' || ch == '\\'; });
				out << (c ? "," : "") << "{\"geom\":\"" << k.geom << "\",\"name\":\"" << nm
					<< "\",\"dynamic\":" << (k.dynamic ? "true" : "false") << ",\"mode\":" << k.mode << "}";
			}
			out << "]}";
		}
		out << "\n]\n}\n";
		logger::info("[SCM] Frame record written: {} ({} frames)", path.string(), s_recFrames.size());
		s_recFrames.clear();
	}

	void RequestShadowFrameRecord(uint32_t a_frames, int32_t a_slot)
	{
		s_recRequestSlot.store(a_slot, std::memory_order_relaxed);
		s_recRequestFrames.store(std::clamp(a_frames, 1u, 600u), std::memory_order_release);
	}

	// -------------------------------------------------------------------------
	// Static/dynamic split caching -- caster classification + append filter
	//
	// The parabolic AppendVirtual hook decides which casters enter a light's
	// shadow render: fewer appends -> fewer draw batches -> less CPU (the same
	// mechanism contribution culling uses). s_cullPassMode picks the subset the
	// hook keeps for the light's single accumulate this frame: StaticOnly on a
	// rare rebake of the parallel static atlas, DynamicOnly (movers only) the
	// rest of the time. The frame-flow that sets the mode is documented at
	// SplitState in ShadowScheduler.cpp.
	// -------------------------------------------------------------------------
	std::atomic<int> s_cullPassMode{ static_cast<int>(CasterPass::All) };

	/// Static/dynamic caster draws classified last frame (Tracy plots for A/B).
	std::atomic<uint32_t> s_staticCasterDraws{ 0 };
	std::atomic<uint32_t> s_dynamicCasterDraws{ 0 };

	// Per-caster movement history. A caster is dynamic while it moved within the
	// last stability window; once stable that long it classifies static (baked
	// into the cache). The stability window lets a caster that just stopped keep
	// rendering in the dynamic pass until the static cache rebuild absorbs it, so
	// its shadow never blinks out mid-transition.
	// Single-threaded: the hook and the per-frame epoch bump both run on the
	// shadow render thread within ScheduleShadowCasters.
	constexpr int kStaticStabilityFrames = 8;
	// Joining or leaving the static set changes the static hash and forces a
	// full StaticOnly re-bake of every tile that sees the caster. Leaving must
	// stay immediate (a baked tile of a caster that moved is stale), so damp the
	// churn on the rejoin side: each oscillation doubles how long the caster must
	// hold still before it is re-admitted, up to this multiple of the base window.
	// An NPC that fidgets every second then stays in the cheap dynamic pass
	// instead of re-baking the cache each time it pauses; furniture still
	// promotes on the base window.
	constexpr int kStaticPromoteBackoffMax = 8;  // 8 * 8 = 64 frames, ~1s at 60fps
	// Bounds framesSinceMove; must clear the longest decay threshold below.
	constexpr int kStaticFramesCap = kStaticStabilityFrames * kStaticPromoteBackoffMax * 4;
	// Settled casters re-verify worldBound only every N epochs, not every
	// visit; kept well inside kSleepRedrawIntervalFrames (45) so a missed
	// move is still caught before that existing tolerance would hide it.
	constexpr int kSettledRecheckFrames = 16;
	constexpr int kSettledAtFactor = 4;  // matches the promoteAt * 4 backoff-reset below
	// Open-addressing map: probed once per appended caster by the cull-walk
	// hook, so lookup cost lands directly on EnableLight's accumulate time.
	ankerl::unordered_dense::map<RE::BSGeometry*, CasterMobility> s_casterMobility;
	int s_casterClassEpoch{ 0 };

	/// Classifies a caster static vs dynamic from its quantized worldBound
	/// movement, memoized once per frame (a caster shared across lights, or
	/// revisited across passes, classifies identically all frame). 1-unit
	/// quantization matches the redraw hash so "moved" means "shadow changed".
	/// Classification record for a non-skinned caster (skinned geometry never
	/// reaches the map -- see IsCasterDynamic).
	static CasterMobility& ClassifyCaster(RE::BSGeometry& geom)
	{
		auto [it, inserted] = s_casterMobility.try_emplace(&geom);
		auto& r = it->second;
		if (r.lastEpoch == s_casterClassEpoch)
			return r;  // already classified this frame

		// Settled fast path: trust the cached classification instead of
		// re-quantizing worldBound. A move mid-window is still caught at the
		// next checkpoint; the mismatchStreak/rebake departure path is unchanged.
		const int settledAt = kStaticStabilityFrames * r.promoteBackoff * kSettledAtFactor;
		if (!inserted && !r.dynamic && r.framesSinceMove >= settledAt &&
			s_casterClassEpoch - r.lastVerifyEpoch < kSettledRecheckFrames) {
			r.lastEpoch = s_casterClassEpoch;  // keep same-frame memo + prune liveness
			return r;
		}

		const auto& wb = geom.worldBound;
		const float cx = std::round(wb.center.x), cy = std::round(wb.center.y),
					cz = std::round(wb.center.z), cr = std::round(wb.radius);
		const bool moved = inserted || cx != r.cx || cy != r.cy || cz != r.cz || cr != r.cr;
		// Settled casters verify sparsely, so framesSinceMove must advance by
		// the elapsed epoch count to keep real-time semantics.
		const int elapsed = r.lastVerifyEpoch < 0 ? 1 : std::max(1, s_casterClassEpoch - r.lastVerifyEpoch);
		if (moved) {
			// Leaving the static set is an oscillation: make the caster earn its
			// way back so a caster that keeps pausing can't re-bake the cache on
			// every pause. A first sighting is not an oscillation.
			if (!r.dynamic && !inserted)
				r.promoteBackoff = std::min(r.promoteBackoff * 2, kStaticPromoteBackoffMax);
			r.framesSinceMove = 0;
			r.foldHashValid = false;  // quantized bound changed
		} else {
			r.framesSinceMove = std::min(r.framesSinceMove + elapsed, kStaticFramesCap);
		}
		r.cx = cx;
		r.cy = cy;
		r.cz = cz;
		r.cr = cr;
		r.lastEpoch = s_casterClassEpoch;
		r.lastVerifyEpoch = s_casterClassEpoch;
		const int promoteAt = kStaticStabilityFrames * r.promoteBackoff;
		r.dynamic = r.framesSinceMove < promoteAt;
		// Held still far past its (backed-off) window: it has settled rather than
		// oscillating, so restore the base window for its next move.
		if (!r.dynamic && r.framesSinceMove >= promoteAt * kSettledAtFactor)
			r.promoteBackoff = 1;
		return r;
	}

	static bool IsCasterDynamic(RE::BSGeometry& geom)
	{
		// Skinned = actor/creature: never let the movement heuristic bake it
		// static, or its silhouette ghosts once it walks off a bake-starved cell.
		if (geom.GetGeometryRuntimeData().skinInstance)
			return true;
		return ClassifyCaster(geom).dynamic;
	}

	// Running static-caster hash for the light currently accumulating; seeded
	// with the light pose in EnableLight, folded per static caster by the hook.
	std::uint64_t s_visitStaticHash{ 0 };

	// Dynamic casters the current accumulate appended (DynamicOnly/All passes
	// only). Atomic because the cull walk can append from worker threads.
	// Reset alongside the hash seed in EnableLight, latched into SplitState
	// after the accumulate for the schedule-time sleep skip.
	std::atomic<uint32_t> s_visitDynamicCount{ 0 };
	// Static casters the current StaticOnly bake appended. Distinguishes a bake
	// that captured geometry from one that captured nothing, so an empty bake is
	// never advertised as real cached content.
	std::atomic<uint32_t> s_visitStaticCount{ 0 };

	// Folds one static caster into the running per-light static hash during the
	// same accumulate that appends the movers -- no second caster walk needed.
	static void FoldStaticCasterHash(RE::BSGeometry& geom, CasterMobility& r)
	{
		if (!r.foldHashValid) {
			// Summed, not chained/XORed: order-independent, and a caster appended
			// by both paraboloid halves still contributes to the set hash.
			const auto raw = reinterpret_cast<std::uintptr_t>(&geom);
			uint64_t h = 0x9e3779b97f4a7c15ull;
			h = HashCombine(h, static_cast<std::uint32_t>(raw));
			h = HashCombine(h, static_cast<std::uint32_t>(raw >> 32));
			// r.cx..cr already hold the 1-unit-quantized worldBound (std::round
			// == QuantizeFloat(x, 1.0f)), refreshed by ClassifyCaster this frame.
			h = HashCombineFloat(h, r.cx);
			h = HashCombineFloat(h, r.cy);
			h = HashCombineFloat(h, r.cz);
			h = HashCombineFloat(h, r.cr);
			r.foldHash = h;
			r.foldHashValid = true;
		}
		s_visitStaticHash += r.foldHash;
	}

	/// Classifies `geom` static/dynamic for the active split-cache pass and
	/// folds it into the running static hash, returning true when the caller
	/// should skip appending it (wrong pass for its class). Shared by the
	/// parabolic (point-light) and base (frustum-light) cull-append hooks so
	/// both vtables enforce identical StaticOnly/DynamicOnly semantics.
	static bool CasterFilteredByPass(RE::BSGeometry& geom)
	{
		if (!AtlasActive())
			return false;
		// Skinned = always dynamic (see IsCasterDynamic); only non-skinned
		// casters have a mobility record, classified once per frame.
		CasterMobility* rec = geom.GetGeometryRuntimeData().skinInstance ?
		                          nullptr :
		                          &ClassifyCaster(geom);
		const bool dynamic = !rec || rec->dynamic;
		// Every static caster folds into the running hash regardless of
		// pass, so the static-set change that triggers a rebake is seen
		// during whichever single accumulate this light runs this frame.
		if (!dynamic)
			FoldStaticCasterHash(geom, *rec);
		switch (static_cast<CasterPass>(s_cullPassMode.load(std::memory_order_relaxed))) {
		case CasterPass::StaticOnly:
			if (dynamic)
				return true;  // bake pass skips movers
			s_visitStaticCount.fetch_add(1, std::memory_order_relaxed);
			break;
		case CasterPass::DynamicOnly:
			if (!dynamic)
				return true;  // composite pass skips baked static geometry
			s_visitDynamicCount.fetch_add(1, std::memory_order_relaxed);
			break;
		case CasterPass::All:
			(dynamic ? s_dynamicCasterDraws : s_staticCasterDraws).fetch_add(1, std::memory_order_relaxed);
			if (dynamic)
				s_visitDynamicCount.fetch_add(1, std::memory_order_relaxed);
			break;
		}
		return false;
	}

	/// Hook of BSCullingProcess::AppendVirtual on the parabolic culling vtable.
	/// Drops a caster (skips the append) when below the contribution-cull
	/// threshold, or when it does not belong to the active split-cache pass.
	struct Hook_ParabolicCullAppend
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
		{
			const float angularMin = s_settings.CasterCullAngularMin;
			RE::BSShadowLight* light = s_currentCullLight.load(std::memory_order_relaxed);
			// Rebuild a missed geometry attachment (see s_accumRebuildAttach)
			// through the engine's own pair-insert, mirroring
			// AttachNearbyLights' gates. Only for lights the engine is
			// provably not attaching (empty geomList), so this can't race a
			// concurrent scene-side attach on the same light.
			//
			// Gated on s_accumRebuildAttach alone (== geomList.empty()), not
			// also `!light->objectNode`: objectNode is set once when the
			// light attaches to its host scene node, unrelated to whether its
			// separate caster-geometry attach ran -- the AND could stay false
			// forever for a light whose node attached fine but geomList didn't.
			if (light && s_accumRebuildAttach.load(std::memory_order_relaxed)) {
				// Dual-paraboloid walks append the same geometry once per half;
				// AttachGeometry is a raw pair-insert, so dedupe per walk.
				bool newlyAttached;
				{
					std::lock_guard lock(s_healAttachedMutex);
					newlyAttached = s_healAttached.insert(&a_visible).second;
				}
				if (auto* ni = light->light.get();
					ni && newlyAttached &&
					GameLightIsInRange(light, &a_visible.worldBound, ni, 1.0f))
					GameAttachGeometry(light, &a_visible);
			}
			if (angularMin > 0.0f && light) {
				const auto& wb = a_visible.worldBound;
				const float dx = wb.center.x - s_cullCameraPos.x;
				const float dy = wb.center.y - s_cullCameraPos.y;
				const float dz = wb.center.z - s_cullCameraPos.z;
				const float distSq = dx * dx + dy * dy + dz * dz;
				const float radiusSq = wb.radius * wb.radius;
				// Camera-relative screen size = radius / distance-to-viewer. A
				// caster far from the camera casts a small on-screen shadow and is
				// culled; one close enough to fill the view is kept regardless of
				// how large it looks from the light. Skip the test when the caster
				// encloses the camera (its shadow can be anywhere on screen).
				// Squared form of dist > radius && radius / dist < angularMin,
				// avoiding the per-caster sqrt (all terms non-negative).
				if (distSq > radiusSq && radiusSq < distSq * (angularMin * angularMin)) {
					s_casterCullCount.fetch_add(1, std::memory_order_relaxed);
					return;  // skip append -- caster dropped from this shadow
				}
			}
			if (s_recLeft.load(std::memory_order_relaxed) > 0) {
				const int32_t recSlot = s_recTargetSlot.load(std::memory_order_relaxed);
				if (recSlot >= 0 && light && recSlot < s_lights.Size &&
					s_lights.Lights[recSlot].Light == light) {
					const char* nm = a_visible.name.c_str();
					s_recCasters.push_back({ &a_visible, nm ? nm : "",
						IsCasterDynamic(a_visible),
						s_cullPassMode.load(std::memory_order_relaxed) });
				}
			}
			if (CasterFilteredByPass(a_visible))
				return;
			// Engine bug: AppendVirtual writes through PopFreeQueueEntry's result
			// with no null check, so exhausting the fixed 8192-entry free pool is
			// a guaranteed CTD. Drop the caster instead; the margin absorbs
			// concurrent worker pops between this read and the engine's own pop.
			{
				constexpr std::uintptr_t kFreePoolOffset = 0x20150;  // identical SE/AE/VR
				constexpr std::uintptr_t kPoolHeadOffset = 0x10000;
				constexpr std::uintptr_t kPoolTailOffset = 0x10008;
				constexpr std::uint32_t kFreeEntryMargin = 16;
				const auto* pool = reinterpret_cast<const std::uint8_t*>(a_this) + kFreePoolOffset;
				const auto head = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolHeadOffset)->load(std::memory_order_relaxed);
				const auto tail = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolTailOffset)->load(std::memory_order_relaxed);
				if (tail - head < kFreeEntryMargin) {
					s_cullPoolDropCount.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
			func(a_this, a_visible, a_alphaGroupIndex);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/// Pool-exhaustion guard for the base culling process (frustum/spot lights'
	/// vtable, RE::VTABLE_BSCullingProcess[0] -- also reached by the engine's
	/// room/scene cull walks, hence the same unchecked AppendVirtual null-write
	/// guard as the parabolic hook above).
	struct Hook_BaseCullAppendGuard
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
		{
			// Same missed-geometry-attachment heal as Hook_ParabolicCullAppend
			// (point/omni lights) -- frustum/spot lights go through THIS vtable,
			// not the parabolic one, and previously had no heal path at all: a
			// light whose one-time engine attachment was missed (e.g. re-entering
			// room/portal visibility after the attach pass already ran) could
			// never recover, since nothing here ever re-attached its geometry.
			// Gated on s_accumRebuildAttach alone -- see the identical fix's
			// rationale at Hook_ParabolicCullAppend above.
			RE::BSShadowLight* light = s_currentCullLight.load(std::memory_order_relaxed);
			if (light && s_accumRebuildAttach.load(std::memory_order_relaxed)) {
				bool newlyAttached;
				{
					std::lock_guard lock(s_healAttachedMutex);
					newlyAttached = s_healAttached.insert(&a_visible).second;
				}
				if (auto* ni = light->light.get();
					ni && newlyAttached &&
					GameLightIsInRange(light, &a_visible.worldBound, ni, 1.0f))
					GameAttachGeometry(light, &a_visible);
			}
			// Static/dynamic split filtering, scoped to this light's own
			// accumulate: `light` (s_currentCullLight) is set only across
			// EnableLight's Accumulate call (RAII-cleared right after), so a
			// null `light` here means this is one of the engine's other
			// room/scene cull walks sharing this vtable slot -- never filter
			// those.
			if (light && CasterFilteredByPass(a_visible))
				return;
			constexpr std::uintptr_t kFreePoolOffset = 0x20150;
			constexpr std::uintptr_t kPoolHeadOffset = 0x10000;
			constexpr std::uintptr_t kPoolTailOffset = 0x10008;
			constexpr std::uint32_t kFreeEntryMargin = 16;
			const auto* pool = reinterpret_cast<const std::uint8_t*>(a_this) + kFreePoolOffset;
			const auto head = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolHeadOffset)->load(std::memory_order_relaxed);
			const auto tail = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolTailOffset)->load(std::memory_order_relaxed);
			if (tail - head < kFreeEntryMargin) {
				s_cullPoolDropCount.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			func(a_this, a_visible, a_alphaGroupIndex);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallCasterCullHook()
	{
		stl::write_vfunc<0x18, Hook_ParabolicCullAppend>(RE::VTABLE_BSParabolicCullingProcess[0]);
		stl::write_vfunc<0x18, Hook_BaseCullAppendGuard>(RE::VTABLE_BSCullingProcess[0]);
	}

	/// True only across a static-cache bake pass; read by the depth-select
	/// hooks to redirect the engine's shadow render into the static atlas.
	std::atomic<bool> s_staticPassActive{ false };

	bool StaticPassRedirectActive()
	{
		return s_staticPassActive.load(std::memory_order_relaxed);
	}
}
