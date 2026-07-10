// ShadowScheduler.cpp
// The shadow caster scheduling core: cached-shadow-map geometry hashing,
// light enable/disable/convert transitions, the per-frame
// ScheduleShadowCasters pass (replacing the engine's
// CalculateActiveShadowCasterLights), and the render dispatch that redraws
// only the scheduled lights.

#include "../../Deferred.h"
#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../../Utils/Game.h"
#include "../../Utils/UI.h"
#include "../Upscaling.h"
#include "../VR.h"
#include "I18n/I18n.h"
#include "ShadowCasterInternal.h"

#include <Windows.h>  // SEH (__try) for the shadow-light usability backstop

#define I18N_KEY_PREFIX "feature.light_limit_fix."

namespace ShadowCasterManager
{
	// =========================================================================
	// Shadow map content hash for cached-shadow-map detection
	// =========================================================================

	/// Mixes a 32-bit value into a running 64-bit hash. boost::hash_combine
	/// constants -- the magic number 0x9e3779b9 is the golden-ratio reciprocal,
	/// chosen for good bit distribution. Fast (a few ALU ops) and we don't
	/// need cryptographic strength -- only that distinct inputs map to
	/// distinct outputs with very high probability.
	static inline std::uint64_t HashCombine(std::uint64_t h, std::uint32_t v) noexcept
	{
		return h ^ (static_cast<std::uint64_t>(v) + 0x9e3779b9ull + (h << 6) + (h >> 2));
	}
	static inline std::uint64_t HashCombineFloat(std::uint64_t h, float f) noexcept
	{
		return HashCombine(h, std::bit_cast<std::uint32_t>(f));
	}

	/// Quantize a float to a step size before hashing. Skyrim's kFlicker /
	/// kPulse light flags oscillate animated torches by sub-unit position /
	/// radius amounts every frame. Bit-exact hashing on those oscillations
	/// produces a fresh hash every frame, defeating cache validity. Quantizing
	/// at sub-pixel-precision thresholds folds imperceptible animations into
	/// a stable hash bucket so the cached-shadow priority demotion fires
	/// correctly for visually-unchanging lights.
	static inline float QuantizeFloat(float f, float step) noexcept
	{
		return std::round(f / step) * step;
	}

	/// Hash of inputs that determine a shadow map's content: the light's
	/// pose + radius, and each caster's worldBound + identity. worldBound
	/// tracks rigid motion and BSDynamicTriShape vertex updates, so mesh
	/// data isn't inspected directly. Identical hashes across frames mean
	/// the cached slot is byte-for-byte current -- caller can skip the
	/// redraw. Returns 0 only on null light/NiLight (sentinel for "never
	/// rendered"); HashCombine constants make a real-data 0 essentially
	/// impossible.

	static std::uint64_t ComputeShadowGeomHash(RE::BSShadowLight* light)
	{
		if (!light)
			return 0;
		auto* ni = light->light.get();
		if (!ni)
			return 0;
		std::uint64_t h = 0x9e3779b97f4a7c15ull;  // arbitrary nonzero seed

		// Quantization thresholds: tuned to be one to two orders of
		// magnitude below perceptible difference in the rendered shadow.
		//   kPosStep   = 1.0 game unit (~1.4 cm world space; sub-texel
		//                at typical 2048 shadow res * 500 unit light radius)
		//   kRotStep   = 0.01 in matrix entries (~0.5 degrees)
		//   kRadiusStep = 1.0 unit (well under any visible frustum
		//                resize from torch pulse animations)
		constexpr float kPosStep = 1.0f;
		constexpr float kRotStep = 0.01f;
		constexpr float kRadiusStep = 1.0f;

		// Light pose
		const auto& t = ni->world.translate;
		h = HashCombineFloat(h, QuantizeFloat(t.x, kPosStep));
		h = HashCombineFloat(h, QuantizeFloat(t.y, kPosStep));
		h = HashCombineFloat(h, QuantizeFloat(t.z, kPosStep));
		const auto& r = ni->world.rotate;
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				h = HashCombineFloat(h, QuantizeFloat(r.entry[i][j], kRotStep));
		// Light radius (NiPointLight uses .x; spotlights use direction in
		// rotation matrix already hashed above).
		const auto& rtd = ni->GetLightRuntimeData();
		h = HashCombineFloat(h, QuantizeFloat(rtd.radius.x, kRadiusStep));
		// Caster set + each caster's worldBound (engine-updated).
		for (auto& nip : light->geomList) {
			auto* ts = nip.get();
			if (!ts)
				continue;
			const auto raw = reinterpret_cast<std::uintptr_t>(ts);
			h = HashCombine(h, static_cast<std::uint32_t>(raw));
			h = HashCombine(h, static_cast<std::uint32_t>(raw >> 32));
			const auto& wb = ts->worldBound;
			h = HashCombineFloat(h, QuantizeFloat(wb.center.x, kPosStep));
			h = HashCombineFloat(h, QuantizeFloat(wb.center.y, kPosStep));
			h = HashCombineFloat(h, QuantizeFloat(wb.center.z, kPosStep));
			h = HashCombineFloat(h, QuantizeFloat(wb.radius, kRadiusStep));
		}
		return h;
	}

	// =========================================================================
	// Light enable / disable helpers
	// =========================================================================

	/// Removes `light` from s_normalConvert and clears its geometry list.
	/// No-op if the light is not in the list.
	static void EraseFromConvertList(RE::BSShadowLight* light)
	{
		for (auto it = s_normalConvert.begin(); it != s_normalConvert.end(); ++it) {
			if (it->light == light) {
				GameClearGeometryList(light);
				s_normalConvert.erase(it);
				return;
			}
		}
	}

	void DisableLight(RE::BSShadowLight* light)
	{
		EraseFromConvertList(light);
		auto* cull = light->cullingProcess;
		if (cull && cull->portalGraphEntry)
			GameClearPortalVisibility(reinterpret_cast<RE::BSPortalGraphEntry*>(cull->portalGraphEntry));
		light->ReturnShadowmaps();
	}

	// Activates a light as a normal (non-shadow) light by inserting it into
	// the scene's active-light list without allocating a shadow slot.
	//
	// Two paths: "already-converted re-enable" (just GameEnableLight) and
	// "first conversion this session" (ReturnShadowmaps + portal-clear +
	// track in s_normalConvert + GameEnableLight). Tracy sub-zones split
	// the cost so the next capture distinguishes the steady-state cost
	// (re-enable only) from the cost of a fresh conversion.
	void ConvertLight(RE::BSShadowLight* light, RE::ShadowSceneNode* ssn, bool isNS)
	{
		// Already converted: just re-enable so geometry picks it up this frame.
		for (auto& c : s_normalConvert) {
			if (c.light == light) {
				ZoneNamedN(zReEnable, "SCM::Engine::ConvertLight::ReEnable", true);
				GameEnableLight(ssn, light);
				return;
			}
		}

		// First conversion this session: release shadow resources, register.
		ZoneNamedN(zFirstConv, "SCM::Engine::ConvertLight::FirstConvert", true);
		auto* cull = GetLightCullingProcess(light);
		if (cull && cull->portalGraphEntry)
			GameClearPortalVisibility(reinterpret_cast<RE::BSPortalGraphEntry*>(cull->portalGraphEntry));
		light->ReturnShadowmaps();

		s_normalConvert.push_back({ light, isNS });
		GameEnableLight(ssn, light);
	}

	// Reset a promoted light's descriptor pool-slot indices to kNONE. BSShadowParabolicLight is
	// allocated non-zeroed and no ctor writes them; the VR engine indexes the depth-stencil pool
	// by the garbage -> nvwgf2umx OOB walk on teardown. kNONE forces re-allocation. Must run for
	// EVERY promoted light, incl. ones never EnableLight'd (else teardown reads the garbage).
	static void InitPromotedDescriptorSlots(RE::BSShadowLight* light)
	{
		if (!light)
			return;
		int32_t idx = s_lights.FindLight(light, s_settings.ShadowLightCount);
		if (idx < 0)
			idx = 0;
		if (globals::game::isVR) {
			auto& vrData = light->GetVRRuntimeData();
			for (auto& desc : vrData.shadowmapDescriptors) {
				desc.renderTarget = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.vrRenderTarget[0] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.vrRenderTarget[1] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.shadowmapIndex = static_cast<uint32_t>(idx);
			}
			for (auto& desc : vrData.focusShadowmapDescriptors) {
				desc.vrRenderTarget[0] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.vrRenderTarget[1] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
			}
		} else {
			for (auto& desc : light->GetRuntimeData().shadowmapDescriptors) {
				desc.renderTarget = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.shadowmapIndex = static_cast<uint32_t>(idx);
			}
		}
	}

	// Activates a non-sun shadow light into slot `slotIndex`.
	static void EnableLight(RE::BSShadowLight* light, RE::NiCamera* camera,
		RE::ShadowSceneNode* ssn, int slotIndex)
	{
		// Remove from conversion list if it was previously converted to normal.
		EraseFromConvertList(light);

		// Focus shadow handling. Gated on s_focusShadowSlots so we only run
		// the engine's focus accumulate when ScheduleShadowCasters has
		// reserved [kFocusShadowBaseSlotIndex .. +s_focusShadowSlots) this
		// frame -- without that reservation the engine would write focus
		// depth into texture slices currently held by point lights. With
		// it, extended mode (ShadowLightCount > 4) is safe; the previous
		// blanket `<= 4` gate is replaced by the reservation contract.
		if (s_focusShadowSlots > 0) {
			bool drawFocus = ShadowField(light, drawFocusShadows);
			if (drawFocus || (!*GetFocusShadowSelected() && light->GetIsFrustumOrDirectionalLight())) {
				GameSetupFocusShadowMaps(light, camera);
				GameSetupFocusShadowAccumulators(light);
				if (globals::game::isVR) {
					for (auto& desc : light->GetVRRuntimeData().focusShadowmapDescriptors) {
						desc.vrRenderTarget[0] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
						desc.vrRenderTarget[1] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
					}
				}
				ShadowField(light, drawFocusShadows) = true;
				*GetFocusShadowSelected() = true;
				*GetSunPtr() = reinterpret_cast<uint64_t>(light);
			}
		}

		GameEnableLight(ssn, light);
		GameSetShadowCasterSlot(ssn, light, *GetAccumLightSlot(), 1);

		{
			uint32_t mi = *GetMaskIndex();
			ShadowField(light, maskIndex) = mi;
			*GetMaskIndex() = mi + 1;
		}

		// Projected bounding box for shadow map region.
		auto* nilight = light->light.get();
		if (nilight) {
			auto lpos = nilight->world.translate;
			auto cpos = camera->world.translate;
			auto delta = lpos - cpos;
			float dx = delta.x, dy = delta.y, dz = delta.z;
			float dist = lpos.GetDistance(cpos);
			float radius = nilight->GetLightRuntimeData().radius.x;

			float left, right, top, bottom;

			if (dist >= radius + camera->GetNearPlane()) {
				float inv = 1.0f / dist;
				float coord[4] = {
					lpos.x - dx * radius * inv,
					lpos.y - dy * radius * inv,
					lpos.z - dz * radius * inv,
					radius
				};
				float r1[2], r2[2];
				GameFrustumOverlap(camera, coord, r1, r2, 0.00001f);

				float vw = (float)*globals::game::viewWidth;
				float vh = (float)*globals::game::viewHeight;
				if (globals::game::isVR) {
					vw *= GetVRDRSWidthRatio();
					vh *= GetVRDRSHeightRatio();
				}

				left = (r1[0] + 1.0f) * 0.5f * vw;
				right = (r2[0] + 1.0f) * 0.5f * vw;
				top = (1.0f - (r1[1] + 1.0f) * 0.5f) * vh;
				bottom = (1.0f - (r2[1] + 1.0f) * 0.5f) * vh;
			} else {
				// Light contains the camera: use full screen.
				*GetShadowMask() |= 1u << *GetAccumLightSlot();
				left = right = top = bottom = -1.0f;
			}

			ShadowField(light, projectedBoundingBox) =
				RE::NiRect<uint32_t>((uint32_t)left, (uint32_t)right, (uint32_t)top, (uint32_t)bottom);
		}

		// Accumulate into shadow slot.
		{
			uint32_t idx = static_cast<uint32_t>(slotIndex);
			light->Accumulate(idx, idx, nullptr);
			*GetAccumLightSlot() += light->shadowMapCount;
		}

		// Extended mode: pre-set kNONE renderTarget so RenderCascade re-runs
		// its slot-allocation block (where Hook_OverwriteShadowMapIndex
		// overrides the global counter with our slot index). Without this,
		// RenderCascade keeps the slot from a prior frame and lights not
		// redrawn this frame would corrupt another light's shadow map.
		// Pool index maps 1:1 to texture slot; slice 0 stays unused.
		if (s_settings.ShadowLightCount > 4)
			InitPromotedDescriptorSlots(light);

		// Only apply lens flare when lensFlareData is non-null; calling it on parabolic lights
		// (null lensFlareData) registers them into the lens flare system, causing a crash
		// in the lens flare pass when it tries to dereference the null sprite data.
		if (light->lensFlareData)
			GameApplyLensFlare(light);
	}

	// =========================================================================
	// Main shadow caster manager
	//
	// Replaces the game's CalculateActiveShadowCasterLights entirely.
	// Runs via stl::detour_thunk; obtains all inputs from game globals.
	// =========================================================================

	// Lightweight per-frame candidate entry used during scheduling.
	//
	// After the validation pass, exactly one of {chosen, excess, invalid}
	// is true (or none if it's the sun, which is processed separately).
	struct CandidateLight
	{
		RE::BSShadowLight* light{ nullptr };
		double score{ 0.0 };
		bool sun{ false };
		bool chosen{ false };         // valid + within ShadowLightCount budget
		bool excess{ false };         // valid but over budget (convert or disable)
		bool invalid{ false };        // shorthand: invalidCamera || invalidPortal
		bool invalidCamera{ false };  // UpdateCamera returned false -- shorthand for
									  // branches that don't care which sub-reason
		bool invalidPortal{ false };  // portal cull: light's cell not visible from
									  // camera's cell. Must DisableLight; converting
									  // routes through cluster lighting which has no
									  // portal awareness and would bleed through walls.

		// Sub-reasons for invalidCamera, recovered from engine side-band flags:
		//   frustrumCull == 0xff -> off-screen, ConvertLight wasted -> drop
		//   lodDimmer == 0.0f    -> past LOD fade end, still visible -> ConvertLight
		//                           (resets lodDimmer so cluster lighting picks it up)
		// Both can fire together; frustum-out wins (contribution is zero either way).
		bool invalidFrustum{ false };  // BSMultiBoundSphere::WithinFrustum / cone-frustum cull
		bool invalidLod{ false };      // engine's LOD-fade zeroed lodDimmer
	};

	// Why a candidate was demoted/disabled this frame, captured from the validation
	// flags so the shadow table can explain each "Conv" row. Populated in the
	// candidate tally loop (the flags are computed regardless); read only when the
	// debug table is open, so no runtime cost in normal play.
	enum class ConvertReason : uint8_t
	{
		None,
		Portal,           // not reachable through the visible portal graph
		FrustumDistance,  // off-screen or beyond the shadow-cull distance (frustrumCull)
		LodFaded,         // past the light's LOD fade distance (lodDimmer == 0)
		Excess,           // ranked below the shadow-caster budget
		CameraOther,      // UpdateCamera rejected it for some other reason
	};
	static std::unordered_map<uintptr_t, ConvertReason> s_convertReason;

	// Headless scheduling-diagnostics snapshot (devbench `inspect kind=llfshadows`).
	// The scheduler (render thread) fills s_schedSnapshot under the mutex at pass end;
	// RequestSchedSnapshot (devbench listener thread) reads it under the same mutex.
	// s_schedDumpFrames latches a short window of passes to keep filling it after a
	// request, so polling returns fresh data even while the menu is closed.
	static std::mutex s_schedSnapshotMutex;
	static SchedSnapshot s_schedSnapshot;
	static std::atomic<int> s_schedDumpFrames{ 0 };

	const char* SchedReasonName(uint8_t a_reason)
	{
		switch (static_cast<ConvertReason>(a_reason)) {
		case ConvertReason::Portal:
			return "portal";
		case ConvertReason::FrustumDistance:
			return "frustum";
		case ConvertReason::LodFaded:
			return "lod";
		case ConvertReason::Excess:
			return "excess";
		case ConvertReason::CameraOther:
			return "other";
		default:
			return "none";
		}
	}

	SchedSnapshot RequestSchedSnapshot()
	{
		// Prime ~2s of scheduling passes so repeated polls return fresh data even with
		// the menu closed; hand back the latest snapshot under the lock.
		s_schedDumpFrames.store(120, std::memory_order_relaxed);
		std::scoped_lock lock(s_schedSnapshotMutex);
		return s_schedSnapshot;
	}

	const char* ConvertReasonText(uintptr_t a_key)
	{
		auto it = s_convertReason.find(a_key);
		if (it == s_convertReason.end())
			return nullptr;
		switch (it->second) {
		case ConvertReason::Portal:
			return T(TKEY("conv_reason_portal"), "Reason: portal-culled -- the light's room isn't reachable through the visible portal graph.");
		case ConvertReason::FrustumDistance:
			return T(TKEY("conv_reason_frustum"), "Reason: frustum/distance-culled -- off-screen, or beyond the shadow-cull distance.");
		case ConvertReason::LodFaded:
			return T(TKEY("conv_reason_lod"), "Reason: LOD-faded -- past the light's LOD fade-out distance.");
		case ConvertReason::Excess:
			return T(TKEY("conv_reason_excess"), "Reason: excess -- ranked below the shadow-caster budget.");
		case ConvertReason::CameraOther:
			return T(TKEY("conv_reason_other"), "Reason: rejected by the engine visibility test.");
		default:
			return nullptr;
		}
	}

	// Shadow-light usability SEH backstop. The ClearLightArrays teardown hook is
	// the primary defense (clears our pool when the engine bulk-frees lights); this
	// SEH catches any residual AV in the membership/usability scan so a missed edge
	// is a skipped light, not a CTD. Kept until broad validation lets it go.
	static void LogShadowSehCatch()
	{
		static std::atomic<int> n{ 0 };
		if (n.fetch_add(1, std::memory_order_relaxed) < 20)
			logger::warn("[SCM] SEH caught AV in shadow-light usability scan (probe missed); skipping light");
	}

	// SEH backstop in its own function (no C++ unwinding objects) so MSVC accepts
	// __try. Returns false (unusable) on any access violation.
	template <class Fn>
	static bool SafeUsable(Fn&& a_fn, RE::BSShadowLight* a_light)
	{
		__try {
			return a_fn(a_light);
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
			LogShadowSehCatch();
			return false;
		}
	}

	// Run UpdateCamera + EnableLight + the validity scan in one SEH region: EnableLight can read a
	// freed light and corrupt accumLightSlot/bounds, AV'ing here; catching it skips the light (the
	// per-frame accumLightSlot reset recovers). __declspec(noinline) is load-bearing -- inlining
	// dissolves the __except. No C++ unwinding objects in this frame (MSVC __try).
	template <class UsableFn>
	__declspec(noinline) static bool SafeEnableAndValidate(LightEntry& e, RE::NiCamera* a_camera,
		RE::ShadowSceneNode* a_ssn, std::uint32_t a_slot, UsableFn&& a_isUsable)
	{
		__try {
			e.Light->UpdateCamera(a_camera);
			EnableLight(e.Light, a_camera, a_ssn, a_slot);
			return e.Light && a_isUsable(e.Light);
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
			LogShadowSehCatch();
			return false;
		}
	}

	void ScheduleShadowCasters()
	{
		ZoneScopedN("SCM::ScheduleShadowCasters");
		// Per-frame diagnostic counters; emitted via TracyPlot at function exit.
		s_schedDiag = SchedDiagCounters{};
		// VR calls CalculateAndDrawShadowCasterLights twice per frame (once per
		// eye). Block the second call: s_lights isn't reentrancy-safe.
		static std::atomic<bool> s_inSchedule{ false };
		if (s_inSchedule.exchange(true, std::memory_order_acquire))
			return;
		struct Guard
		{
			~Guard() { s_inSchedule.store(false, std::memory_order_release); }
		} guard;

		// VR display guard: skip scheduling when the HMD display is not active.
		if (globals::game::isVR && !GetVRDrawShadows())
			return;

		auto* ssn = GetShadowSceneNode();
		auto* camera = GetWorldCamera();
		if (!ssn || !camera)
			return;

		// Pause while the interior portal graph is mid-rebuild (cell transition).
		if (IsPortalGraphTransitioning())
			return;

		// Drain a pending teardown reset before touching any slot, then SKIP this pass.
		// ClearLightArrays freed the previous scene's lights but doesn't shrink
		// activeShadowLights, so snapshotting or scoring it now would touch freed memory.
		// The engine left the array vanilla-valid this frame; the next pass runs once it is
		// rebuilt. (Load-bearing: removing this return reintroduces the 20 SEH catches.)
		if (s_pendingSessionReset.exchange(false, std::memory_order_acquire)) {
			ResetSession();
			return;
		}

		// Hold a strong ref to every active shadow light for the whole pass. The scheduler
		// walks raw BSShadowLight*, but a concurrent bulk cell-teardown (ReleaseChildren,
		// which bypasses our RemoveLight hook) can free one mid-pass; a later write through
		// the stale pointer corrupts the recycled occupant's vtable -> CTD. Snapshot now
		// while activeShadowLights is valid; local so it releases at every return path.
		std::vector<RE::NiPointer<RE::BSShadowLight>> heldRefs;
		{
			auto& alive = ssn->GetRuntimeData().activeShadowLights;
			heldRefs.reserve(alive.size() + 1);
			for (auto& sp : alive)
				if (sp)
					heldRefs.push_back(sp);
		}

		// Couple the shadow-cull distance to the light fade before the validation
		// pass runs UpdateCamera (which reads the cached square). No-op unless
		// MatchShadowToLightFade is enabled.
		ApplyShadowToLightFadeMatch();

		// Maintain the demotion diagnostics this pass only when something can read them:
		// the open settings menu (Conv tooltip) or a recent devbench dump request. Keeps
		// the per-light hash churn + snapshot copy off the hot path otherwise.
		const bool wantDiag = Menu::GetSingleton()->IsEnabled ||
		                      s_schedDumpFrames.load(std::memory_order_relaxed) > 0;

		// Read the engine's per-frame focus-shadow actor count and reserve
		// matching pool slots. Eject any point lights that occupy a slot the
		// engine now claims for focus rendering -- the displaced lights are
		// reassigned to a free slot or fall through to the existing excess
		// path. When the count drops, the slots naturally rejoin the pool's
		// FindFreeIndex range on the next allocation.
		s_focusShadowSlots = std::clamp(GetFocusShadowActorCount(), 0, kFocusShadowMaxSlots);
		for (int i = kFocusShadowBaseSlotIndex; i < kFocusShadowBaseSlotIndex + s_focusShadowSlots && i < s_lights.Size; ++i) {
			if (s_lights.Lights[i].Light)
				s_lights.Lights[i].Clear();
		}

		// Do NOT clear shadowLightsAccum or reset the slot counter here. The
		// outer CalculateAndDrawShadowCasterLights calls ResetCalculatedShadow-
		// CasterLights before our hook fires, and that function clears the
		// array, resets the counter, AND installs the sun at slot 0. Re-
		// clearing here wipes the sun (sun->Accumulate is the focus vfunc,
		// not a slot allocator) and the engine then skips the directional
		// cascade pass entirely.

		s_budget.Begin(0);

		int doneLightCount = 0;
		RE::BSShadowLight* sunLight = nullptr;

		// ---- Sun / directional light ----
		if (!GetSunBool2()) {
			auto* sun = ssn->GetRuntimeData().sunShadowDirLight;
			if (sun) {
				static REL::Relocation<bool*> vrUpdateFlag{ REL::Offset(0x1ed62f8) };
				uint8_t vrFlag = globals::game::isVR ? static_cast<uint8_t>(*vrUpdateFlag) + 1 : 0;
				sun->Accumulate(*GetAccumLightSlot(), 0, nullptr, vrFlag);

				if (sun->lensFlareData && !globals::game::isVR)
					GameApplyLensFlare(sun);

				if (globals::game::isVR && !GetVRAccumFirst()) {
					GameVRPrepareShadowMaps(sun);
					GameVRAccumulateShadowMaps(sun);
				}

				sunLight = sun;
			}
		}

		// Extended mode: scrub drawFocusShadows on every active light and the
		// sun. A stale flag on a parabolic (point/spot) light occupying a
		// kSHADOWMAPS slot in [4..7] sends BSShadowParabolicLight::Render
		// into its focus-shadow loop on a non-directional light and CTDs.
		// Mirrors Intellightent's mitigation (see main.cpp:1411-1420); the
		// byte patches at SetupResources are belt-and-braces for the engine's
		// global gate, this is belt-and-braces for the per-light flag.
		if (s_settings.ShadowLightCount > 4) {
			for (auto& sp : ssn->GetRuntimeData().activeShadowLights) {
				if (auto* l = sp.get())
					ShadowField(l, drawFocusShadows) = false;
			}
			if (auto* sun2 = ssn->GetRuntimeData().sunShadowDirLight)
				ShadowField(sun2, drawFocusShadows) = false;
		}

		*GetSunPtr() = 0;

		// ---- Score all candidate lights ----
		// Reuse a static vector so we don't allocate per frame -- the
		// scheduler runs every frame and the candidate list is the same
		// shape size each call (a few hundred lights at most).
		static std::vector<CandidateLight> candidates;

		{
			ZoneScopedN("SCM::ScoreCandidates");
			SetupSceneFormula(camera);

			candidates.clear();
			candidates.reserve(ssn->GetRuntimeData().activeShadowLights.size());

			int32_t tmpIndex = 0;
			for (auto& sp : ssn->GetRuntimeData().activeShadowLights) {
				auto* l = sp.get();
				if (!l || l == sunLight)
					continue;
				// Promoted lights are allocated non-zeroed; their descriptor pool-slots stay
				// garbage until init. EnableLight only inits lights that win a render slot, so
				// init here (once) -- this is the type-safe BSShadowLight source -- to cover a
				// promoted light added but never enabled before teardown reads it.
				if (s_settings.ShadowLightCount > 4) {
					if (auto* ni = l->light.get(); ni) {
						std::scoped_lock lk(s_shadowConvertMutex);
						if (s_shadowConvert.contains(ni) && !s_shadowConvertDescriptorInited.contains(ni)) {
							InitPromotedDescriptorSlots(l);
							s_shadowConvertDescriptorInited.insert(ni);
						}
					}
				}
				auto& c = candidates.emplace_back();
				c.light = l;
				c.sun = false;
				c.score = CalculateLightScore(l, camera, tmpIndex++);
			}
#ifdef TRACY_ENABLE
			char buf[32];
			const int n = snprintf(buf, sizeof(buf), "candidates=%zu", candidates.size());
			if (n > 0)
				ZoneText(buf, static_cast<size_t>(n));
#endif
		}

		// Drop tracking entries whose NiLight left activeShadowLights -- pointer membership
		// only, never deref a freed light. This is the only safe cleanup: ResetSession can't
		// wipe (deferred, races the new cell's promotions) and bulk ClearLightArrays bypasses
		// the per-light erase, so without it promoted lights leak in and read as native.
		{
			std::scoped_lock lk(s_shadowConvertMutex);
			if (!s_shadowConvert.empty() || !s_shadowConvertDescriptorInited.empty()) {
				std::set<RE::NiLight*> liveNi;
				for (auto& c : candidates)
					if (auto* ni = c.light->light.get())
						liveNi.insert(ni);
				std::erase_if(s_shadowConvert, [&](RE::NiLight* ni) { return !liveNi.contains(ni); });
				std::erase_if(s_shadowConvertDescriptorInited, [&](RE::NiLight* ni) { return !liveNi.contains(ni); });
			}
		}

		// Validation, redraw-interval scoring, and RedrawFrame marking all
		// happen before the atomic loop. Tracy capture analysis showed this
		// block dominates SCM::ScheduleShadowCasters (98%+ of the function's
		// runtime), so a dedicated zone scopes that cost separately from
		// ScoreCandidates and ScheduleLoop. Named variant because the
		// enclosing function already declares a ZoneScopedN.
		ZoneNamedN(zoneValBudget, "SCM::ValidateAndScheduleBudget", true);

		// Apply debug pins: bias scoring so pinned-shadow lights sort to the
		// top (forced into the chosen pool up to ShadowLightCount) and
		// pinned-convert lights sort to the bottom (forced into the excess pool
		// where ConvertLight runs unconditionally — see c.excess branch below).
		// Pin sets are mutually exclusive (SetPinned* enforces that), but if a
		// stale entry slips through, pin-shadow wins because the bias is checked
		// first.
		for (auto& c : candidates) {
			auto key = reinterpret_cast<uintptr_t>(c.light);
			if (s_pinShadow.count(key))
				c.score += 1e15;
			else if (s_pinConvert.count(key))
				c.score -= 1e15;
		}

		// Sort descending by score (highest priority first); sun always first.
		std::sort(candidates.begin(), candidates.end(),
			[](const CandidateLight& a, const CandidateLight& b) {
				if (a.sun != b.sun)
					return a.sun;
				return a.score > b.score;
			});

		// ---- Validation pass (no game mutations) ----
		//
		// Mirrors Intellightent's per-iteration validation gates. Splitting
		// validation from mutation lets us defer all game-state changes
		// (DisableLight / ConvertLight / EnableLight) to a single atomic loop
		// later, eliminating the dangling-pointer crash window where mutations
		// in an earlier phase invalidated raw pointers held in s_lights[].
		//
		// Slot 0 is reserved for the sun; point lights fill slots 1..ShadowLightCount.
		// Do not count the sun against ShadowLightCount -- it uses focus cascade DSV slots,
		// not parabolic point-light slots.
		auto* globalCull = *reinterpret_cast<RE::BSCullingProcess**>(
			*reinterpret_cast<uintptr_t**>(
				REL::RelocationID(528077, 415022).address()));

		int wantCount = 0;

		// Per-candidate UpdateCamera vfunc + portal-graph visibility walk
		// + chosen/excess tagging. Captured separately so memoization or
		// caching of UpdateCamera/portal verdicts can be measured.
		{
			ZoneNamedN(zoneCandVal, "SCM::CandidateValidation", true);
			for (auto& c : candidates) {
				auto* l = c.light;
				// UpdateCamera (vfunc 16, +0x80) is the engine's type-aware visibility
				// test. Verified via Ghidra (BSShadowParabolicLight_UpdateCamera at
				// 0x14151b620 in 1.6.1170, 0x14132ddf0 in 1.6.640, 0x141370c80 in VR):
				//
				//   - BSShadowParabolicLight: TWO cull conditions, both setting
				//     frustrumCull=0xff:
				//       (1) BSMultiBoundSphere::WithinFrustum (BSMultiBoundShape
				//           vfunc 0x29) -- sphere(niLight.pos, niLight.Radius.x)
				//           vs camera frustum. Geometrically correct;
				//           failure means no visible pixel can be lit because the
				//           light's bounding sphere doesn't touch the camera frustum.
				//           The radius source matches what the cluster builder reads
				//           (LightLimitFix.cpp's `runtimeData.radius.x`).
				//       (2) Shadow-distance LOD -- if (lodFade flag set on
				//           BSShadowLight) AND
				//           ((camDist^2 - radius^2) * camera.LodAdjust) >
				//               ShadowDistanceSquared_Current => cull.
				//           ShadowDistanceSquared_Current = fShadowDistance^2
				//           (8000^2 outdoors, 3000^2 indoors by default).
				//           This is NOT a visibility test -- it's "skip per-light
				//           shadow rendering at this distance". A light past
				//           shadow distance can still be IN the camera frustum and
				//           illuminating visible pixels via cluster lighting.
				//
				//   - BSShadowFrustumLight: cone-vs-frustum test (cone-aware so an
				//     off-screen spot pointing INTO the frustum is correctly kept).
				//
				//   - BSShadowDirectionalLight: cascades, separate code path.
				//
				// Implication for SCM: a `frustrumCull != 0` verdict does NOT mean
				// "geometrically off-screen". The convertOrDisable path below treats
				// all c.invalid cases uniformly (omnis convert, spots disable, portal
				// disable) so distant lights past shadow distance still reach the
				// cluster pipeline. The cluster builder's own
				// `(color * fade) > 1e-4 && radius > 1e-4` filter discards lights
				// that genuinely don't contribute.
				if (!l->UpdateCamera(camera)) {
					c.invalidCamera = true;
					c.invalid = true;
					// Recover the sub-reason from the engine's side-band flags.
					// Both can be true (a light off-screen AND LOD-faded);
					// recorded as independent bits for analysis. Action loop
					// below treats frustum-out as terminal (drop) and
					// LOD-faded-in-frustum as convert.
					c.invalidFrustum = (l->frustrumCull != 0);
					c.invalidLod = (l->lodDimmer == 0.0f);
					continue;
				}
				// Portal culling only applies in interior cells where a portal graph exists.
				// Lights with no culling process (e.g. WSU spotlights outside cell bounds)
				// or no portal are unconditionally visible; skip the check for them.
				// Promoted lights carry a rebuilt culling process whose portal-graph entry the
				// engine never room-associates (always visibleUnboundSpace), so the test
				// false-culls an in-view light. The verdict is unreliable for them by
				// construction -- always skip the demotion; native lights still get it.
				auto* cull = IsPromotedLight(l->light.get()) ? nullptr : GetLightCullingProcess(l);
				if (cull) {
					auto* portal = reinterpret_cast<RE::BSPortalGraphEntry*>(cull->portalGraphEntry);
					if (portal) {
						auto* gPortal = globalCull ? reinterpret_cast<RE::BSPortalGraphEntry*>(globalCull->portalGraphEntry) : nullptr;
						if (gPortal && !GamePortalHasSharedVisibility(gPortal, portal)) {
							c.invalidPortal = true;
							c.invalid = true;
							continue;
						}
					}
				}

				// Effective point-light capacity excludes the engine-claimed
				// focus shadow slots; excess candidates fall through to the
				// existing convert/disable path.
				if (wantCount < s_settings.ShadowLightCount - s_focusShadowSlots) {
					c.chosen = true;
					wantCount++;
				} else {
					c.excess = true;
				}
			}

			// Tracy candidate breakdown: emits per-frame so a capture can be
			// queried alongside the per-action counters to verify the math
			// (chosen + excess + invalid_camera + invalid_portal == total).
			// Populate the demotion map only when wantDiag (menu open or a devbench
			// dump was requested) -- skip the per-frame hash churn otherwise.
			if (wantDiag)
				s_convertReason.clear();
			for (auto& c : candidates) {
				s_schedDiag.candidates_total++;
				if (c.chosen)
					s_schedDiag.candidates_chosen++;
				if (c.excess)
					s_schedDiag.candidates_excess++;

				// Capture why a non-chosen light is demoted, for the shadow table.
				// Portal wins (distinct disable path), then frustum/distance, LOD,
				// excess -- matching the atomic loop's branch precedence.
				if (wantDiag && !c.chosen) {
					ConvertReason r = ConvertReason::None;
					if (c.invalidPortal)
						r = ConvertReason::Portal;
					else if (c.invalidFrustum)
						r = ConvertReason::FrustumDistance;
					else if (c.invalidLod)
						r = ConvertReason::LodFaded;
					else if (c.excess)
						r = ConvertReason::Excess;
					else if (c.invalidCamera)
						r = ConvertReason::CameraOther;
					if (r != ConvertReason::None)
						s_convertReason[reinterpret_cast<uintptr_t>(c.light)] = r;
				}
				if (c.invalidCamera)
					s_schedDiag.candidates_invalid_camera++;
				if (c.invalidPortal)
					s_schedDiag.candidates_invalid_portal++;
				// Sub-reason breakdown of invalidCamera. A single light may
				// be both frustum-out AND LOD-faded -- both bits are counted
				// so the sum can exceed candidates_invalid_camera. The
				// "other" bucket catches UpdateCamera failures where the
				// engine cleared frustrumCull and left lodDimmer > 0 (rare
				// edge cases like internal state changes).
				if (c.invalidCamera) {
					if (c.invalidFrustum)
						s_schedDiag.candidates_invalid_frustum++;
					if (c.invalidLod)
						s_schedDiag.candidates_invalid_lod++;
					if (!c.invalidFrustum && !c.invalidLod)
						s_schedDiag.candidates_invalid_other++;
				}
			}
		}  // end SCM::CandidateValidation

		// Pool membership update: drop expired pointers, drop unchosen,
		// add newly chosen, sync sun slot.
		{
			ZoneNamedN(zonePoolMem, "SCM::UpdatePoolMembership", true);
			// ---- Sync s_lights (our active pool) ----
			//
			// First drop entries whose pointers are no longer in the scene's
			// activeShadowLights (game-side may have freed them since last frame).
			// This protects subsequent slot-stability lookups from dereferencing
			// dangling pointers.
			std::unordered_set<RE::BSShadowLight*> aliveSet;
			{
				auto& alive = ssn->GetRuntimeData().activeShadowLights;
				aliveSet.reserve(alive.size() + 1);
				if (sunLight)
					aliveSet.insert(sunLight);
				for (auto& sp : alive)
					if (auto* l = sp.get())
						aliveSet.insert(l);
			}
			for (int i = 0; i < s_lights.Size; i++) {
				if (!s_lights.Lights[i].Light)
					continue;
				if (aliveSet.find(s_lights.Lights[i].Light) == aliveSet.end()) {
					s_schedDiag.reconciliation_clears++;
					s_lights.Lights[i].Clear();
				}
			}

			// ---- Sync s_normalConvert (converted-to-non-shadow set) ----
			//
			// Two-tier filter:
			//
			// Tier 1: drop entries the engine has removed from BOTH active
			// lists. Hook_ConvertLights_Remove fires on individual RemoveLight
			// calls but the engine's bulk cell-teardown path bypasses it, so
			// this is our safety net for dangling pointers.
			//
			// Tier 2: drop entries that are functionally dead -- still in
			// activeShadowLights / activeLights (because GameEnableLight from
			// ConvertLight activates an entry that the engine never
			// auto-deactivates), but with fade=0 / lodDimmer=0 / null NiLight
			// so addLight in LightLimitFix would skip them anyway.
			//
			// Without tier 2 the set grows unbounded across a session: every
			// converted light stays pinned in s_normalConvert until the engine
			// triggers a removal we can hook. Heavy modlists hit 400+ entries,
			// keeping freed-then-recycled BSLight memory referenced by
			// downstream pass captures longer than necessary. The criteria
			// mirror addLight's discard filter -- entries failing it
			// contribute nothing to the cluster or engine lighting paths and
			// have no business staying in our set.
			if (!s_normalConvert.empty()) {
				std::unordered_set<RE::BSLight*> normalAlive;
				normalAlive.reserve(aliveSet.size() + ssn->GetRuntimeData().activeLights.size());
				for (auto* p : aliveSet)
					normalAlive.insert(static_cast<RE::BSLight*>(p));
				for (auto& sp : ssn->GetRuntimeData().activeLights)
					if (auto* l = sp.get())
						normalAlive.insert(l);

				const std::size_t before = s_normalConvert.size();
				std::erase_if(s_normalConvert, [&](const ConvertedLight& c) {
					// Tier 1: dangling / engine-removed.
					if (!c.light || normalAlive.find(static_cast<RE::BSLight*>(c.light)) == normalAlive.end())
						return true;
					// Tier 2: functionally dead. Cheap derefs only -- no
					// virtual calls or extra hash lookups.
					auto* niLight = c.light->light.get();
					if (!niLight)
						return true;
					const auto& rt = niLight->GetLightRuntimeData();
					const float colorSum = rt.diffuse.red + rt.diffuse.green + rt.diffuse.blue;
					if (colorSum * rt.fade <= 1e-4f)
						return true;
					if (rt.radius.x <= 1e-4f)
						return true;
					return false;
				});
				const std::size_t after = s_normalConvert.size();
				if (before != after) {
					static int loggedShrink = 0;
					if (loggedShrink++ < 20 || (before - after) > 32) {
						logger::debug("[SCM] s_normalConvert reconcile: {} -> {} ({} dropped)",
							before, after, before - after);
					}
				}
			}

			// Drop entries no longer chosen. Rank-drift suppression now lives
			// in CalculateLightScore via the lightframessincerender decay term
			// in the default ScoreFormula; the slot pool itself is a dumb
			// container that follows the chosen set without policy of its own.
			// The atomic loop's c.excess / c.invalid branches handle the
			// engine-side ConvertLight / DisableLight call for the dropped
			// occupants on the same frame.
			for (int i = 0; i < s_lights.Size; i++) {
				if (!s_lights.Lights[i].Light)
					continue;
				bool stillChosen = (i == 0 && s_lights.Sun);  // sun slot
				if (!stillChosen) {
					for (auto& c : candidates) {
						if (c.light == s_lights.Lights[i].Light && c.chosen) {
							stillChosen = true;
							break;
						}
					}
				}
				if (!stillChosen)
					s_lights.Lights[i].Clear();
			}

			// Add newly chosen lights (assigned to first free slot; keeps existing chosen lights in place).
			for (auto& c : candidates) {
				if (!c.chosen)
					continue;
				bool alreadyIn = false;
				for (int i = 0; i < s_lights.Size && !alreadyIn; i++)
					if (s_lights.Lights[i].Light == c.light)
						alreadyIn = true;
				if (alreadyIn)
					continue;

				int idx = s_lights.FindFreeIndex(true, s_settings.ShadowLightCount, s_settings.ConvertedShadowSlots);
				if (idx < 0)
					continue;
				// Eviction nulls Light* but leaves the rest of LightEntry intact
				// so it can serve as a cache key. Clear at acquire so the new
				// occupant doesn't inherit LastDrawnFrame / lastGeomHash from the
				// previous owner (which would skip its first render and let the
				// cluster pipeline sample stale kSHADOWMAPS[idx] content).
				s_lights.Lights[idx].Clear();
				s_lights.Lights[idx].Light = c.light;
			}

			// Update sun slot (slot 0).
			if (sunLight) {
				if (s_lights.Lights[0].Light != sunLight) {
					s_lights.Lights[0].Clear();
					s_lights.Lights[0].Light = sunLight;
				}
				s_lights.Sun = true;
			} else {
				// Sun is gone. If slot 0 was tracking the sun, clear the stale
				// pointer. If Sun was already false coming in, slot 0 holds a
				// regular point light (sun-aware FindFreeIndex allocates point
				// lights to slot 0 when Sun=false) -- do NOT wipe it. This
				// matches Intellightent's reference behaviour (no unconditional
				// slot-0 clear in the no-sun branch).
				if (s_lights.Sun)
					s_lights.Lights[0].Clear();
				s_lights.Sun = false;
			}
		}  // end SCM::UpdatePoolMembership

		// ---- Temporal budget: decide which lights redraw this frame ----
		double budget = s_settings.RedrawBudgetMs;
		{
			// Frame-time EMA + budget formula evaluation. Scoped separately
			// from ScheduleLoop so the once-per-frame budget cost is visible
			// distinct from the per-light scheduling cost.
			{
				ZoneNamedN(zoneCompBud, "SCM::ComputeBudget", true);
				// Update frame-time EMA and ring buffer (always, for formula params and UI).
				const float dtMs = *globals::game::deltaTime * 1000.0f;
				s_ftRing[s_ftHead] = dtMs;
				s_ftHead = (s_ftHead + 1) % kFrameWindow;
				if (s_ftCount < kFrameWindow)
					++s_ftCount;
				s_ftEMA = (s_ftCount == 1) ? dtMs : 0.1f * dtMs + 0.9f * s_ftEMA;

				const float target_ms = ComputeFrameTimePercentile90();
				if (s_ftEMA < target_ms)
					s_stableFrames = std::min(s_stableFrames + 1, 45);
				else
					s_stableFrames = 0;

				FormulaHelper::SetParam(kFormulaParam_FrameTime, static_cast<double>(s_ftEMA));
				FormulaHelper::SetParam(kFormulaParam_FrameTarget, static_cast<double>(target_ms));
				FormulaHelper::SetParam(kFormulaParam_StableFrames, static_cast<double>(s_stableFrames));

				// Evaluate the budget for the whole frame.
				//   Manual:  fixed slider value (RedrawBudgetMs).
				//   Formula: user-editable exprtk expression.
				if (s_settings.BudgetMode == BudgetModeEnum::Formula && s_formulaRedrawBudget) {
					budget = s_formulaRedrawBudget->Calculate();
				}
				s_autoBudgetMs = static_cast<float>(budget);
			}  // end SCM::ComputeBudget

			s_redrawnLightsThisFrame = 0;
			s_totalShadowLightsThisFrame = s_settings.ShadowLightCount;

			ZoneScopedN("SCM::ScheduleLoop");
			int maxRedraw = std::min(s_settings.MaxRedrawPerFrame, s_lights.Size);
			int32_t budgetRemain = static_cast<int32_t>(budget * 1000.0);
			bool isFirst = true;
			int32_t now = *globals::game::frameCounter;

			// Clear RedrawFrame on slots OUTSIDE the point-light range (converted /
			// otherwise-allocated). Note PointLightEnd accounts for the sun
			// bookkeeping slot when Sun=true, so a converted-slot light at
			// pool[ShadowLightCount + 1] correctly gets cleared.
			for (int i = s_lights.PointLightEnd(s_settings.ShadowLightCount); i < s_lights.Size; i++)
				s_lights.Lights[i].RedrawFrame = false;

			// First pass: sun only. Point-light slots fall through to the
			// importance-scored pending loop below so new lights compete
			// fairly with existing redraws (sorted by importance, not pool
			// order). AllowDrawNewLight is honoured by the pending loop's
			// filter.
			for (int i = 0; i < s_lights.Size; i++) {
				auto& e = s_lights.Lights[i];
				if (!e.Light) {
					e.RedrawFrame = false;
					continue;
				}
				e.RedrawFrame = (i == 0 && s_lights.Sun);
				if (e.RedrawFrame) {
					e.LastDrawnFrame = now;
					isFirst = false;
					maxRedraw--;
					// Sun's budget cost is bookkept at 0 (different texture
					// pipeline -- it has its own cascade buffer), so no
					// budgetRemain decrement.
				}
			}

			if (maxRedraw > 0 && budgetRemain > 0) {
				std::vector<LightEntry*> pending;
				for (int i = 0; i < s_lights.Size; i++) {
					auto& e = s_lights.Lights[i];
					if (!e.Light || e.RedrawFrame)
						continue;
					// Honour AllowDrawNewLight: when disabled, brand-new
					// entries (LastDrawnFrame < 0) wait until the next frame
					// rather than competing for this frame's budget. Existing
					// lights re-entering view still schedule normally.
					if (!s_settings.AllowDrawNewLight && e.LastDrawnFrame < 0)
						continue;
					pending.push_back(&e);
				}

				for (auto* e : pending) {
					double interval = 0.0;
					if (s_formulaRedrawInterval) {
						SetupLightFormula(e->Light, camera, 0);
						// e->Index is the pool index. Beyond PointLightEnd are converted slots.
						if (e->Index >= s_lights.PointLightEnd(s_settings.ShadowLightCount))
							FormulaHelper::SetParam(kFormulaParam_LightConverted, 1.0);

						// Compute how far the light has moved since its last shadow map render.
						// Exposed as `lightdisplacement` so the formula can prioritise fast-moving
						// lights (e.g. player torches) without relying on distance-to-camera alone.
						if (auto* nilight = e->Light->light.get()) {
							auto& curr = nilight->world.translate;
							float dx = curr.x - e->lastRenderedPos.x;
							float dy = curr.y - e->lastRenderedPos.y;
							float dz = curr.z - e->lastRenderedPos.z;
							FormulaHelper::SetParam(kFormulaParam_LightDisplacement,
								static_cast<double>(sqrtf(dx * dx + dy * dy + dz * dz)));
						}

						interval = s_formulaRedrawInterval->Calculate();
					}
					interval += 1.0;

					// Contribution-weighted redraw interval:
					//   importance = luminance(diffuse × fade) × max(att_cam, att_plr)
					//   att(pos)   = max(1 - (dist/radius)^2, 0)^2     (Skyrim falloff)
					//   interval  *= 2.0 * (0.025/2.0)^importance
					// importance=0 -> x2.0 (deprioritise), 0.5 -> ~x0.32, 1.0 -> ~x0.05.
					// Refs: Wimmer & Scherzer 2006 "Instant Shadow Maps" sec. 3;
					//       Valient 2014 "Practical Shadow Maps".

					float importance = 0.0f;

					if (auto* ni = e->Light->light.get()) {
						auto& rtd = ni->GetLightRuntimeData();
						float lightRadius = rtd.radius.x;
						auto lp = ni->world.translate;

						// Perceptual luminance (Rec.709) × engine fade factor.
						float lum = 0.2126f * rtd.diffuse.red +
						            0.7152f * rtd.diffuse.green +
						            0.0722f * rtd.diffuse.blue;
						float effectiveLum = lum * rtd.fade;

						// Primary: screen-space projected solid angle.
						// "How much of the view does this light's influence
						// sphere occupy?" Industry standard for many-light
						// shadow prioritisation -- see Olsson & Assarsson 2012,
						// "Clustered Deferred and Forward Shading"; Wronski
						// 2014, "Sample Distribution Shadow Maps"; CryEngine
						// shadow LOD docs. Approximates angular radius from
						// camera as radius/viewZ; solid angle ~ angularRadius^2.
						// Constants (screenH / 2*tan(fovY/2))^2 drop out -- they're
						// the same across all lights and don't affect ranking.
						//
						// Edge cases:
						//   viewZ < -radius : light fully behind camera, coverage=0
						//   |viewZ| < radius : light intersects camera plane;
						//                      clamp effectiveZ to avoid blow-up.
						float coverage = 0.0f;
						if (camera) {
							auto cp = camera->world.translate;
							RE::NiPoint3 fwd = camera->world.rotate.GetVectorY();
							float rx = lp.x - cp.x, ry = lp.y - cp.y, rz = lp.z - cp.z;
							float viewZ = fwd.x * rx + fwd.y * ry + fwd.z * rz;
							if (viewZ > -lightRadius) {
								float effectiveZ = std::max(viewZ, lightRadius * 0.5f);
								float angularRadius = lightRadius / effectiveZ;
								coverage = angularRadius * angularRadius;
							}
						}

						// Fallback: Skyrim-style quadratic distance falloff
						// from camera/player. Covers two cases where coverage
						// alone returns 0 but the user still sees shadows:
						//   1. Light just outside the frustum (around a corner)
						//      illuminating a visible wall.
						//   2. Player-held torch behind the camera lighting
						//      geometry ahead.
						// Weighted at 0.3 -- coverage dominates when the light
						// is in view, but out-of-view lights still get a floor
						// proportional to their illumination at the viewer.
						auto computeAtt = [&](const RE::NiPoint3& pos) -> float {
							float dx = pos.x - lp.x, dy = pos.y - lp.y, dz = pos.z - lp.z;
							float dist2 = dx * dx + dy * dy + dz * dz;
							float r2 = lightRadius * lightRadius;
							if (dist2 >= r2)
								return 0.0f;
							float t = dist2 / r2;
							float a = 1.0f - t;
							return a * a;  // matches Skyrim (1-(d/r)^2)^2 falloff
						};
						auto* plr = RE::PlayerCharacter::GetSingleton();
						float attCam = camera ? computeAtt(camera->world.translate) : 0.0f;
						float attPlr = plr ? computeAtt(plr->GetPosition()) : attCam;
						float distanceFallback = std::max(attCam, attPlr) * 0.3f;

						importance = effectiveLum * std::max(coverage, distanceFallback);
					}

					// Exponential interval scaling: maxScale*(minScale/maxScale)^clamp(importance,0,1)
					float kMaxMult = s_settings.ImportanceMaxScale;
					float kMinMult = std::min(s_settings.ImportanceMinScale, kMaxMult);
					float clampedImp = std::min(importance, 1.0f);
					interval *= static_cast<double>(kMaxMult * powf(kMinMult / kMaxMult, clampedImp));

					FormulaHelper::SetParam(kFormulaParam_LightImportance, static_cast<double>(importance));
					e->RedrawScore = e->LastDrawnFrame + interval;
					e->lastImportance = importance;

					// Cached shadow maps: if the geometry hash matches what we
					// rendered last time, the shadow map currently in the slot
					// is byte-identical to what a fresh re-render would produce.
					// No need to redraw -- push the score sky-high so this entry
					// loses every budget contest unless literally nothing else
					// needs redrawing (defensive: still allow eventual refresh
					// against any hashing bugs).
					//
					// Industry-standard pattern: UE5 "Cached Shadow Maps",
					// Frostbite movable-light caching. The hash captures
					// (1) light's own pose + radius and (2) each caster's
					// worldBound + identity -- both rigid motion and engine-
					// updated bounds (BSDynamicTriShape vertex changes update
					// worldBound).
					e->pendingGeomHash = ComputeShadowGeomHash(e->Light);
					if (e->LastDrawnFrame >= 0 && e->lastGeomHash != 0 &&
						e->pendingGeomHash == e->lastGeomHash) {
						e->RedrawScore += 1e15;
					}
				}

				// Count lights meaningfully illuminating the viewer area.
				s_highImportanceLightCount = static_cast<uint32_t>(
					std::count_if(pending.begin(), pending.end(),
						[](const LightEntry* e) { return e->lastImportance > 0.1f; }));

				std::sort(pending.begin(), pending.end(),
					[](const LightEntry* a, const LightEntry* b) { return a->RedrawScore < b->RedrawScore; });

				for (auto* e : pending) {
					if (maxRedraw <= 0)
						break;
					if (budgetRemain <= 0)
						break;
					int32_t budgetEstimate = s_budget.GetCost(e->Light);
					if (isFirst) {
						if (!s_lights.Sun || e->Index > 0)
							budgetRemain -= budgetEstimate;
						maxRedraw--;
						e->RedrawFrame = true;
						e->LastDrawnFrame = now;
						e->lastGeomHash = e->pendingGeomHash;
						isFirst = false;
						continue;
					}
					if (budgetEstimate <= budgetRemain) {
						budgetRemain -= budgetEstimate;
						maxRedraw--;
						e->RedrawFrame = true;
						e->LastDrawnFrame = now;
						e->lastGeomHash = e->pendingGeomHash;
						continue;
					}
				}
			}
		}

		// Count how many shadow lights are scheduled to redraw this frame.
		// Iterate the point-light range (sun-aware: skips pool[0] when Sun=true).
		s_redrawnLightsThisFrame = 0;
		for (int j = s_lights.PointLightFirst(); j < s_lights.PointLightEnd(s_settings.ShadowLightCount); j++) {
			if (s_lights.Lights[j].RedrawFrame)
				++s_redrawnLightsThisFrame;
		}

		// EWMA so the UI counter doesn't flicker frame-to-frame.
		s_redrawnLightsSmoothed = 0.8f * s_redrawnLightsSmoothed + 0.2f * s_redrawnLightsThisFrame;

		// Atomic per-candidate loop: process each score-sorted candidate to
		// completion before moving on. Branch dispatch:
		//   chosen + RedrawFrame + slot in budget: EnableLight + render
		//   chosen otherwise:                      DisableLight (re-added below
		//                                          via GameSetShadowCasterSlot)
		//   excess + ConvertExcessToNormal:        ConvertLight
		//   excess otherwise / invalid:            DisableLight
		//
		// Ordering matters: chosen (rank < ShadowLightCount) runs before any
		// excess. ConvertLight's ReturnShadowmaps can mutate activeShadowLights
		// and free other BSShadowLights, but by then chosen entries have
		// already completed EnableLight + budget pairing in-iteration -- no
		// later phase walks those pointers.
		//
		// isUsableLight() per-iteration guard catches dangling pointers if an
		// earlier EnableLight invalidated a later candidate via scene mutation.

		auto* shadowSceneNodeRT = &ssn->GetRuntimeData();

		// Two-stage validity check used before any virtual dispatch on a
		// BSShadowLight from s_lights[] or candidates[]:
		//   (1) Is the pointer still in the scene's activeShadowLights?
		//       (catches "removed since last frame")
		//   (2) Is the vtable non-zero?
		//       (catches "freed and zeroed by tbbmalloc / EngineFixes via a path
		//        that bypassed BSSmartPointer ref-counting" — the pointer is
		//        still in activeShadowLights but the object is dead)
		// Either failure → caller must skip the light.
		auto isAliveNow = [shadowSceneNodeRT, sunLight](RE::BSShadowLight* l) -> bool {
			if (!l)
				return false;
			if (l == sunLight)
				return true;
			// Membership scan over activeShadowLights. The ClearLightArrays
			// teardown hook (+ ResetSession/skip) keeps us from scanning a
			// torn-down array; SafeUsable (SEH) is the remaining backstop.
			for (auto& sp : shadowSceneNodeRT->activeShadowLights)
				if (sp.get() == l)
					return true;
			return false;
		};
		auto isVtableValid = [](RE::BSShadowLight* l) -> bool {
			return l && *reinterpret_cast<const uintptr_t*>(l) != 0;
		};
		auto isUsableLight = [&](RE::BSShadowLight* l) -> bool {
			return isAliveNow(l) && isVtableValid(l);
		};

		auto findSlotForLight = [](RE::BSShadowLight* l) -> int {
			for (int i = 0; i < s_lights.Size; i++)
				if (s_lights.Lights[i].Light == l)
					return i;
			return -1;
		};

		// Single decision point for "this light won't shadow this frame --
		// Convert (keeps diffuse via cluster pipeline) or Disable (light
		// vanishes)?". Used by both the c.invalid and c.excess branches.
		//
		// Spots always Disable: the engine has no NiSpotLight equivalent, so
		// ConvertLight on a BSShadowFrustumLight would make the cone-shaped
		// illumination spherical and bleed through walls behind the cone.
		// Omnis/hemis Convert when ConvertExcessToNormal is on or a debug
		// pin-convert is set on this light. The pin override applies even
		// when the user disabled ConvertExcessToNormal globally.
		//
		// allowConvert is a callsite veto -- the c.invalid path passes it
		// false for invalidPortal (cluster has no portal-graph awareness,
		// converting would leak light across cells) so portal-occluded
		// lights always Disable.
		//
		// Returns true on Convert, false on Disable, so callers can apply
		// path-specific follow-ups (e.g. lodDimmer=1 reset on the invalidLod
		// path so the converted light still contributes to clusters).
		auto convertOrDisable = [&](RE::BSShadowLight* light, bool allowConvert) -> bool {
			const bool isSpot = light->GetIsFrustumLight();
			const bool forceConvert = s_pinConvert.count(reinterpret_cast<uintptr_t>(light)) > 0;
			if (allowConvert && (s_settings.ConvertExcessToNormal || forceConvert) && !isSpot) {
				ConvertLight(light, ssn, false);
				return true;
			}
			DisableLight(light);
			return false;
		};

		// Sun slot (slot 0) is processed inline below — sun setup happened at the
		// top of the function; we only need to mark its mask index here.
		if (s_lights.Sun && s_lights.Lights[0].Light && s_lights.Lights[0].RedrawFrame) {
			ShadowField(s_lights.Lights[0].Light, maskIndex) = 0;
			doneLightCount++;
		}

		// Per-candidate Begin/EnableLight/End mutation loop. EnableLight may
		// trigger synchronous shadow render dispatches in the engine, so this
		// zone captures both our scheduler work and any engine-side rendering
		// it pulls in for chosen lights.
		{
			// Hold the graph lock shared for the whole mutation loop so ResetScene (main thread)
			// can't null ssn->portalGraph while the engine mutations below (ConvertLight /
			// DisableLight / EnableLight -> AccumulateLight) deref it. try_to_lock: skip the
			// pass if a teardown holds it exclusive, rather than block.
			std::shared_lock<std::shared_mutex> graphLock(s_portalGraphMutex, std::try_to_lock);
			if (!graphLock.owns_lock())
				return;
			if (IsPortalGraphTransitioning())  // re-check once; stable now under the lock
				return;
			ZoneNamedN(zoneAtomic, "SCM::AtomicMutationLoop", true);
			for (auto& c : candidates) {
				if (c.invalid) {
					// isUsableLight (membership + vtable) is the same gate the
					// excess branch uses. Both ConvertLight and DisableLight
					// fan into virtually-dispatched callees (ReturnShadowmaps),
					// so a freed-but-canonical pointer must be skipped for
					// either path.
					if (!isUsableLight(c.light))
						continue;

					// All c.invalid cases route through convertOrDisable. Per the
					// Ghidra-verified UpdateCamera analysis above, frustrumCull
					// is set both by the genuine sphere-vs-frustum cull AND by
					// the shadow-distance LOD cull; treating them uniformly lets
					// distant lights past shadow distance still reach the
					// cluster pipeline. allowConvert=c.invalidCamera so portal-
					// occluded omnis fall to Disable (cluster lighting has no
					// portal-graph awareness and would leak across cells).
					ZoneNamedN(zCvt, "SCM::Engine::convertOrDisable(invalid)", true);
					if (convertOrDisable(c.light, /*allowConvert=*/c.invalidCamera)) {
						s_schedDiag.converted_invalid++;
						// UpdateCamera zeros lodDimmer on its shadow-distance LOD cull; the cluster
						// builder multiplies fade*lodDimmer and drops the light below 1e-4. Restore
						// only when fully zeroed (preserves any smooth fade), matching UpdateLights.
						// Re-validate first: a concurrent free could recycle c.light, and a 1.0f store
						// would corrupt the new occupant's vtable -> CTD (heldRefs should cover it).
						if (SafeUsable(isUsableLight, c.light) && c.light->lodDimmer == 0.0f)
							c.light->lodDimmer = 1.0f;
					} else {
						s_schedDiag.disabled_invalid++;
					}
					continue;
				}

				if (c.chosen) {
					int slot = findSlotForLight(c.light);
					if (slot < 0)
						continue;  // matches old behaviour: chosen-but-no-slot is a no-op
					if (slot == 0 && s_lights.Sun)
						continue;  // sun handled above

					auto& e = s_lights.Lights[slot];

					// Render-this-frame path is reserved for chosen point-light slots
					// (excludes converted slots which start at PointLightEnd). Use
					// the sun-aware bound so pool[ShadowLightCount] (the highest
					// point-light slot when Sun=true) is included.
					if (e.RedrawFrame && slot < s_lights.PointLightEnd(s_settings.ShadowLightCount)) {
						// Render-this-frame path. A previous iteration's EnableLight
						// may have transitively freed this light via game-side scene
						// mutations (membership change OR tbbmalloc-zeroed memory),
						// so re-validate before any virtual dispatch.
						if (!SafeUsable(isUsableLight, e.Light)) {
							e.Light = nullptr;
							continue;
						}

						auto* lightSnapshot = e.Light;  // value snapshot for budget pairing
						s_budget.BeginLight(lightSnapshot, 0);
						// EnableLight can null e.Light or free the BSShadowLight mid-call (then read
						// shadowMapCount from it) -> AV. SafeEnableAndValidate catches it (noinline SEH).
						bool stillUsable;
						{
							ZoneNamedN(zEnable, "SCM::Engine::EnableLight", true);
							stillUsable = SafeEnableAndValidate(e, camera, ssn, slot, isUsableLight);
						}
						if (!stillUsable)
							continue;
						s_budget.EndLight(lightSnapshot, 0);

						if (auto* nilight = e.Light->light.get())
							e.lastRenderedPos = nilight->world.translate;

						ShadowField(e.Light, maskIndex) = static_cast<uint32_t>(slot);
						doneLightCount++;
					}
					// Cached-shadow path (chosen + !RedrawFrame, or i >= ShadowLightCount):
					// do nothing here. The non-redrawn light keeps its stale shadow map and
					// is re-inserted by the GameSetShadowCasterSlot loop below at endIdx.
					// Calling DisableLight here would invoke ReturnShadowmaps, releasing the
					// cached shadow data for one frame and producing visible flicker that
					// worsens as the budget gets more constrained.
					continue;
				}

				if (c.excess) {
					if (!isUsableLight(c.light))
						continue;

					// Atomic ordering: by the time we reach excess (rank
					// >= ShadowLightCount), all chosen lights have completed
					// their Begin/EnableLight/End sequence. ConvertLight's
					// ReturnShadowmaps side effect can only invalidate
					// pointers we are no longer walking. LightLimitFix::
					// UpdateLights then iterates activeShadowLights to pick
					// up converted lights for the cluster pipeline.
					//
					// Rank-drift suppression (a torch's importance score
					// bobbing across the chosen/excess boundary frame-to-
					// frame) lives in the score formula via the
					// lightframessincerender decay term, not here.
					ZoneNamedN(zCvt, "SCM::Engine::convertOrDisable(excess)", true);
					if (convertOrDisable(c.light, /*allowConvert=*/true))
						s_schedDiag.converted_excess++;
					else
						s_schedDiag.disabled_excess++;
					continue;
				}
			}
		}  // end SCM::AtomicMutationLoop

		// Non-redrawn chosen lights: insert at end of shadow caster array without rendering.
		// GetAccumLightSlot() already advanced past all EnableLight()-rendered slots.
		//
		// Re-rebuild the alive set: the atomic loop above may have invalidated
		// pointers (e.g. ConvertLight on excess removes from activeShadowLights).
		// Skip s_lights entries whose pointer is no longer in the scene to avoid
		// dereferencing freed BSShadowLight memory below.
		{
			ZoneNamedN(zonePostAtomic, "SCM::PostAtomicRevalidate", true);
			std::unordered_set<RE::BSShadowLight*> aliveAfterAtomic;
			{
				auto& alive = ssn->GetRuntimeData().activeShadowLights;
				aliveAfterAtomic.reserve(alive.size() + 1);
				if (sunLight)
					aliveAfterAtomic.insert(sunLight);
				for (auto& sp : alive)
					if (auto* l = sp.get())
						aliveAfterAtomic.insert(l);
			}

			int endIdx = (int)*GetAccumLightSlot();

			for (int i = 0; i < s_lights.Size; i++) {
				auto& e = s_lights.Lights[i];
				// Re-insert (without rendering) every chosen+!RedrawFrame light
				// AND every converted-slot light (i >= PointLightEnd). The
				// PointLightEnd bound is sun-aware so converted slots correctly
				// start one slot later when Sun=true.
				if (e.Light && (!e.RedrawFrame || i >= s_lights.PointLightEnd(s_settings.ShadowLightCount))) {
					// Membership check uses the snapshot built above (a
					// game-mutation in the atomic loop may have invalidated
					// pointers; aliveAfterAtomic captures the current scene
					// state in O(N) for O(1) membership queries here).
					if (aliveAfterAtomic.find(e.Light) == aliveAfterAtomic.end()) {
						s_schedDiag.reconciliation_clears++;
						e.Clear();
						continue;
					}
					// First-render gate: a chosen light whose slot has never
					// been rendered for IT (LastDrawnFrame < 0) has no valid
					// shadow content in its kSHADOWMAPS slice -- the depth
					// content is either cleared or carries the evicted
					// previous occupant's shadow. Inserting the light as a
					// shadow caster would make the cluster shader sample stale
					// depth and project a wrong shadow shape through the new
					// light. Skip insertion this frame; the light still
					// illuminates via the cluster pipeline as a non-shadow
					// light, with no false shadow. Once it wins a redraw turn
					// LastDrawnFrame goes >= 0 and it joins the shadow set
					// normally.
					//
					// Converted-slot range (i >= PointLightEnd) is unaffected:
					// converted lights don't sample kSHADOWMAPS via this slot
					// path; they participate via the s_normalConvert non-shadow
					// pipeline.
					if (i < s_lights.PointLightEnd(s_settings.ShadowLightCount) &&
						e.LastDrawnFrame < 0 &&
						!(s_lights.Sun && i == 0)) {
						s_schedDiag.first_render_skips++;
						continue;
					}

					// Cached-shadow reuse (the UE5 / CryEngine / Frostbite
					// pattern). We unconditionally sample the cached
					// kSHADOWMAPS slice even when the geometry hash mismatches
					// (light or caster moved since the cached render). For
					// small motion the staleness is sub-pixel and invisible;
					// for large motion the shadow visibly lags the light by
					// 1-2 frames, which is much less objectionable than the
					// full-frame on/off flicker that hash-gated suppression
					// produces on every animated torch. The hash-mismatch
					// priority hint above keeps stale entries at the front of
					// the redraw queue, so the lag self-corrects within budget
					// cycles.
					//
					// The first_render_skips gate above is the only safety
					// gate that DOES suppress insertion: a slot with no
					// rendered content for its current owner (LastDrawnFrame
					// < 0) has no valid cached shadow to fall back on; the
					// GPU slice is either cleared or contains an evicted
					// previous occupant. Hash mismatch on an existing slice
					// is at worst a small visual lag.
					// GameSetShadowCasterSlot calls Accumulate virtually; reuse
					// isUsableLight's vtable guard to catch tbbmalloc-zeroed
					// objects that are still in activeShadowLights but freed.
					if (!isVtableValid(e.Light)) {
						e.Light = nullptr;
						continue;
					}
					GameSetShadowCasterSlot(ssn, e.Light, endIdx, 1);
					// Same hazard as the post-EnableLight site: the engine can
					// free the light during this call. Use isUsableLight, not
					// just null check.
					if (!e.Light || !SafeUsable(isUsableLight, e.Light))
						continue;
					endIdx += e.Light->shadowMapCount;
					ShadowField(e.Light, maskIndex) = static_cast<uint32_t>(i);

					// GameSetShadowCasterSlot (via Accumulate) overwrites shadowmapIndex
					// with the sequential endIdx counter, diverging from the stable
					// container-slot index that CopyShadowLightData and Prepass expect.
					// All shadow-slot light types are affected:
					//   Spot (!IsParabolicLight): 1 descriptor, 1 atlas slice.
					//   Hemi (IsParabolicLight && !IsOmniLight): 1 descriptor, 1 atlas slice.
					//   Omni (IsParabolicLight && IsOmniLight): both paraboloids packed into
					//     a single atlas slice via UV splitting in GetOmnidirectionalShadow,
					//     so all descriptors should also point to i.
					// Restore shadowmapIndex = i for every non-redrawn shadow-slot light.
					// Only restore shadowmapIndex for point-light slots (skip converted).
					// PointLightEnd accounts for sun bookkeeping so the highest point-light
					// slot (Sun=true: pool[ShadowLightCount]) is included.
					if (s_settings.ShadowLightCount > 4 && i < s_lights.PointLightEnd(s_settings.ShadowLightCount)) {
						// Restore descriptor.shadowmapIndex for cached (non-redrawn)
						// chosen lights so RenderCascade samples their preserved
						// depth slice. Sun (pool[0] when Sun=true) is skipped —
						// it renders via the directional cascade path, not
						// kSHADOWMAPS, so its descriptor.shadowmapIndex is unused.
						if (s_lights.Sun && i == 0)
							continue;
						if (globals::game::isVR) {
							for (auto& desc : e.Light->GetVRRuntimeData().shadowmapDescriptors)
								desc.shadowmapIndex = static_cast<uint32_t>(i);
						} else {
							for (auto& desc : e.Light->GetRuntimeData().shadowmapDescriptors)
								desc.shadowmapIndex = static_cast<uint32_t>(i);
						}
					}
				}
			}
		}
		// Update rolling redraw and budget statistics.
		{
			int redrawing = 0;
			int32_t consumed = 0;
			for (int i = 0; i < s_lights.Size; i++) {
				auto& e = s_lights.Lights[i];
				if (e.Light && e.RedrawFrame) {
					if (i != 0 || !s_lights.Sun)
						consumed += s_budget.GetCost(e.Light);
					redrawing++;
				}
			}
			s_redrawSum -= s_redrawHistory[s_redrawHistoryPos];
			s_redrawHistory[s_redrawHistoryPos] = redrawing;
			s_redrawSum += redrawing;
			s_redrawHistoryPos = (s_redrawHistoryPos + 1) % kRedrawHistorySize;

			s_budgetSum -= s_budgetHistory[s_budgetHistoryPos];
			s_budgetHistory[s_budgetHistoryPos] = consumed;
			s_budgetSum += consumed;
			s_budgetHistoryPos = (s_budgetHistoryPos + 1) % kRedrawHistorySize;
		}

		ssn->GetRuntimeData().firstPersonShadowMask = *GetShadowMask();
		*GetFrameLightCount() = static_cast<uint32_t>(doneLightCount);

		// =====================================================================
		// Tracy per-frame plots: scheduler diagnostic counters + live config.
		// Emitting both in the same frame lets a capture be queried for A/B
		// behaviour without re-running the game: the cfg_* plots are the
		// independent variables, the scm.* plots are the dependent outcomes.
		// =====================================================================
		{
			// Sample slot occupancy at frame end (post-reconciliation).
			for (int i = 0; i < s_lights.Size; i++)
				if (s_lights.Lights[i].Light)
					s_schedDiag.slots_in_use++;

			// Publish the scheduling snapshot for headless inspection (devbench
			// inspect kind=llfshadows). wantDiag already gated the per-light reason
			// capture above; copy + swap under the lock so the listener thread reads a
			// consistent snapshot, then consume one dump-request pass.
			if (wantDiag) {
				SchedSnapshot snap;
				snap.valid = true;
				snap.frame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
				snap.total = s_schedDiag.candidates_total;
				snap.chosen = s_schedDiag.candidates_chosen;
				snap.excess = s_schedDiag.candidates_excess;
				snap.invalidCamera = s_schedDiag.candidates_invalid_camera;
				snap.invalidPortal = s_schedDiag.candidates_invalid_portal;
				snap.invalidFrustum = s_schedDiag.candidates_invalid_frustum;
				snap.invalidLod = s_schedDiag.candidates_invalid_lod;
				snap.invalidOther = s_schedDiag.candidates_invalid_other;
				snap.slotsInUse = s_schedDiag.slots_in_use;
				snap.demoted.reserve(s_convertReason.size());
				for (const auto& [ptr, reason] : s_convertReason)
					snap.demoted.emplace_back(ptr, static_cast<uint8_t>(reason));
				{
					std::scoped_lock lock(s_schedSnapshotMutex);
					s_schedSnapshot = std::move(snap);
				}
				if (s_schedDumpFrames.load(std::memory_order_relaxed) > 0)
					s_schedDumpFrames.fetch_sub(1, std::memory_order_relaxed);
			}

			TracyPlot("scm.candidates.total", (int64_t)s_schedDiag.candidates_total);
			TracyPlot("scm.candidates.chosen", (int64_t)s_schedDiag.candidates_chosen);
			TracyPlot("scm.candidates.excess", (int64_t)s_schedDiag.candidates_excess);
			TracyPlot("scm.candidates.invalid_camera", (int64_t)s_schedDiag.candidates_invalid_camera);
			TracyPlot("scm.candidates.invalid_portal", (int64_t)s_schedDiag.candidates_invalid_portal);
			TracyPlot("scm.candidates.invalid_frustum", (int64_t)s_schedDiag.candidates_invalid_frustum);
			TracyPlot("scm.candidates.invalid_lod", (int64_t)s_schedDiag.candidates_invalid_lod);
			TracyPlot("scm.candidates.invalid_other", (int64_t)s_schedDiag.candidates_invalid_other);
			TracyPlot("scm.converted.invalid", (int64_t)s_schedDiag.converted_invalid);
			TracyPlot("scm.converted.excess", (int64_t)s_schedDiag.converted_excess);
			TracyPlot("scm.disabled.invalid", (int64_t)s_schedDiag.disabled_invalid);
			TracyPlot("scm.disabled.excess", (int64_t)s_schedDiag.disabled_excess);
			TracyPlot("scm.reconciliation.clears", (int64_t)s_schedDiag.reconciliation_clears);
			TracyPlot("scm.slots.in_use", (int64_t)s_schedDiag.slots_in_use);
			TracyPlot("scm.first_render_skips", (int64_t)s_schedDiag.first_render_skips);

			// Live config plots — record the *current* settings on each frame so
			// a single capture spanning a settings change captures both sides.
			TracyPlot("cfg.ShadowLightCount", (int64_t)s_settings.ShadowLightCount);
			TracyPlot("cfg.MaxRedrawPerFrame", (int64_t)s_settings.MaxRedrawPerFrame);
			TracyPlot("cfg.ConvertExcessToNormal", (int64_t)(s_settings.ConvertExcessToNormal ? 1 : 0));
			TracyPlot("cfg.Enabled", (int64_t)(s_settings.Enabled ? 1 : 0));
			TracyPlot("cfg.RedrawBudgetMs", (double)s_settings.RedrawBudgetMs);
		}
	}

	// =========================================================================
	// Render hook: replaces RenderActiveShadowCasterLights
	// Iterates s_lights and calls Render() on lights flagged RedrawFrame.
	// Uses install_context_hook at a specific call site in the render loop (see Install()).
	// =========================================================================

	void RenderScheduledShadowLights()
	{
		// A cell-transition teardown (ClearLightArrays) frees the engine accumulator's renderPass
		// storage. A teardown landing between schedule and this pass would flush a freed accumulator
		// -> AV in BSBatchRenderer. Skip while a reset is pending; ScheduleShadowCasters owns the
		// drain next frame -- only LOAD here, never exchange, or s_lights would never reset.
		if (s_pendingSessionReset.load(std::memory_order_acquire))
			return;

		// Pause while the interior portal graph is mid-rebuild (cell transition);
		// BSShadowLight::Render walks portal culling that derefs ssn->portalGraph.
		if (IsPortalGraphTransitioning())
			return;

		// VR: RenderActiveShadowCasterLights normally saves+clears g_drawStereo before
		// iterating shadow casters, then restores it. Without this, each hemisphere
		// render is doubled for both eyes -> 4-quadrant shadow map texture.
		bool savedStereo = false;
		if (globals::game::isVR) {
			savedStereo = *globals::game::drawStereo;
			*globals::game::drawStereo = false;
		}

		ZoneScopedN("SCM::RenderScheduledShadowLights");
		CS_GPU_PASS("SCM::RenderScheduledShadowLights");

		s_budget.Begin(1);

		uint32_t tmp = 0;
		// Sun first: BSShadowDirectionalLight::Render emits the "Directional
		// Light Shadowmaps" marker and writes the cascade depth maps to
		// kSHADOWMAPS_ESRAM. The engine's vanilla RenderActiveShadowCasterLights
		// dispatches this via the same vtable walk it uses for point lights;
		// we replaced that walk with this loop, so we need to call sun.Render
		// explicitly. Without this, the directional cascade pass is skipped
		// and exterior scenes render with no sun shadow.
		if (s_lights.Sun && s_lights.Lights[0].Light &&
			!s_pendingSessionReset.load(std::memory_order_acquire)) {
			ZoneNamedN(zSun, "SCM::Render::Sun", true);
			CS_GPU_PASS("SCM::Render::Sun");
			s_budget.BeginLight(s_lights.Lights[0].Light, 1);
			s_lights.Lights[0].Light->Render(tmp);
			s_budget.EndLight(s_lights.Lights[0].Light, 1);
		}

		// Point lights from PointLightFirst onwards. PointLightFirst skips
		// slot 0 (handled above when Sun=true). PointLightEnd includes the
		// highest point-light slot when Sun=true.
		{
			ZoneNamedN(zPoint, "SCM::Render::PointLights", true);
			CS_GPU_PASS("SCM::Render::PointLights");
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++) {
				// A teardown can begin mid-pass; ClearLightArrays sets the flag at its
				// entry before freeing, so bailing here stops flushing into an accumulator
				// being torn down (narrows the window to a single in-flight Render call).
				if (s_pendingSessionReset.load(std::memory_order_acquire))
					break;
				auto& e = s_lights.Lights[i];
				if (!e.Light || !e.RedrawFrame)
					continue;
				s_budget.BeginLight(e.Light, 1);
				e.Light->Render(tmp);
				s_budget.EndLight(e.Light, 1);
			}
		}

		if (globals::game::isVR)
			*globals::game::drawStereo = savedStereo;
	}

	// Replaces the call to RenderActiveShadowCasterLights.
}
