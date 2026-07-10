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
#include <unordered_set>
#include <vector>

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

	/// Total LightEntry slots: sun (1) + shadow casters (≥4) + converted pool.
	int32_t LightContainerSize(const Settings& s);
}
