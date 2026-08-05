// ShadowCasterInternal.h
// Shared internal state for ShadowCasterManager implementation.

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "ShadowCasterManager.h"
#include "Utils/BootSnapshot.h"

namespace ShadowCasterManager
{
	/// Prunes map if size exceeds cap to bound memory consumption.
	template <class Map>
	inline void PruneIfOversized(Map& map, size_t cap)
	{
		if (map.size() > cap)
			map.clear();
	}

	// ---------------------------------------------------------------------
	// Core scheduler state (definitions in ShadowCasterManager.cpp)
	// -------------------------------------------------------------------------

	extern Settings s_settings;
	extern LightContainer s_lights;
	extern BudgetTracker s_budget;

	// External conflict detection
	extern bool s_externalConflict;
	extern std::string s_conflictMessage;

	// Per-frame slot count claimed by engine focus shadow renderer.
	extern int s_focusShadowSlots;

	// Width of vanilla shadow-caster bitmasks.
	inline constexpr uint32_t kShadowMaskBits = 32u;

	// Rolling redraw / budget history window for UI statistics.
	inline constexpr int kRedrawHistorySize = 128;
	extern int32_t s_redrawHistory[kRedrawHistorySize];
	extern int32_t s_redrawHistoryPos;
	extern int32_t s_redrawSum;
	extern int32_t s_budgetHistory[kRedrawHistorySize];
	extern int32_t s_budgetHistoryPos;
	extern int64_t s_budgetSum;

	// Frame-time tracking used by formula parameters and UI stats.
	inline constexpr int kFrameWindow = 120;
	extern float s_ftRing[kFrameWindow];
	extern int s_ftHead;
	extern int s_ftCount;
	extern float s_ftEMA;
	extern int s_stableFrames;
	extern float s_autoBudgetMs;

	// Headroom thresholds for shared frame-state diagnostic.
	inline constexpr float kFrameHeadroomDeadZoneMs = 0.3f;
	inline constexpr float kFrameHeadroomSafetyMs = 0.5f;

	// Budget tracking for UI display
	extern int32_t s_redrawnLightsThisFrame;
	extern int32_t s_totalShadowLightsThisFrame;
	extern uint32_t s_highImportanceLightCount;
	extern float s_redrawnLightsSmoothed;

	// Per-slot GPU screen-visibility demand (see LightLimitFix::shadowDemandEMA),
	// pushed in once per frame via SetShadowDemand.
	extern std::array<float, kMaxShadowDemandSlots> s_shadowDemandEMA;
	extern bool s_shadowDemandEMAInitialized;

	// Phase-2 half of the same publication: raw per-slot tile maxima plus the
	// per-sample validity metadata the consecutive-sample streak needs.
	extern ShadowDemandSample s_shadowDemand;

	// Raw accumulator units (1024 == 1.0 demand) at or below which a slot counts
	// as untouched. demandWeight is luminance*fade*atten, so this is a floor on
	// attenuation once brightness is accounted for, not an absolute brightness
	// bar: a light dimmer than ~0.5 demand at atten=1 could never clear a
	// higher floor regardless of visibility (the bug this constant fixes,
	// found from a live candle/sunbeam light class that never cleared 512).
	// 16 keeps the residual brightness coupling to a narrow band of the
	// light's own radius across the plausible luminance range.
	inline constexpr uint32_t kDemandUntouchedMaxRaw = 16;

	// Consecutive distinct below-floor samples before a light counts as absent.
	// One jittered tap per 64x64 tile per sample means a small lit footprint is
	// hit only every few dozen samples; this window must dwarf that gap or
	// visible flame-class lights flip in and out of the skip between hits.
	// A live same-session A/B (SE, outdoor brazier scene) tried shrinking this
	// to 90 paired with kDemandTapCount=4 spatial taps/frame (a binomial model
	// predicted equal-or-better false-skip safety at K*N=360 vs today's 240):
	// measured WORSE flicker at every combination tried (240: 63 warnings/60s;
	// 90 K=1: 290; 90 K=4: 626) -- more taps made a flame's real per-frame
	// flicker amplitude more visible to the sampler, not less, which the
	// binomial model (built for spatial sampling noise, not temporal signal
	// variation) didn't account for. 240 is the empirically-validated value;
	// do not shrink it without new live evidence.
	inline constexpr uint32_t kZeroDemandSkipStreak = 240;

	// Distinct below-floor samples earning full occluded-ceiling stretch (see
	// occlusionConfidence, ShadowScheduler.cpp). Half the hard skip's streak
	// above because the stretch is a weaker claim: it only delays a redraw to
	// the occluded ceiling, where the hard skip suppresses it outright for
	// kZeroDemandRedrawIntervalFrames. Derived from kZeroDemandSkipStreak
	// rather than tuned separately so a devbench override of one moves both.
	inline constexpr uint32_t kOccludedStretchStreakDivisor = 2;

	// Occluded redraw ceiling = RedrawIntervalMaxFrames * this multiplier, not
	// an independent user-facing setting: the two moved in lockstep at their
	// old defaults (20 / 120), so exposing both was one slider users had no
	// real reason to tune separately from the other.
	inline constexpr float kOccludedRedrawMultiplier = 6.0f;

	// Drains older than this are stale: without the gate a wedged readback would
	// freeze the snapshot and let every light in it look permanently absent.
	inline constexpr uint64_t kDemandStaleFrames = 8;

	// Entries in the engine's global BSBatchRenderer alpha GeometryGroup array,
	// the ceiling Hook_StartGroupingAlphas enforces. The array is bump-allocated
	// with no capacity check, and its element count is a literal in the binary's
	// own array constructor -- VR was built with twice the slots, so this is a
	// genuine runtime difference and not a divergence worth unifying away.
	inline constexpr uint32_t kAlphaGeometryGroupCapacityFlat = 512;
	inline constexpr uint32_t kAlphaGeometryGroupCapacityVR = 1024;

	// Slots held back from the ceiling. The engine claims entries with LOCK XADD,
	// so between the guard's read and that increment every other worker already
	// past the read can still claim one: the reserve must therefore exceed the
	// number of threads that can be inside the function at once, not merely be
	// "some margin". Sized well above any plausible core count for that reason,
	// which makes it independent of the capacity it is subtracted from.
	inline constexpr uint32_t kAlphaGeometryGroupReserve = 64;

	// High-water alpha GeometryGroup count since load, and grouping requests
	// refused at the ceiling. A peak well under the capacity means no scene came
	// near the array; any drop means one reached it.
	extern std::atomic<uint32_t> s_alphaGroupPeak;
	extern std::atomic<uint64_t> s_alphaGroupDrops;

	/// Diagnostic counters reset each scheduler frame for Tracy profiler reporting.
	struct SchedDiagCounters
	{
		int candidates_total = 0;
		int candidates_chosen = 0;
		int candidates_excess = 0;
		int candidates_invalid_camera = 0;
		int candidates_invalid_portal = 0;
		int candidates_invalid_frustum = 0;
		int candidates_invalid_lod = 0;
		int candidates_invalid_other = 0;
		int converted_invalid = 0;
		int converted_excess = 0;
		int disabled_invalid = 0;
		int disabled_excess = 0;
		int reconciliation_clears = 0;
		int slots_in_use = 0;
		int first_render_skips = 0;
		int sleep_skips = 0;
		int demand_skips = 0;

		// Stage-A zero-demand-skip audit; see SchedSnapshot for the field meanings.
		int frustum_audit_candidates = 0;
		int frustum_audit_kept_out = 0;
		int frustum_audit_suspects = 0;
		int demand_slotted = 0;
		int demand_zero = 0;
		int demand_sub_tap = 0;
		int demand_skip_eligible = 0;
		int demand_swap_in = 0;
		int demand_swap_in_above_eps = 0;
		int demand_redraws_saved = 0;
		bool demand_budget_saturated = false;

		// Stop-motion metric (see LightEntry::dirtyStallFrames). stall_max is
		// this frame's pool-wide worst streak; demand_ratio is the demanded
		// redraws/frame across `pending`, for comparing against admission
		// capacity to tell a tuning problem from genuine overload.
		int stall_max = 0;
		int stall_over_threshold = 0;
		int stall_worst_slot = -1;
		double demand_ratio = 0.0;
	};
	extern SchedDiagCounters s_schedDiag;

	// Maximum ShadowLightCount supported by installed infrastructure.
	extern int32_t s_installedShadowLightCount;

	// Requested slot count passed to engine texture allocation.
	extern uint32_t s_requestedSlotCount;

	// Total kSHADOWMAPS texture-array capacity as actually allocated.
	extern uint32_t s_installedSlotCount;

	// True once verification result has been logged.
	extern bool s_slotCountLogged;

	// Active formula instances
	extern std::unique_ptr<FormulaHelper> s_formulaScore;
	extern std::unique_ptr<FormulaHelper> s_formulaRedrawInterval;
	extern std::unique_ptr<FormulaHelper> s_formulaRedrawBudget;

	// Lights converted to normal (non-shadow) lights for diffuse rendering
	struct ConvertedLight
	{
		RE::BSShadowLight* light;
		bool isNS;
	};
	extern std::vector<ConvertedLight> s_normalConvert;
	extern std::set<RE::NiLight*> s_shadowConvert;
	extern std::set<RE::NiLight*> s_shadowConvertDescriptorInited;
	extern std::mutex s_shadowConvertMutex;

	// Set on engine bulk teardown to signal pending reset.
	extern std::atomic<bool> s_pendingSessionReset;

	// Set on ShadowSceneNode::ResetScene (cell-grid shift). A surviving
	// light's static-bake cache is keyed on caster geometry identity, which
	// a cell swap can silently recycle -- drop it explicitly on this signal.
	extern std::atomic<bool> s_pendingCellReset;

	// Protects portalGraph reads against scene transition resets.
	extern std::shared_mutex s_portalGraphMutex;

	// Synchronizes engine teardown with active shadow render passes.
	extern std::atomic<int> s_shadowFlushReaders;
	extern std::atomic<bool> s_teardownWaiting;

	// User light suppression set.
	extern std::unordered_set<uintptr_t> s_suppressedLights;

	// Manual debugging overrides
	extern std::unordered_set<uintptr_t> s_pinShadow;
	extern std::unordered_set<uintptr_t> s_pinConvert;
	extern uintptr_t s_soloLight;
	extern uintptr_t s_hoverLightKey;

	// ---------------------------------------------------------------------
	// Formula module (ShadowFormula.cpp)
	// ---------------------------------------------------------------------

	struct FormulaVarInfo
	{
		const char* name;
		const char* description;
		int32_t index;
	};

	// Authoritative list of formula variables for symbol registration and UI.
	inline constexpr FormulaVarInfo kFormulaVars[] = {
		{ "lightindex", "sequential index of this candidate light", kFormulaParam_LightIndex },
		{ "lightintensity", "NiLight fade/intensity", kFormulaParam_LightIntensity },
		{ "lightdistance", "camera-to-light distance (game units; 1 unit ~= 1.428 cm)", kFormulaParam_LightDistance },
		{ "lightradius", "light radius/range (game units; 1 unit ~= 1.428 cm)", kFormulaParam_LightRadius },
		{ "lightx", "light world X", kFormulaParam_LightX },
		{ "lighty", "light world Y", kFormulaParam_LightY },
		{ "lightz", "light world Z", kFormulaParam_LightZ },
		{ "lightr", "diffuse red", kFormulaParam_LightR },
		{ "lightg", "diffuse green", kFormulaParam_LightG },
		{ "lightb", "diffuse blue", kFormulaParam_LightB },
		{ "lightambientr", "ambient red", kFormulaParam_LightAmbientR },
		{ "lightambientg", "ambient green", kFormulaParam_LightAmbientG },
		{ "lightambientb", "ambient blue", kFormulaParam_LightAmbientB },
		{ "lightchosenlastframe", "1 if this light held a slot last frame", kFormulaParam_LightChosenLastFrame },
		{ "lightframessincerender", "frames since this light's slot was last actually rendered into the shadow atlas; 1e6 sentinel when never rendered or unassigned", kFormulaParam_LightFramesSinceRender },
		{ "lightneverfades", "1 if lodFade disabled (permanent light)", kFormulaParam_LightNeverFades },
		{ "lightportalstrict", "1 if portal-strict (always 1 for shadow casters)", kFormulaParam_LightPortalStrict },
		{ "lightns", "1 if promoted from normal light (PromoteNormalToShadow)", kFormulaParam_LightNS },
		{ "lightconverted", "1 if light is in the converted (non-shadow) slot range", kFormulaParam_LightConverted },
		{ "lightdisplacement", "distance this light moved since its last shadow map render (game units; 0 when not yet tracked or in score formula)", kFormulaParam_LightDisplacement },
		{ "playerlightdistance", "distance from the player character to the light (game units; falls back to lightdistance when player unavailable)", kFormulaParam_PlayerLightDistance },
		// Dead in BOTH formulas as currently wired: the C++ side only computes and
		// sets this AFTER the redraw-interval formula has already evaluated for the
		// frame (ShadowScheduler.cpp), so it always reads 0 here too, not just in
		// the score formula. Fix requires reordering that computation ahead of the
		// formula call, not done yet -- do not rely on this parameter.
		{ "lightimportance", "contribution score: lum(diffuse*fade) * max(att_cam,att_plr) where att=(1-(dist/radius)^2)^2; currently always 0 in both formulas, see comment above", kFormulaParam_LightImportance },
		{ "lightisspot", "1 if this is a spot/frustum shadow light (BSShadowFrustumLight); 0 for omni / hemi / sun", kFormulaParam_LightIsSpot },
		{ "lightspotvisible", "1 if the spot's cone plausibly reaches the camera frustum, 0 otherwise. Always 1 for non-spot lights so existing omni-only formulas are unaffected", kFormulaParam_LightSpotVisible },
		{ "lightplayerattached", "1 if the light is attached to the player's scene graph (held torch, Candlelight); its shadow sits at the viewer, where artifacts are most visible", kFormulaParam_LightPlayerAttached },
		{ "lightcoverage", "projected screen coverage: (radius/viewZ)^2, clamped when the camera is inside the light (~4 max); 0 when fully behind the camera", kFormulaParam_LightCoverage },
		{ "lightscreenarea", "view-impact 0..1: fraction of the screen the light's influence sphere covers, clamped to the frustum. ~1 for a light filling the view, ~0 for one whose lit volume barely reaches the screen. Correctly counts lights behind the camera whose light (and shadows) still reach the visible scene -- prefer this over lightcoverage", kFormulaParam_LightScreenArea },
		{ "lightlum", "Rec.709 luminance of the diffuse color x engine fade", kFormulaParam_LightLum },
		{ "lightattcam", "Skyrim falloff attenuation (1-(d/r)^2)^2 at the camera; 0 outside the radius", kFormulaParam_LightAttCam },
		{ "lightattplayer", "Skyrim falloff attenuation (1-(d/r)^2)^2 at the player; 1 for a carried light", kFormulaParam_LightAttPlayer },
		{ "camerax", "camera world X", kFormulaParam_CameraX },
		{ "cameray", "camera world Y", kFormulaParam_CameraY },
		{ "cameraz", "camera world Z", kFormulaParam_CameraZ },
		{ "isinterior", "1 in interior cells, 0 outdoors", kFormulaParam_IsInterior },
		{ "timeofday", "in-game hour (0.0-24.0)", kFormulaParam_TimeOfDay },
		{ "frametime", "EMA-smoothed frame time (ms)", kFormulaParam_FrameTime },
		{ "frametarget", "90th-percentile recent frame time (ms) -- headroom ceiling", kFormulaParam_FrameTarget },
		{ "stableframes", "consecutive frames EMA has been below frametarget", kFormulaParam_StableFrames },
	};

	/// True if the light's scene-graph ancestry reaches the player's 3D
	/// (either person): held torches and Candlelight-style spell lights.
	bool IsPlayerAttachedLight(const RE::NiLight* ni);

	/// Geometric + photometric per-light signals computed once per candidate.
	struct LightGeometry
	{
		float lum = 0.0f;         ///< Rec.709 luminance of diffuse x fade
		float coverage = 0.0f;    ///< Projected solid-angle proxy; 0 behind camera
		float attCam = 0.0f;      ///< Skyrim falloff attenuation at camera
		float attPlr = 0.0f;      ///< Skyrim falloff attenuation at player
		float sizeProxy = 0.0f;   ///< Classifier input: max(sqrt(coverage), att)
		float screenArea = 0.0f;  ///< Viewport-clamped projected sphere area [0,1]
	};
	LightGeometry ComputeLightGeometry(const RE::BSShadowLight* light, const RE::NiCamera* camera, float lightRadius);

	/// Drops the EMA position anchor ComputeLightGeometry keeps for `ni`, if
	/// any. Call when a pool slot acquires a light pointer (fresh or
	/// recycled-address) so a stale anchor from whatever light previously
	/// held that address can't poison the newcomer's score for ~25 frames.
	void ResetScoreAnchor(const RE::NiLight* ni);

	/// Sets camera/scene formula params once per scheduler frame.
	void SetupSceneFormula(const RE::NiCamera* camera);

	/// Sets per-light formula params for a candidate light.
	void SetupLightFormula(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index);

	/// Evaluates s_formulaScore for a light.
	double CalculateLightScore(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index, float* outImpact = nullptr);

	/// True if NiLight was promoted normal to shadow.
	bool IsPromotedLight(RE::NiLight* ni);

	// ---------------------------------------------------------------------
	// Budget module (ShadowBudget.cpp)
	// ---------------------------------------------------------------------

	/// 90th-percentile of recent frame-time ring.
	float ComputeFrameTimePercentile90();

	// ---------------------------------------------------------------------
	// Slot allocator module (ShadowSlotAllocator.cpp)
	// ---------------------------------------------------------------------

	inline constexpr int32_t kFocusShadowBaseSlotIndex = 4;
	inline constexpr int32_t kFocusShadowMaxSlots = 4;

	extern std::int32_t s_initialShadowMapResolution;

	/// VRAM allocation verdict vs DXGI budget limits.
	struct VRAMVerdict
	{
		bool tight = false;
		bool over = false;
		ImVec4 colour{ 0.55f, 0.85f, 0.55f, 1 };
	};
	VRAMVerdict EvaluateVRAMVerdict(std::uint64_t shadowBytes, std::uint64_t freeBytes, std::uint64_t budgetBytes);

	/// Verifies actual kSHADOWMAPS slice count against requested count.
	void RefreshInstalledSlotCount();

	/// Reads kSHADOWMAPS underlying Texture2D descriptor.
	bool TryReadShadowTextureDesc(D3D11_TEXTURE2D_DESC& out);

	// ---------------------------------------------------------------------
	// Caster classifier module (ShadowCasterClassifier.cpp)
	// ---------------------------------------------------------------------

	// Contribution-culling diagnostics, defined in ShadowCasterClassifier.cpp;
	// exchanged/read by ScheduleShadowCasters for Tracy plots and the snapshot.
	extern std::atomic<uint32_t> s_casterCullCount;
	extern std::atomic<uint32_t> s_cullPoolDropCount;
	extern std::atomic<uint64_t> s_cullPoolDropTotal;
	extern std::atomic<uint64_t> s_casterCullTotal;

	// Accumulate-scoped handoff between EnableLight (writer) and the
	// AppendVirtual cull hooks (reader), set around each light's Accumulate call.
	extern std::atomic<RE::BSShadowLight*> s_currentCullLight;
	extern std::atomic<bool> s_accumRebuildAttach;
	extern std::unordered_set<const RE::BSGeometry*> s_healAttached;

	/// Static/dynamic split-cache caster-pass selector, written by EnableLight
	/// and read by the cull-append hooks.
	enum class CasterPass : int
	{
		All = 0,
		StaticOnly = 1,
		DynamicOnly = 2
	};
	extern std::atomic<int> s_cullPassMode;
	extern std::atomic<uint32_t> s_staticCasterDraws;
	extern std::atomic<uint32_t> s_dynamicCasterDraws;

	// Per-accumulate split-cache visitation state, reset/consumed by EnableLight.
	extern std::uint64_t s_visitStaticHash;
	extern std::atomic<uint32_t> s_visitDynamicCount;
	extern std::atomic<uint32_t> s_visitStaticCount;

	/// Per-caster movement-history record; classification memoized once per
	/// frame by ClassifyCaster (ShadowCasterClassifier.cpp).
	struct CasterMobility
	{
		int lastEpoch = -1;
		int lastVerifyEpoch = -1;
		int framesSinceMove = 0;
		int promoteBackoff = 1;
		float cx = 0.0f, cy = 0.0f, cz = 0.0f, cr = 0.0f;
		uint64_t foldHash = 0;
		bool foldHashValid = false;
		bool dynamic = true;
	};
	// Pruned periodically by ScheduleShadowCasters (ShadowScheduler.cpp).
	extern ankerl::unordered_dense::map<RE::BSGeometry*, CasterMobility> s_casterMobility;
	extern int s_casterClassEpoch;

	// Camera position captured at accumulate start (EnableLight, writer); the
	// contribution-cull hooks (reader) measure caster screen size from here.
	extern RE::NiPoint3 s_cullCameraPos;

	// True only across a static-cache bake pass, written by
	// RenderScheduledShadowLights; read via StaticPassRedirectActive() below.
	extern std::atomic<bool> s_staticPassActive;

	/// Services the multi-frame diagnostic recorder (devbench capture
	/// kind=shadowmaps); called once per frame from RenderScheduledShadowLights.
	void ServiceShadowFrameRecord();

	// ---------------------------------------------------------------------
	// Engine hooks module (ShadowEngineHooks.cpp)
	// ---------------------------------------------------------------------

#define ShadowField(light, member) \
	(globals::game::isVR ? (light)->GetVRRuntimeData().member : (light)->GetRuntimeData().member)

	RE::ShadowSceneNode* GetShadowSceneNode();
	RE::NiCamera* GetWorldCamera();

	/// True while interior cell BSPortalGraph is transitionally null.
	bool IsPortalGraphTransitioning();

	/// Engine count of focus shadow actors.
	int GetFocusShadowActorCount();
	bool GetSunBool2();

	/// Recomputes cached shadow-cull distance from settings.
	void CallUpdateShadowDistance(bool a_interior);

	/// Couples shadow-cull distance to light fade-out distance.
	void ApplyShadowToLightFadeMatch();

	bool* GetFocusShadowSelected();
	uint64_t* GetSunPtr();
	uint32_t* GetAccumLightSlot();
	uint32_t* GetMaskIndex();
	uint32_t* GetShadowMask();
	uint32_t* GetFrameLightCount();

	// VR-only globals
	bool GetVRDrawShadows();
	bool GetVRAccumFirst();
	float GetVRDRSWidthRatio();
	float GetVRDRSHeightRatio();

	// Engine function wrappers
	void GameSetupFocusShadowAccumulators(RE::BSShadowLight* light);
	void GameSetupFocusShadowMaps(RE::BSShadowLight* light, RE::NiCamera* cam);
	void GameEnableLight(RE::ShadowSceneNode* ssn, RE::BSLight* light);
	void GameSetShadowCasterSlot(RE::ShadowSceneNode* ssn, RE::BSLight* light, uint32_t index, uint32_t unk);
	void GameClearPortalVisibility(RE::BSPortalGraphEntry* entry);
	bool GamePortalHasSharedVisibility(RE::BSPortalGraphEntry* a, RE::BSPortalGraphEntry* b);
	void GameClearGeometryList(RE::BSLight* light);
	void GameAttachGeometry(RE::BSLight* light, RE::BSGeometry* geom);
	bool GameLightIsInRange(RE::BSLight* light, const RE::NiBound* bound, RE::NiLight* niLight, float scale);
	void GameApplyLensFlare(RE::BSLight* light);
	void GameVRPrepareShadowMaps(RE::BSLight* light);
	void GameVRAccumulateShadowMaps(RE::BSLight* light);
	void GameFrustumOverlap(RE::NiCamera* cam, float* coord, float* r1, float* r2, float eps);

	/// Culling process for first shadow descriptor of a light.
	RE::BSCullingProcess* GetLightCullingProcess(RE::BSShadowLight* light);

	/// Installs AppendVirtual hook for caster culling.
	void InstallCasterCullHook();

	extern bool s_bootEnabled;
	extern bool s_bootEnabledCaptured;
	extern Util::Settings::BootSnapshot<Settings> s_bootSnapshot;

	// ---------------------------------------------------------------------
	// Shadow atlas module (ShadowAtlas.cpp)
	// ---------------------------------------------------------------------

	extern bool s_bootAtlasEnabled;

	inline constexpr int32_t kVanillaShadowSliceCount = 8;
	inline constexpr uint32_t kAtlasMaxResolution = 16384;

	/// Slot's atlas tile metadata in texels.
	struct AtlasTileTexels
	{
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t size = 0;
		uint32_t lastRenderFrame = 0;
		bool contentValid = false;
	};

	/// Cumulative atlas clear path statistics.
	struct AtlasClearStats
	{
		uint32_t swallowed = 0;
		uint32_t passedThrough = 0;
		uint32_t tileClears = 0;
		uint32_t tileReallocs = 0;
		uint32_t ownerInvalidations = 0;
		/// EnsureSlotTile calls that walked down without granting the
		/// requested (or larger) class -- allocator-denied, not budget-capped.
		uint32_t allocDenied = 0;
	};
	AtlasClearStats GetAtlasClearStats();

	/// True when atlas is boot-enabled and resources exist.
	bool AtlasActive();

	/// Creates/updates atlas resources per frame.
	void UpdateAtlas();

	ID3D11DepthStencilView* AtlasDSV(bool readOnly);
	ID3D11ShaderResourceView* AtlasSRV();
	uint32_t AtlasDim();
	uint32_t AtlasBaseTile();
	float AtlasOccupancy();
	uint64_t AtlasVRAMBytes();
	uint32_t AtlasSnapResolution(uint32_t requested);
	uint32_t AtlasCapacityCells();
	uint32_t CellsForScale(float scale);
	bool EnsureSlotTile(int32_t poolSlot, float scale);
	void MarkSlotTileRendered(int32_t poolSlot, bool a_swapComplete = true);

	/// Radius and bias snapshot rasterized into a tile depth.
	struct ShadowBakeSnapshot
	{
		float radius = 0.0f;
		float bias = 0.0f;
	};
	bool SlotBakeSnapshotPending(int32_t poolSlot);
	void StoreSlotBakeSnapshot(int32_t poolSlot, const ShadowBakeSnapshot& snap);
	bool LoadSlotBakeSnapshot(int32_t poolSlot, ShadowBakeSnapshot& out);

	void FreeSlotTile(int32_t poolSlot);
	void FreeAllTiles();

	bool GetSlotTileTexels(int32_t poolSlot, AtlasTileTexels& out);
	int32_t FindFreeSlotByOwner(const void* a_owner);
	void DumpSlotTileRegion(int32_t poolSlot, uint32_t a_stamp);

	/// Slot's tile shader UV transform.
	struct AtlasRectUV
	{
		float scaleX = 0.0f;
		float scaleY = 0.0f;
		float biasX = 0.0f;
		float biasY = 0.0f;
		float classScale = 1.0f;
	};
	bool GetSlotAtlasRectUV(int32_t poolSlot, AtlasRectUV& out);

	/// Clears slot tile to far depth.
	void ClearSlotTile(int32_t poolSlot);

	// --- Static/dynamic split cache (parallel static depth atlas) ------------

	/// True when parallel static-cache atlas resources are ready.
	bool StaticAtlasReady();

	/// Depth-stencil view for parallel static-cache atlas.
	ID3D11DepthStencilView* StaticAtlasDSV(bool readOnly);

	/// True across static-cache bake pass.
	bool StaticPassRedirectActive();

	/// Clears slot static-cache tile to far depth.
	void ClearStaticSlotTile(int32_t poolSlot);

	/// Copies slot's static tile into live atlas tile.
	void CopyStaticTileToLive(int32_t poolSlot);

	/// Reads slot's static-cache bookkeeping. emptyOut, if non-null, reports whether
	/// the baked tile captured zero casters (a blank bake latched valid).
	bool GetSlotStaticState(int32_t poolSlot, uint64_t& hashOut, bool& validOut, bool* emptyOut = nullptr);

	/// Marks slot static tile baked with static-caster hash; a_sawCasters=false records a blank bake.
	void MarkSlotStaticRendered(int32_t poolSlot, uint64_t staticHash, bool a_sawCasters);

	/// Drops every occupied slot's static cache (not the live tile or ownership) --
	/// cell-grid-shift response, see s_pendingCellReset.
	void InvalidateAllStaticBakes();

	// ---------------------------------------------------------------------
	// Scheduler module entry points
	// ---------------------------------------------------------------------

	/// Replaces engine's CalculateActiveShadowCasterLights.
	void ScheduleShadowCasters();

	/// Replaces RenderActiveShadowCasterLights.
	void RenderScheduledShadowLights();

	/// Deactivates a shadow light.
	void DisableLight(RE::BSShadowLight* light);

	/// Demotes a shadow light to normal non-shadow light.
	void ConvertLight(RE::BSShadowLight* light, RE::ShadowSceneNode* ssn, bool isNS);

	/// Human-readable reason a light was converted.
	const char* ConvertReasonText(uintptr_t a_key);

	// ---------------------------------------------------------------------
	// Shared slot/viz state
	// ---------------------------------------------------------------------

	inline constexpr const char* kShadowTypeNames[] = { "Spot", "Hemisphere", "Omni" };

	extern std::vector<ShadowSlotInfo> s_shadowSlotInfos;
	extern uint32_t s_shadowSlotUsage;
	extern std::unordered_map<uintptr_t, ShadowSlotInfo> s_knownLights;
	extern bool s_shadowResolutionDirty;
	extern bool s_shadowDistanceDirty;

	/// Re-applies engine cached shadow-distance values after slider edit.
	void RefreshEngineShadowDistanceCache();

	/// Setting lookup in Display INI pref collection.
	RE::Setting* GetDisplaySetting(const char* a_name);
}
