// ShadowCasterInternal.h
// Shared state and cross-module declarations for the ShadowCasterManager
// implementation. Internal to the shadow scheduling subsystem -- include only
// from the ShadowCaster*.cpp / Shadow*.cpp translation units in this
// directory. The public API lives in ShadowCasterManager.h.

#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "ShadowCasterManager.h"

namespace ShadowCasterManager
{
	// ---------------------------------------------------------------------
	// Core scheduler state (definitions in ShadowCasterManager.cpp)
	// ---------------------------------------------------------------------

	extern Settings s_settings;
	extern LightContainer s_lights;
	extern BudgetTracker s_budget;

	// External conflict detection -- set during Install(), checked by Update()
	// and DrawSettings().
	extern bool s_externalConflict;
	extern std::string s_conflictMessage;

	// Per-frame count of kSHADOWMAPS slots claimed by the engine's focus
	// shadow renderer (player + tracked NPCs, max 4). Read from
	// FocusShadowActors.size each frame; values clamp to [0, 4]. Reserves
	// the slot range [kFocusShadowBaseSlotIndex .. +s_focusShadowSlots) =
	// [4 .. 4+count) from the point-light pool dynamically: zero focus
	// actors means the full pool is available, four means slots 4-7 are
	// off-limits. Point lights occupying a freshly-claimed slot are
	// ejected at scheduling time and re-allocated to a free slot or
	// converted as excess.
	extern int s_focusShadowSlots;

	// Rolling redraw / budget-consumed history (128-frame window) for
	// DrawSettings statistics.
	inline constexpr int kRedrawHistorySize = 128;
	extern int32_t s_redrawHistory[kRedrawHistorySize];
	extern int32_t s_redrawHistoryPos;
	extern int32_t s_redrawSum;
	extern int32_t s_budgetHistory[kRedrawHistorySize];
	extern int32_t s_budgetHistoryPos;
	extern int64_t s_budgetSum;

	// Frame-time tracking — used by Formula's frametime/frametarget/stableframes
	// formula params, the shared frame-state diagnostic block, and stats UI.
	// Persists in both Manual and Formula modes; the cost is one float per frame.
	inline constexpr int kFrameWindow = 120;  // ~2 s at 60 fps
	extern float s_ftRing[kFrameWindow];
	extern int s_ftHead;
	extern int s_ftCount;
	extern float s_ftEMA;
	extern int s_stableFrames;
	extern float s_autoBudgetMs;  // last computed budget; used by UI, scheduling, and stats

	// "Steady" state thresholds for the shared frame-state diagnostic.
	// Mirror the old Auto-mode hysteresis values so the indicator behaves the
	// same way users grew used to, just informational rather than driving control.
	inline constexpr float kFrameHeadroomDeadZoneMs = 0.3f;  // |headroom| below this = "steady"
	inline constexpr float kFrameHeadroomSafetyMs = 0.5f;    // headroom must clear this before "growing"

	// Budget tracking for UI display
	extern int32_t s_redrawnLightsThisFrame;
	extern int32_t s_totalShadowLightsThisFrame;
	extern uint32_t s_highImportanceLightCount;
	extern float s_redrawnLightsSmoothed;  // EMA-smoothed for stable UI display

	// Tracy diagnostic counters reset at the start of each scheduler frame.
	// Each candidate-handling path increments its bucket; values are emitted
	// as TracyPlot at frame end so a capture can be queried to identify
	// which paths fire under which budget/setting combinations. Cross-
	// reference per-action ZoneText emissions (light pointer, reason) to
	// identify *which* lights are hitting each path.
	struct SchedDiagCounters
	{
		int candidates_total = 0;
		int candidates_chosen = 0;
		int candidates_excess = 0;
		int candidates_invalid_camera = 0;
		int candidates_invalid_portal = 0;
		int candidates_invalid_frustum = 0;  // sub-reason: outside camera frustum
		int candidates_invalid_lod = 0;      // sub-reason: lodDimmer zeroed (engine LOD fade)
		int candidates_invalid_other = 0;    // invalidCamera but neither frustum nor LOD flag
		int converted_invalid = 0;           // ConvertLight from c.invalidCamera path
		int converted_excess = 0;            // ConvertLight from c.excess path
		int disabled_invalid = 0;            // DisableLight from c.invalid path (portal/spot/no-convert)
		int disabled_excess = 0;             // DisableLight from c.excess path (spot/no-convert)
		int reconciliation_clears = 0;       // slot freed because light gone from activeShadowLights
		int slots_in_use = 0;                // sampled at frame end
		int first_render_skips = 0;          // chosen lights deferred from shadow set: no valid slice yet
	};
	extern SchedDiagCounters s_schedDiag;

	// Maximum ShadowLightCount the installed infrastructure supports.
	// Set once by Install() to the *requested* count; later refined by
	// RefreshInstalledSlotCount() to reflect what the GPU actually allocated.
	// Update() clamps the user-facing setting to this.
	extern int32_t s_installedShadowLightCount;

	// What SCM asked the engine for. Equals settings.ShadowLightCount --
	// the sun lives in a separate texture (kSHADOWMAPS_ESRAM), so there's
	// no +1 sun cascade slice in kSHADOWMAPS. Captured at Install so the
	// post-allocation verification can detect VRAM-exhaustion fallbacks
	// where the actual texture ends up smaller than requested.
	extern uint32_t s_requestedSlotCount;

	// Total kSHADOWMAPS texture-array capacity *as actually allocated*.
	// 0 until kSHADOWMAPS exists and we've read its real ArraySize back.
	// Owned here (not in Deferred) because SCM is the only thing that
	// modifies the engine's allocation request, and verification of that
	// request is the same code path. Consumers (LLF cluster pipeline,
	// SCM scheduler clamp, SCM UI) read via GetInstalledSlotCount().
	extern uint32_t s_installedSlotCount;

	// True once we've logged a verification result. Prevents spam if the
	// SRV stays null forever (vanilla-disabled session) or oscillates.
	extern bool s_slotCountLogged;

	// Formula instances (allocated at Init if formula strings are non-empty)
	extern std::unique_ptr<FormulaHelper> s_formulaScore;
	extern std::unique_ptr<FormulaHelper> s_formulaRedrawInterval;
	extern std::unique_ptr<FormulaHelper> s_formulaRedrawBudget;

	// Lights converted to normal (non-shadow) lights for diffuse-only rendering
	struct ConvertedLight
	{
		RE::BSShadowLight* light;
		bool isNS;
	};
	extern std::vector<ConvertedLight> s_normalConvert;
	extern std::set<RE::NiLight*> s_shadowConvert;
	// Promoted lights whose descriptor pool-slots were initialized once (scheduler-time),
	// covering lights never enabled -- EnableLight only inits slot winners.
	extern std::set<RE::NiLight*> s_shadowConvertDescriptorInited;
	// Guards both sets above. Game-thread hooks (Add/SetLight/Remove) mutate them while the
	// render-thread scheduler reads and reconciles; concurrent std::set mutation corrupts the
	// tree and hangs a later traversal. Lock every access; never re-entered while held.
	extern std::mutex s_shadowConvertMutex;

	// Set by Hook_ClearLightArrays on engine bulk teardown, drained atop ScheduleShadowCasters
	// so stale s_lights/convert pointers clear before the next pass dereferences a slot.
	extern std::atomic<bool> s_pendingSessionReset;

	// Serializes render-thread portalGraph readers (scheduler/AccumulateLight, shared) against
	// ResetScene nulling/swapping ssn->portalGraph (exclusive) -- the cross-thread null deref.
	extern std::shared_mutex s_portalGraphMutex;

	// User suppression set (lightKey = BSShadowLight pointer cast to uintptr_t).
	// Persisted across light lifetimes so suppressing a torch survives the player
	// leaving and returning to a cell.
	extern std::unordered_set<uintptr_t> s_suppressedLights;

	// Debugging overrides — see header docs for ClearAllOverrides / SetPinnedShadow / etc.
	// The scheduler reads s_pinShadow / s_pinConvert to bias candidate scoring;
	// the settings/overlay tables write them.
	extern std::unordered_set<uintptr_t> s_pinShadow;   ///< force chosen (top of score sort)
	extern std::unordered_set<uintptr_t> s_pinConvert;  ///< force excess + ConvertLight
	extern uintptr_t s_soloLight;                       ///< 0 = no solo
	extern uintptr_t s_hoverLightKey;                   ///< transient (per table draw)

	// ---------------------------------------------------------------------
	// Formula module (ShadowFormula.cpp)
	// ---------------------------------------------------------------------

	struct FormulaVarInfo
	{
		const char* name;
		const char* description;
		int32_t index;
	};

	// Single authoritative list of formula variables.
	// Drives both symbol table registration and the formula editor help text.
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
		{ "lightimportance", "contribution score: lum(diffuse*fade) * max(att_cam,att_plr) where att=(1-(dist/radius)^2)^2; 0 in score formula", kFormulaParam_LightImportance },
		{ "lightisspot", "1 if this is a spot/frustum shadow light (BSShadowFrustumLight); 0 for omni / hemi / sun", kFormulaParam_LightIsSpot },
		{ "lightspotvisible", "1 if the spot's cone plausibly reaches the camera frustum, 0 otherwise. Always 1 for non-spot lights so existing omni-only formulas are unaffected", kFormulaParam_LightSpotVisible },
		{ "camerax", "camera world X", kFormulaParam_CameraX },
		{ "cameray", "camera world Y", kFormulaParam_CameraY },
		{ "cameraz", "camera world Z", kFormulaParam_CameraZ },
		{ "isinterior", "1 in interior cells, 0 outdoors", kFormulaParam_IsInterior },
		{ "timeofday", "in-game hour (0.0-24.0)", kFormulaParam_TimeOfDay },
		{ "frametime", "EMA-smoothed frame time (ms)", kFormulaParam_FrameTime },
		{ "frametarget", "90th-percentile recent frame time (ms) -- headroom ceiling", kFormulaParam_FrameTarget },
		{ "stableframes", "consecutive frames EMA has been below frametarget", kFormulaParam_StableFrames },
	};

	/// Sets camera/scene formula params. Called once per scheduler frame.
	void SetupSceneFormula(const RE::NiCamera* camera);

	/// Sets all per-light formula params for a candidate light.
	void SetupLightFormula(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index);

	/// Runs SetupLightFormula then evaluates s_formulaScore (0.0 when unset).
	double CalculateLightScore(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index);

	/// True if this NiLight was promoted normal->shadow (PromoteNormalToShadow).
	bool IsPromotedLight(RE::NiLight* ni);

	// ---------------------------------------------------------------------
	// Budget module (ShadowBudget.cpp)
	// ---------------------------------------------------------------------

	/// 90th-percentile of the recent frame-time ring (see FrameTimePercentile90).
	float ComputeFrameTimePercentile90();

	// ---------------------------------------------------------------------
	// Slot allocator module (ShadowSlotAllocator.cpp)
	// ---------------------------------------------------------------------

	// Engine focus-shadow slot range within kSHADOWMAPS: the engine claims
	// [base, base+max) for focus rendering; extended mode reserves them for
	// point lights instead.
	inline constexpr int32_t kFocusShadowBaseSlotIndex = 4;
	inline constexpr int32_t kFocusShadowMaxSlots = 4;

	// Resolution actually used to allocate kSHADOWMAPS this session; latched
	// lazily from the real texture geometry, not the boot-time INI value.
	extern std::int32_t s_initialShadowMapResolution;

	// Verdict for a candidate shadow-array footprint vs the DXGI budget.
	// "tight" = free VRAM below 512 MB or shadow array > 25% of budget.
	// "over"  = free VRAM below 128 MB or shadow array > 50% of budget.
	// Driven by free headroom rather than shadow share because a small
	// array next to a tight budget is just as risky as a huge one in a
	// roomy budget.
	struct VRAMVerdict
	{
		bool tight = false;
		bool over = false;
		ImVec4 colour{ 0.55f, 0.85f, 0.55f, 1 };  // green by default
	};
	VRAMVerdict EvaluateVRAMVerdict(std::uint64_t shadowBytes, std::uint64_t freeBytes, std::uint64_t budgetBytes);

	/// Lazily verifies the engine's actual kSHADOWMAPS slice count against the
	/// requested count, clamping the scheduler on mismatch. Self-healing until
	/// the texture becomes readable.
	void RefreshInstalledSlotCount();

	// ---------------------------------------------------------------------
	// Engine hooks module (ShadowEngineHooks.cpp): thin wrappers around game
	// globals and engine functions, shared with the scheduler. All
	// REL::RelocationID pairs are (SE_id, AE_id); VR addresses verified
	// against the VR address library CSV. Raw pointers returned by the
	// Get*Ptr/Get*Selected accessors are engine globals resolved once via
	// the address library -- stable for the process lifetime, safe to cache.
	// ---------------------------------------------------------------------

// Convenience: runtime-aware shadow-light field accessor (SE vs VR RuntimeData differ).
// Usage: ShadowField(light, maskIndex) = 3;
// Function-style macro: it ignores the namespace and leaks into every TU that
// includes this header, which must stay SCM-internal.
#define ShadowField(light, member) \
	(globals::game::isVR ? (light)->GetVRRuntimeData().member : (light)->GetRuntimeData().member)

	RE::ShadowSceneNode* GetShadowSceneNode();
	RE::NiCamera* GetWorldCamera();

	/// True while an interior cell's BSPortalGraph is transiently null mid-transition.
	bool IsPortalGraphTransitioning();

	/// Engine per-frame count of focus shadow actors (player + tracked NPCs).
	int GetFocusShadowActorCount();
	bool GetSunBool2();

	/// Recompute the engine's cached shadow-cull square from the live settings.
	void CallUpdateShadowDistance(bool a_interior);

	/// Couple the point-light shadow-cull distance to the light fade-out distance.
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
	void GameApplyLensFlare(RE::BSLight* light);
	void GameVRPrepareShadowMaps(RE::BSLight* light);
	void GameVRAccumulateShadowMaps(RE::BSLight* light);
	void GameFrustumOverlap(RE::NiCamera* cam, float* coord, float* r1, float* r2, float eps);

	/// Culling process for the first shadow descriptor of a light.
	RE::BSCullingProcess* GetLightCullingProcess(RE::BSShadowLight* light);

	// Boot-latched Enabled flag (captured by Install; read by IsActive and the
	// restart-required UI label).
	extern bool s_bootEnabled;
	extern bool s_bootEnabledCaptured;

	// ---------------------------------------------------------------------
	// Scheduler module entry points (called from the engine hook thunks)
	// ---------------------------------------------------------------------

	/// Replaces the engine's CalculateActiveShadowCasterLights.
	void ScheduleShadowCasters();

	/// Replaces RenderActiveShadowCasterLights; redraws lights flagged RedrawFrame.
	void RenderScheduledShadowLights();

	/// Deactivates a shadow light: releases its shadow maps and portal visibility.
	void DisableLight(RE::BSShadowLight* light);

	/// Demotes a shadow light to a normal (non-shadow) light for diffuse-only rendering.
	void ConvertLight(RE::BSShadowLight* light, RE::ShadowSceneNode* ssn, bool isNS);

	/// Human-readable reason a light was converted (for the UI table); "" when untracked.
	const char* ConvertReasonText(uintptr_t a_key);

	// ---------------------------------------------------------------------
	// Slot/viz state shared between the slot-frame API
	// (ShadowCasterManager.cpp) and the UI module (ShadowCasterUI.cpp)
	// ---------------------------------------------------------------------

	inline constexpr const char* kShadowTypeNames[] = { "Spot", "Hemisphere", "Omni" };

	extern std::vector<ShadowSlotInfo> s_shadowSlotInfos;
	extern uint32_t s_shadowSlotUsage;

	// Persists last-seen ShadowSlotInfo for every light ever recorded this session,
	// so suppressed lights that leave the active slots still have metadata for the settings table.
	extern std::unordered_map<uintptr_t, ShadowSlotInfo> s_knownLights;

	// Set when the user edits INI-owned values this session; drives the
	// restart-required labels and SaveINISettings persistence.
	extern bool s_shadowResolutionDirty;
	extern bool s_shadowDistanceDirty;

	/// Re-applies the engine's cached shadow-distance values after a slider edit.
	void RefreshEngineShadowDistanceCache();

	/// Setting lookup in the Display INI pref collection; nullptr when absent.
	RE::Setting* GetDisplaySetting(const char* a_name);
}
