// ShadowCasterInternal.h
// Shared state for the ShadowCasterManager implementation; include only from Shadow*.cpp. Public API is ShadowCasterManager.h.

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
#include "Utils/BootSnapshot.h"

namespace ShadowCasterManager
{
	/// Bounds a per-light/per-caster memo map keyed by raw engine pointers:
	/// dead keys accumulate silently, so drop the whole map past a cap. State
	/// rebuilds transparently the next frame, so a full clear is cheaper than
	/// tracking liveness.
	template <class Map>
	inline void PruneIfOversized(Map& map, size_t cap)
	{
		if (map.size() > cap)
			map.clear();
	}

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

	// Per-frame slot count [0,4] the engine's focus shadow renderer claims;
	// reserves [kFocusShadowBaseSlotIndex, +s_focusShadowSlots) from the pool.
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
		int sleep_skips = 0;                 // redraws elided by the empty-dynamic sleep predicate
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

	// Serializes engine bulk teardown (ClearLightArrays) against an in-flight
	// render: freeing mid-walk zeroes render-pass nodes under the walker.
	extern std::atomic<int> s_shadowFlushReaders;
	extern std::atomic<bool> s_teardownWaiting;

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

	/// Geometric + photometric per-light signals, computed once per candidate
	/// and shared between the formula variables and the scheduler.
	struct LightGeometry
	{
		float lum = 0.0f;         ///< Rec.709 luminance of diffuse x fade
		float coverage = 0.0f;    ///< projected solid-angle proxy; 0 behind camera
		float attCam = 0.0f;      ///< Skyrim falloff attenuation at the camera
		float attPlr = 0.0f;      ///< Skyrim falloff attenuation at the player
		float sizeProxy = 0.0f;   ///< classifier input: max(sqrt(coverage), att)
		float screenArea = 0.0f;  ///< viewport-clamped projected sphere area [0,1] (view impact)
	};
	LightGeometry ComputeLightGeometry(const RE::NiLight* ni, const RE::NiCamera* camera, float lightRadius);

	/// Sets camera/scene formula params. Called once per scheduler frame.
	void SetupSceneFormula(const RE::NiCamera* camera);

	/// Sets all per-light formula params for a candidate light.
	void SetupLightFormula(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index);

	/// Runs SetupLightFormula then evaluates s_formulaScore (0.0 when unset).
	/// outImpact (optional): max(screenArea, attCam, attPlr) -- the light's
	/// on-screen shadow relevance, for the impact-floor cull.
	double CalculateLightScore(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index, float* outImpact = nullptr);

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

	/// Reads kSHADOWMAPS's underlying Texture2D desc (the SRV's ViewDimension
	/// lies about the array). false while the texture isn't readable yet.
	bool TryReadShadowTextureDesc(D3D11_TEXTURE2D_DESC& out);

	// ---------------------------------------------------------------------
	// Engine hooks module (ShadowEngineHooks.cpp)
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
	void GameAttachGeometry(RE::BSLight* light, RE::BSGeometry* geom);
	bool GameLightIsInRange(RE::BSLight* light, const RE::NiBound* bound, RE::NiLight* niLight, float scale);
	void GameApplyLensFlare(RE::BSLight* light);
	void GameVRPrepareShadowMaps(RE::BSLight* light);
	void GameVRAccumulateShadowMaps(RE::BSLight* light);
	void GameFrustumOverlap(RE::NiCamera* cam, float* coord, float* r1, float* r2, float eps);

	/// Culling process for the first shadow descriptor of a light.
	RE::BSCullingProcess* GetLightCullingProcess(RE::BSShadowLight* light);

	/// Installs the parabolic AppendVirtual hook that contribution-culls
	/// point-light shadow casters. Called once from Install().
	void InstallCasterCullHook();

	// Boot-latched Enabled flag (captured by Install; read by IsActive and the
	// restart-required UI label).
	extern bool s_bootEnabled;
	extern bool s_bootEnabledCaptured;

	// Boot-latched snapshot of the kRestartFields hook toggles; latched by
	// Install() alongside s_bootEnabled, read by the pending-restart banners.
	extern Util::Settings::BootSnapshot<Settings> s_bootSnapshot;

	// ---------------------------------------------------------------------
	// Shadow atlas module (ShadowAtlas.cpp)
	// ---------------------------------------------------------------------

	// Boot-latched atlas enable: the kSHADOWMAPS array extension decision at
	// BSShaderRenderTargets::Create depends on it, so runtime toggles cannot
	// change it (mirrors s_bootEnabled).
	extern bool s_bootAtlasEnabled;

	// Engine kSHADOWMAPS slice count without the creation-loop patch; atlas
	// mode keeps the array at this size, and pre-atlas frames must not
	// schedule slots beyond it.
	inline constexpr int32_t kVanillaShadowSliceCount = 8;

	// D3D11 texture dimension ceiling; also the largest UI-selectable
	// atlas resolution.
	inline constexpr uint32_t kAtlasMaxResolution = 16384;

	/// A slot's atlas tile in texels. contentValid only after the tile has
	/// rendered at least once (freshly allocated tiles hold stale depth).
	struct AtlasTileTexels
	{
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t size = 0;
		uint32_t lastRenderFrame = 0;
		bool contentValid = false;
	};

	/// Diagnostic counters for the atlas clear paths (cumulative since boot).
	struct AtlasClearStats
	{
		uint32_t swallowed = 0;           ///< full-surface clears blocked on atlas views
		uint32_t passedThrough = 0;       ///< clears on other views (proves the hook is live)
		uint32_t tileClears = 0;          ///< our own per-tile ClearView calls
		uint32_t tileReallocs = 0;        ///< class-change tile reallocations (cache busts)
		uint32_t ownerInvalidations = 0;  ///< slot handed to a different light (content dropped)
	};
	AtlasClearStats GetAtlasClearStats();

	/// True when the atlas is boot-enabled and its resources exist. Cheap
	/// flag check, safe inside draw-time hooks; never creates resources.
	bool AtlasActive();

	/// Creates atlas resources on first use (runs the ClearView probe, which
	/// stalls on a GPU readback) and tracks pool reallocation. Call once per
	/// frame at the shadow-pass entry, never mid-pass: readiness must not
	/// flip between draws of the same frame.
	void UpdateAtlas();

	ID3D11DepthStencilView* AtlasDSV(bool readOnly);
	ID3D11ShaderResourceView* AtlasSRV();
	uint32_t AtlasDim();
	uint32_t AtlasBaseTile();
	float AtlasOccupancy();

	/// Atlas texture footprint in bytes (0 until resources exist).
	uint64_t AtlasVRAMBytes();

	/// The dimension a requested AtlasResolution would clamp+snap to this
	/// session (0 until resources exist). UI restart banners must compare
	/// through this, not the raw setting.
	uint32_t AtlasSnapResolution(uint32_t requested);

	/// Total allocator cells (quarter-class tiles) the atlas holds.
	uint32_t AtlasCapacityCells();

	/// Cells a tile of the given class scale consumes (full 16, half 4,
	/// quarter 1).
	uint32_t CellsForScale(float scale);

	/// Ensures the slot has a tile sized for `scale` (reallocates on class
	/// change; walks down classes under atlas pressure). false = no tile.
	bool EnsureSlotTile(int32_t poolSlot, float scale);

	/// Marks the slot's tile content valid; call after the light's Render.
	/// a_swapComplete: the raster produced complete content, so a staged
	/// promotion tile may swap in (false for bakes / movers-only composites).
	void MarkSlotTileRendered(int32_t poolSlot, bool a_swapComplete = true);

	/// Radius/bias a tile's depth was rastered with. Between redraws the upload
	/// must advertise these instead of the live light's: flame flicker animates
	/// the radius every frame, and receiver depth normalized by the live radius
	/// drifts off depth baked at the old one (pulsing false occlusion over the
	/// light's whole footprint). ShadowProj stays LIVE -- freezing it pins a
	/// moving light's shadow in world space and goes stale across loads.
	struct ShadowBakeSnapshot
	{
		float radius = 0.0f;
		float bias = 0.0f;
	};
	/// True when content landed since the last store; the renderer must refresh
	/// the snapshot from the live values it uploads that frame.
	bool SlotBakeSnapshotPending(int32_t poolSlot);
	void StoreSlotBakeSnapshot(int32_t poolSlot, const ShadowBakeSnapshot& snap);
	bool LoadSlotBakeSnapshot(int32_t poolSlot, ShadowBakeSnapshot& out);

	void FreeSlotTile(int32_t poolSlot);
	void FreeAllTiles();

	bool GetSlotTileTexels(int32_t poolSlot, AtlasTileTexels& out);
	/// Free slot still holding a_owner's rendered tile within the orphan grace
	/// window (-1 if none) -- lets a gate-flapped light reclaim its content.
	int32_t FindFreeSlotByOwner(const void* a_owner);
	/// Diagnostic: readback ONE slot's tile region to Captures/scm_rec_slot<S>_f<N>.dds (GPU stall).
	void DumpSlotTileRegion(int32_t poolSlot, uint32_t a_stamp);

	/// A slot's tile as the shader UV transform (uv * scale + bias). Owns the
	/// AtlasRect packing convention. False until the tile has rendered content.
	struct AtlasRectUV
	{
		float scaleX = 0.0f;
		float scaleY = 0.0f;
		float biasX = 0.0f;
		float biasY = 0.0f;
		/// Advertised tile size / full class, from the same tile as the rect;
		/// feeds ShadowParam.w so the shader's bias scaling matches the rect.
		float classScale = 1.0f;
	};
	bool GetSlotAtlasRectUV(int32_t poolSlot, AtlasRectUV& out);

	/// Rect-clears the slot's tile to far depth. Call once per redraw before
	/// the light renders (halves of a dual paraboloid share the tile).
	void ClearSlotTile(int32_t poolSlot);

	// --- Static/dynamic split cache (parallel static depth atlas) ------------

	/// True once the parallel static-cache atlas resources exist. Lazily
	/// created by UpdateAtlas once the live atlas is ready; a creation
	/// failure latches the feature off (live atlas only).
	bool StaticAtlasReady();

	/// DSV of the parallel static-cache atlas (same dims/layout as the live
	/// atlas). The depth-select hook returns this during a StaticOnly bake pass.
	ID3D11DepthStencilView* StaticAtlasDSV(bool readOnly);

	/// True only across a static-cache bake pass, so the depth-select hooks
	/// route the engine's shadow render into the static atlas instead of live.
	bool StaticPassRedirectActive();

	/// Rect-clears the slot's static-cache tile to far depth (before a bake).
	void ClearStaticSlotTile(int32_t poolSlot);

	/// Copies the slot's baked static tile from the static atlas into the live
	/// atlas tile, seeding the dynamic pass with the cached static depth.
	void CopyStaticTileToLive(int32_t poolSlot);

	/// Reads the slot's static-cache bookkeeping: the geom-hash baked into the
	/// static tile and whether it holds valid content. False = no tile.
	bool GetSlotStaticState(int32_t poolSlot, uint64_t& hashOut, bool& validOut);

	/// Marks the slot's static tile baked with the given static-caster hash.
	void MarkSlotStaticRendered(int32_t poolSlot, uint64_t staticHash);

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
