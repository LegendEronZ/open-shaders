// ShadowScheduler.cpp
// The shadow caster scheduling core: geometry hashing, light transitions, ScheduleShadowCasters, and render dispatch.

#include <bit>
#include <cassert>

#include "../../Deferred.h"
#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../../Utils/Game.h"
#include "../../Utils/UI.h"
#include "../LightLimitFix.h"
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

	/// Hash of inputs that determine a shadow map's content: the light's
	/// pose + radius, and each caster's worldBound + identity. worldBound
	/// tracks rigid motion and BSDynamicTriShape vertex updates, so mesh
	/// data isn't inspected directly. Identical hashes across frames mean
	/// the cached slot is byte-for-byte current -- caller can skip the
	/// redraw. Returns 0 only on null light/NiLight (sentinel for "never
	/// rendered"); HashCombine constants make a real-data 0 essentially
	/// impossible.

	/// Folds a light's position, orientation, and radius -- shared by the
	/// redraw hash and the static-cache hash so both agree on what "moved"
	/// means. posStep is caller-scaled to the tile's world-units-per-texel,
	/// floored at 1.0 so sub-texel motion never busts the cache. Radius uses
	/// the EMA-anchored value (HASH input only, never the raster/ShadowParam,
	/// which stay live): NiPointLight's radius.x is flicker-jittered every
	/// frame by more than this fold's 1-unit step, so a raw radius would flip
	/// the hash constantly and permanently defeat the static-bake cache.
	static std::unordered_map<const RE::NiLight*, float> s_hashRadiusAnchor;

	static float AnchoredRadiusForHash(const RE::NiLight* ni, float liveRadius)
	{
		PruneIfOversized(s_hashRadiusAnchor, 1024);
		auto [it, isNew] = s_hashRadiusAnchor.try_emplace(ni, liveRadius);
		if (!isNew)
			it->second += 0.15f * (liveRadius - it->second);
		return it->second;
	}

	static std::uint64_t FoldLightPose(std::uint64_t h, RE::NiLight* ni, float posStep)
	{
		const float kPosStep = std::max(posStep, 1.0f);
		constexpr float kRotStep = 0.01f;
		constexpr float kRadiusStep = 1.0f;

		const auto& t = ni->world.translate;
		h = HashCombineFloat(h, QuantizeFloat(t.x, kPosStep));
		h = HashCombineFloat(h, QuantizeFloat(t.y, kPosStep));
		h = HashCombineFloat(h, QuantizeFloat(t.z, kPosStep));
		// Spot direction lives in the rotation matrix, so this covers a spot
		// re-aiming without translating.
		const auto& r = ni->world.rotate;
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				h = HashCombineFloat(h, QuantizeFloat(r.entry[i][j], kRotStep));
		// NiPointLight uses .x. Hashed on the EMA anchor, not the live flicker-
		// jittered value -- see AnchoredRadiusForHash above.
		h = HashCombineFloat(h,
			QuantizeFloat(AnchoredRadiusForHash(ni, ni->GetLightRuntimeData().radius.x), kRadiusStep));
		return h;
	}

	static std::uint64_t ComputeShadowGeomHash(RE::BSShadowLight* light, float posStep)
	{
		if (!light)
			return 0;
		auto* ni = light->light.get();
		if (!ni)
			return 0;
		std::uint64_t h = 0x9e3779b97f4a7c15ull;  // arbitrary nonzero seed
		// Coarse radius fold: a permanent radius change (scripted) must retire
		// the baked depth + snapshot; 64-unit steps intend to ignore flame
		// flicker but this is a truncation (integer division), not a
		// quantization with hysteresis -- a live radius sitting near a 64-unit
		// boundary flips this bucket on flicker alone (e.g. 619-649 truncates
		// to 9 or 10 depending on the exact frame). Anchored for the same
		// reason as FoldLightPose's own radius fold below.
		h = h * 31 + static_cast<std::uint64_t>(
						 AnchoredRadiusForHash(ni, ni->GetLightRuntimeData().radius.x) / 64.0f);

		h = FoldLightPose(h, ni, posStep);

		// Caster set + each caster's worldBound (engine-updated). Same steps
		// the pose fold uses, so caster motion and light motion agree on what
		// counts as "moved".
		const float kPosStep = std::max(posStep, 1.0f);
		constexpr float kRadiusStep = 1.0f;
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

		// Player-only dynamic-caster proxy: NPCs that cast in this light are
		// already in geomList above (folded via worldBound), so they refresh it
		// as they move. The player's own geometry is reliably NOT in geomList at
		// scheduling time, so a stationary light the player walks through would
		// hash constant and get the "unchanged -> skip redraw" penalty, freezing
		// the player's shadow. Fold only the player when enclosed -- folding all
		// actors per light instead saturates the redraw budget and starves
		// distant lights into empty tiles.
		if (auto* plr = RE::PlayerCharacter::GetSingleton()) {
			const auto pp = plr->GetPosition();
			const auto& lp = ni->world.translate;
			const float dx = pp.x - lp.x, dy = pp.y - lp.y, dz = pp.z - lp.z;
			const float r = ni->GetLightRuntimeData().radius.x;
			if (dx * dx + dy * dy + dz * dz < r * r) {
				const float actorStep = std::min(kPosStep, 8.0f);
				h = HashCombineFloat(h, QuantizeFloat(pp.x, actorStep));
				h = HashCombineFloat(h, QuantizeFloat(pp.y, actorStep));
				h = HashCombineFloat(h, QuantizeFloat(pp.z, actorStep));
			}
		}
		return h;
	}

	/// Frame number of a light's most recent UpdateCamera pass (exit hysteresis
	/// at the validation gate, keyed by recency rather than a resettable
	/// consecutive-failure counter). A candidate is held valid if it passed
	/// within the last kCameraExitStreak frames -- regardless of whether it
	/// currently holds an atlas slot. A prior consecutive-streak version only
	/// consulted this state on the already-slotted branch (GetShadowSlot >= 0),
	/// so a never-yet-slotted candidate -- every light in a scene right after a
	/// load or zone transition -- got zero grace and flapped in/out of
	/// candidacy at raw flicker frequency until it happened to land one lucky
	/// frame. Render thread only; keys are never dereferenced, so a dead
	/// light's stale entry is harmless until the size prune.
	std::unordered_map<RE::BSShadowLight*, int32_t> s_lastValidFrame;

	/// UpdateCamera's own inputs (light position, radius) are jittered every
	/// frame by the engine's flame-flicker effect (TESObjectLIGH::flicker
	/// MovementAmplitude/flickerIntensityAmplitude), so a light parked near
	/// the sphere-vs-frustum or shadow-distance boundary flaps at flicker
	/// frequency, not camera-movement frequency. 15 was an arbitrary starting
	/// point (hand-copied from the unrelated ShadowImpactFloor streak). Live
	/// devbench capture showed a real failure run of ~33 frames on one light
	/// -- an earlier bump to 30 still wasn't enough. Matches the existing
	/// 60-frame promoteStreak precedent in this file, not a fresh measurement
	/// of this specific gate's flicker period. A held light still counts
	/// toward the chosen-candidate budget (see cameraHold below), so this
	/// widens that exposure too -- if flicker persists, size this from a
	/// real run-length histogram instead of stepping the guess up again.
	constexpr uint32_t kCameraExitStreak = 60;

	/// Consecutive-desire frames (see the rank-budget promotion hold below)
	/// before a light is allowed to grow into a larger tile class. Same
	/// hand-picked magnitude as kCameraExitStreak, not a fresh measurement --
	/// unlike the old version of this gate, it is now leaky and geometry-only
	/// (see the promoteStreak comment at its use site), so 60 is no longer
	/// reset by unrelated pool churn; revisit from real data if it still
	/// proves too slow in practice.
	constexpr uint16_t kPromoteStreakFrames = 60;

	/// Lights held this frame (UpdateCamera failed, exit streak immature, but
	/// the light already held a slot). Rebuilt each candidate-validation pass;
	/// consumed by the redraw-admission loop to exclude held lights from
	/// `pending` before budget accounting -- a held light's camera state is
	/// rejected, so it must keep its cached tile, not spend redraw budget on
	/// a stale/rejected camera. Render thread only, same dereference notes as
	/// s_lastValidFrame above.
	std::unordered_set<RE::BSShadowLight*> s_cameraHold;

	/// Consecutive frames a light scored below ShadowImpactFloor (exit hysteresis
	/// mirroring s_lastValidFrame above). Without this, a light hovering near the
	/// floor drops its atlas slot and re-bakes every time it dips back above --
	/// EnableLight's own pose-rebake counter can then latch splitExcluded after a
	/// handful of these flaps within its window, permanently downgrading the
	/// light to full renders. Render thread only; same dereference/prune notes.
	std::unordered_map<RE::BSShadowLight*, uint32_t> s_belowFloorStreak;

	// CPU-only meters (steady_clock). The budget tracker's per-light cost is a
	// GPU timestamp interval; these answer the walk-vs-submission CPU question
	// it cannot. Accum = the engine Accumulate (cull walk + appends);
	// Submit = Render() (pass setup + draw submission).
	std::atomic<uint64_t> s_cpuAccumUs{ 0 };
	std::atomic<uint64_t> s_cpuSubmitUs{ 0 };
	std::atomic<uint32_t> s_cpuAccumN{ 0 };
	std::atomic<uint32_t> s_cpuSubmitN{ 0 };
	// EnableLight's own cost (setup + accumulate), isolated from Render's
	// GPU-submit cost above -- diagnostic for array-vs-atlas CPU comparisons.
	std::atomic<uint64_t> s_cpuEnableUs{ 0 };
	std::atomic<uint32_t> s_cpuEnableN{ 0 };

	template <class Fn>
	static uint64_t TimeUs(Fn&& fn)
	{
		const auto t0 = std::chrono::steady_clock::now();
		fn();
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
	}

	/// Frame + slot of each light's most recent Accumulate (render thread only).
	/// Prevents duplicate Accumulate registrations per light per frame.
	std::unordered_map<RE::BSShadowLight*, std::pair<uint32_t, uint32_t>> s_lightAccumFrame;

	/// Camera world position captured when the current light's accumulate begins.
	/// The contribution cull measures each caster's size from HERE (the viewer),
	/// not from the light: a caster's on-screen shadow footprint -- how much of
	/// the view it occupies -- is what the player perceives. Set on the same
	/// render thread just before s_currentCullLight, so the hook reads it safely.
	RE::NiPoint3 s_cullCameraPos{};

	/// StaticOnly re-bakes issued this frame. A bake re-rasterizes a light's
	/// whole static caster set into its cache tile, so this is the cost the
	/// cache trades against: sustained non-zero means the static set keeps
	/// churning and the cache is paying for itself repeatedly.
	std::atomic<uint32_t> s_staticBakeCount{ 0 };
	/// Cumulative bakes since load, published in the snapshot so a headless A/B
	/// can difference it across a run without attaching a profiler.
	std::atomic<uint64_t> s_staticBakeTotal{ 0 };

	/// Cumulative s_pendingCellReset drains since load -- diagnostic for
	/// whether Hook_ResetScene fires only on real zone transitions or also
	/// on ordinary exterior cell-grid streaming during normal movement.
	std::atomic<uint64_t> s_cellResetTotal{ 0 };

	// -------------------------------------------------------------------------
	// Static-cache split: single accumulate per light per frame
	//
	// Accumulate APPENDS casters (the engine resets it once per frame), so
	// each light gets exactly ONE filtered accumulate in EnableLight:
	// normally DynamicOnly, occasionally StaticOnly (staggered) to rebake the
	// static cache when its caster set changed. The caster-pass filter lives
	// in ShadowCasterClassifier.cpp; s_visitStaticHash detects the set change.
	// -------------------------------------------------------------------------
	// Per-light handoff between the phase-A accumulate (which picks the filter
	// mode) and the phase-B render. What is actually baked is owned by the atlas
	// slot (GetSlotStaticState), so a tile realloc invalidates the cache without
	// this state going stale.
	struct SplitState
	{
		uint64_t pendingHash = 0;      ///< static hash observed on the latest accumulate
		bool bakeQueued = true;        ///< a rebake is due -- next accumulate is StaticOnly
		bool bakeThisFrame = false;    ///< this frame's accumulate was StaticOnly (render to cache)
		uint8_t mismatchStreak = 0;    ///< consecutive accumulates whose hash differed from the bake
		RE::NiPoint3 bakePos{};        ///< light position the static tile was baked at
		RE::NiMatrix3 bakeRot{};       ///< light rotation the static tile was baked at (frustum-light drift check)
		uint8_t poseRebakes = 0;       ///< pose-drift rebakes inside the current window
		uint32_t poseWindowStart = 0;  ///< frame the pose-rebake window opened
		bool splitExcluded = false;    ///< jitter outruns the bake's validity: render full, no split
		bool fullThisFrame = false;    ///< this frame's accumulate was All (cap/exclusion fallback)
		/// Last completed accumulate appended >= 1 dynamic caster. Defaults
		/// true (never sleep a light until an accumulate proves it moverless).
		bool sawDynamicLastAccum = true;
		/// The latest StaticOnly bake appended >= 1 static caster. False for a
		/// bake taken before any caster settled: its cache tile is blank.
		bool bakeSawStatic = false;
	};
	std::unordered_map<RE::BSShadowLight*, SplitState> s_splitState;

	// --- Empty-dynamic sleep: schedule-time skip of moverless redraws --------

	// A composited bake stays valid only while the light sits within this
	// drift of the pose it was baked at (world units). Carried torches move
	// past this every frame, which is what keeps their shadows live.
	constexpr float kSplitPoseDriftMax = 4.0f;

	// Staleness backstop for sleeping lights: a real redraw at least this
	// often bounds every change the sleep predicate cannot observe (player,
	// off-screen movers, static-set edits) to under a second at 60 fps.
	constexpr int32_t kSleepRedrawIntervalFrames = 45;
	// Slot-phase stagger so lights that fell asleep together (scene load)
	// don't all take their backstop redraw on the same frame.
	constexpr int32_t kSleepStaggerStride = 7;

	// Stop-motion reporting floor (LightEntry::dirtyStallFrames): ~133ms at
	// 60fps, roughly the point a held shadow reads as visible judder. Well
	// inside kSleepRedrawIntervalFrames so a real stall is flagged before the
	// sleep backstop's own periodic redraw would mask it.
	constexpr int32_t kStallReportThreshold = 8;

	// Staleness backstop for zero-demand skips. Far longer than the sleep
	// backstop because that predicate proves the tile is unchanged while this one
	// only claims nothing samples it -- and the residual false negative (a caster
	// thinner than the tap pitch, or one visible only through a water reflection)
	// is a permanent zero no streak length detects, so this is its only bound.
	constexpr int32_t kZeroDemandRedrawIntervalFrames = 240;

	// Cumulative schedule-time sleep skips since load (snapshot metric).
	std::atomic<uint64_t> s_sleepSkipTotal{ 0 };
	// Cumulative schedule-time zero-demand skips since load (snapshot metric).
	std::atomic<uint64_t> s_demandSkipTotal{ 0 };

	// =========================================================================
	// Stage-A zero-demand-skip audit (measurement only; nothing here changes
	// scheduling). Two independent questions share this block:
	//   Q1 an oracle cross-checking the engine's own frustum cull, and
	//   Q2 a counterfactual re-run of budget admission with the lights a hard
	//      skip would remove taken out, which is the only way to say what the
	//      skip would actually buy (the sort key is age-dominated, so freeing a
	//      slot admits the next entry in rank order, not "some denied visible light").
	// =========================================================================

	// Outside by a tenth of the radius rather than by a texel: the engine's own
	// UpdateCamera runs at a different point in the frame than this read, so a
	// boundary-width disagreement carries no information.
	constexpr float kFrustumAuditMargin = 1.10f;
	// Consecutive frames a light must stay in the suspect quadrant. A moving
	// camera makes any single-frame disagreement meaningless.
	constexpr uint32_t kFrustumAuditStreak = 30;

	/// Consecutive frames a candidate sat in the "engine kept it, sphere is out"
	/// quadrant. Pointer-keyed like s_lastValidFrame, and with the same contract:
	/// keys are never dereferenced and address recycling can only misreport a
	/// diagnostic, never a scheduling decision.
	std::unordered_map<RE::BSShadowLight*, uint32_t> s_frustumSuspectStreak;

	// Debugging aid, not gated on a setting: lights already logged once as a
	// "high-priority light is demand-skipped" contradiction this episode, so
	// the warning doesn't repeat every frame for the same still-skipped
	// light. Cleared per-light the moment it stops being skip-eligible
	// (rejoins pending), same lifecycle as s_frustumSuspectStreak.
	std::unordered_set<RE::BSShadowLight*> s_highPrioritySkipLogged;

	std::atomic<uint64_t> s_demandSkipEligibleTotal{ 0 };
	std::atomic<uint64_t> s_demandSwapInTotal{ 0 };
	std::atomic<uint64_t> s_demandRedrawsSavedTotal{ 0 };

	int32_t s_demandAuditLastLogFrame = 0;
	constexpr int32_t kDemandAuditLogInterval = 300;

	/// Whether a streak consumer was live last frame. Streaks freeze while none
	/// is, and a frozen count says nothing about the sampling since.
	bool s_demandStreaksActive = false;

	/// Audit totals accumulated across the log interval. s_schedDiag resets every
	/// frame, so logging it directly would report one arbitrary sample rather
	/// than a rate.
	struct DemandAuditWindow
	{
		uint64_t frames = 0;
		uint64_t saturatedFrames = 0;
		uint64_t budgetSaturatedFrames = 0;
		uint64_t candidates = 0;
		uint64_t keptOut = 0;
		uint64_t suspects = 0;
		uint64_t slotted = 0;
		uint64_t zero = 0;
		uint64_t subTap = 0;
		uint64_t skipEligible = 0;
		uint64_t swapIn = 0;
		uint64_t swapInAboveEps = 0;
		uint64_t skips = 0;
		int64_t redrawsSaved = 0;
		// Streak-length histograms feeding the tap-count/window data gate:
		// bucket i covers samples [2^(i-1), 2^i - 1] for i>=1, bucket 0 is
		// exactly 0. resetHistogram buckets a streak's length the moment an
		// above-floor sample ends it (completed occlusion events only --
		// right-censored, a light occluded past the whole log interval never
		// contributes here). liveSnapshotHistogram buckets every live entry's
		// CURRENT streak once per log interval, recovering the in-progress
		// population the reset histogram misses. Neither alone is the real
		// distribution; the pair is.
		std::array<uint64_t, 9> resetHistogram{};
		std::array<uint64_t, 9> liveSnapshotHistogram{};

		// Stop-motion window (LightEntry::dirtyStallFrames / SchedDiagCounters).
		// Separate from the demand-streak histograms above by design: a stall
		// is a scheduler-starvation signal, a demand streak is an occlusion-
		// duration signal, and conflating them (an earlier draft of this design
		// did) drowns the scheduler signal under the much larger, expected
		// occlusion population.
		uint32_t stallMaxOfMax = 0;
		int32_t stallWorstSlot = -1;
		uint64_t stallMaxSum = 0;
		uint64_t stallOver = 0;
		double demandRatioSum = 0.0;
		std::array<uint64_t, 6> stallHistogram{};  // 0, 1-2, 3-7, 8-15, 16-44, 45+
	};
	DemandAuditWindow s_demandAuditWindow;

	static uint32_t EffectiveZeroDemandStreak()
	{
		return kZeroDemandSkipStreak;
	}

	// Mirrors EffectiveZeroDemandStreak() for the producer's tap count, purely
	// for audit-log visibility here -- the actual dispatch reads
	// LightLimitFix::settings directly (LightLimitFix.cpp).
	static uint32_t EffectiveDemandTapCount()
	{
		return LightLimitFix::kDemandTapCount;
	}

	/// True when the published sample supports a per-slot absence verdict. Fails
	/// open on every axis: unmeasured, stale or cluster-saturated frames all
	/// read as "fully visible".
	static bool DemandSampleUsable()
	{
		if (!s_shadowDemand.initialized)
			return false;
		if (s_shadowDemand.frameCounter - s_shadowDemand.lastDrainFrame >= kDemandStaleFrames)
			return false;
		return !s_shadowDemand.clusterSaturated;
	}

	/// Demand array index for a pool entry, or -1 when it has no reading: the
	/// sun's bookkeeping slot and anything past the fixed demand array (which
	/// lands in the producer's overflow counter and always reads 0).
	static int32_t DemandSlotFor(const LightEntry& e)
	{
		if (e.Index < 0 || (s_lights.Sun && e.Index == 0))
			return -1;
		if (static_cast<uint32_t>(e.Index) >= kMaxShadowDemandSlots)
			return -1;
		return e.Index;
	}

	/// Advances each slot's consecutive-samples-below-epsilon streak. Only a
	/// distinct sample counts: a frame-counted streak lets a stalled readback
	/// re-count one reading and complete a streak without ever exercising a
	/// second jitter offset.
	static void AdvanceDemandStreaks()
	{
		// A streak measured under a different tap pattern is not evidence about
		// this one, so restart every count when the consumers come back on.
		if (!s_demandStreaksActive) {
			s_demandStreaksActive = true;
			for (int i = 0; i < s_lights.Size; i++)
				s_lights.Lights[i].untouchedSamples = 0;
		}
		const bool usable = DemandSampleUsable();
		for (int i = 0; i < s_lights.Size; i++) {
			auto& e = s_lights.Lights[i];
			// An empty slot's reading belongs to nobody. Zeroing here rather than
			// skipping is what stops a light reclaiming its own free slot -- the
			// one acquire path that deliberately preserves the entry -- from
			// resuming a streak measured before it left.
			if (!e.Light) {
				e.untouchedSamples = 0;
				continue;
			}
			if (!usable || e.lastDemandSerial == s_shadowDemand.sampleSerial)
				continue;
			const int32_t slot = DemandSlotFor(e);
			if (slot < 0)
				continue;
			// The demand array is indexed by pool slot, so any divergence between
			// the two attributes one light's visibility to another. Debug-only:
			// GetShadowSlot is a linear scan.
			assert(e.Index == GetShadowSlot(e.Light));
			e.lastDemandSerial = s_shadowDemand.sampleSerial;
			// Hard reset, not a decay: one above-floor tap is strong evidence of
			// presence (the sparse sampler rarely hits a small lit footprint), so
			// it must erase the whole absence streak. The tolerance for sampling
			// gaps lives in the streak length, never in softening this reset.
			if (s_shadowDemand.maxLatest[slot] <= kDemandUntouchedMaxRaw)
				e.untouchedSamples++;
			else {
				// Bucket the completed streak's length before erasing it -- this
				// is the only point a finished occlusion event's duration is ever
				// observable. Right-censored by construction (an occlusion that
				// outlives the whole log interval never resets, so never lands
				// here); EmitDemandAuditLog's periodic snapshot recovers that
				// population separately.
				s_demandAuditWindow.resetHistogram[DemandStreakBucket(e.untouchedSamples)]++;
				e.untouchedSamples = 0;
			}
		}
	}

	/// Accumulates this frame's audit counters and, on the log cadence, emits the
	/// two findings as separate lines. They are never merged: the first is a
	/// correctness finding expected to read zero (and a non-zero value is first
	/// evidence the oracle is mis-modelled, not that the engine is wrong), while
	/// the second is a capacity finding expected to be large and is the normal
	/// frustum-versus-visibility gap.
	static void EmitDemandAuditLog(int32_t now)
	{
		auto& w = s_demandAuditWindow;
		w.frames++;
		w.saturatedFrames += s_shadowDemand.clusterSaturated ? 1 : 0;
		w.budgetSaturatedFrames += s_schedDiag.demand_budget_saturated ? 1 : 0;
		w.candidates += static_cast<uint64_t>(s_schedDiag.frustum_audit_candidates);
		w.keptOut += static_cast<uint64_t>(s_schedDiag.frustum_audit_kept_out);
		w.suspects += static_cast<uint64_t>(s_schedDiag.frustum_audit_suspects);
		w.slotted += static_cast<uint64_t>(s_schedDiag.demand_slotted);
		w.zero += static_cast<uint64_t>(s_schedDiag.demand_zero);
		w.subTap += static_cast<uint64_t>(s_schedDiag.demand_sub_tap);
		w.skipEligible += static_cast<uint64_t>(s_schedDiag.demand_skip_eligible);
		w.swapIn += static_cast<uint64_t>(s_schedDiag.demand_swap_in);
		w.swapInAboveEps += static_cast<uint64_t>(s_schedDiag.demand_swap_in_above_eps);
		w.skips += static_cast<uint64_t>(s_schedDiag.demand_skips);
		w.redrawsSaved += s_schedDiag.demand_redraws_saved;

		if (static_cast<uint32_t>(s_schedDiag.stall_max) > w.stallMaxOfMax) {
			w.stallMaxOfMax = static_cast<uint32_t>(s_schedDiag.stall_max);
			w.stallWorstSlot = s_schedDiag.stall_worst_slot;
		}
		w.stallMaxSum += static_cast<uint64_t>(s_schedDiag.stall_max);
		w.stallOver += static_cast<uint64_t>(s_schedDiag.stall_over_threshold);
		w.demandRatioSum += s_schedDiag.demand_ratio;

		if (now - s_demandAuditLastLogFrame < kDemandAuditLogInterval)
			return;
		s_demandAuditLastLogFrame = now;
		const double frames = static_cast<double>(std::max<uint64_t>(w.frames, 1));

		// Snapshot every live entry's CURRENT (possibly still in-progress, i.e.
		// right-censored) streak once per interval -- the population the reset
		// histogram above structurally cannot see, since an occlusion that
		// outlives the whole interval never triggers a reset.
		for (int i = 0; i < s_lights.Size; i++) {
			const auto& e = s_lights.Lights[i];
			if (e.Light) {
				w.liveSnapshotHistogram[DemandStreakBucket(e.untouchedSamples)]++;
				w.stallHistogram[StallBucket(e.dirtyStallFrames)]++;
			}
		}

		logger::info(
			"[SCM] frustum audit: cand={:.1f} kept_sphere_out={:.1f} suspect={:.1f} per frame "
			"(margin {:.2f}, >={}f, non-VR, no sun)",
			w.candidates / frames, w.keptOut / frames, w.suspects / frames,
			kFrustumAuditMargin, kFrustumAuditStreak);
		logger::info(
			"[SCM] visibility headroom: slotted={:.1f} zero={:.1f} (subTap={:.1f} occludedOrHidden={:.1f}) "
			"skipEligible={:.1f} skips={:.1f} swapIn={:.1f} swapInAboveEps={:.1f} redrawsSaved={} "
			"budgetSaturated={}/{} saturatedFrames={} eps={} streak={} taps={} phase1={} skipActive={}",
			w.slotted / frames, w.zero / frames, w.subTap / frames,
			static_cast<double>(w.zero - w.subTap) / frames,
			w.skipEligible / frames, w.skips / frames, w.swapIn / frames, w.swapInAboveEps / frames,
			w.redrawsSaved, w.budgetSaturatedFrames, w.frames, w.saturatedFrames,
			kDemandUntouchedMaxRaw, EffectiveZeroDemandStreak(), EffectiveDemandTapCount(),
			true, s_settings.SkipZeroDemandRedraw);
		logger::info(
			"[SCM] demand streak histogram [0,1-2,3-7,8-15,16-31,32-63,64-127,128-255,256+]: "
			"reset=[{},{},{},{},{},{},{},{},{}] live=[{},{},{},{},{},{},{},{},{}]",
			w.resetHistogram[0], w.resetHistogram[1], w.resetHistogram[2], w.resetHistogram[3],
			w.resetHistogram[4], w.resetHistogram[5], w.resetHistogram[6], w.resetHistogram[7], w.resetHistogram[8],
			w.liveSnapshotHistogram[0], w.liveSnapshotHistogram[1], w.liveSnapshotHistogram[2],
			w.liveSnapshotHistogram[3], w.liveSnapshotHistogram[4], w.liveSnapshotHistogram[5],
			w.liveSnapshotHistogram[6], w.liveSnapshotHistogram[7], w.liveSnapshotHistogram[8]);
		logger::info(
			"[SCM] redraw stall: maxOfMax={} (slot={}) meanMax={:.1f} over{}={:.1f}/frame "
			"hist[0,1-2,3-7,8-15,16-44,45+]=[{},{},{},{},{},{}] demandRatio={:.2f} dueGate={}",
			w.stallMaxOfMax, w.stallWorstSlot, static_cast<double>(w.stallMaxSum) / frames,
			kStallReportThreshold, static_cast<double>(w.stallOver) / frames,
			w.stallHistogram[0], w.stallHistogram[1], w.stallHistogram[2],
			w.stallHistogram[3], w.stallHistogram[4], w.stallHistogram[5],
			w.demandRatioSum / frames, s_shadowDemand.redrawDueGate);
		w = DemandAuditWindow{};
	}

	/// What Stage B would skip. The ceiling on any win this feature can produce,
	/// and the input the Q2 counterfactual removes.
	static bool DemandSkipCandidate(const LightEntry& e)
	{
		if (!DemandSampleUsable())
			return false;
		const int32_t slot = DemandSlotFor(e);
		if (slot < 0 || e.LastDrawnFrame < 0)
			return false;
		return s_shadowDemand.maxLatest[slot] <= kDemandUntouchedMaxRaw &&
		       e.untouchedSamples >= EffectiveZeroDemandStreak();
	}

	/// True when this light's single accumulate can run DynamicOnly: split
	/// cache on and usable for it, slot bake valid, pose within bake drift.
	/// Phase A (EnableLight) picks its filter mode through this and the
	/// schedule-time sleep skip reuses it, so the two can never drift apart.
	static bool SplitDynamicOnlyEligible(RE::BSShadowLight* light, const SplitState& st, bool staticValid,
		float pendingScale)
	{
		if (!StaticAtlasReady())
			return false;
		if (st.splitExcluded || st.bakeQueued || !staticValid)
			return false;
		if (auto* ni = light->light.get()) {
			// Pose freshness: compositing movers over a bake taken at a
			// drifted pose shows two misaligned shadows at once (reads as
			// extra darkness).
			const float px = ni->world.translate.x - st.bakePos.x;
			const float py = ni->world.translate.y - st.bakePos.y;
			const float pz = ni->world.translate.z - st.bakePos.z;
			if (px * px + py * py + pz * pz > kSplitPoseDriftMax * kSplitPoseDriftMax)
				return false;
			// Rotation freshness (frustum lights only): a re-aimed spot's
			// static bake shows the OLD beam direction under the composited
			// movers until the hash-mismatch rebake catches up, several
			// accumulates later. Reject once the forward axis has drifted
			// more than the bake's own texel resolution can represent --
			// finer than that is invisible. semiWidth is tan(halfFOV); a
			// texel subtends dtheta ~= 2*semiWidth/texels radians, so the
			// dot-product (cosine) threshold is the small-angle cosine
			// deficit 1-cos(dtheta) ~= dtheta^2/2, not semiWidth/texels
			// itself. semiWidth<=0 means an unreadable/degenerate field
			// (see ShadowFormula.cpp's identical guard) -- skip rather than
			// fail closed into a permanent full-render fallback.
			if (const auto* frustumLight = skyrim_cast<const RE::BSShadowFrustumLight*>(light)) {
				const auto& frustumRtd = frustumLight->GetShadowFrustumLightRuntimeData();
				if (frustumRtd.semiWidth > 0.0f) {
					const float baseTileTexels = s_initialShadowMapResolution > 0 ?
					                                 static_cast<float>(s_initialShadowMapResolution) :
					                                 2048.0f;
					const float texels = baseTileTexels * std::max(pendingScale, kTileScaleFloor);
					const float halfAngle = frustumRtd.semiWidth / texels;
					const float maxCosDrift = std::clamp(1.0f - 2.0f * halfAngle * halfAngle, -1.0f, 1.0f);
					const RE::NiPoint3 fwd = ni->world.rotate.GetVectorY();
					const RE::NiPoint3 bakeFwd = st.bakeRot.GetVectorY();
					const float dot = fwd.x * bakeFwd.x + fwd.y * bakeFwd.y + fwd.z * bakeFwd.z;
					if (dot < maxCosDrift)
						return false;
				}
			}
		}
		return true;
	}

	/// Schedule-time sleep predicate: every condition proving this light's
	/// redraw would reproduce the tile it already holds, so the scheduler
	/// skips it outright (no accumulate, no budget, no render). Slot state is
	/// re-read every frame so an atlas reclaim or realloc wakes the light.
	static bool SleepSkipEligible(const LightEntry& e, int32_t slot, int32_t now)
	{
		if (e.LastDrawnFrame < 0)
			return false;
		// A staged class change must rerender before the light may sleep.
		if (e.pendingScale != e.renderedScale)
			return false;
		const auto it = s_splitState.find(e.Light);
		if (it == s_splitState.end())
			return false;
		const SplitState& st = it->second;
		// A pending static-set divergence or an observed mover means the
		// next accumulate would change the tile.
		if (st.mismatchStreak != 0 || st.sawDynamicLastAccum)
			return false;
		uint64_t bakedHash = 0;
		bool staticValid = false;
		if (!GetSlotStaticState(slot, bakedHash, staticValid))
			return false;
		AtlasTileTexels tile{};
		if (!GetSlotTileTexels(slot, tile) || !tile.contentValid)
			return false;
		if (!SplitDynamicOnlyEligible(e.Light, st, staticValid, e.pendingScale))
			return false;
		// Staleness backstop: never skip once the backstop redraw is due,
		// and keep pressing for it every frame until the budget grants it.
		if (now - e.LastDrawnFrame >= kSleepRedrawIntervalFrames)
			return false;
		if (((now + slot * kSleepStaggerStride) % kSleepRedrawIntervalFrames) == 0)
			return false;
		return true;
	}

	/// Schedule-time zero-demand predicate: a sibling of SleepSkipEligible on the
	/// same gate, never folded into it. Sleep asserts the redraw would reproduce
	/// the identical tile; this asserts the redraw may well change the tile but
	/// nothing on screen samples it. So it reuses only sleep's tile-*existence*
	/// conditions and deliberately not its static-bake, split-cache or mover
	/// conditions, which exist for an unrelated reason -- folding the two would
	/// let a zero-demand light bypass them. Every condition fails open.
	static bool DemandSkipEligible(const LightEntry& e, int32_t slot, int32_t now)
	{
		if (!s_settings.SkipZeroDemandRedraw)
			return false;
		// Unmeasured, stale or cluster-saturated all read as fully visible.
		if (!DemandSampleUsable())
			return false;
		const int32_t demandSlot = DemandSlotFor(e);
		if (demandSlot < 0)
			return false;
		// The atlas tile is keyed by the pool slot and the demand array by
		// e.Index; any divergence between the two and this suppresses one
		// light's redraw on another light's visibility. Debug-only:
		// GetShadowSlot is a linear scan.
		assert(slot == e.Index && e.Index == GetShadowSlot(e.Light));
		if (e.LastDrawnFrame < 0)
			return false;
		// A staged class change must rerender before the light may be skipped.
		if (e.pendingScale != e.renderedScale)
			return false;
		AtlasTileTexels tile{};
		if (!GetSlotTileTexels(slot, tile) || !tile.contentValid)
			return false;
		// "Never measured" is structurally distinct from "measured absent": new
		// lights and newly installed slots start the streak at zero.
		if (e.untouchedSamples < EffectiveZeroDemandStreak())
			return false;
		if (s_shadowDemand.maxLatest[demandSlot] > kDemandUntouchedMaxRaw)
			return false;
		// Staleness backstop: never skip once the backstop redraw is due, and
		// keep pressing for it every frame until the budget grants it.
		if (now - e.LastDrawnFrame >= kZeroDemandRedrawIntervalFrames)
			return false;
		if (((now + slot * kSleepStaggerStride) % kZeroDemandRedrawIntervalFrames) == 0)
			return false;
		return true;
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
				if (const uint32_t slot = *GetAccumLightSlot(); slot < kShadowMaskBits)
					*GetShadowMask() |= 1u << slot;
				left = right = top = bottom = -1.0f;
			}

			ShadowField(light, projectedBoundingBox) =
				RE::NiRect<uint32_t>((uint32_t)left, (uint32_t)right, (uint32_t)top, (uint32_t)bottom);
		}

		// Accumulate into shadow slot. Publish the light so the parabolic
		// AppendVirtual hook can contribution-cull its casters; the RAII guard
		// clears it so the hook only acts during this light's accumulate.
		//
		// Static-cache split: pick this frame's single filter mode BEFORE the
		// (one) accumulate. A queued rebake makes it StaticOnly (bake the cache);
		// otherwise DynamicOnly (append only movers). The hook folds the static
		// hash either way; a change from the baked hash queues the next rebake.
		{
			bool split = StaticAtlasReady();
			SplitState* st = nullptr;
			CasterPass mode = CasterPass::All;
			uint64_t bakedHash = 0;
			bool staticValid = false;
			bool staticEmpty = false;
			if (split) {
				st = &s_splitState[light];
				st->fullThisFrame = false;
				if (st->splitExcluded) {
					// Latched jitter light: the cache can never stay fresh for
					// it, so it renders full every redraw (no bake, no copy).
					st->bakeThisFrame = false;
					st->fullThisFrame = true;
					split = false;
				}
			}
			if (split) {
				// The atlas slot owns what's baked, so a tile realloc (class
				// change) that drops the cache reads back as invalid here and
				// forces a rebake -- state keyed on the light alone would miss it.
				GetSlotStaticState(slotIndex, bakedHash, staticValid, &staticEmpty);
				// Pose drift past kSplitPoseDriftMax rebakes: this light is
				// redrawing anyway, so the bake replaces (not adds to) a render.
				//
				// Do not inline this read back into SplitDynamicOnlyEligible's call
				// as a 4th argument: C++'s unspecified argument-evaluation order let
				// the (inlined) callee's own pose-drift float reuse slotIndex's
				// spilled stack slot before this array index was read, corrupting it.
				const bool slotInRange = slotIndex >= 0 && slotIndex < s_lights.Size;
				const float pendingScale = slotInRange ? s_lights.Lights[slotIndex].pendingScale : 1.0f;
				mode = (slotInRange && SplitDynamicOnlyEligible(light, *st, staticValid, pendingScale)) ?
				           CasterPass::DynamicOnly :
				           CasterPass::StaticOnly;
				// Bake budget: a hash-upset wave (scene entry, cell attach)
				// otherwise bakes every light in the same few frames and
				// starves the redraw budget. Deferred bakes stay queued; with
				// no valid seed the light renders full instead.
				if (mode == CasterPass::StaticOnly &&
					s_staticBakeCount.load(std::memory_order_relaxed) >= 2u) {
					st->bakeQueued = true;
					if (staticValid) {
						mode = CasterPass::DynamicOnly;
					} else {
						mode = CasterPass::All;
						st->fullThisFrame = true;
					}
				}
				if (mode == CasterPass::StaticOnly) {
					if (auto* ni = light->light.get()) {
						st->bakePos = ni->world.translate;
						st->bakeRot = ni->world.rotate;
					}
					st->bakeQueued = false;
				}
				st->bakeThisFrame = (mode == CasterPass::StaticOnly);
				if (st->bakeThisFrame) {
					s_staticBakeCount.fetch_add(1, std::memory_order_relaxed);
					// Exclude a light that re-bakes for ANY reason (pose drift,
					// class oscillation, churn-invalidation): a pose-stable light
					// bakes once per window, so >=4 bakes in 300 frames means the
					// cache never holds for it -- render full and stop the storm.
					const uint32_t nowFrame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
					if (nowFrame < st->poseWindowStart || nowFrame - st->poseWindowStart > 300u) {
						st->poseWindowStart = nowFrame;
						st->poseRebakes = 0;
					}
					if (++st->poseRebakes >= 4)
						st->splitExcluded = true;
				}
				// Pose fold for bake validity. posStep is coarse (16 units) on
				// purpose: flame flicker jitters the light position a few units
				// every frame, and a 1-unit step re-hashed every jitter --
				// queueing a rebake per flicker and flashing the static-only
				// bake into the live tile on a visible cycle.
				s_visitStaticHash = 0x9e3779b97f4a7c15ull;
				if (auto* ni = light->light.get())
					s_visitStaticHash = FoldLightPose(s_visitStaticHash, ni, 16.0f);
				s_visitDynamicCount.store(0, std::memory_order_relaxed);
				s_visitStaticCount.store(0, std::memory_order_relaxed);
				s_cullPassMode.store(static_cast<int>(mode), std::memory_order_relaxed);
			}

			if (camera)
				s_cullCameraPos = camera->world.translate;  // viewer, for the caster cull
			s_currentCullLight.store(light, std::memory_order_relaxed);
			struct ClearCullLight
			{
				~ClearCullLight()
				{
					s_currentCullLight.store(nullptr, std::memory_order_relaxed);
					// The mode must never outlive the accumulate it was chosen for:
					// the split-path reset below is skipped entirely when `split` is
					// false (atlas not ready, or a splitExcluded light), which would
					// otherwise leave a stale DynamicOnly filtering the next walk.
					s_cullPassMode.store(static_cast<int>(CasterPass::All), std::memory_order_relaxed);
				}
			} clearGuard;

			uint32_t idx = static_cast<uint32_t>(slotIndex);
			// One Accumulate per light per frame (see s_lightAccumFrame): dedup
			// the ring-forming double and log which two slots collided.
			const uint32_t accumFrame =
				globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
			bool duplicateAccum = false;
			if (auto [it, inserted] = s_lightAccumFrame.try_emplace(light, accumFrame, idx); !inserted) {
				duplicateAccum = it->second.first == accumFrame;
				if (duplicateAccum) {
					static std::atomic<uint32_t> s_dupAccumCount{ 0 };
					const uint32_t n = s_dupAccumCount.fetch_add(1, std::memory_order_relaxed) + 1;
					if (n <= 8u || (n % 1000u) == 0u)
						logger::warn("[SCM] Skipped duplicate same-frame Accumulate (light={}, firstSlot={}, thisSlot={}, frame={}, n={})",
							(void*)light, it->second.second, idx, accumFrame, n);
				} else {
					it->second = { accumFrame, idx };
				}
			}
			if (s_lightAccumFrame.size() > 512)
				std::erase_if(s_lightAccumFrame, [&](const auto& kv) { return kv.second.first != accumFrame; });
			if (!duplicateAccum) {
				// Rebuild missed attachments while this accumulate walks the
				// scene: the engine attaches geometry to lights only once per
				// geometry (kRenderUse latch), so a light created after the
				// scene attached -- every light of an in-game same-cell load --
				// otherwise casts nothing forever.
				s_healAttached.clear();
				s_accumRebuildAttach.store(light->geomList.empty(), std::memory_order_relaxed);
				s_cpuAccumUs.fetch_add(TimeUs([&] { light->Accumulate(idx, idx, nullptr); }), std::memory_order_relaxed);
				s_accumRebuildAttach.store(false, std::memory_order_relaxed);
				s_cpuAccumN.fetch_add(1, std::memory_order_relaxed);
				*GetAccumLightSlot() += light->shadowMapCount;
			}

			if (split) {
				st->pendingHash = s_visitStaticHash;
				// Latch mover presence for the sleep skip. Only passes that
				// could see movers may clear it: a StaticOnly bake filters
				// them before the count, and a deduped accumulate saw nothing.
				if (mode != CasterPass::StaticOnly && !duplicateAccum)
					st->sawDynamicLastAccum = s_visitDynamicCount.load(std::memory_order_relaxed) != 0;
				// Same latch for the bake side: a deduped accumulate appended
				// nothing, so its bake cannot have captured geometry either.
				if (mode == CasterPass::StaticOnly)
					st->bakeSawStatic = !duplicateAccum && s_visitStaticCount.load(std::memory_order_relaxed) != 0;
				// A DynamicOnly accumulate observes the current static set; queue
				// a rebake only after the divergence PERSISTS. A flickering hash
				// that oscillates across the baked value resets the streak and
				// never rebakes; a genuine static-set change mismatches every
				// accumulate and rebakes after three.
				if (mode == CasterPass::DynamicOnly && st->pendingHash != bakedHash) {
					if (st->mismatchStreak < 0xFF)
						st->mismatchStreak++;
					// staticEmpty has nothing to lose to a false-positive rebake --
					// the 3-frame hysteresis exists to protect a GOOD cache, not
					// one that was never good. Bypass it here.
					if (staticEmpty || st->mismatchStreak >= 3)
						st->bakeQueued = true;
				} else {
					st->mismatchStreak = 0;
				}
				s_cullPassMode.store(static_cast<int>(CasterPass::All), std::memory_order_relaxed);
			}
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
		bool belowFloor{ false };     // on-screen impact below ShadowImpactFloor
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

		// UpdateCamera failed this frame but the exit streak hasn't matured AND
		// the light already held a slot last frame -- keep its slot/tile, skip
		// its redraw, but let it stay `chosen` instead of dropping instantly.
		// See s_lastValidFrame / s_cameraHold above.
		bool cameraHold{ false };
	};

	/// Q1: an independent sphere-vs-frustum test, compared against the engine's
	/// frustrumCull flag and never against SCM's own verdict -- the validation
	/// gate applies a kCameraExitStreak-frame exit hysteresis, so cross-tabbing
	/// against SCM state would report that intentional lag as a stream of engine bugs.
	/// Only "engine kept a light whose sphere is out" can indicate a defect: the
	/// engine's predicate conjoins the sphere test with a shadow-distance test
	/// and a strictly tighter cone test, so culling more than the sphere is
	/// always sound and the other quadrants are uninformative by construction.
	static void AuditFrustumCull(const CandidateLight& c, RE::NiCamera* camera)
	{
		auto* ni = c.light->light.get();
		if (!ni)
			return;
		// Scaled world radius: a light parented to a scaled NiNode models a
		// larger sphere than radius.x alone, and testing the unscaled radius
		// manufactures suspects out of nothing.
		const float scale = ni->world.scale;
		const float radius = ni->GetLightRuntimeData().radius.x * scale;
		if (!(radius > 0.0f))
			return;  // directional or degenerate: a sphere test on it is meaningless
		s_schedDiag.frustum_audit_candidates++;

		if (camera->PointInFrustum(ni->world.translate, radius * kFrustumAuditMargin) ||
			c.light->frustrumCull != 0) {
			s_frustumSuspectStreak.erase(c.light);
			return;
		}
		s_schedDiag.frustum_audit_kept_out++;
		PruneIfOversized(s_frustumSuspectStreak, 512);
		const uint32_t frames = ++s_frustumSuspectStreak[c.light];
		if (frames < kFrustumAuditStreak)
			return;
		s_schedDiag.frustum_audit_suspects++;
		if (frames != kFrustumAuditStreak)
			return;  // one line per suspect episode, not per frame

		const int32_t slot = GetShadowSlot(c.light);
		const uint32_t demandMax = (slot >= 0 && static_cast<uint32_t>(slot) < kMaxShadowDemandSlots) ?
		                               s_shadowDemand.maxLatest[slot] :
		                               0u;
		const auto cp = camera->world.translate;
		const auto lp = ni->world.translate;
		const float dist = std::sqrt((lp.x - cp.x) * (lp.x - cp.x) + (lp.y - cp.y) * (lp.y - cp.y) +
									 (lp.z - cp.z) * (lp.z - cp.z));
		logger::info("[SCM]       [suspect] light={:#x} name={} r={:.1f} scale={:.3f} centerDist={:.1f} frames={} demandMax={}",
			reinterpret_cast<uintptr_t>(c.light), ni->name.c_str(), radius, scale, dist, frames, demandMax);
	}

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
	//
	// Explicitly resets s_currentCullLight/s_cullPassMode rather than trusting
	// EnableLight's ClearCullLight RAII guard to have run: an AV inside
	// EnableLight (e.g. light->Accumulate on a freed light) is caught by the
	// __except in the CALLING frame (SafeEnableAndValidate/SafeUsable), and
	// MSVC does not guarantee C++ destructors in the unwound intervening
	// frame run during SEH-caught hardware-exception propagation without
	// /EHa. Left unreset, a mid-accumulate AV permanently stuck
	// s_currentCullLight non-null and s_cullPassMode off All, filtering every
	// subsequent cull walk on this thread -- including the sun's, which never
	// sets either -- until another light's EnableLight call happened to
	// complete normally and reset them.
	static void LogShadowSehCatch(RE::BSShadowLight* a_light = nullptr)
	{
		s_currentCullLight.store(nullptr, std::memory_order_relaxed);
		s_cullPassMode.store(static_cast<int>(CasterPass::All), std::memory_order_relaxed);
		static std::atomic<int> n{ 0 };
		if (n.fetch_add(1, std::memory_order_relaxed) < 20)
			logger::warn("[SCM] SEH caught AV in shadow-light usability scan (probe missed); skipping light. light={:#x}",
				reinterpret_cast<uintptr_t>(a_light));
	}

	// SEH backstop in its own function (no C++ unwinding objects) so MSVC accepts
	// __try. Returns false (unusable) on any access violation.
	template <class Fn>
	static bool SafeUsable(Fn&& a_fn, RE::BSShadowLight* a_light)
	{
		__try {
			return a_fn(a_light);
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
			LogShadowSehCatch(a_light);
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
			LogShadowSehCatch(e.Light);
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

		// Advance the caster-classification epoch once per frame here -- before
		// this frame's accumulate and its later render-split passes -- so a
		// caster classifies identically across both phases (a mid-frame bump
		// would double-count movement and flip casters static too early).
		// Periodically prune casters not seen for a while.
		if (++s_casterClassEpoch % 300 == 0) {
			std::erase_if(s_casterMobility,
				[](const auto& kv) { return s_casterClassEpoch - kv.second.lastEpoch > 300; });
			// Split state keys dead lights until this cap; state rebuilds
			// harmlessly, so a plain size-gated clear matches the sibling maps.
			PruneIfOversized(s_splitState, 512);
		}

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

		// Exclusive against ShadowCasterManager::Update's pool resize (a settings
		// change can delete[]/new[] s_lights.Lights on a different call path than
		// this pass); held for the whole pass so every s_lights.Lights[slot] access
		// below sees a consistent pointer/Size pair. RAII: released on every return.
		std::shared_lock poolLock(s_lightsPoolMutex);

		// Drain a pending teardown reset before touching any slot, then SKIP this pass.
		// ClearLightArrays freed the previous scene's lights but doesn't shrink
		// activeShadowLights, so snapshotting or scoring it now would touch freed memory.
		// The engine left the array vanilla-valid this frame; the next pass runs once it is
		// rebuilt. (Load-bearing: removing this return reintroduces the 20 SEH catches.)
		if (s_pendingSessionReset.exchange(false, std::memory_order_acquire)) {
			ResetSession();
			return;
		}

		// Drain a pending cell-grid shift. Slots stay owned (unlike the
		// session reset above), so this doesn't skip the pass -- it only
		// drops caches keyed on caster geometry identity, which a cell swap
		// can silently recycle onto unrelated new geometry.
		if (s_pendingCellReset.exchange(false, std::memory_order_acquire)) {
			s_cellResetTotal.fetch_add(1, std::memory_order_relaxed);
			s_casterMobility.clear();
			s_splitState.clear();  // bakeQueued defaults true: every light re-bakes
			ShadowCasterManager::InvalidateAllStaticBakes();
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
			s_cameraHold.clear();
			ClearBelowFloor();
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
				float impact = 1.0f;
				c.score = CalculateLightScore(l, camera, tmpIndex++,
					s_settings.ShadowImpactFloor > 0.0f ? &impact : nullptr);
				if (s_settings.ShadowImpactFloor > 0.0f && impact < s_settings.ShadowImpactFloor) {
					// Exit hysteresis, own consecutive-count gate (unlike the
					// recency-based s_lastValidFrame above): a light
					// hovering near the floor must fail 15 consecutive frames before
					// it's actually dropped, or it flaps its atlas slot every time it
					// dips back above and re-bakes on return.
					PruneIfOversized(s_belowFloorStreak, 512);
					if (++s_belowFloorStreak[l] >= 15) {
						c.belowFloor = true;
						AddBelowFloor(reinterpret_cast<uintptr_t>(l));
					}
				} else {
					s_belowFloorStreak.erase(l);
				}
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

		// Prune split state for departed lights: pointer keys recycle, and a new
		// light inheriting a stale splitExcluded latch renders full forever.
		// Threshold avoids churning state for gate-flapping lights in small scenes.
		if (s_splitState.size() > 128) {
			std::set<RE::BSShadowLight*> liveLights;
			for (auto& c : candidates)
				liveLights.insert(c.light);
			std::erase_if(s_splitState, [&](const auto& kv) { return !liveLights.contains(kv.first); });
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
		// Deterministic tiebreak on exact score ties: two fixtures at the same
		// distance/intensity (e.g. symmetric braziers) can score identically,
		// and std::sort's order for equivalent elements isn't stable -- an
		// unstable tie flips which of them claims a free slot first in the
		// re-add pass below, producing a visible flicker between the two.
		// Matches the LastDrawnFrame/Index tiebreak precedent in schedOrder
		// further down this file.
		std::sort(candidates.begin(), candidates.end(),
			[](const CandidateLight& a, const CandidateLight& b) {
				if (a.sun != b.sun)
					return a.sun;
				if (a.score != b.score)
					return a.score > b.score;
				return reinterpret_cast<uintptr_t>(a.light) < reinterpret_cast<uintptr_t>(b.light);
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
			// Non-VR only: the engine culls against two frustums there, so a
			// single-sphere-vs-frustum verdict is uninterpretable regardless of
			// what the demand producer measures.
			const bool auditFrustum = s_shadowDemand.instrumentation && !globals::game::isVR;
			for (auto& c : candidates) {
				auto* l = c.light;
				// Unconditional and ahead of every gate: run inside the
				// UpdateCamera failure branch instead and a light the engine
				// KEPT never reaches it, making the one informative quadrant
				// structurally unreachable and guaranteed to read zero.
				if (auditFrustum)
					AuditFrustumCull(c, camera);
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
					// Exit hysteresis: the gate's inputs (light position vs
					// frustum, distance vs fShadowDistance) are flicker-jittered,
					// so a boundary light flaps valid/invalid every frame. Honor
					// invalid only after it persists; a departed light drops
					// kCameraExitStreak frames late, off-view anyway. Any valid
					// frame resets it.
					//
					// Age-based prune, NOT PruneIfOversized: that clears the whole
					// map, and a missing entry here means "never passed UpdateCamera"
					// -> instant demotion for every failing light at once. An entry
					// older than the exit streak can no longer grant grace anyway, so
					// dropping exactly those is both bounded and semantically inert.
					if (s_lastValidFrame.size() > 512) {
						const int32_t pruneNow = *globals::game::frameCounter;
						std::erase_if(s_lastValidFrame, [pruneNow](const auto& kv) {
							return (pruneNow - kv.second) >= static_cast<int32_t>(kCameraExitStreak);
						});
					}
					// Capture the engine's side-band sub-reason BEFORE the restore
					// below overwrites lodDimmer; otherwise invalidLod always reads
					// false and the LOD bucket/UI reason go permanently dead.
					const bool lodFadedNow = (l->lodDimmer == 0.0f);
					// UpdateCamera's LOD sub-test can zero lodDimmer even on a
					// held frame (not just on eventual conversion) -- without
					// this, addShadowLight's fade*=lodDimmer would render a
					// held light at zero intensity despite it keeping its slot
					// below, i.e. "fixed the flicker" would really mean "kept
					// an invisible slot". Unconditional: a non-held candidate
					// hits the c.invalidLod branch below and gets no benefit
					// from this, but it's harmless there too.
					RestoreZeroedLodDimmer(l);
					const auto lastValidIt = s_lastValidFrame.find(l);
					const int32_t nowFrame = *globals::game::frameCounter;
					// No entry at all means this light has NEVER passed
					// UpdateCamera -- must not claim chosen/budget status on
					// its very first candidate frame; that invariant is
					// unchanged from the old never-slotted branch.
					const bool recentlyValid = lastValidIt != s_lastValidFrame.end() &&
					                           (nowFrame - lastValidIt->second) <
					                               static_cast<int32_t>(kCameraExitStreak);
					if (recentlyValid) {
						c.cameraHold = true;
						s_cameraHold.insert(l);
					} else {
						c.invalidCamera = true;
						c.invalid = true;
						// Recover the sub-reason from the engine's side-band flags.
						// Both can be true (a light off-screen AND LOD-faded);
						// recorded as independent bits for analysis. Action loop
						// below treats frustum-out as terminal (drop) and
						// LOD-faded-in-frustum as convert.
						c.invalidFrustum = (l->frustrumCull != 0);
						c.invalidLod = lodFadedNow;
						continue;
					}
				} else {
					s_lastValidFrame[l] = *globals::game::frameCounter;
				}
				// Portal culling only applies in interior cells where a portal graph exists.
				// Lights with no culling process (e.g. WSU spotlights outside cell bounds)
				// or no portal are unconditionally visible; skip the check for them.
				// Promoted lights carry a rebuilt culling process whose portal-graph entry the
				// engine never room-associates (always visibleUnboundSpace), so the test
				// false-culls an in-view light. The verdict is unreliable for them by
				// construction -- always skip the demotion; native lights still get it.
				// A held light's UpdateCamera failed this frame, so its culling
				// state is stale too -- skip the portal check rather than act on
				// a verdict from a rejected camera pass.
				auto* cull = (c.cameraHold || IsPromotedLight(l->light.get())) ? nullptr : GetLightCullingProcess(l);
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

				// Impact floor: a below-floor light converts to a non-shadow
				// light (keeps diffuse via clusters, drops its shadow redraw),
				// the same path as an over-budget light. The table's "Low"
				// group-hover highlight (with the floor off) is the preview.
				if (c.belowFloor) {
					c.excess = true;
				}
				// Effective point-light capacity excludes the engine-claimed
				// focus shadow slots; excess candidates fall through to the
				// existing convert/disable path.
				else if (wantCount < s_settings.ShadowLightCount - s_focusShadowSlots) {
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
			// Two passes, not one: two lights both re-entering on the same
			// frame (e.g. a lockstep gate flap -- see s_cameraHold above)
			// are score-sorted, so the higher-ranked one used to run first
			// and could fall through to FindFreeIndex, landing on and
			// Clear()/FreeSlotTile()-ing the OTHER light's still-orphaned
			// slot before it got a chance to reclaim it -- destroying a
			// peer's cache instead of just its own. Reclaim-by-owner for
			// every chosen light first (pass 1), THEN hand out fresh slots
			// to whoever's left unplaced (pass 2), so no light can steal
			// another's orphaned tile.
			static std::vector<CandidateLight*> unplaced;
			unplaced.clear();
			for (auto& c : candidates) {
				if (!c.chosen)
					continue;
				bool alreadyIn = false;
				for (int i = 0; i < s_lights.Size && !alreadyIn; i++)
					if (s_lights.Lights[i].Light == c.light)
						alreadyIn = true;
				if (alreadyIn)
					continue;

				// Array parity on re-entry: a light returning after a brief
				// eviction (gate flap) reclaims the free slot that still holds
				// ITS OWN rendered tile -- content resumes sampling immediately,
				// exactly like an array slice surviving the round-trip. The
				// entry is left intact too (its cache keys are its own).
				bool reclaimed = false;
				if (auto* ni = c.light->light.get()) {
					const int ownerIdx = FindFreeSlotByOwner(ni);
					if (ownerIdx >= 0 && !s_lights.Lights[ownerIdx].Light) {
						s_lights.Lights[ownerIdx].Light = c.light;
						reclaimed = true;
					}
				}
				if (!reclaimed)
					unplaced.push_back(&c);
			}
			for (auto* cp : unplaced) {
				const int idx = s_lights.FindFreeIndex(true, s_settings.ShadowLightCount, s_settings.ConvertedShadowSlots);
				if (idx < 0)
					continue;
				// Eviction nulls Light* but leaves the rest of LightEntry intact
				// so it can serve as a cache key. Clear at acquire so the new
				// occupant doesn't inherit LastDrawnFrame / lastGeomHash from the
				// previous owner (which would skip its first render and let the
				// cluster pipeline sample stale kSHADOWMAPS[idx] content).
				s_lights.Lights[idx].Clear();
				// Drop the previous occupant's tile: its depth must not be
				// advertised under the new light's projection.
				FreeSlotTile(idx);
				// A fresh slot occupant is a genuinely different light (new,
				// or a recycled BSShadowLight*/NiLight* address). The
				// pointer-keyed EMA/streak caches below key off that address
				// too and are NOT cleared by Clear() above -- without this,
				// a recycled address inherits the previous occupant's score
				// anchor (biasing it toward "distant/unimportant" for ~25
				// frames) and invalid-streak count (which can already be
				// near the exit threshold), most visible right after a cell
				// load when addresses get recycled in a burst.
				s_lastValidFrame.erase(cp->light);
				s_belowFloorStreak.erase(cp->light);
				// Keyed by the same recycled BSShadowLight* address. A stale
				// splitExcluded latch makes the new occupant render full forever
				// (see the prune comment above); the size-gated prune only covers
				// pools past 128 entries.
				s_splitState.erase(cp->light);
				if (auto* ni = cp->light->light.get()) {
					ResetScoreAnchor(ni);
					s_hashRadiusAnchor.erase(ni);
				}
				s_lights.Lights[idx].Light = cp->light;
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

			// Publish each occupant's ScoreFormula value: the one priority that
			// ordered selection also orders the atlas cell budget (within a
			// class band) and drives the redraw curve percentile.
			{
				std::unordered_map<const RE::BSShadowLight*, double> scoreByLight;
				scoreByLight.reserve(candidates.size());
				for (const auto& c : candidates)
					scoreByLight.emplace(c.light, c.score);
				for (int i = 0; i < s_lights.Size; i++) {
					auto& e = s_lights.Lights[i];
					if (!e.Light)
						continue;
					if (auto it = scoreByLight.find(e.Light); it != scoreByLight.end())
						e.lastScore = it->second;
				}
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

			// Maintains the per-slot consecutive-absence streak, consumed by the
			// audit's Q2 counterfactual, the real hard skip below, and the
			// occluded-redraw-ceiling stretch (occlusionConfidence, further down
			// this loop). The demand tiebreaker always needs it live.
			const bool auditDemand = s_shadowDemand.instrumentation;
			AdvanceDemandStreaks();

			// Eligible population, counted over the whole pool so the ceiling
			// metric means the same thing whether or not the skip is live (the
			// entries it removes never reach `pending`).
			if (auditDemand)
				for (int i = 0; i < s_lights.Size; i++)
					if (s_lights.Lights[i].Light && DemandSkipCandidate(s_lights.Lights[i]))
						s_schedDiag.demand_skip_eligible++;

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
					maxRedraw--;
					// Sun's budget cost is bookkept at 0, so no budgetRemain
					// decrement. isFirst deliberately survives the sun -- it's
					// the point lights' starvation guarantee, which the sun
					// used to consume before an over-budget light could use it.
				}
			}

			if (maxRedraw > 0 && budgetRemain > 0) {
				std::vector<LightEntry*> pending;
				// Debugging aid (temporary, not gated on a setting -- this is
				// diagnosing a live report of a skipped center-screen light):
				// a demand-skipped light whose own lastScore ranks above the
				// median of the live pool is a direct contradiction -- the
				// priority system rates it important, the demand system rates
				// it invisible. Logged once per continuous skip episode, not
				// per frame, via s_highPrioritySkipLogged below.
				struct HighPrioritySkip
				{
					LightEntry* entry;
					RE::BSShadowLight* light;
				};
				static std::vector<HighPrioritySkip> highPrioritySkips;
				highPrioritySkips.clear();
				// Sleep-skipped entries never reach the `pending` loop either,
				// so without their own desiredScale refresh (below, alongside
				// highPrioritySkips) they're exposed to the same stale-high-
				// class atlas hoarding D1 fixed for demand skips -- they were
				// simply never included in that fix's coverage.
				static std::vector<LightEntry*> sleepSkips;
				sleepSkips.clear();
				// Two more lists that never reach `pending` either, so without
				// their own desiredScale refresh below they're exposed to the
				// same stale-high-class atlas hoarding the sleep/demand-skip
				// fix covers: a held or not-yet-admitted light can go small/
				// distant/occluded while skipped and keep outranking genuinely
				// visible lights for the whole skip window on stale geometry.
				static std::vector<LightEntry*> cameraHoldSkips;
				cameraHoldSkips.clear();
				static std::vector<LightEntry*> newLightSkips;
				newLightSkips.clear();
				for (int i = 0; i < s_lights.Size; i++) {
					auto& e = s_lights.Lights[i];
					if (!e.Light || e.RedrawFrame)
						continue;
					// Cleared here (not just at the two skip sites below) so a
					// light that falls through to `pending` -- the common case --
					// reads correctly as "not skipped" for the stall sweep.
					e.skippedThisFrame = false;
					// A held light (UpdateCamera failed, exit streak immature,
					// slot protected -- see s_cameraHold above) keeps its
					// cached tile but must not redraw: its shadow camera was
					// rejected this frame. Excluded from `pending` before
					// budget accounting so it cannot consume redraw budget a
					// visible light needs. skippedThisFrame=true so the stall
					// sweep doesn't accrue against a light that can't act.
					if (s_cameraHold.count(e.Light)) {
						e.skippedThisFrame = true;
						cameraHoldSkips.push_back(&e);
						continue;
					}
					// Honour AllowDrawNewLight: when disabled, brand-new
					// entries (LastDrawnFrame < 0) wait until the next frame
					// rather than competing for this frame's budget. Existing
					// lights re-entering view still schedule normally.
					if (!s_settings.AllowDrawNewLight && e.LastDrawnFrame < 0) {
						newLightSkips.push_back(&e);
						continue;
					}
					// Empty-dynamic sleep: a moverless light with a valid, fresh
					// static bake would redraw an identical tile -- skip it
					// entirely (no accumulate, no budget); it keeps sampling its
					// cached tile via the non-redrawn insertion path. Its shadow
					// content is provably unchanged (that's what SleepSkipEligible
					// proves), so it's not just skipped -- it's genuinely clean.
					if (SleepSkipEligible(e, i, now)) {
						s_schedDiag.sleep_skips++;
						s_sleepSkipTotal.fetch_add(1, std::memory_order_relaxed);
						e.skippedThisFrame = true;
						e.schedDirty = false;
						sleepSkips.push_back(&e);
						continue;
					}
					// Zero-demand skip: the GPU measured nothing on screen
					// sampling this light for a sustained streak. Tested after
					// sleep so the two can never double-count the same entry.
					// schedDirty is deliberately left at its last computed value
					// (not cleared): if the occlusion test wrongly marked a truly-
					// dirty light as skippable, that's real starvation risk, not
					// noise -- but skippedThisFrame still freezes the stall counter
					// here so the (expected, common) case of correctly-occluded
					// lights doesn't dominate the stall metric with their demand-
					// skip duration, which is already tracked separately via
					// untouchedSamples/the demand streak histogram.
					if (DemandSkipEligible(e, i, now)) {
						s_schedDiag.demand_skips++;
						s_demandSkipTotal.fetch_add(1, std::memory_order_relaxed);
						e.skippedThisFrame = true;
						highPrioritySkips.push_back({ &e, e.Light });
						continue;
					}
					s_highPrioritySkipLogged.erase(e.Light);
					pending.push_back(&e);
				}

				// Base texels for the coverage classifier; lazily captured from
				// the live texture, so fall back until it is readable.
				const float baseTileTexels = s_initialShadowMapResolution > 0 ?
				                                 static_cast<float>(s_initialShadowMapResolution) :
				                                 2048.0f;

				// Priority percentile across the live pool: 1.0 = highest score.
				// Rank, not raw value, so the redraw curve is invariant to the
				// user formula's scale.
				static std::vector<double> scoreRank;
				scoreRank.clear();
				for (int i = 0; i < s_lights.Size; i++)
					if (s_lights.Lights[i].Light)
						scoreRank.push_back(s_lights.Lights[i].lastScore);
				std::sort(scoreRank.begin(), scoreRank.end());
				auto scorePercentile = [&](double score) -> float {
					if (scoreRank.size() < 2)
						return 1.0f;
					const auto it = std::lower_bound(scoreRank.begin(), scoreRank.end(), score);
					return static_cast<float>(it - scoreRank.begin()) / static_cast<float>(scoreRank.size() - 1);
				};

				// Debugging aid: score and demand are complementary, not
				// redundant (score has no occlusion term), so a high-percentile
				// light reading zero demand is expected -- an occluded light can
				// legitimately be top-ranked by score. Logged once per
				// continuous episode (guarded by s_highPrioritySkipLogged,
				// cleared the frame the light exits the skip set above), debug
				// level only: this is a rate proxy for fixed-scene A/B, not an
				// error.
				for (const auto& skip : highPrioritySkips) {
					const float percentile = scorePercentile(skip.entry->lastScore);
					if (percentile < 0.5f || s_highPrioritySkipLogged.contains(skip.light))
						continue;
					s_highPrioritySkipLogged.insert(skip.light);
					auto* ni = skip.light->light.get();
					const int32_t slot = GetShadowSlot(skip.light);
					const uint32_t demandMax = (slot >= 0 && static_cast<uint32_t>(slot) < kMaxShadowDemandSlots) ?
					                               s_shadowDemand.maxLatest[slot] :
					                               0u;
					const float dist = ni ? std::sqrt(
												(ni->world.translate.x - camera->world.translate.x) * (ni->world.translate.x - camera->world.translate.x) +
												(ni->world.translate.y - camera->world.translate.y) * (ni->world.translate.y - camera->world.translate.y) +
												(ni->world.translate.z - camera->world.translate.z) * (ni->world.translate.z - camera->world.translate.z)) :
					                        -1.0f;
					logger::debug(
						"[SCM] high-priority light demand-skipped: light={:#x} name={} percentile={:.2f} score={:.1f} demandMax={} streak={} centerDist={:.1f}",
						reinterpret_cast<uintptr_t>(skip.light), ni ? ni->name.c_str() : "?", percentile,
						skip.entry->lastScore, demandMax, skip.entry->untouchedSamples, dist);
				}

				// Refresh desiredScale for skip-eligible entries too: they never
				// reach the `pending` loop below, so without this their class
				// freezes at whatever it was on the frame they stopped drawing.
				// The atlas rank budget below sorts by desiredScale over the
				// WHOLE pool, so a stale-high class from a light that has since
				// gone small/distant/occluded would otherwise keep outranking
				// genuinely visible lights for the entire skip window.
				auto refreshSkippedDesiredScale = [&](LightEntry* e) {
					if (auto* ni = e->Light->light.get()) {
						const auto geom = ComputeLightGeometry(e->Light, camera, ni->GetLightRuntimeData().radius.x);
						float sizeProxy = geom.sizeProxy;
						if (geom.attCam <= 0.0f && geom.attPlr <= 0.0f)
							sizeProxy = std::min(sizeProxy, 0.25f);
						e->desiredScale = AtlasActive() ?
						                      TileScaleForCoverage(sizeProxy, baseTileTexels, e->desiredScale) :
						                      1.0f;
						if (!AtlasActive())
							e->pendingScale = e->desiredScale;
					}
				};
				for (const auto& skip : highPrioritySkips)
					refreshSkippedDesiredScale(skip.entry);
				for (auto* e : sleepSkips)
					refreshSkippedDesiredScale(e);
				for (auto* e : cameraHoldSkips)
					refreshSkippedDesiredScale(e);
				for (auto* e : newLightSkips)
					refreshSkippedDesiredScale(e);

				for (auto* e : pending) {
					// Displacement is measured unconditionally (not just when a
					// custom formula wants it): the dirty-eligibility check below
					// needs it too, and computing it here once covers both.
					double displacementMagnitude = 0.0;
					// Also feeds schedDirty's displacementTexels check further below
					// (not just the formula): both compare against the SAME fixed
					// e->lastRenderedPos, which only moves on an actual redraw, so
					// deadzoning here cannot mask real motion from the dirty flag --
					// a light with genuine cumulative drift still crosses the
					// threshold once total displacement exceeds it; only zero-mean
					// flicker jitter (which never accumulates directionally) stays
					// suppressed. Without this, a stable-but-flickering light's raw
					// per-frame jitter kept tripping schedDirty every frame it
					// exceeded one texel, re-rasterizing (and visibly dancing) even
					// while fully admitted/chosen -- the exit-hysteresis streak in
					// UpdateCamera's own gate can't reach this, since it only guards
					// admission/eviction, not per-frame render content.
					double formulaDisplacement = 0.0;
					if (auto* nilight = e->Light->light.get()) {
						auto& curr = nilight->world.translate;
						float dx = curr.x - e->lastRenderedPos.x;
						float dy = curr.y - e->lastRenderedPos.y;
						float dz = curr.z - e->lastRenderedPos.z;
						displacementMagnitude = static_cast<double>(sqrtf(dx * dx + dy * dy + dz * dz));
						formulaDisplacement = displacementMagnitude;

						// Deadzone sub-texel motion before it reaches the formula:
						// an animated flame's own idle-jitter wobble is well under
						// a texel and produces zero visible difference in the
						// shadow map, but was still read as "displacement" every
						// frame -- shortening the computed interval on noise, not
						// real movement, which made the redraw admit/skip pattern
						// irregular enough to read as flicker on torches/braziers.
						// Uses last frame's tile scale (e->pendingScale, not yet
						// updated this iteration) as a cheap approximation, and the
						// light's own radius as the reference size -- skip entirely
						// for a degenerate zero-radius light rather than floor to
						// an effectively-zero threshold that would defeat it.
						const float radius = nilight->GetLightRuntimeData().radius.x;
						if (radius > 0.0f) {
							const float approxPosStep = radius /
							                            std::max(baseTileTexels * std::max(e->pendingScale, kTileScaleFloor), 1.0f);
							// The engine's own flame-flicker jitter (TESObjectLIGH::
							// flickerMovementAmplitude) is a FIXED world-space wobble,
							// independent of tile resolution -- but approxPosStep above
							// SHRINKS as resolution grows (a bigger tile means a smaller
							// texel in world units). At full class, a nearby flickering
							// light's pure jitter easily exceeds one texel even though
							// it isn't real motion, defeating the deadzone exactly for
							// the highest-priority lights (close, full-res) that need it
							// most. Floor the deadzone at the light's own amplitude, only
							// for lights that actually flicker/pulse. Cached per NiLight
							// -- the base-form lookup is one-time, not per-frame.
							static std::unordered_map<const RE::NiLight*, float> s_flickerAmplitude;
							PruneIfOversized(s_flickerAmplitude, 1024);
							auto [ampIt, ampNew] = s_flickerAmplitude.try_emplace(nilight, 0.0f);
							if (ampNew) {
								if (auto* ref = nilight->GetUserData()) {
									if (auto* base = ref->GetObjectReference()) {
										if (auto* ligh = base->As<RE::TESObjectLIGH>(); ligh && !ligh->GetNoFlicker())
											ampIt->second = ligh->data.flickerMovementAmplitude;
									}
								}
							}
							const float deadzone = std::max(approxPosStep, ampIt->second);
							if (formulaDisplacement < static_cast<double>(std::max(deadzone, 1e-4f)))
								formulaDisplacement = 0.0;
						}
					}

					double interval = 0.0;
					if (s_formulaRedrawInterval) {
						SetupLightFormula(e->Light, camera, 0);
						// e->Index is the pool index. Beyond PointLightEnd are converted slots.
						if (e->Index >= s_lights.PointLightEnd(s_settings.ShadowLightCount))
							FormulaHelper::SetParam(kFormulaParam_LightConverted, 1.0);

						// Exposed as `lightdisplacement` so the formula can prioritise
						// fast-moving lights (e.g. player torches) without relying on
						// distance-to-camera alone.
						FormulaHelper::SetParam(kFormulaParam_LightDisplacement, formulaDisplacement);

						interval = s_formulaRedrawInterval->Calculate();
					}
					interval += 1.0;
					// The shipped formula's displacement tail can pull interval
					// negative (a close-range, fast-moving light: base term ~0,
					// displacement term down to -10). The importance multiply below
					// is monotonically DECREASING in importance, which is only
					// correct for a non-negative interval -- scaling a negative
					// value by a smaller factor moves it toward zero, i.e. LATER in
					// the ascending sort, inverting priority for exactly the
					// close/fast/important case the displacement term exists to
					// catch. Clamp here so the multiply's direction is always right.
					interval = std::max(interval, 0.0);

					// Contribution-weighted redraw interval:
					//   interval *= kMaxMult * (kMinMult/kMaxMult)^scorePercentile
					// Empirical curve, no external citation. Top-percentile lights
					// get x(kMinMult/kMaxMult), bottom-percentile get x1.

					float importance = 0.0f;
					float sizeProxy = 0.0f;

					if (auto* ni = e->Light->light.get()) {
						const auto geom = ComputeLightGeometry(e->Light, camera, ni->GetLightRuntimeData().radius.x);
						// Legacy contribution metric, kept as the lightimportance
						// formula variable; ranking decisions use lastScore (the
						// ScoreFormula value) so one function owns priority.
						importance = geom.lum * std::max(geom.coverage, std::max(geom.attCam, geom.attPlr) * 0.3f);
						sizeProxy = geom.sizeProxy;
						// A light that reaches neither the camera nor the player
						// cannot show a close-up shadow: cap its tile class so
						// out-of-range embedded lights (large radius, zero
						// attenuation at the viewer) stop hoarding full tiles the
						// rank budget then can't give to visible lights.
						if (geom.attCam <= 0.0f && geom.attPlr <= 0.0f)
							sizeProxy = std::min(sizeProxy, 0.25f);

						// Q2 headroom bands. The sub-tap band is load-bearing and
						// must not be lumped into "occluded": the producer takes
						// one tap per 64x64 tile, so a light illuminating only
						// thin geometry can read zero for many consecutive
						// samples while being plainly visible.
						const int32_t demandSlot = auditDemand && DemandSampleUsable() ? DemandSlotFor(*e) : -1;
						if (demandSlot >= 0) {
							s_schedDiag.demand_slotted++;
							if (s_shadowDemand.maxLatest[demandSlot] == 0) {
								s_schedDiag.demand_zero++;
								if (s_shadowDemand.tileCount > 0 &&
									geom.screenArea * static_cast<float>(s_shadowDemand.tileCount) < 1.0f)
									s_schedDiag.demand_sub_tap++;
							}
						}
					}

					// Exponential interval scaling driven by the light's priority
					// PERCENTILE among currently slotted lights (scale-invariant
					// to user formula rescaling): maxScale*(minScale/maxScale)^pct.
					float kMaxMult = s_settings.ImportanceMaxScale;
					float kMinMult = std::min(s_settings.ImportanceMinScale, kMaxMult);
					float clampedImp = scorePercentile(e->lastScore);
					interval *= static_cast<double>(kMaxMult * powf(kMinMult / kMaxMult, clampedImp));

					e->RedrawScore = e->LastDrawnFrame + interval;
					e->lastImportance = importance;

					e->desiredScale = AtlasActive() ?
					                      TileScaleForCoverage(sizeProxy, baseTileTexels, e->desiredScale) :
					                      1.0f;
					// Atlas mode: the render-pass rank budget owns pendingScale.
					if (!AtlasActive())
						e->pendingScale = e->desiredScale;

					// Position step scaled to the tile class: at 128px a fire's
					// flicker orbit is sub-texel and must not bust the cache;
					// at full class the same motion is visible and should.
					float posStep = 1.0f;
					if (auto* ni2 = e->Light->light.get()) {
						const float texels = baseTileTexels * std::max(e->pendingScale, kTileScaleFloor);
						posStep = ni2->GetLightRuntimeData().radius.x / std::max(texels, 1.0f);
					}
					// ComputeShadowGeomHash's full geomList walk measured ~17us/light;
					// reuse the cached hash until the caster count changes or this many
					// frames pass. Winners latch this value below (see latchGeomHash).
					constexpr int32_t kGeomHashRehashInterval = 4;
					const auto geomSize = static_cast<std::uint32_t>(e->Light->geomList.size());
					const bool dueForRehash = e->lastHashComputeFrame < 0 ||
					                          geomSize != e->lastHashGeomListSize ||
					                          (now - e->lastHashComputeFrame) >= kGeomHashRehashInterval;
					if (dueForRehash) {
						e->cachedPendingGeomHash = ComputeShadowGeomHash(e->Light, posStep);
						e->lastHashComputeFrame = now;
						e->lastHashGeomListSize = geomSize;
					}
					e->pendingGeomHash = e->cachedPendingGeomHash;

					// Occlusion confidence: 0 (fully visible/unmeasured) unless a real
					// GPU reading confirms absence. Computed before schedDirty/the
					// tiebreaker so both scale by it consistently.
					//
					// Evidence is the MAX channel + absence streak, not a smoothed EMA:
					// an EMA's magnitude scales with tile count, so no fixed threshold
					// is resolution-stable -- an earlier EMA version read a
					// plainly-visible torch as ~93% occluded.
					float occlusionConfidence = 0.0f;
					if (DemandSampleUsable() && e->LastDrawnFrame >= 0) {
						const int32_t demandSlot = DemandSlotFor(*e);
						if (demandSlot >= 0) {
							// GetShadowSlot is a linear scan; debug-only cross-check
							// that the demand slot and atlas slot agree.
							assert(e->Index == demandSlot && e->Index == GetShadowSlot(e->Light));
							if (s_shadowDemand.maxLatest[demandSlot] <= kDemandUntouchedMaxRaw) {
								const float fullStreak = static_cast<float>(
									std::max(1u, EffectiveZeroDemandStreak() / kOccludedStretchStreakDivisor));
								occlusionConfidence = std::clamp(
									static_cast<float>(e->untouchedSamples) / fullStreak, 0.0f, 1.0f);
							}
						}
					}
					// Floored at 1: fed into std::clamp below as `hi` against a `lo`
					// of 1.0, so a sub-1 setting (devbench JSON patch bypasses the
					// UI slider's 4-60 range) would invert lo/hi (UB) and feeds an
					// out-of-range cast to uint32_t at starvationFloorFrames below.
					const double redrawIntervalMaxFrames = std::max(1.0, static_cast<double>(s_settings.RedrawIntervalMaxFrames));
					const double occludedCeilingFrames = redrawIntervalMaxFrames * kOccludedRedrawMultiplier;

					// Eligibility (dirty/clean) is a FILTER ahead of RedrawScore ranking,
					// kept separate so "not due yet" and "provably unchanged" can't be
					// conflated. The staggered age fallback backstops hash false
					// negatives (sub-texel motion reads unchanged); its window blends
					// toward occludedCeilingFrames by occlusionConfidence so it can't
					// force a confirmed-occluded light dirty before that ceiling elapses.
					//
					// An invalid atlas tile is a separate dirty signal: geometry/hash/
					// scale can be unchanged while tile content is garbage, which the
					// hash/displacement checks can't catch.
					const double backstopBaseFrames = static_cast<double>(kSleepRedrawIntervalFrames) +
					                                  static_cast<double>(occlusionConfidence) *
					                                      (occludedCeilingFrames - static_cast<double>(kSleepRedrawIntervalFrames));
					// Modulo by the window itself, not a fixed `index % 7` (which only
					// varies this bigger, per-light window by 0-6 frames near its top
					// and left same-phase lights re-triggering as one visible batch).
					// Capped to 60% of the window so no index can collapse the modulo
					// near window-1 and shrink the backstop to ~1 frame.
					const int32_t backstopWindowFrames = std::max(static_cast<int32_t>(backstopBaseFrames), 1);
					const int32_t backstopSpreadCap = std::max(1, static_cast<int32_t>(backstopWindowFrames * 0.6));
					const int32_t staggeredBackstopWindow =
						backstopWindowFrames - ((e->Index * kSleepStaggerStride) % backstopSpreadCap);
					const double displacementTexels =
						formulaDisplacement / static_cast<double>(std::max(posStep, 1e-4f));
					AtlasTileTexels schedDirtyTile{};
					const bool tileInvalid = GetSlotTileTexels(e->Index, schedDirtyTile) && !schedDirtyTile.contentValid;
					e->schedDirty = e->LastDrawnFrame < 0 ||
					                e->lastGeomHash == 0 ||
					                e->pendingGeomHash != e->lastGeomHash ||
					                e->pendingScale != e->renderedScale ||
					                displacementTexels >= 1.0 ||
					                tileInvalid ||
					                (now - e->LastDrawnFrame) >= staggeredBackstopWindow;

					// Phase-1 VSM-style demand tiebreaker: deprioritize a light measured
					// barely visible last frame. Must stay in RedrawScore's native
					// frame-count scale -- a 1e14-magnitude version of this term became
					// the sole sort key instead of a tiebreaker (measured: ~2.4x
					// per-occurrence PointLights cost). Scaled by the confidence
					// headroom, not a fixed nudge, so it can actually reach the
					// expanded occluded ceiling.
					if (occlusionConfidence > 0.0f) {
						const double demandHeadroomFrames = occludedCeilingFrames - redrawIntervalMaxFrames;
						e->RedrawScore += static_cast<double>(occlusionConfidence) * demandHeadroomFrames;
					}

					// Bound total delay since last redraw so a light returning from a
					// long sleep/demand skip can't monopolize admission via an
					// artificially-ancient deadline. Floor of 1 (not 0) closes a
					// tie-window where the displacement tail can compute exactly 0.
					// Ceiling blends toward occludedCeilingFrames by
					// occlusionConfidence (continuous, not a step) to match the
					// tiebreaker's own scaling above.
					const double effectiveMaxFrames = redrawIntervalMaxFrames +
					                                  static_cast<double>(occlusionConfidence) *
					                                      (occludedCeilingFrames - redrawIntervalMaxFrames);
					const double effectiveDelay = std::clamp(e->RedrawScore - e->LastDrawnFrame, 1.0, effectiveMaxFrames);
					e->RedrawScore = e->LastDrawnFrame + effectiveDelay;
					// Backlog clamp: bound by occludedCeilingFrames (the worst delay ANY
					// light can accumulate), not this light's current effectiveMaxFrames
					// -- trimming by the current-frame value would clip legitimately-
					// earned age the instant a light's confidence collapses.
					const double maxBacklogFrames = occludedCeilingFrames + static_cast<double>(s_settings.ShadowLightCount);
					e->RedrawScore = std::max(e->RedrawScore, static_cast<double>(now) - maxBacklogFrames);

					// Desync a tied batch: occlusionConfidence computes identically for
					// similarly-idle lights, so a group can share byte-identical
					// RedrawScore and re-sync every cycle (reads as flicker on an
					// animated flame). Applied after the backlog clamp, not before --
					// the clamp's floor would erase an earlier offset. Bounded to a
					// fraction of THIS light's own delay, never the shared ceiling, so
					// it can't invert priority against a faster light.
					const int32_t staggerCapFrames = std::max(1, static_cast<int32_t>(effectiveDelay * 0.5));
					const int32_t deadlineStagger = (e->Index * 41) % staggerCapFrames;
					e->RedrawScore -= static_cast<double>(deadlineStagger);

					// Schedulability signal: demanded redraws/frame across `pending`.
					// Compared against actual admissions/frame, this tells a tuning
					// problem (demand within capacity, so a stall means a bad
					// deadline) apart from genuine overload (demand exceeds
					// capacity, so nothing but a lower cap or bigger budget helps).
					s_schedDiag.demand_ratio += 1.0 / effectiveDelay;
				}

				// Count lights meaningfully illuminating the viewer area.
				s_highImportanceLightCount = static_cast<uint32_t>(
					std::count_if(pending.begin(), pending.end(),
						[](const LightEntry* e) { return e->lastImportance > 0.1f; }));

				// Dirty lights before clean ones (eligibility filter), RedrawScore
				// ascending within each group (deadline-style priority -- self-aging,
				// since a losing light's LastDrawnFrame stays frozen while winners'
				// advance, so it migrates toward the front on its own). Shared with
				// the Q2 union counterfactual replay below so both stay consistent.
				//
				// The last two keys make the order a pure function of the entries,
				// never of input order: the backlog clamp below floors every
				// sufficiently-old light onto one identical RedrawScore, so a group
				// revealed together (occlusionConfidence drops in the same sample
				// for all of them) arrives here exactly tied, and an unkeyed
				// comparator lets the non-stable sort churn their relative order
				// every frame. LastDrawnFrame ascending restores the age ordering
				// the clamp erased (oldest served first; a never-drawn -1 sorts
				// first, which is correct); Index ascending is unique across the
				// pool, so the comparator is a total order and no pair is ever
				// equivalent -- std::stable_sort cannot substitute for this pair of
				// keys: pending's input order is the pool scan (Index ascending)
				// while the Q2 union's input order is already-sorted pending plus
				// appended skips, so stability alone would give the two call sites
				// DIFFERENT tiebreaks off the same comparator, desyncing exactly
				// the counterfactual the comment below warns about.
				// Starvation escape hatch: a light whose occlusion confidence has
				// pinned to 1 gets RedrawScore pushed out to occludedCeilingFrames
				// (the additive term above, then the ceiling clamp) and, since it
				// is never redrawn, never earns a fresh GPU demand sample to
				// correct that confidence -- self-reinforcing, can stall
				// indefinitely even while genuinely visible next to the camera.
				// dirtyStallFrames (reset only on admission or going clean, see
				// the frame-end accounting below) is a lagging but exact count of
				// exactly that condition. A light stalled past the same ceiling
				// its own score is bounded by sorts first regardless of
				// RedrawScore, landing on the existing isFirst floor below
				// (unconditional admission, no budget/due-gate check) -- this
				// only forces through a light that was already eligible to reach
				// the occluded ceiling, never a genuinely fresh/low-priority one.
				// Floored at 1 (see the identical guard above): an unfloored
				// negative setting would also be UB at the cast to uint32_t.
				const uint32_t starvationFloorFrames = static_cast<uint32_t>(
					std::max(1.0, static_cast<double>(s_settings.RedrawIntervalMaxFrames)) * kOccludedRedrawMultiplier);
				auto schedOrder = [starvationFloorFrames](const LightEntry* a, const LightEntry* b) {
					if (a->schedDirty != b->schedDirty)
						return a->schedDirty && !b->schedDirty;
					const bool aStarved = a->dirtyStallFrames >= starvationFloorFrames;
					const bool bStarved = b->dirtyStallFrames >= starvationFloorFrames;
					if (aStarved != bStarved)
						return aStarved && !bStarved;
					if (aStarved && a->dirtyStallFrames != b->dirtyStallFrames)
						return a->dirtyStallFrames > b->dirtyStallFrames;
					if (a->RedrawScore != b->RedrawScore)
						return a->RedrawScore < b->RedrawScore;
					if (a->LastDrawnFrame != b->LastDrawnFrame)
						return a->LastDrawnFrame < b->LastDrawnFrame;
					return a->Index < b->Index;
				};
				std::sort(pending.begin(), pending.end(), schedOrder);

				// Winners latch the scoring-pass hash: lastGeomHash staleness is
				// bounded by kGeomHashRehashInterval, inside the
				// kSleepRedrawIntervalFrames backstop.
				auto latchGeomHash = [](LightEntry* e) {
					e->lastGeomHash = e->pendingGeomHash;
				};

				// Entry state of the admission loop, so the Q2 counterfactual
				// below can replay the identical algorithm on untouched copies.
				const int cfMaxRedrawStart = maxRedraw;
				const int32_t cfBudgetStart = budgetRemain;
				const bool cfIsFirstStart = isFirst;
				// A candidate refused purely for cost is still saturation: the
				// loop doesn't break on it (a cheaper later entry may still
				// fit), so maxRedraw/budgetRemain alone can read "not
				// saturated" while every remaining candidate was too
				// expensive to admit.
				bool anyCostRejected = false;

				for (auto* e : pending) {
					if (maxRedraw <= 0)
						break;
					if (budgetRemain <= 0)
						break;
					// Due-gate (devbench-only, default off -- see
					// ShadowDemandSample::redrawDueGate). Without this the budget is
					// spent unconditionally every frame -- removing a candidate
					// (a demand/sleep skip) only promotes the next-ranked one into
					// its slot instead of reducing total redraw work. isFirst stays
					// exempt, matching its unconditional admission below (the
					// starvation floor). pending is [dirty by RedrawScore asc][clean
					// by RedrawScore asc] (schedOrder above), so nothing from the
					// clean partition is ever admitted here; a dirty entry can still
					// be gated by its own RedrawScore not being due yet.
					if (s_shadowDemand.redrawDueGate && !isFirst) {
						if (!e->schedDirty)
							break;
						if (e->RedrawScore > static_cast<double>(now))
							break;
					}
					int32_t budgetEstimate = s_budget.GetCost(e->Light);
					if (isFirst) {
						if (!s_lights.Sun || e->Index > 0)
							budgetRemain -= budgetEstimate;
						maxRedraw--;
						e->RedrawFrame = true;
						if (e->LastDrawnFrame < 0)
							e->FadeStartFrame = now;
						e->LastDrawnFrame = now;
						latchGeomHash(e);
						isFirst = false;
						continue;
					}
					if (budgetEstimate <= budgetRemain) {
						budgetRemain -= budgetEstimate;
						maxRedraw--;
						e->RedrawFrame = true;
						if (e->LastDrawnFrame < 0)
							e->FadeStartFrame = now;
						e->LastDrawnFrame = now;
						latchGeomHash(e);
						continue;
					}
					anyCostRejected = true;
				}

				// Q2: what would a hard zero-demand skip actually buy? Correlating
				// admissions against demand cannot answer that -- the sort key is
				// age-dominated, so freeing a slot admits the next entry in rank
				// order, which may itself be another invisible light. Instead
				// replay the identical admission algorithm on the counterfactual
				// pool and take the set difference against what really admitted.
				// Scratch state only; nothing the scheduler reads is written.
				//
				// The counterfactual pool flips with the setting: with the skip
				// OFF, the candidates never left `pending`, so the replay REMOVES
				// them (simulates turning the skip on). With the skip ON, they
				// already left `pending` (see the `continue` above), so the
				// replay ADDS them back via `highPrioritySkips` (simulates
				// turning the skip off) -- otherwise there would be nothing left
				// to remove and the estimator would silently read zero forever.
				if (auditDemand) {
					int realAdmitted = 0;
					for (auto* e : pending)
						if (e->RedrawFrame)
							realAdmitted++;

					if (!s_settings.SkipZeroDemandRedraw) {
						int cfMaxRedraw = cfMaxRedrawStart;
						int32_t cfBudget = cfBudgetStart;
						bool cfIsFirst = cfIsFirstStart;
						int cfAdmitted = 0;

						auto admit = [&](LightEntry* e) {
							cfAdmitted++;
							if (e->RedrawFrame)
								return;
							// Admitted only in the counterfactual: the lights the skip
							// would buy. Their demand decides whether that is a real
							// quality win or just another invisible light taking the slot.
							s_schedDiag.demand_swap_in++;
							const int32_t slot = DemandSlotFor(*e);
							if (slot < 0 || s_shadowDemand.maxLatest[slot] > kDemandUntouchedMaxRaw)
								s_schedDiag.demand_swap_in_above_eps++;
						};

						for (auto* e : pending) {
							if (DemandSkipCandidate(*e))
								continue;
							if (cfMaxRedraw <= 0 || cfBudget <= 0)
								break;
							const int32_t cost = s_budget.GetCost(e->Light);
							if (cfIsFirst) {
								if (!s_lights.Sun || e->Index > 0)
									cfBudget -= cost;
								cfMaxRedraw--;
								cfIsFirst = false;
								admit(e);
								continue;
							}
							if (cost <= cfBudget) {
								cfBudget -= cost;
								cfMaxRedraw--;
								admit(e);
							}
						}
						// Positive only when the candidate set drops below budget; in
						// the saturated regime the budget is simply reallocated and
						// this stays at zero by design.
						s_schedDiag.demand_redraws_saved = realAdmitted - cfAdmitted;
						s_demandSwapInTotal.fetch_add(
							static_cast<uint64_t>(s_schedDiag.demand_swap_in), std::memory_order_relaxed);
					} else {
						int cfMaxRedraw = cfMaxRedrawStart;
						int32_t cfBudget = cfBudgetStart;
						bool cfIsFirst = cfIsFirstStart;
						int cfAdmittedUnion = 0;

						// pending already excludes the real skips; merge them back
						// in by RedrawScore so the replay sees the same rank order
						// the real loop would have without the skip.
						static std::vector<LightEntry*> s_cfUnion;
						s_cfUnion.clear();
						s_cfUnion.reserve(pending.size() + highPrioritySkips.size());
						s_cfUnion.insert(s_cfUnion.end(), pending.begin(), pending.end());
						for (const auto& skip : highPrioritySkips)
							s_cfUnion.push_back(skip.entry);
						// Same ordering the real sort above uses (schedOrder) -- a
						// mismatched comparator here would desync this counterfactual
						// from what the live scheduler actually did.
						std::sort(s_cfUnion.begin(), s_cfUnion.end(), schedOrder);

						for (auto* e : s_cfUnion) {
							if (cfMaxRedraw <= 0 || cfBudget <= 0)
								break;
							const int32_t cost = s_budget.GetCost(e->Light);
							if (cfIsFirst) {
								if (!s_lights.Sun || e->Index > 0)
									cfBudget -= cost;
								cfMaxRedraw--;
								cfIsFirst = false;
								cfAdmittedUnion++;
								continue;
							}
							if (cost <= cfBudget) {
								cfBudget -= cost;
								cfMaxRedraw--;
								cfAdmittedUnion++;
							}
						}
						// Positive = redraws the live skip actually prevented this
						// frame (the union replay admits more than really ran).
						// Zero in the saturated regime, same as the other arm --
						// there the union's extra candidates just get refused too.
						s_schedDiag.demand_redraws_saved = cfAdmittedUnion - realAdmitted;
					}
					if (s_schedDiag.demand_redraws_saved > 0)
						s_demandRedrawsSavedTotal.fetch_add(
							static_cast<uint64_t>(s_schedDiag.demand_redraws_saved), std::memory_order_relaxed);
				}
				// Which regime the frame ran in, and the running eligibility
				// ceiling. Both are meaningful whether or not the replay ran.
				// A candidate refused purely for cost (anyCostRejected) counts as
				// saturated even when maxRedraw/budgetRemain didn't bottom out --
				// see the loop above.
				if (auditDemand) {
					s_schedDiag.demand_budget_saturated = maxRedraw <= 0 || budgetRemain <= 0 || anyCostRejected;
					s_demandSkipEligibleTotal.fetch_add(
						static_cast<uint64_t>(s_schedDiag.demand_skip_eligible), std::memory_order_relaxed);
				}
			}
		}

		// Count how many shadow lights are scheduled to redraw this frame, and
		// update the stop-motion stall counters. Deliberately outside the
		// maxRedraw>0/budgetRemain>0 guard above (unconditional every frame) --
		// a frame where the scheduler bails entirely is exactly the worst case
		// for a stall, and it must still be counted.
		// Iterate the point-light range (sun-aware: skips pool[0] when Sun=true).
		s_redrawnLightsThisFrame = 0;
		s_schedDiag.stall_max = 0;
		s_schedDiag.stall_over_threshold = 0;
		s_schedDiag.stall_worst_slot = -1;
		for (int j = s_lights.PointLightFirst(); j < s_lights.PointLightEnd(s_settings.ShadowLightCount); j++) {
			auto& e = s_lights.Lights[j];
			if (e.RedrawFrame)
				++s_redrawnLightsThisFrame;
			// Reset on admission or on going clean; frozen (untouched) while
			// skippedThisFrame -- see LightEntry::dirtyStallFrames.
			if (!e.Light || e.RedrawFrame || !e.schedDirty)
				e.dirtyStallFrames = 0;
			else if (!e.skippedThisFrame && e.dirtyStallFrames < 0xFFFFu)
				++e.dirtyStallFrames;
			if (static_cast<int>(e.dirtyStallFrames) > s_schedDiag.stall_max) {
				s_schedDiag.stall_max = e.dirtyStallFrames;
				s_schedDiag.stall_worst_slot = j;
			}
			if (e.dirtyStallFrames >= kStallReportThreshold)
				s_schedDiag.stall_over_threshold++;
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
						// Re-validate first: a concurrent free could recycle c.light, and a
						// lodDimmer store would corrupt the new occupant's vtable -> CTD
						// (heldRefs should cover it).
						if (SafeUsable(isUsableLight, c.light))
							RestoreZeroedLodDimmer(c.light);
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
							s_cpuEnableUs.fetch_add(TimeUs([&] { stillUsable = SafeEnableAndValidate(e, camera, ssn, slot, isUsableLight); }), std::memory_order_relaxed);
							s_cpuEnableN.fetch_add(1, std::memory_order_relaxed);
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
			// Read + reset the per-frame caster-cull count here (unconditionally,
			// not inside TracyPlot's arg -- that expression is elided in non-Tracy
			// builds, which would leak the counter).
			// [[maybe_unused]]: consumed only by TracyPlot below, which is elided
			// in non-Tracy builds -- but the exchange must still run to reset the
			// counters, so they're read here unconditionally.
			const uint32_t culledThisFrame = s_casterCullCount.exchange(0, std::memory_order_relaxed);
			if (culledThisFrame)
				s_casterCullTotal.fetch_add(culledThisFrame, std::memory_order_relaxed);
			const uint32_t poolDropsThisFrame = s_cullPoolDropCount.exchange(0, std::memory_order_relaxed);
			if (poolDropsThisFrame)
				s_cullPoolDropTotal.fetch_add(poolDropsThisFrame, std::memory_order_relaxed);
			[[maybe_unused]] const uint32_t staticDraws = s_staticCasterDraws.exchange(0, std::memory_order_relaxed);
			[[maybe_unused]] const uint32_t dynamicDraws = s_dynamicCasterDraws.exchange(0, std::memory_order_relaxed);
			// Accumulated (not just plotted): the snapshot publishes the running
			// total so a headless A/B can difference it across a run.
			const uint32_t bakesThisFrame = s_staticBakeCount.exchange(0, std::memory_order_relaxed);
			if (bakesThisFrame)
				s_staticBakeTotal.fetch_add(bakesThisFrame, std::memory_order_relaxed);

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
				const auto& slotInfos = GetSlotInfos();
				for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++) {
					const auto& e = s_lights.Lights[i];
					if (!e.Light)
						continue;
					SchedSnapshot::SlotState st;
					st.index = i;
					st.light = reinterpret_cast<uintptr_t>(e.Light);
					st.importance = e.lastImportance;
					st.score = e.lastScore;
					st.desiredScale = e.desiredScale;
					st.budgetScale = e.budgetScale;
					st.pendingScale = e.pendingScale;
					st.renderedScale = e.renderedScale;
					st.redrawnThisFrame = e.RedrawFrame;
					st.schedDirty = e.schedDirty;
					st.dirtyStallFrames = e.dirtyStallFrames;
					st.redrawScore = e.RedrawScore;
					st.lastDrawnFrame = e.LastDrawnFrame;
					st.cameraHold = s_cameraHold.count(e.Light) > 0;
					st.geomListSize = static_cast<uint32_t>(e.Light->geomList.size());
					AtlasTileTexels tile{};
					if (GetSlotTileTexels(i, tile)) {
						st.tileX = tile.x;
						st.tileY = tile.y;
						st.tileSize = tile.size;
						st.tileContentValid = tile.contentValid;
					}
					{
						uint64_t staticHash = 0;
						GetSlotStaticState(i, staticHash, st.staticValid, &st.staticEmpty);
					}
					if (static_cast<size_t>(i) < slotInfos.size()) {
						const auto& rec = slotInfos[i];
						st.uploadRecorded = rec.valid;
						st.uploadParamY = rec.paramY;
						st.uploadRange = rec.range;
					}
					st.suppressed = IsSuppressed(reinterpret_cast<uintptr_t>(e.Light));
					if (auto* ni = e.Light->light.get()) {
						std::scoped_lock convLock(s_shadowConvertMutex);
						st.promoted = s_shadowConvert.count(ni) > 0;
					}
					snap.slots.push_back(st);
				}
				snap.baseTileTexels = s_initialShadowMapResolution > 0 ?
				                          static_cast<float>(s_initialShadowMapResolution) :
				                          2048.0f;
				snap.atlasDim = AtlasDim();
				snap.atlasCapacityCells = AtlasCapacityCells();
				snap.atlasOccupancy = AtlasOccupancy();
				snap.atlasVramBytes = AtlasVRAMBytes();
				const auto clearStats = GetAtlasClearStats();
				snap.atlasTileReallocs = clearStats.tileReallocs;
				snap.atlasOwnerInvalidations = clearStats.ownerInvalidations;
				snap.atlasAllocDenied = clearStats.allocDenied;
				snap.cullPoolDropsTotal = s_cullPoolDropTotal.load(std::memory_order_relaxed);
				snap.casterCullDropsTotal = s_casterCullTotal.load(std::memory_order_relaxed);
				if (const uint32_t n = s_cpuAccumN.exchange(0, std::memory_order_relaxed))
					snap.cpuAccumUsAvg = static_cast<uint32_t>(s_cpuAccumUs.exchange(0, std::memory_order_relaxed) / n);
				if (const uint32_t n = s_cpuSubmitN.exchange(0, std::memory_order_relaxed))
					snap.cpuSubmitUsAvg = static_cast<uint32_t>(s_cpuSubmitUs.exchange(0, std::memory_order_relaxed) / n);
				if (const uint32_t n = s_cpuEnableN.exchange(0, std::memory_order_relaxed))
					snap.cpuEnableUsAvg = static_cast<uint32_t>(s_cpuEnableUs.exchange(0, std::memory_order_relaxed) / n);
				snap.avgLightCostUs = s_budget.GetAverageCostUs();
				snap.avgRedrawsPerFrame = static_cast<float>(s_redrawSum) / static_cast<float>(kRedrawHistorySize);
				snap.staticBakesTotal = s_staticBakeTotal.load(std::memory_order_relaxed);
				snap.cellResetsTotal = s_cellResetTotal.load(std::memory_order_relaxed);
				snap.sleepSkips = s_schedDiag.sleep_skips;
				snap.sleepSkipsTotal = s_sleepSkipTotal.load(std::memory_order_relaxed);
				snap.demandSkips = s_schedDiag.demand_skips;
				snap.demandSkipsTotal = s_demandSkipTotal.load(std::memory_order_relaxed);
				snap.alphaGroupPeak = s_alphaGroupPeak.load(std::memory_order_relaxed);
				snap.alphaGroupDrops = s_alphaGroupDrops.load(std::memory_order_relaxed);
				snap.frustumAuditCandidates = s_schedDiag.frustum_audit_candidates;
				snap.frustumAuditKeptOut = s_schedDiag.frustum_audit_kept_out;
				snap.frustumAuditSuspects = s_schedDiag.frustum_audit_suspects;
				snap.demandSlotted = s_schedDiag.demand_slotted;
				snap.demandZero = s_schedDiag.demand_zero;
				snap.demandSubTap = s_schedDiag.demand_sub_tap;
				snap.demandSkipEligible = s_schedDiag.demand_skip_eligible;
				snap.demandSwapIn = s_schedDiag.demand_swap_in;
				snap.demandSwapInAboveEps = s_schedDiag.demand_swap_in_above_eps;
				snap.demandRedrawsSaved = s_schedDiag.demand_redraws_saved;
				snap.demandBudgetSaturated = s_schedDiag.demand_budget_saturated;
				// Phase-1's demand penalty already does part of Stage B's job via
				// the sort, so every Q2 reading is only interpretable alongside
				// the config it was taken under.
				snap.demandPhase1Enabled = true;
				snap.demandSkipActive = s_settings.SkipZeroDemandRedraw;
				snap.demandSkipEligibleTotal = s_demandSkipEligibleTotal.load(std::memory_order_relaxed);
				snap.demandSwapInTotal = s_demandSwapInTotal.load(std::memory_order_relaxed);
				snap.demandRedrawsSavedTotal = s_demandRedrawsSavedTotal.load(std::memory_order_relaxed);
				snap.stallMax = s_schedDiag.stall_max;
				snap.stallWorstSlot = s_schedDiag.stall_worst_slot;
				snap.demandRatio = s_schedDiag.demand_ratio;
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
			TracyPlot("scm.sleep_skips", (int64_t)s_schedDiag.sleep_skips);
			TracyPlot("scm.demand_skips", (int64_t)s_schedDiag.demand_skips);
			TracyPlot("scm.alpha_groups", (int64_t)s_alphaGroupPeak.load(std::memory_order_relaxed));
			TracyPlot("scm.demand.skip_eligible", (int64_t)s_schedDiag.demand_skip_eligible);
			TracyPlot("scm.demand.swap_in", (int64_t)s_schedDiag.demand_swap_in);
			TracyPlot("scm.demand.swap_in_above_eps", (int64_t)s_schedDiag.demand_swap_in_above_eps);
			TracyPlot("scm.demand.redraws_saved", (int64_t)s_schedDiag.demand_redraws_saved);
			TracyPlot("scm.demand.zero", (int64_t)s_schedDiag.demand_zero);
			TracyPlot("scm.demand.sub_tap", (int64_t)s_schedDiag.demand_sub_tap);
			TracyPlot("scm.frustum_audit.suspects", (int64_t)s_schedDiag.frustum_audit_suspects);
			TracyPlot("scm.redraw.stall_max", (int64_t)s_schedDiag.stall_max);
			TracyPlot("scm.redraw.stall_over", (int64_t)s_schedDiag.stall_over_threshold);
			TracyPlot("scm.redraw.demand_ratio", (double)s_schedDiag.demand_ratio);

			// Live config plots — record the *current* settings on each frame so
			// a single capture spanning a settings change captures both sides.
			TracyPlot("cfg.ShadowLightCount", (int64_t)s_settings.ShadowLightCount);
			TracyPlot("cfg.MaxRedrawPerFrame", (int64_t)s_settings.MaxRedrawPerFrame);
			TracyPlot("cfg.ConvertExcessToNormal", (int64_t)(s_settings.ConvertExcessToNormal ? 1 : 0));
			TracyPlot("cfg.Enabled", (int64_t)(s_settings.Enabled ? 1 : 0));
			TracyPlot("cfg.RedrawBudgetMs", (double)s_settings.RedrawBudgetMs);
			TracyPlot("cfg.CasterCullAngularMin", (double)s_settings.CasterCullAngularMin);
			TracyPlot("scm.casters_culled", (int64_t)culledThisFrame);
			TracyPlot("scm.cull_pool_drops", (int64_t)poolDropsThisFrame);
			TracyPlot("scm.casters_static", (int64_t)staticDraws);
			TracyPlot("scm.casters_dynamic", (int64_t)dynamicDraws);
			TracyPlot("scm.static_bakes", (int64_t)bakesThisFrame);

			if (s_shadowDemand.instrumentation)
				EmitDemandAuditLog(*globals::game::frameCounter);
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

		// Exclusive against ShadowCasterManager::Update's pool resize; see the
		// matching lock in ScheduleShadowCasters for why this must cover the
		// whole pass, not just the initial s_lights.Lights read.
		std::shared_lock poolLock(s_lightsPoolMutex);

		// Reader side of the teardown serialization: ClearLightArrays must not
		// free lights/passes while this pass iterates them. Counter (not a
		// shared_mutex) so nested engine re-entry on this thread stays defined;
		// skip the pass outright when a teardown is already waiting.
		if (s_teardownWaiting.load(std::memory_order_acquire))
			return;
		s_shadowFlushReaders.fetch_add(1, std::memory_order_acq_rel);
		struct FlushReaderGuard
		{
			~FlushReaderGuard() { s_shadowFlushReaders.fetch_sub(1, std::memory_order_acq_rel); }
		} flushReaderGuard;
		if (s_teardownWaiting.load(std::memory_order_acquire))
			return;  // teardown won the race between our check and increment

		// Atlas resource creation happens here at the pass boundary so
		// readiness cannot flip between draws of the same frame.
		UpdateAtlas();

		// Atlas rank budget: in importance order, each light gets the biggest
		// class that still leaves a quarter cell for every lower-ranked
		// light; without it, first arrivals hoard full tiles and later
		// lights get no tile at all.
		if (AtlasActive()) {
			static std::vector<LightEntry*> ranked;
			ranked.clear();
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++)
				if (s_lights.Lights[i].Light && !s_cameraHold.count(s_lights.Lights[i].Light))
					// A held light (camera-invalid but keeping its slot, see
					// s_cameraHold above) is excluded from `pending`/the render
					// loop this frame, so it never calls EnsureSlotTile and its
					// budgetScale is moot -- but it was still consuming a
					// `remaining` slot and `cellsLeft` cells here, silently
					// shrinking every active light's headroom for as long as it
					// stays held (up to kCameraExitStreak frames). Most visible
					// right after a zone transition, where multiple old-zone
					// lights go held at once while the new zone's lights are
					// trying to grow.
					ranked.push_back(&s_lights.Lights[i]);
			// Two-key rank: geometry picks the class band, priority orders
			// within it; score jitter can never reorder across bands, which
			// keeps tile assignments cache-stable.
			std::sort(ranked.begin(), ranked.end(),
				[](const LightEntry* a, const LightEntry* b) {
					if (a->desiredScale != b->desiredScale)
						return a->desiredScale > b->desiredScale;
					return a->lastScore > b->lastScore;
				});
			uint32_t cellsLeft = AtlasCapacityCells();
			uint32_t remaining = static_cast<uint32_t>(ranked.size());
			for (auto* e : ranked) {
				remaining--;
				float scale = e->desiredScale;
				while (scale > kTileScaleFloor &&
					   (cellsLeft < CellsForScale(scale) || cellsLeft - CellsForScale(scale) < remaining))
					scale *= 0.5f;
				e->budgetScale = scale;
				// No demotion hold: holding pendingScale above the budget
				// target across frames reproducibly crashes the engine batch
				// renderer (either cell-accounting variant); root-cause before
				// reintroducing (reproducer: Dragonsreach recorded replay).
				//
				// Promotion hold is the opposite direction and safe: adopt a
				// LARGER class only after it stays wanted ~60 frames. Flicker
				// jitter otherwise oscillates the class across a boundary, and at
				// high occupancy each promotion realloc darkens a reclaim victim.
				//
				// The streak itself tracks geometric desire (desiredScale vs
				// pendingScale) only, NOT the budget-clamped `scale`/`target`
				// below -- budgetScale recomputes from the whole live pool every
				// frame, so a single frame of pool churn (any light entering or
				// leaving, unrelated to this one) used to reset the count to 0
				// even though nothing about THIS light's own geometry changed.
				// A zone transition is sustained churn, so the streak
				// realistically never matured there and whatever class a light
				// latched during the churn window became permanent. Leaky
				// (decrement, not reset-to-zero) so one isolated non-growing
				// frame doesn't erase an otherwise-continuous accumulation.
				if (e->desiredScale > e->pendingScale) {
					if (e->promoteStreak < kPromoteStreakFrames)
						e->promoteStreak++;
				} else if (e->promoteStreak > 0) {
					e->promoteStreak--;
				}
				float target = std::min(e->desiredScale, scale);
				if (target > e->pendingScale && e->promoteStreak < kPromoteStreakFrames)
					target = e->pendingScale;  // not enough sustained desire yet
				e->pendingScale = target;
				cellsLeft -= std::min(cellsLeft, CellsForScale(scale));
			}
		}

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
		s_budget.BeginRenderBatch();

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
				bool keepPriorContent = false;
				bool emptyRender = false;
				if (AtlasActive()) {
					// Tile before raster: (re)size for the pending class and
					// rect-clear once per redraw -- the paraboloid halves share
					// the tile, so clearing inside the cascade would wipe the
					// first half.
					if (!EnsureSlotTile(i, e.pendingScale)) {
						// Atlas exhausted even at the quarter class: the raster must
						// still run. EnableLight already registered this light's passes,
						// and an unconsumed group's passes are freed-but-not-unlinked at
						// frame end; a recycled pass re-registered by RegisterPassSorted
						// closes passGroupNext into a ring (RenderBatches never returns).
						// The viewport hook collapses a tile-less raster to zero size,
						// so consuming the group draws nothing. No tile marks are set,
						// so RedrawFrame keeps the retry.
						s_budget.BeginLight(e.Light, 1);
						s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);
						s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
						s_budget.EndLight(e.Light, 1);
						// This frame bailed before running the bake EnableLight
						// already armed -- restore bakeQueued so it retries next
						// redraw instead of being silently lost.
						if (auto it = s_splitState.find(e.Light); it != s_splitState.end() && it->second.bakeThisFrame)
							it->second.bakeQueued = true;
						continue;
					}
					// Split cache: the light's single accumulate this frame was
					// filtered in EnableLight. On a bake frame render the static
					// subset into the cache atlas; otherwise copy the cache into
					// the tile and composite the movers over it (no clear -- the
					// copy is the clear). Falls through to the full pass until the
					// static atlas is ready (the first frames after atlas creation),
					// or once EnableLight has latched fullThisFrame/splitExcluded
					// for this light (jitter/pose-drift storm, or a deferred bake
					// with no valid seed) -- s_splitState[light] still exists in
					// that case, so this check must key on those flags, not on
					// whether the entry is present.
					if (StaticAtlasReady() &&
						!s_splitState[e.Light].splitExcluded && !s_splitState[e.Light].fullThisFrame) {
						SplitState& st = s_splitState[e.Light];
						if (st.bakeThisFrame) {
							ZoneNamedN(zBake, "SCM::Render::StaticBake", true);
							s_staticPassActive.store(true, std::memory_order_relaxed);
							ClearStaticSlotTile(i);
							s_budget.BeginLight(e.Light, 1);
							s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);
							s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
							s_budget.EndLight(e.Light, 1);
							s_staticPassActive.store(false, std::memory_order_relaxed);
							MarkSlotStaticRendered(i, st.pendingHash, st.bakeSawStatic);  // atlas slot = source of truth
							st.bakeThisFrame = false;
							// Keep last frame's complete live content on bake
							// frames -- copying the fresh static-only bake in
							// flashed a mover-less (wrong-looking) shadow for one
							// frame. Only a tile with no valid content (fresh
							// alloc) takes the copy, where static-only beats
							// garbage depths.
							AtlasTileTexels bakeTile{};
							const bool liveHasContent = GetSlotTileTexels(i, bakeTile) && bakeTile.contentValid;
							// A bake that captured nothing is a blank rect: seeding a
							// fresh tile from it would advertise a flat tile as content.
							if (!liveHasContent && st.bakeSawStatic)
								CopyStaticTileToLive(i);
							if (liveHasContent || st.bakeSawStatic) {
								e.renderedScale = e.pendingScale;
								// Live content unchanged this frame: never swap a
								// staged promotion in on a bake.
								MarkSlotTileRendered(i, false);
							}
							continue;
						}
						{
							// Re-check bake validity AT COMPOSITE TIME: mode selection
							// (phase A) ran before the per-frame owner reconciliation,
							// so a reassigned slot can reach here holding the PREVIOUS
							// light's bake -- compositing it displayed a different
							// light's shadow. Movers-only over a clear for one frame
							// beats that; the queued bake heals it next redraw.
							uint64_t compositeHash = 0;
							bool compositeValid = false;
							bool compositeEmpty = false;
							GetSlotStaticState(i, compositeHash, compositeValid, &compositeEmpty);
							// Real content = a non-blank static seed, or movers this
							// frame. Neither means this frame draws a flat tile:
							// mirror the full-pass empty guard below and hold the
							// prior content instead of advertising the flat one.
							const bool composedContent =
								(compositeValid && !compositeEmpty) || st.sawDynamicLastAccum;
							AtlasTileTexels liveTile{};
							const bool compositeKeepPrior = !composedContent &&
							                                GetSlotTileTexels(i, liveTile) && liveTile.contentValid;
							if (compositeValid) {
								if (!compositeKeepPrior)
									CopyStaticTileToLive(i);  // seed the tile with cached static depth
							} else {
								if (!compositeKeepPrior)
									ClearSlotTile(i);
								st.bakeQueued = true;
							}
							s_budget.BeginLight(e.Light, 1);
							s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);  // composite movers on top (no clear)
							s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
							s_budget.EndLight(e.Light, 1);
							// A movers-only frame (invalid seed) must not swap a
							// staged promotion in: keep sampling the old complete
							// tile until a seeded composite or full render lands.
							if (!compositeKeepPrior && composedContent) {
								e.renderedScale = e.pendingScale;
								MarkSlotTileRendered(i, compositeValid);
							}
							continue;
						}
					}
					// Empty-render guard (full-pass path): a redraw whose accumulate
					// produced no casters -- e.g. a transient cull under redraw-budget
					// churn -- would clear the tile and mark the empty result valid,
					// degenerating a good shadow into a flat (wrong) tile. Mirror the
					// bake-path guard above: keep the prior content by skipping the
					// clear and the re-mark. The render MUST still run to consume the
					// passes EnableLight registered (skipping it closes passGroupNext
					// into a ring); with an empty geomList it draws nothing, so the
					// held content survives. A tile with no valid content yet (fresh
					// alloc / staged realloc reads contentValid=false) is not held --
					// it clears and renders normally.
					emptyRender = e.Light->geomList.empty();
					AtlasTileTexels held{};
					keepPriorContent = emptyRender &&
					                   GetSlotTileTexels(i, held) && held.contentValid;
					if (!keepPriorContent)
						ClearSlotTile(i);
				}
				s_budget.BeginLight(e.Light, 1);
				s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);
				s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
				s_budget.EndLight(e.Light, 1);
				// Commit the content scale only after the raster actually ran:
				// a skipped render must keep advertising the scale the slot
				// still holds, or shaders sample tile UVs against full-slice
				// content until the geometry hash happens to change.
				if (!keepPriorContent) {
					e.renderedScale = e.pendingScale;
					// Never mark an EMPTY render valid: a starved or transiently
					// culled fresh tile stays contentValid=false, so the sample side
					// skips it and the light reads UNSHADOWED -- never a degenerate
					// (cleared) tile. A high-pressure scene thus cannot show one.
					if (!emptyRender)
						MarkSlotTileRendered(i);
				}
			}
		}

		ServiceShadowFrameRecord();
		s_budget.EndRenderBatch();

		if (globals::game::isVR)
			*globals::game::drawStereo = savedStereo;
	}
}
