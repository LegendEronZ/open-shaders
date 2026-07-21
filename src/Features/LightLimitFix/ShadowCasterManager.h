// ShadowCasterManager.h
// Shadow caster scheduling for LightLimitFix.
//
// Based on Intellightent by meh321
//   https://www.nexusmods.com/skyrimspecialedition/mods/172423
//
// Ported and adapted for Community Shaders by the Community Shaders team with permission.
//
// The original plugin managed shadow caster selection, temporal shadow
// update scheduling, and depth buffer extension entirely outside Community
// Shaders.  This file houses the CPU-side shadow scheduling subsystem so it
// can live alongside (and share settings with) LightLimitFix's GPU-side
// clustered light culling without coupling the two concerns inside a single
// translation unit.

#pragma once

#include <d3d11.h>
#include <functional>

#include "RE/B/BSShadowLight.h"
#include "RE/S/ShadowSceneNode.h"

#include "Features/LightLimitFix/ShadowCasterMath.h"
#include "Utils/RestartSettings.h"

struct ImVec4;

namespace ShadowCasterManager
{
	// Type-based shadow-caster check that bypasses the IsShadowLight vtable
	// hook (which flips to false for ConvertExcessToNormal-demoted lights).
	// Use this when callers need the intrinsic type, not the current shadow
	// state -- e.g. InverseSquareLighting's cutoff selection, where the
	// radius shouldn't oscillate as a light flips in and out of conversion.
	// Cost: one vtable-pointer compare.
	inline bool IsShadowLightType(RE::BSLight* bsLight)
	{
		return skyrim_cast<RE::BSShadowLight*>(bsLight) != nullptr;
	}

	// shadowLightsAccum iterator. shadowLightsAccum is a flat slot array
	// where a dual-paraboloid light occupies shadowMapCount==2 consecutive
	// physical slots (second is null). ForEachShadowLight advances by
	// shadowMapCount so each logical light is visited once.
	//
	// WARNING: _size is never updated -- do not push_back or use range-for /
	// BSTArray iterators on this directly.
	/// Conservative upper bound on shadowLightsAccum iteration index, derived
	/// from the active scheduler settings (ShadowLightCount + sun cascades).
	/// Used by ForEachShadowLight as a setting-aware safety cap so corrupt
	/// / non-null-terminated arrays can't loop forever, while still allowing
	/// iteration past BSTArray's static _capacity.
	std::uint32_t MaxShadowAccumIterationBound();

	/// kSHADOWMAPS texture-array slot count the engine actually allocated.
	/// 0 until the SRV becomes readable; the read is lazy and self-healing
	/// across frames so callers can use it without timing constraints.
	/// Consumers in the cluster pipeline, scheduler, and UI should call
	/// this rather than reaching into Deferred / the renderer directly.
	std::uint32_t GetInstalledSlotCount();

	/// True while an engine shadow-scene teardown (ClearLightArrays) is pending
	/// and not yet drained by ScheduleShadowCasters. During this window the
	/// engine frees shadow lights and their GPU resources, so per-frame consumers
	/// must skip iterating the accumulator and binding shadow resources to avoid
	/// referencing freed objects. Non-draining load; the scheduler owns the drain.
	bool IsSessionResetPending();

	/// Live VRAM telemetry used for shadow-array sizing decisions and stats.
	/// All values in bytes; populated from IDXGIAdapter3::QueryVideoMemoryInfo
	/// + the kSHADOWMAPS texture's actual geometry. valid=false when the
	/// adapter/texture aren't ready yet (e.g. before SetupResources).
	struct VRAMInfo
	{
		std::uint64_t currentUsageBytes = 0;  ///< VRAM currently allocated to this process (local heap)
		std::uint64_t budgetBytes = 0;        ///< Driver-suggested budget for this process
		std::uint64_t shadowArrayBytes = 0;   ///< Bytes currently used by the kSHADOWMAPS texture array
		std::uint32_t shadowWidth = 0;        ///< Per-slice width
		std::uint32_t shadowHeight = 0;       ///< Per-slice height
		std::uint32_t shadowSlices = 0;       ///< Current kSHADOWMAPS ArraySize
		std::uint32_t bytesPerSlice = 0;      ///< Per-slice byte cost (width*height*format size)
		bool valid = false;
	};
	VRAMInfo GetVRAMInfo();

	/// Predict the kSHADOWMAPS texture-array byte size for a given slice count
	/// using the current per-slice geometry. Returns 0 if VRAMInfo isn't valid yet.
	std::uint64_t ProjectShadowArrayBytes(std::uint32_t sliceCount);

	template <typename Fn>
	inline void ForEachShadowLight(const RE::BSTArray<RE::BSShadowLight*>& accum, Fn&& fn)
	{
		// Engine writes via SetShadowCasterLightArrayEntry which bypasses
		// BSTArray::push_back, so _capacity stays at the initial preallocation
		// -- using capacity as the bound silently caps SLF at vanilla shadow
		// counts. Use the null sentinel instead, with a setting-derived
		// safety cap (ShadowLightCount + sun cascades, with a small margin).
		// Per-pointer plausibility (low-address floor + alignment + user-mode
		// range) handles non-null garbage between our prepass and this read.
		const std::uint32_t maxIdx = MaxShadowAccumIterationBound();
		std::uint32_t idx = 0;
		while (idx < maxIdx) {
			RE::BSShadowLight* light = accum[idx];
			const auto raw = reinterpret_cast<std::uintptr_t>(light);
			if (!IsPlausibleShadowLightPtr(raw))  // null / misaligned / non-canonical
				break;
			fn(light);
			const std::uint32_t step = light->shadowMapCount;
			if (step == 0)
				break;
			const std::uint64_t next = static_cast<std::uint64_t>(idx) + step;
			if (next >= maxIdx)
				break;
			idx = static_cast<std::uint32_t>(next);
		}
	}

	// -------------------------------------------------------------------------
	// Formula parameter indices
	// -------------------------------------------------------------------------
	enum FormulaParams
	{
		kFormulaParam_LightIndex,
		kFormulaParam_LightIntensity,
		kFormulaParam_LightDistance,
		kFormulaParam_LightRadius,
		kFormulaParam_LightX,
		kFormulaParam_LightY,
		kFormulaParam_LightZ,
		kFormulaParam_LightR,
		kFormulaParam_LightG,
		kFormulaParam_LightB,
		kFormulaParam_LightAmbientR,
		kFormulaParam_LightAmbientG,
		kFormulaParam_LightAmbientB,
		kFormulaParam_LightChosenLastFrame,
		kFormulaParam_LightFramesSinceRender,  ///< frames since this light's slot was last rendered; large sentinel if never rendered or no slot
		kFormulaParam_LightNeverFades,
		kFormulaParam_LightPortalStrict,
		kFormulaParam_LightNS,
		kFormulaParam_LightConverted,
		kFormulaParam_LightDisplacement,    ///< distance moved since last shadow map render (game units)
		kFormulaParam_PlayerLightDistance,  ///< distance from the player character to the light (game units)
		kFormulaParam_LightImportance,      ///< contribution importance: lum(diffuse*fade) * max(att_cam, att_plr); set in interval loop only
		kFormulaParam_LightIsSpot,          ///< 1 if light is a spot (BSShadowFrustumLight), 0 otherwise
		kFormulaParam_LightSpotVisible,     ///< 1 if a spot's cone is plausibly visible to the camera (cone-aimed-at-frustum). Always 1 for non-spots so omni-only formulas aren't affected.
		kFormulaParam_LightPlayerAttached,  ///< 1 if the light rides the player's scene graph (held torch, Candlelight)
		kFormulaParam_LightCoverage,        ///< projected solid-angle proxy ((radius/viewZ)^2, camera-inside clamped)
		kFormulaParam_LightScreenArea,      ///< viewport-clamped projected sphere area [0,1]; correct view-impact (behind-camera safe)
		kFormulaParam_LightLum,             ///< Rec.709 luminance of diffuse x engine fade
		kFormulaParam_LightAttCam,          ///< Skyrim falloff attenuation at the camera
		kFormulaParam_LightAttPlayer,       ///< Skyrim falloff attenuation at the player

		kFormulaParam_CameraX,
		kFormulaParam_CameraY,
		kFormulaParam_CameraZ,
		kFormulaParam_IsInterior,
		kFormulaParam_TimeOfDay,

		kFormulaParam_FrameTime,     ///< EMA-smoothed frame time (ms)
		kFormulaParam_FrameTarget,   ///< 90th-percentile frame time (ms) — target budget ceiling
		kFormulaParam_StableFrames,  ///< consecutive frames the EMA has been below FrameTarget

		kFormulaParam_Max
	};

	// -------------------------------------------------------------------------
	// Expression-based formula evaluator (wraps exprtk)
	// -------------------------------------------------------------------------
	struct FormulaHelper
	{
		FormulaHelper();
		~FormulaHelper();

		FormulaHelper(const FormulaHelper&) = delete;
		FormulaHelper& operator=(const FormulaHelper&) = delete;
		FormulaHelper(FormulaHelper&&) = delete;
		FormulaHelper& operator=(FormulaHelper&&) = delete;

		bool Parse(const std::string& input);
		double Calculate();

		/// Re-parse with a new expression, replacing any previously compiled formula.
		/// Returns true on success. On failure the old formula remains active.
		bool Reparse(const std::string& input);

		/// Compile `input` into a temporary expression and return true if it succeeds.
		/// On failure, `errorOut` receives the first parser error message.
		/// Does NOT affect the active formula.
		static bool Validate(const std::string& input, std::string& errorOut);

		static void SetParam(int32_t index, double value);
		static double GetParam(int32_t index);

	private:
		void* _ptr;
	};
	// -------------------------------------------------------------------------
	// -------------------------------------------------------------------------
	// Budget mode enum
	// -------------------------------------------------------------------------
	enum class BudgetModeEnum : int32_t
	{
		Auto = 0,     ///< DEPRECATED: kept only for save-file backward compat. Migrated to Formula at load.
		Manual = 1,   ///< Fixed slider value
		Formula = 2,  ///< User-editable exprtk expression (default)
	};

	// -------------------------------------------------------------------------
	// Settings
	// All shadow-scheduling knobs.  Held inside LightLimitFix::Settings and
	// serialised as part of that JSON blob.  Pass a const-ref to Init().
	// -------------------------------------------------------------------------
	struct Settings
	{
		/// Enable the shadow caster scheduler entirely.  Requires a game restart to take effect.
		bool Enabled = true;

		/// Number of simultaneous shadow-casting point/spot lights (NOT counting the directional sun).
		/// 0 = scheduler active but selects no point lights (sun/directional unaffected).
		/// 4 = vanilla point light count with intelligent selection replacing the game's default.
		/// 5-127 = extended mode; depth buffer array is expanded beyond game's 8-slot limit
		///   when this exceeds 8. The practical ceiling is VRAM (per-slice cost
		///   of the kSHADOWMAPS texture array) -- the in-game settings panel
		///   shows a live projection so users can see when a value won't fit.
		/// Higher values allow more lights to hold stale shadow maps between redraws at
		/// the cost of startup memory. The redraw budget and interval formula control
		/// per-frame GPU cost independently.
		int32_t ShadowLightCount = 16;

		/// Number of additional converted-light slots (lights treated as normal lights
		/// for geometry but tracked alongside shadow casters when ConvertExcessToNormal is enabled).
		int32_t ConvertedShadowSlots = 32;

		/// Allow a newly-chosen light to draw even if it was not chosen last frame.
		bool AllowDrawNewLight = true;

		/// Hard cap on how many lights may re-render their shadow maps in one
		/// frame. Floored at kMinMaxRedrawPerFrame.
		int32_t MaxRedrawPerFrame = 16;

		/// Lower bound for MaxRedrawPerFrame. Below this the per-slot redraw
		/// rotation is slow enough that camera-relative jitter on the cluster
		/// shadow lookup crosses occluder silhouettes between TAA frames,
		/// producing visible shadow flicker on the nearest light's
		/// contribution. Enforced in the ImGui slider and on JSON load.
		static constexpr int32_t kMinMaxRedrawPerFrame = 4;

		/// How the per-frame shadow redraw budget is determined.
		/// Manual is the default — predictable, doesn't ping-pong, and matches
		/// the spirit of Intellightent's original behaviour. Formula is available
		/// for power users who want adaptive logic, with the caveat that any
		/// formula referencing `frametime` will tend to oscillate (rendering
		/// shadows raises frametime, which removes the headroom that allowed
		/// the budget — classic feedback loop without hysteresis).
		BudgetModeEnum BudgetMode = BudgetModeEnum::Manual;

		/// Per-frame time budget for shadow re-renders (milliseconds).
		/// Used in Manual mode.  Lights whose estimated GPU cost would exceed this
		/// are deferred to a later frame.
		float RedrawBudgetMs = 5.0f;

		/// Demote shadow lights that exceed the active caster limit to normal (non-shadow) lights
		/// so they still contribute diffuse lighting without a shadow-map cost.
		bool ConvertExcessToNormal = true;

		/// Promote normal (non-shadow) lights to shadow casters when there is budget.
		/// Disabled by default; experimental.
		bool PromoteNormalToShadow = false;

		/// Match the shadow-cull distance to the engine's light LOD fade-out
		/// distance each frame, so a caster's shadow exists exactly as long as its
		/// light is visible -- no on-approach pop, no shadows past where the light
		/// fades. Auto-adapts to interior-cell / weather overrides. Leaves the user's
		/// fInteriorShadowDistance/fShadowDistance prefs untouched. On by default --
		/// only takes effect while the SLF shadow scheduler is active, which already
		/// owns a redraw budget, so the extra reach is bounded.
		bool MatchShadowToLightFade = true;

		/// Store point/spot shadows in one variable-tile atlas texture instead
		/// of one full kSHADOWMAPS slice per light. VRAM becomes fixed
		/// (AtlasResolution^2 at the engine's shadow format) regardless of
		/// ShadowLightCount, and each light pays only its tile's fill cost.
		/// Boot-latched: the engine texture-array allocation depends on it.
		/// Requires extended mode and a driver that passes the ClearView
		/// depth-rect probe (else the array path is used and a warning logged).
		bool ShadowAtlas = true;

		/// Atlas texture dimension in texels (square). Snapped down to a
		/// buddy-aligned multiple of the quarter-tile size at creation.
		std::uint32_t AtlasResolution = 8192;

		/// Force-enable portal-strict on shadow casters as they're added by
		/// the engine. Per-type because portal-strict on spotlights drops
		/// culled-but-visible spots entirely, while on omnis/hemispheres it
		/// usefully tightens the engine's portal-graph visibility test.
		///
		/// Defaults: omni + hemi enforced, spotlights left to their
		/// engine-authored portal-strict flag.
		bool ForceEnablePortalStrictOmni = true;
		bool ForceEnablePortalStrictHemi = true;
		bool ForceEnablePortalStrictSpot = false;

		// --- Formula strings (exprtk expressions) ---

		/// Light priority scoring formula (exprtk). Variables:
		///   lightindex, lightintensity, lightdistance, playerlightdistance,
		///   lightradius, lightx/y/z, lightr/g/b, lightambientr/g/b,
		///   lightchosenlastframe, lightframessincerender, lightneverfades,
		///   lightportalstrict, lightns, lightconverted, camerax/y/z,
		///   isinterior, timeofday, lightisspot, lightspotvisible
		/// Industry-grounded (tiled/clustered-shadow importance): projected screen
		/// area of the light's influence x its luminance, with temporal
		/// stickiness. lightscreenarea is the correct view-impact term (counts
		/// lights behind the camera whose shadows still reach the view, unlike
		/// lightcoverage).
		/// The attenuation floor keeps a light that strongly lights the camera or
		/// the player even when its own screen footprint is small. Screen area
		/// alone is measured from the camera, so in third person a lamp beside the
		/// character -- where the player is actually looking -- scores near zero
		/// and loses its shadow. Third-person engines bias importance toward the
		/// hero for the same reason; RedrawIntervalFormula already does it via
		/// min(lightdistance, playerlightdistance). Keep the camera term primary,
		/// or a distant light filling the view gets starved instead.
		/// Luminance is COMPRESSED to [0.5, 1] deliberately: it modulates within a
		/// class band, it does not rank across them. Raw lightlum as a multiplier
		/// lets a bright distant brazier (HDR intensity ~2) outscore a dim carried
		/// torch by 4x and steal its tile, even though the torch encloses the
		/// camera and scores a full 1.0 of screen area. Geometry decides the band;
		/// intensity only orders within it. No carried-light special case is
		/// needed once luminance cannot out-shout the projection.
		/// max(0, 1 - lightframessincerender / 8) * 0.2 is smooth hysteresis:
		/// recently-rendered lights resist demotion across small score
		/// perturbations at the selection cutoff, decaying over 8 frames.
		std::string ScoreFormula = "max(lightscreenarea, 0.3 * max(lightattcam, lightattplayer)) * (0.5 + 0.5 * min(lightlum, 2) / 2) * (1 + max(0, 1 - lightframessincerender / 8) * 0.2)";

		/// Redraw interval formula (per light).  Higher = less frequent redraws.
		/// Uses min(lightdistance, playerlightdistance) so that a light near the player
		/// character is always treated as close even in third-person (camera is further away).
		/// `lightdisplacement` further reduces the interval for lights that have moved.
		std::string RedrawIntervalFormula = "min(10, (max(0, min(lightdistance, playerlightdistance) - lightradius * 0.5) / 500) / max(0.5, lightintensity)) * (lightconverted * 5 + 1) - min(lightdisplacement / 5, 10)";

		/// Redraw budget formula (per frame, in ms).  Used in Formula mode.
		/// Default mirrors Intellightent's original behaviour: a flat 1 ms outdoors
		/// (`isinterior` = 0) and 2 ms indoors (`isinterior` = 1). Predictable and
		/// doesn't oscillate.
		///
		/// Available variables: frametime (smoothed ms), frametarget (90th-pct ms),
		/// stableframes, isinterior, plus the per-light variables (used by ScoreFormula
		/// and RedrawIntervalFormula but evaluated to last-light values here).
		///
		/// Avoid adaptive formulas that subtract shadow GPU cost from frametime
		/// headroom -- they oscillate (rendering shadows raises frametime,
		/// which zeroes the budget, which drops frametime, restoring the
		/// budget). exprtk has no hysteresis state. Use static expressions.
		std::string RedrawBudgetFormula = "1 + isinterior";

		// --- Contribution-based caster culling ---

		/// Cull a shadow caster from a light's shadow render when its screen size
		/// from the CAMERA (caster world-bound radius / distance to the viewer) is
		/// below this. Measured from the viewer, not the light, so a caster close
		/// enough to fill the view is kept even if small, while a distant one in a
		/// corner is dropped even if large -- matching what the player perceives.
		/// Distant/peripheral casters produce tiny on-screen shadows that cost
		/// draw-call submission for no visible result, so trimming them shrinks
		/// the caster set and with it the per-light CPU cost. 0 disables.
		float CasterCullAngularMin = 0.0f;
		/// Light-level impact cull: a light whose on-screen relevance
		/// (max of screen-area and camera/player attenuation) stays below this
		/// converts to a non-shadow light (keeps diffuse, drops the shadow-map
		/// redraw). 0 disables. Distinct from CasterCullAngularMin, which culls
		/// casters WITHIN a light.
		float ShadowImpactFloor = 0.0f;

		// --- Importance scheduling curve ---

		/// Interval multiplier applied to high-importance lights (importance >= 1).
		/// Lower values make frequently-contributing lights update shadows more aggressively.
		/// Default: 0.05 (updates 40x more frequently than unimportant lights).
		float ImportanceMinScale = 0.05f;

		/// Interval multiplier applied to unimportant lights (importance == 0).
		/// Higher values defer dim or distant lights more aggressively.
		/// Default: 2.0.
		float ImportanceMaxScale = 2.0f;
	};

	/// Superseded ScoreFormula defaults, kept verbatim so LoadSettings can
	/// upgrade an untouched formula to the current default while leaving any
	/// user-customized formula alone.
	// The last released default; a persisted copy migrates to the current
	// default. Intra-branch intermediate defaults are omitted -- no shipped
	// build wrote them, so no user setting holds one.
	inline constexpr const char* kLegacyScoreFormulas[] = {
		"lightradius * lightintensity / (1 + ((1 - lightneverfades) * lightdistance) / 1000) * (1 + max(0, 1 - lightframessincerender / 8) * 0.4) * (1 + lightisspot * lightspotvisible)",
	};

	NLOHMANN_JSON_SERIALIZE_ENUM(BudgetModeEnum,
		{ { BudgetModeEnum::Auto, 0 }, { BudgetModeEnum::Manual, 1 }, { BudgetModeEnum::Formula, 2 } })

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		Settings,
		Enabled,
		ShadowLightCount,
		ConvertedShadowSlots,
		AllowDrawNewLight,
		MaxRedrawPerFrame,
		BudgetMode,
		RedrawBudgetMs,
		ConvertExcessToNormal,
		PromoteNormalToShadow,
		MatchShadowToLightFade,
		ShadowAtlas,
		AtlasResolution,
		ForceEnablePortalStrictOmni,
		ForceEnablePortalStrictHemi,
		ForceEnablePortalStrictSpot,
		ScoreFormula,
		RedrawIntervalFormula,
		RedrawBudgetFormula,
		CasterCullAngularMin,
		ShadowImpactFloor,
		ImportanceMinScale,
		ImportanceMaxScale)

	/// Restart-gated hook toggles: Install() applies them once at boot, so a
	/// runtime edit only takes effect after a restart. Drives pending banners.
	inline constexpr Util::Settings::RestartTable<Settings, 7> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, ConvertExcessToNormal, "Convert Excess Lights to Normal"),
		UTIL_RESTART_FIELD(Settings, PromoteNormalToShadow, "Promote Normal Lights to Shadow Casters"),
		UTIL_RESTART_FIELD(Settings, ForceEnablePortalStrictOmni, "Force Portal Strict on Omni Lights"),
		UTIL_RESTART_FIELD(Settings, ForceEnablePortalStrictHemi, "Force Portal Strict on Hemisphere Lights"),
		UTIL_RESTART_FIELD(Settings, ForceEnablePortalStrictSpot, "Force Portal Strict on Spot Lights"),
		UTIL_RESTART_FIELD(Settings, ShadowAtlas, "Shadow Atlas"),
		UTIL_RESTART_FIELD(Settings, AtlasResolution, "Atlas Resolution"),
	} };

	// -------------------------------------------------------------------------
	// Per-light schedule entry
	// -------------------------------------------------------------------------
	struct LightEntry
	{
		RE::BSShadowLight* Light{ nullptr };

		/// Sort key: LastDrawnFrame + computed interval.  Lower = higher priority.
		double RedrawScore{ 0.0 };

		/// Frame number this light last rendered its shadow map.
		int32_t LastDrawnFrame{ -1 };

		/// Set each frame by the scheduler; consumed by the render hook.
		bool RedrawFrame{ false };

		/// Slot index in the LightContainer array.
		int32_t Index{ -1 };

		/// World position of the light at its last rendered shadow map frame.
		/// Used to prioritise redraws for lights that have moved significantly.
		RE::NiPoint3 lastRenderedPos{ 0.0f, 0.0f, 0.0f };

		/// Consecutive frames the budget has wanted a larger tile class.
		/// Promotions apply only after the demand is stable (see scheduler).
		uint16_t promoteStreak{ 0 };

		/// Contribution-weighted importance score from the last scheduling frame.
		/// importance = luminance(diffuse × fade) × attenuation²(viewer, radius)
		/// where attenuation = max(1 − (dist/radius)², 0)  (Skyrim's quadratic falloff).
		/// Typically in [0, 1]; can exceed 1 for very bright lights at close range.
		/// Higher = light strongly illuminates the area around the viewer.
		float lastImportance{ 0.0f };

		/// Hash of the shadow scene at the most recent successful redraw:
		/// light pose + radius + each caster's worldBound + identity. Compared
		/// against the current frame's hash to detect when the cached shadow
		/// map is still pixel-correct (no geometric change since last render).
		/// Industry term: "cached shadow maps" (UE5, CryEngine, Frostbite).
		/// 0 sentinel = never-rendered; treat as "needs redraw" on first frame.
		std::uint64_t lastGeomHash{ 0 };

		/// Hash computed for the current frame's scoring pass. Promoted into
		/// lastGeomHash only when this entry actually redraws (RedrawFrame=true),
		/// so the cache key reflects what's *in the slot*, not what we observed.
		std::uint64_t pendingGeomHash{ 0 };

		/// ComputeShadowGeomHash() result cache; winners latch this value (via
		/// pendingGeomHash) into lastGeomHash, so its staleness bound
		/// (kGeomHashRehashInterval) is also lastGeomHash's bound.
		std::uint64_t cachedPendingGeomHash{ 0 };
		int32_t lastHashComputeFrame{ -1 };  ///< frame cachedPendingGeomHash was last (re)computed
		uint32_t lastHashGeomListSize{ 0 };  ///< light->geomList.size() as of that recompute

		/// Tile scale the slot content was actually rasterized at (fraction of
		/// the kSHADOWMAPS slice, corner-anchored). Shaders must sample with
		/// THIS value until the next redraw, regardless of the desired class.
		float renderedScale{ 1.0f };

		/// Tile scale the scheduler wants for the next redraw. A mismatch with
		/// renderedScale invalidates the cached shadow map (forces a redraw).
		/// Kept at min(desiredScale, budgetScale) so it is stable per frame;
		/// flipping between the importance class and the capacity clamp would
		/// defeat the cache check and redraw every frame.
		float pendingScale{ 1.0f };

		/// Importance-domain class (with promote/demote hysteresis) before the
		/// atlas capacity clamp.
		float desiredScale{ 1.0f };

		/// Largest class the atlas rank budget affords this light; 1 outside
		/// atlas mode.
		float budgetScale{ 1.0f };

		/// ScoreFormula value from the last scheduling frame: the single
		/// priority that ordered selection, and therefore also orders the
		/// atlas cell budget within a class band and drives the redraw
		/// importance curve (as a percentile).
		double lastScore{ 0.0 };

		void Clear()
		{
			Light = nullptr;
			LastDrawnFrame = -1;
			RedrawFrame = false;
			lastRenderedPos = { 0.0f, 0.0f, 0.0f };
			lastImportance = 0.0f;
			lastGeomHash = 0;
			pendingGeomHash = 0;
			cachedPendingGeomHash = 0;
			lastHashComputeFrame = -1;
			lastHashGeomListSize = 0;
			renderedScale = 1.0f;
			pendingScale = 1.0f;
			desiredScale = 1.0f;
			budgetScale = 1.0f;
			lastScore = 0.0;
		}
	};

	// -------------------------------------------------------------------------
	// Container for the active light pool
	// -------------------------------------------------------------------------
	struct LightContainer
	{
		LightEntry* Lights{ nullptr };

		/// true when index 0 is the directional sun (always active, never rescheduled).
		bool Sun{ false };

		/// Total allocated slots (ShadowLightCount + ConvertedShadowSlots).
		int32_t Size{ 0 };

		/// Returns the first free shadow-caster slot index, or -1 if full.
		int32_t FindFreeIndex(bool shadowSlot, int32_t shadowCount, int32_t convertCount) const;

		/// Returns the index of a light pointer in the shadow-caster range, or -1.
		int32_t FindLight(RE::BSShadowLight* light, int32_t shadowCount) const;

		/// First pool index of the point-light range. Equals 1 when Sun=true
		/// (slot 0 reserved for sun bookkeeping), 0 when Sun=false.
		int32_t PointLightFirst() const { return Sun ? 1 : 0; }

		/// One-past-last pool index of the point-light range, given the
		/// configured ShadowLightCount. Use as the exclusive upper bound for
		/// `for (i = PointLightFirst(); i < PointLightEnd(N); ++i)` iteration
		/// over chosen+candidate point lights (excludes converted slots which
		/// follow at [PointLightEnd..PointLightEnd + ConvertedShadowSlots)).
		///
		/// Off-by-one history: pre-this-helper, code iterated [0, shadowCount),
		/// which missed pool[shadowCount] when Sun=true. The highest point-light
		/// slot was then unfindable / unrendered / un-redrawn — silent loss of
		/// one shadow caster slot when a sun is present.
		int32_t PointLightEnd(int32_t shadowCount) const { return PointLightFirst() + shadowCount; }
	};

	// -------------------------------------------------------------------------
	// Per-light GPU timing tracker (sliding-window average over 8 frames)
	// -------------------------------------------------------------------------
	static constexpr int kBudgetWindowSize = 8;

	struct BudgetEntry
	{
		uint64_t Key{ 0 };
		uint32_t Tracked[kBudgetWindowSize]{};  ///< Ring buffer of per-frame µs costs.
		int32_t TrackedCount{ 0 };
		int32_t LastTrackedHelper{ -1 };
		uint32_t Progress{ 0 };  ///< Accumulated step-0 cost awaiting step-1.
		int32_t Current{ 0 };    ///< Rolling sum of Tracked[].

		void BeginStep(int32_t step);
		void EndStep(int32_t step, int32_t helperCounter);

		/// Commits one measured render cost (µs) into the ring. Used by the
		/// deferred GPU-timestamp path, which resolves several frames after
		/// the render was issued.
		void CommitCost(uint32_t costUs, int32_t helperCounter);

		/// Microseconds since the matching BeginStep, without committing.
		uint32_t ElapsedSinceBeginUs() const;

		/// Returns true when the entry hasn't been updated in ~600 scheduler ticks.
		bool IsExpired(int32_t helperCounter) const;

	private:
		int64_t _startTime{ 0 };
	};

	/// D3D11 timestamp machinery for per-light GPU render cost (defined in
	/// ShadowBudget.cpp). Owned via pimpl so the public header stays free of
	/// query plumbing.
	struct BudgetGpuTimer;

	struct BudgetTracker
	{
		BudgetTracker();
		~BudgetTracker();

		void Begin(int32_t step);
		void BeginLight(RE::BSShadowLight* light, int32_t step);
		void EndLight(RE::BSShadowLight* light, int32_t step);

		/// Brackets the shadow render pass for GPU cost measurement (one
		/// disjoint timestamp batch per frame). Step-1 BeginLight/EndLight
		/// pairs inside the bracket are timed on the GPU timeline; results
		/// commit asynchronously a few frames later. Falls back to the CPU
		/// (QPC) timing when queries are unavailable or the interval was
		/// disjoint. Render thread only.
		void BeginRenderBatch();
		void EndRenderBatch();

		/// Returns estimated render cost (µs) for a light.
		/// Falls back to the mean of all tracked lights for unseen lights.
		int32_t GetCost(RE::BSShadowLight* light) const;

		/// Returns the mean GPU cost (µs) averaged over all currently tracked lights.
		int32_t GetAverageCostUs() const;

	private:
		int32_t _counter{ 0 };
		std::unordered_map<uint64_t, std::unique_ptr<BudgetEntry>> _map;
		std::unique_ptr<BudgetGpuTimer> _gpu;

		void CleanupExpired();
		void CommitResolved(uint64_t key, uint32_t costUs);

		friend struct BudgetGpuTimer;
	};

	// -------------------------------------------------------------------------
	// Per-slot visualization metadata (filled by LLF::CopyShadowLightData)
	// -------------------------------------------------------------------------
	struct ShadowSlotInfo
	{
		uint32_t type = 0;       ///< Shadow type: 0=spot/frustum, 1=hemisphere, 2=omnidirectional
		float range = 0.0f;      ///< Light range (world units) -- radius for point lights, cone distance for spots
		bool valid = false;      ///< true when this slot was written this frame
		uintptr_t lightKey = 0;  ///< Light object pointer (stable key for suppression)
		/// Final uploaded ShadowParam.y: >0 valid radius, 0 safe-lit sentinel
		/// (empty descriptors or missing atlas tile), <0 suppression sentinel.
		float paramY = 0.0f;
		/// Owner reference's display name (falls back to the light node's
		/// scenegraph name, then form ID) so a table row identifies the light.
		std::string name;
	};

	/// Resets slot metadata for a new frame.  Call at the start of CopyShadowLightData.
	void BeginSlotFrame(uint32_t slotCount);

	/// Records metadata for one filled shadow slot.
	void RecordSlot(uint32_t depthSlot, const ShadowSlotInfo& info);

	/// Queues a one-shot disk dump of the shadow atlas depth texture (DDS)
	/// plus a slot-manifest JSON to CommunityShaders/Captures, serviced by
	/// the render thread's next shadow pass. Ground truth for tile contents
	/// without a RenderDoc attach (which perturbs the pipeline enough to
	/// hide some bugs). Thread-safe; no-op while the atlas is inactive.
	void RequestAtlasDump();
	/// Arms the multi-frame shadow recorder (frames clamped to [1,600]); with
	/// a_slot >= 0 also records that light's visited caster set per pass mode
	/// and, for frames <= 16, per-frame tile DDS dumps. One JSON under
	/// CommunityShaders/Captures at completion.
	void RequestShadowFrameRecord(uint32_t a_frames, int32_t a_slot);

	/// Returns true if the light with this pointer key has been suppressed by the user.
	/// Includes implicit suppression from solo mode (every key except the soloed one).
	bool IsSuppressed(uintptr_t lightKey);

	/// Returns true if any lights are currently suppressed (explicit or via solo).
	bool HasSuppressedLights();

	/// Returns true if any debug override is active (suppress / pin shadow /
	/// pin convert / solo). Used by the LLF overlay's visibility gate so the
	/// overlay stays available while users have any override in effect, even
	/// without the visualisation modes or the explicit ShowShadowOverlay toggle.
	bool HasAnyOverrides();

	// -------------------------------------------------------------------------
	// Scheduling diagnostics snapshot (headless inspection via devbench
	// `inspect kind=llfshadows`). The scheduler fills it only while the settings
	// menu is open or a snapshot was recently requested, to keep diagnostics off
	// the hot path otherwise.
	// -------------------------------------------------------------------------
	struct SchedSnapshot
	{
		bool valid = false;      ///< false until at least one pass has filled it
		uint32_t frame = 0;      ///< render frame the snapshot was taken on
		int total = 0;           ///< candidates examined this pass
		int chosen = 0;          ///< picked as shadow casters
		int excess = 0;          ///< over the shadow-caster budget
		int invalidCamera = 0;   ///< rejected by the engine UpdateCamera test
		int invalidPortal = 0;   ///< portal-graph unreachable
		int invalidFrustum = 0;  ///< off-screen / beyond shadow-cull distance
		int invalidLod = 0;      ///< past the light's LOD fade-out distance
		int invalidOther = 0;    ///< UpdateCamera failure, neither frustum nor LOD
		int slotsInUse = 0;      ///< occupied shadow slots at pass end
		/// Per non-chosen light: (light pointer, demotion-reason byte). Name via SchedReasonName().
		std::vector<std::pair<uintptr_t, uint8_t>> demoted;

		/// Per occupied point-light pool slot: tile classing and atlas
		/// placement, for headless assertion of the variable-resolution path.
		struct SlotState
		{
			int index = 0;  ///< pool slot (== kSHADOWMAPS slice / atlas slot key)
			uintptr_t light = 0;
			float importance = 0.0f;  ///< raw importance from the last scoring pass
			double score = 0.0;       ///< unified priority (ScoreFormula) that ranked this light
			float desiredScale = 1.0f;
			float budgetScale = 1.0f;
			float pendingScale = 1.0f;
			float renderedScale = 1.0f;
			uint32_t tileX = 0;  ///< atlas texels; tileSize 0 = no atlas tile
			uint32_t tileY = 0;
			uint32_t tileSize = 0;
			bool tileContentValid = false;
			// Read-side outcome, from the last upload's slot record: paramY is
			// the decisive sentinel (>0 shadows, 0 forced fully lit, <0 forced
			// dark); a healthy tile with paramY 0 means the descriptor/upload
			// stage bailed (e.g. the promoted-light empty-descriptor family).
			float uploadParamY = 0.0f;
			float uploadRange = 0.0f;
			bool uploadRecorded = false;  ///< slot record written this frame
			bool suppressed = false;
			bool promoted = false;  ///< light was promoted to shadow caster (s_shadowConvert)
		};
		std::vector<SlotState> slots;

		// Atlas summary; all zero while the atlas is inactive.
		uint32_t atlasDim = 0;
		uint32_t atlasCapacityCells = 0;
		float atlasOccupancy = 0.0f;
		uint64_t atlasVramBytes = 0;
		uint32_t atlasTileReallocs = 0;        ///< cumulative class-change reallocs (cache health)
		uint32_t atlasOwnerInvalidations = 0;  ///< cumulative slot-reassignment content drops
		uint32_t cpuAccumUsAvg = 0;            ///< CPU-only avg per Accumulate (cull walk + appends)
		uint32_t cpuSubmitUsAvg = 0;           ///< CPU-only avg per Render (pass setup + submission)
		uint32_t cpuEnableUsAvg = 0;           ///< CPU-only avg per EnableLight (setup + SafeEnableAndValidate)

		// Budget-tracker aggregates (GPU timestamps): the REST perf A/B
		// reads these instead of needing an external profiler attach.
		int32_t avgLightCostUs = 0;       ///< mean measured GPU cost per caster
		float avgRedrawsPerFrame = 0.0f;  ///< rolling mean of casters redrawn per frame

		/// Cumulative StaticOnly re-bakes since load. A bake re-rasterizes a
		/// light's whole static caster set into its cache tile, so differencing
		/// this across a run measures what the static cache spends rebuilding
		/// itself -- the cost its per-frame savings are netted against.
		uint64_t staticBakesTotal = 0;

		/// Redraws elided by the empty-dynamic sleep skip (a chosen light whose
		/// valid static bake saw no movers): this pass, and cumulative since
		/// load -- the direct measure of what the early-out saves.
		int sleepSkips = 0;
		uint64_t sleepSkipsTotal = 0;
	};

	/// Requests and returns the latest scheduling-diagnostics snapshot. Thread-safe
	/// (callable off the render thread). The request primes the scheduler to fill the
	/// snapshot for a short window, so the first call after an idle period may return a
	/// stale/empty snapshot (valid == false) -- poll again after a frame for fresh data.
	SchedSnapshot RequestSchedSnapshot();

	/// Stable lowercase name for a demotion-reason byte (SchedSnapshot::demoted.second):
	/// "portal" | "frustum" | "lod" | "excess" | "other" | "none".
	const char* SchedReasonName(uint8_t reason);

	// -------------------------------------------------------------------------
	// Debugging override API
	//
	// Per-light state pins (Shadow / Convert) override the scheduler's automatic
	// chosen/excess decision. Useful for isolating a single light's behaviour
	// when chasing scheduler / cluster pipeline regressions:
	//   - Pin Shadow: bias scoring so the light is forced into the chosen pool
	//     (gets a real shadow slot up to ShadowLightCount).
	//   - Pin Convert: bias scoring to the bottom and force ConvertLight in the
	//     excess branch regardless of the ConvertExcessToNormal user setting
	//     (still honours the spot-gate -- spots that can't safely convert are
	//     disabled instead).
	//   - Suppress: existing behaviour (ShadowParam.y = -1 for casters; cluster
	//     filter for converted / non-shadow lights via solo).
	//   - Solo: when set, every key OTHER than the soloed one is reported as
	//     suppressed via IsSuppressed(). Lets you isolate one light's
	//     contribution against a black scene.
	// -------------------------------------------------------------------------
	bool IsPinnedShadow(uintptr_t lightKey);
	bool IsPinnedConvert(uintptr_t lightKey);

	void SetPinnedShadow(uintptr_t lightKey, bool pinned);
	void SetPinnedConvert(uintptr_t lightKey, bool pinned);

	uintptr_t GetSoloLight();
	void SetSoloLight(uintptr_t lightKey);  // 0 clears solo

	/// Mouse-hover key for the per-frame debug pulse. Set per row by the table
	/// when the row is hovered; reset to 0 when the table redraws or the cursor
	/// leaves the table. The cluster light builder (LightLimitFix::UpdateLights)
	/// reads this to apply a magenta pulse to the matching light, making it
	/// visible in 3D against the rest of the scene.
	uintptr_t GetHoveredLight();
	void SetHoveredLight(uintptr_t lightKey);

	/// Below-impact-floor light set (rebuilt per schedule): the cluster builder
	/// magenta-tints these. The table's group-button hover populates it.
	void ClearHighlight();
	void AddHighlight(uintptr_t lightKey);
	bool IsHighlighted(uintptr_t lightKey);

	/// Lights the Light Impact Floor culled this frame; empty while the floor
	/// is 0, so the UI group only lists lights actually being cut.
	void ClearBelowFloor();
	void AddBelowFloor(uintptr_t lightKey);
	bool IsBelowFloor(uintptr_t lightKey);

	/// Drops every override (suppress / pin shadow / pin convert / solo).
	/// Useful when a debugging session has accumulated state and lights are
	/// mysteriously hidden — one click resets to the scheduler's auto behaviour.
	void ClearAllOverrides();

	/// Returns the number of shadow slots consumed this frame.
	uint32_t GetSlotUsage();

	/// Returns the number of active shadow-casting lights whose importance score
	/// exceeds 0.1 (lights meaningfully illuminating the camera or player area).
	uint32_t GetHighImportanceCount();

	/// Read-only view of the per-slot metadata for the current frame.
	const std::vector<ShadowSlotInfo>& GetSlotInfos();

	/// Returns the display name for a shadow type index (0=Spot, 1=Hemi, 2=Omni).
	const char* GetShadowTypeName(uint32_t type);

	/// Returns the golden-ratio hue colour for shadow-map slot slotIdx as an ImVec4.
	/// Matches the mode-8 shader visualisation colour.
	ImVec4 ShadowSlotHueColor(uint32_t slotIdx);

	/// Draw the interactive shadow caster table (suppress/filter/sort).
	/// compact=true caps height; showColor adds a hue swatch column (viz mode 8).
	/// sceneOnly=true shows only lights currently in the scene (overlay); false shows all known lights including disabled ones (settings).
	/// readOnly hides the per-row Mode/Solo buttons (overlay when the menu is
	/// closed isn't interactive anyway, so the buttons just take up space).
	void DrawShadowLightTable(bool compact, bool showColor, bool sceneOnly = false, bool readOnly = false);

	/// Canonical one-place "where are we vs the limits" summary. Used by both
	/// the menu's Active Casters block and the overlay header so the same
	/// numbers appear identically in both views. clusterCount/clusterMax come
	/// from LightLimitFix; the rest is read from SCM internal state.
	void DrawShadowSummary(uint32_t clusterCount, uint32_t clusterMax, uint32_t shadowUnshadowedLightCount);

	// -------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------

	/// Call once from LightLimitFix::PostPostLoad() before Install().
	/// Allocates the light container and initialises state from settings.
	void Init(const Settings& settings);

	/// Install all game hooks.  Call from LightLimitFix::PostPostLoad().
	void Install(const Settings& settings);

	/// Per-frame update: refreshes installed slot count from the texture-array
	/// capacity and applies settings changes (pool resize, etc.). The actual
	/// scheduling -- choosing which lights cast shadows this frame -- happens
	/// in the hooked `CalculateActiveShadowCasters` path via ScheduleShadowCasters.
	/// Call from LightLimitFix::Prepass().
	void Update(const Settings& settings, RE::ShadowSceneNode* shadowSceneNode,
		RE::NiCamera* worldCamera);

	/// Clear all transient session state (pool entries, converted-light
	/// tracking, debug overrides). Used by the LoadingMenu hook to drop
	/// pointers to lights the engine is about to free during fast-travel
	/// / cell change. The per-frame reconciliation in ScheduleShadowCasters
	/// covers the same ground for incremental changes; this is the wholesale
	/// reset for known scene boundaries so the UI counter and table read 0
	/// during the loading screen instead of carrying stale entries forward.
	/// Driven by LightLimitFix::OnSceneTransitionReset on the render thread (LoadingMenu open),
	/// so the clear serializes with the settings-menu table iteration instead of racing it.
	void ResetSession();

	/// Returns a read-only view of the active light pool for UI/visualization.
	const LightContainer& GetLights();

	/// True when SCM owns shadow scheduling this session (enabled at boot, no
	/// external conflict). False = engine's vanilla pipeline. Boot-latched.
	bool IsActive();

	/// Returns the kSHADOWMAPS texture-array slot for an active point/spot
	/// shadow light as a raw slice index 0..GetInstalledSlotCount()-1, or -1
	/// when the light is either not active in the SCM pool OR is the sun.
	/// Consumers (ShadowRenderer upload, LightLimitFix cluster builder,
	/// strict-light shadow-flag setup) treat the -1 sentinel as "skip" --
	/// the sun renders to kSHADOWMAPS_ESRAM (a separate texture) and has no
	/// kSHADOWMAPS slice; inactive lights have no slot at all.
	/// When SCM is active, reads the s_lights pool (not the descriptor's
	/// shadowmapIndex, which ReturnShadowmaps() can corrupt). When inactive the
	/// pool is empty, so it falls back to the engine's own
	/// shadowmapDescriptors[0].shadowmapIndex (the vanilla slice).
	int32_t GetShadowSlot(RE::BSShadowLight* light);

	/// Tile scale the kSHADOWMAPS slot content was last rasterized at.
	/// Takes the pool slot GetShadowSlot returned
	/// (pool index == texture slice for point lights); out-of-range slots
	/// return 1.0. Consumed by the ShadowRenderer upload as ShadowParam.w so
	/// sampling always matches the rasterized footprint.
	float GetRenderedTileScale(int32_t poolSlot);

	/// Visit every shadow light currently demoted to non-shadow rendering via
	/// ConvertExcessToNormal.  These lights live in the engine's activeShadowLights
	/// list (0x148) but are reported as non-shadow by Hook_IsShadowLight.  The
	/// cluster pipeline (LightLimitFix::UpdateLights) needs to inject them into
	/// lightsData[] without the Shadow flag so they still contribute diffuse light.
	///
	/// Visitor signature: void(RE::BSShadowLight* light).  Pointers are stable for
	/// the duration of the call (no concurrent scheduler mutation).
	void ForEachConvertedLight(const std::function<void(RE::BSShadowLight*)>& visitor);

	/// Draw scheduler stats (avg redraws/frame and avg per-light cost).
	/// Reads internal SCM state so the caller doesn't need accessors. Intended
	/// to render directly under DrawShadowLightTable for testing context.
	void DrawShadowSchedulerStats();

	/// Draw per-mode overlay info for shadow-related visualisation modes (3-9).
	/// Call from LightLimitFix::DrawOverlay() inside the vizOn block for modes >= 3.
	/// totalLightCount is the current clustered light count owned by LightLimitFix.
	void DrawOverlayShadowModeInfo(uint32_t mode, uint32_t shadowUnshadowedLightCount, uint32_t totalLightCount);

	/// Appends tooltip text for visualisation modes 3-9 (all shadow-specific).
	/// Call from LightLimitFix::DrawSettings() inside the LightsVisualisationMode hover tooltip,
	/// immediately after the LLF-owned entries for modes 0-2.
	void DrawVisualisationTooltipShadowModes();

	/// Draw the ImGui settings panel for the shadow caster scheduler.
	/// Call from LightLimitFix::DrawSettings().
	void DrawSettings(Settings& settings);

	/// Caster Cull + Light Impact Floor presets and their sliders. Shared by
	/// DrawSettings' own Advanced panel and LightLimitFix::DrawVRPerformanceSettings
	/// so both stay bound to the same settings and never drift out of sync.
	void DrawImpactCullControls(Settings& settings);

	/// Apply any Skyrim-side INI overrides SCM owns (currently just
	/// iShadowMapResolution:Display) at LoadSettings time. The engine has
	/// already read SkyrimPrefs.ini at startup so this is a no-op for now,
	/// but the seam exists for future overrides that need a pre-Install
	/// hook. Call from LightLimitFix::LoadSettings.
	void LoadINISettings();

	/// Persist any Skyrim-side INI settings SCM owns directly to the user's
	/// SkyrimPrefs.ini in their Documents folder. Only writes when the user
	/// actually edited a value this session. Call from
	/// LightLimitFix::SaveSettings.
	void SaveINISettings();

}
